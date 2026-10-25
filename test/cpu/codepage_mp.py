#!/usr/bin/env python3
"""A code change made by one processor and run by another: a two-processor
boot image for the code-page map's windows (JIT-05; docs/cpu-fidelity.md,
"The code-page map between processors").

    codepage_mp.py <alphabox-binary> [label] [--cpu ROW] [--port N]
                   [--iterations N] [--polls K] [--no-audit] [--keep]

Two processors of the row (default ev68cb) on the ES40, the image in place
of the console (image.py's PALcode and loader), processor 0 starting
processor 1 through the dual-port RAM as quiesce_mp.py does. Processor 0,
the writer, and processor 1, the executor, go through N iterations, each
on a page P of its own that no block was ever compiled from:

  writer    stores the old code into P (LDA R0,#1; RET) and a few data
            quadwords after it, from compiled code: P is now cached as
            writable inline in its data page cache. Signals READY.
  executor  MB, IMB, then calls P 15 times (interpreted: one short of the
            compile threshold). Signals WARM.
  writer    runs the poll code below once, before GO: the executor's last
            real IMB made this processor drop its blocks, and a block's
            first run after that passes the dispatcher, which would honour
            any request. Then GO, and the same code again, chained from end
            to end: K polls, each a block of its own (a BR starts it) with
            no backward branch, so no gated exit, and nothing the other
            processor asks of the writer is looked at until the run ends.
            A poll is a Cchip MISC read (slow and harmless, so that the run
            lasts) and a load of CODE; from the poll that sees CODE on,
            each stores the new instruction word (LDA R0,#2) into P,
            executes MB and stores DONE: the ARM's write, MB, flag for
            another processor's I-stream, without a branch. The store goes
            through the translation cached before P became code, unless the
            writer dropped it. A run that ends without CODE waits for it in
            a loop, which honours the request first, and stores the new
            code after that ("late").
  executor  calls P 25 more times: the block is compiled, P becomes a code
            page and every processor is asked to drop its inline-writable
            translation of it. Then MB, IMB (a real flush: P's marking
            counted a write), one more call, and CODE. On DONE: MB, IMB,
            one call of P, and ACK. The call must return 2, the new code; 1
            is the old block run after a correct code change.

The image counts the outcomes and prints them; the run passes when no
iteration ran the old code and nothing else went wrong. With --audit (the
default) the image runs a second time under ALPHABOX_JIT_NOPFLUSH=2, which
flushes every time but reports a block whose source changed while the
code-page map said nothing was written: the window itself, whether or not
the flush that run did saved it. Both must be clean. Exit status 0 when
everything holds. "IMB" is the image's CALL_PAL 8E, its IC_FLUSH, after an MB
as a PALcode's IMB does. A JIT_VERIFY build compiles at the first call, before
GO, and never chains, so it has nothing to show; nor has the interpreter;
both must pass.
"""
import argparse
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import alpha_asm as A  # noqa: E402
import cpu_oracle  # noqa: E402
import image  # noqa: E402
import rows  # noqa: E402
from alpha_asm import OP, R31, Asm, call_pal, jmp, mem, mfc, opl, opr  # noqa: E402
from image import kseg  # noqa: E402

DATA_PA = 0x1E0000            # the shared page: one 64-byte line each
WHO, ALIVE, READY, WARM, GO, CODE, DONE, ACK = (
    0x000, 0x040, 0x080, 0x0C0, 0x100, 0x140, 0x180, 0x1C0)
# Counts (writer's, then executor's), one quadword each from 0x400.
RES = 0x400
FIELDS = ("iterations", "in the run", "late", "new code", "old code",
          "other value", "wrong before", "first old", "run cycles",
          "executor cycles")
P_BASE = 0x400000             # page i at P_BASE + i * 8 KB
PAGE = 0x2000
DATA_OFF = 0x1000             # the writer's data quadwords in P

DPR_START1 = 0x80110000000 + (0x3428 << 6)   # DPR byte 0x3428: start CPU 1
CCHIP_MISC = 0x801A0000080

OLD_W0 = mem(OP["LDA"], 0, R31, 1)            # LDA R0,#1
NEW_W0 = mem(OP["LDA"], 0, R31, 2)            # LDA R0,#2
RET_W1 = jmp(2, R31, 26)                      # RET R31,(R26)
WARM_CALLS = 15
HOT_CALLS = 25

CFG = """sys0 = tsunami
{
  memory.bits = 26;
  rom.srm = "absent.exe";
  rom.decompressed = "oracle.rom";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  exit_on_pal_halt = true;

  cpu0 = %(cpu)s
  {
    speed = 800M;%(tpus)s
  }
  cpu1 = %(cpu)s
  {
    speed = 800M;%(tpus)s
  }

  serial0 = serial
  {
    address = "127.0.0.1";
    port = %(port)d;
  }

  pci0.7 = ali
  {
  }
}
"""


def seg48(pa):
    return image.SEG48 | pa


def build(iterations, polls, warm=WARM_CALLS):
    a = Asm(image.CODE_PA)
    D = 20                                      # r20: the shared page
    P = 21                                      # r21: this iteration's page
    I = 22                                      # r22: the iteration
    N = 23                                      # r23: how many
    PB = 19                                     # r19: P_BASE (KSEG)
    K = 6                                       # r6: i + 1, the flag value
    a.branch(OP["BR"], R31, "main")
    image.emit_print_hex(a)
    a.label("fatal")
    a.li(image.R_UART, image.UART_VA)
    image.emit_print_text(a, "\n@@FATAL\n")
    a.li(16, kseg(image.TR_PA))
    a.li(17, 7)
    a.branch(OP["BSR"], 18, "print_hex")
    a.emit(call_pal(0x00))

    def wait_eq(off, tag):
        """Spin (a backward loop) until the quadword at off is r6."""
        a.label(tag)
        a.emit(mem(OP["LDQ"], 1, D, off))
        a.emit(opr(0x10, 0x29, 1, K, 1))         # SUBQ r1,r6,r1
        a.branch(OP["BNE"], 1, tag)

    def page_of_i():
        a.emit(opl(0x12, 0x39, I, 13, 1))         # SLL r22,#13,r1
        a.emit(opr(0x10, 0x20, PB, 1, P))         # ADDQ r19,r1,r21
        a.emit(opl(0x10, 0x20, I, 1, K))          # ADDQ r22,#1,r6

    def count(off):
        a.emit(mem(OP["LDQ"], 1, D, RES + off))
        a.emit(opl(0x10, 0x20, 1, 1, 1))
        a.emit(mem(OP["STQ"], 1, D, RES + off))

    def forward_run():
        """Forward-only code, no backward branch: K polls. Each is a Cchip
        MISC read (slow and harmless, so that the run lasts) and a load of
        CODE; r9 = 1 once CODE was r6. From then on each poll stores the new
        code into P, executes MB and sets DONE: the ARM's write, MB, flag,
        without a branch (before CODE the store goes to P's data half, and
        DONE gets 0, which nobody waits for)."""
        for _ in range(polls):
            # A BR over one word ends the block (forward: no gate). Without
            # one, straight-line code is interpreted in dispatch batches,
            # and a span a batch cuts is never recorded, so never compiled.
            a.emit(A.bra(OP["BR"], R31, 1))
            a.emit(opr(0x11, 0x20, R31, R31, R31))  # skipped
            a.emit(mem(OP["LDQ"], 11, 18, 0))       # Cchip MISC
            a.emit(mem(OP["LDQ"], 1, D, CODE))
            a.emit(opr(0x10, 0x2D, 1, K, 1))      # CMPEQ r1,r6,r1
            a.emit(opr(0x11, 0x20, 9, 1, 9))      # BIS r9,r1,r9
            a.emit(opr(0x11, 0x20, 16, 16, 2))    # BIS: r2 = P + 0x1800
            a.emit(opr(0x11, 0x26, 9, P, 2))      # CMOVNE r9,r21,r2
            a.emit(mem(OP["STL"], 4, 2, 0))       # the new code (or data)
            a.emit(mfc(A.MISC["MB"]))
            a.emit(opr(0x11, 0x20, R31, R31, 3))
            a.emit(opr(0x11, 0x26, 9, K, 3))      # CMOVNE r9,r6,r3
            a.emit(mem(OP["STQ"], 3, D, DONE))

    def rpcc(r):
        a.emit(mfc(A.MISC["RPCC"], r))
        a.emit(opl(0x12, 0x31, r, 0x0F, r))       # ZAPNOT: 32-bit count

    # --- both processors -------------------------------------------------
    a.label("main")
    a.li(image.R_UART, image.UART_VA)
    a.li(D, kseg(DATA_PA))
    a.li(PB, kseg(P_BASE))
    a.li(N, iterations)
    a.emit(mem(OP["LDA"], I, R31, 0))
    a.emit(mem(OP["LDQ"], 1, D, WHO))
    a.branch(OP["BNE"], 1, "executor")
    a.emit(mem(OP["LDA"], 1, R31, 1))
    a.emit(mem(OP["STQ"], 1, D, WHO))
    a.emit(mfc(A.MISC["MB"]))
    a.li(5, seg48(DPR_START1))                    # start processor 1
    a.emit(mem(OP["STQ"], R31, 5, 0))
    a.emit(mem(OP["LDA"], K, R31, 1))
    wait_eq(ALIVE, "w_alive")

    # --- processor 0: the writer -------------------------------------------
    a.li(18, seg48(CCHIP_MISC))                   # r18: the slow device read
    a.li(12, OLD_W0)                              # r12, r13: the old code
    a.li(13, RET_W1)
    a.li(4, NEW_W0)
    a.label("w_iter")
    page_of_i()
    a.emit(mem(OP["STL"], 12, P, 0))              # the old code
    a.emit(mem(OP["STL"], 13, P, 4))
    a.emit(mem(OP["LDA"], 7, R31, 8))             # and data after it
    a.emit(opr(0x11, 0x20, P, P, 8))              # BIS: r8 = P
    a.label("w_data")
    a.emit(mem(OP["STQ"], 7, 8, DATA_OFF))
    a.emit(opl(0x10, 0x20, 8, 8, 8))
    a.emit(opl(0x10, 0x29, 7, 1, 7))
    a.branch(OP["BGT"], 7, "w_data")
    a.emit(mfc(A.MISC["MB"]))
    a.emit(mem(OP["STQ"], K, D, READY))
    wait_eq(WARM, "w_warm")
    a.emit(mem(OP["LDA"], 16, P, 0x1800))         # r16: P's data half
    # The run twice: first before GO, where CODE cannot be r6 yet, so that
    # every block of it is linked to the next after this processor dropped
    # its blocks for the other's last real IMB (a block's first run after
    # that goes through the dispatcher, which honours the flush request);
    # then after GO, chained from end to end.
    a.emit(mem(OP["LDA"], 15, R31, 0))
    a.label("w_pass")
    a.emit(opr(0x11, 0x20, R31, R31, 9))          # r9: CODE seen in the run
    rpcc(10)
    forward_run()
    rpcc(11)
    a.branch(OP["BNE"], 15, "w_after")
    a.emit(mem(OP["STQ"], K, D, GO))
    a.emit(mem(OP["LDA"], 15, R31, 1))
    a.branch(OP["BR"], R31, "w_pass")
    a.label("w_after")
    a.emit(opr(0x10, 0x29, 11, 10, 11))
    a.emit(opl(0x12, 0x31, 11, 0x0F, 11))
    a.emit(mem(OP["STQ"], 11, D, RES + 8 * 8))    # run cycles (the last)
    a.branch(OP["BEQ"], 9, "w_late")
    count(8 * 1)
    a.branch(OP["BR"], R31, "w_ack")
    a.label("w_late")
    wait_eq(CODE, "w_code")
    a.emit(mem(OP["STL"], 4, P, 0))
    a.emit(mfc(A.MISC["MB"]))
    a.emit(mem(OP["STQ"], K, D, DONE))
    count(8 * 2)
    a.label("w_ack")
    wait_eq(ACK, "w_wack")
    a.emit(opl(0x10, 0x20, I, 1, I))
    a.emit(opr(0x10, 0x1D, I, N, 1))              # CMPULT r22,r23,r1
    a.branch(OP["BNE"], 1, "w_iter")
    a.emit(mem(OP["STQ"], N, D, RES))
    image.emit_print_text(a, "\n@@BEGIN %d\n" % len(FIELDS))
    a.li(16, kseg(DATA_PA + RES))
    a.li(17, len(FIELDS))
    a.branch(OP["BSR"], 18, "print_hex")
    image.emit_print_text(a, "@@END\n")
    a.emit(call_pal(0x00))

    # --- processor 1: the executor -----------------------------------------
    def call_p():
        a.emit(jmp(1, 26, P))                     # JSR R26,(R21)

    a.label("executor")
    a.emit(mem(OP["LDA"], 1, R31, 1))
    a.emit(mem(OP["STQ"], 1, D, ALIVE))
    a.emit(mem(OP["LDA"], 1, R31, -1))
    a.emit(mem(OP["STQ"], 1, D, RES + 8 * 7))     # first old: none yet
    a.label("x_iter")
    page_of_i()
    wait_eq(READY, "x_ready")
    a.emit(mfc(A.MISC["MB"]))
    a.emit(call_pal(0x8E))                        # IMB (IC_FLUSH)
    for n, tag in ((warm, "x_warm"), (HOT_CALLS, "x_hot")):
        a.emit(mem(OP["LDA"], 7, R31, n))
        a.label(tag)
        call_p()
        a.emit(mem(OP["LDA"], 1, 0, -1))
        a.branch(OP["BEQ"], 1, tag + "_ok")
        count(8 * 6)                              # wrong before the change
        a.label(tag + "_ok")
        a.emit(opl(0x10, 0x29, 7, 1, 7))
        a.branch(OP["BGT"], 7, tag)
        if tag == "x_warm":
            a.emit(mem(OP["STQ"], K, D, WARM))
            wait_eq(GO, "x_go")
            rpcc(12)
    a.emit(mfc(A.MISC["MB"]))
    a.emit(call_pal(0x8E))                        # IMB: P is compiled now
    call_p()
    a.emit(mem(OP["STQ"], K, D, CODE))
    rpcc(13)
    a.emit(opr(0x10, 0x29, 13, 12, 13))
    a.emit(opl(0x12, 0x31, 13, 0x0F, 13))
    a.emit(mem(OP["STQ"], 13, D, RES + 8 * 9))    # executor cycles (the last)
    wait_eq(DONE, "x_done")
    a.emit(mfc(A.MISC["MB"]))
    a.emit(call_pal(0x8E))                        # IMB, then run the new code
    call_p()
    a.emit(mem(OP["LDA"], 1, 0, -2))
    a.branch(OP["BNE"], 1, "x_not_new")
    count(8 * 3)
    a.branch(OP["BR"], R31, "x_ack")
    a.label("x_not_new")
    a.emit(mem(OP["LDA"], 1, 0, -1))
    a.branch(OP["BNE"], 1, "x_other")
    count(8 * 4)                                  # the old code ran
    a.emit(mem(OP["LDQ"], 1, D, RES + 8 * 7))
    a.branch(OP["BGE"], 1, "x_ack")
    a.emit(mem(OP["STQ"], I, D, RES + 8 * 7))     # the first one
    a.branch(OP["BR"], R31, "x_ack")
    a.label("x_other")
    count(8 * 5)
    a.label("x_ack")
    a.emit(mfc(A.MISC["MB"]))
    a.emit(mem(OP["STQ"], K, D, ACK))
    a.emit(opl(0x10, 0x20, I, 1, I))
    a.emit(opr(0x10, 0x1D, I, N, 1))
    a.branch(OP["BNE"], 1, "x_iter")
    a.label("idle")
    a.branch(OP["BR"], R31, "idle")
    code = a.finish()
    assert image.CODE_PA + 4 * len(code) <= image.CODE_TOP, hex(len(code))

    img = bytearray(image.IMAGE_TOP)
    pal = image.build_pal(image.TR_PA, kseg(a.labels["main"]),
                          kseg(a.labels["fatal"]))
    for addr, w in pal.items():
        struct.pack_into("<I", img, addr, w)
    for i, w in enumerate(code):
        struct.pack_into("<I", img, image.CODE_PA + 4 * i, w)
    return img


def run_once(args, row, workdir, env_extra):
    os.makedirs(workdir)
    image.write_rom(os.path.join(workdir, "oracle.rom"),
                    build(args.iterations, args.polls, args.warm))
    port = args.port or cpu_oracle.free_port()
    env = {"ALPHABOX_CPU_ANY_BOARD": "1"}
    env.update(env_extra)
    orig = cpu_oracle.CFG
    tpus = "\n    tpus = 1;" if row.family == "EV8" else ""
    cpu_oracle.CFG = CFG.replace("%(tpus)s", tpus)
    text, emu, secs = cpu_oracle.run(os.path.abspath(args.binary), workdir,
                                     row.name, port, env, args.timeout)
    cpu_oracle.CFG = orig
    with open(os.path.join(workdir, "serial.txt"), "w") as f:
        f.write(text)
    vals = []
    i = text.find("@@BEGIN")
    if i >= 0:
        for ln in text[i:].replace("\r", "").split("\n")[1:]:
            ln = ln.strip()
            if ln == "@@END":
                break
            if ln:
                vals.append(int(ln, 16))
    return vals, emu, secs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="codepage-mp")
    ap.add_argument("--cpu", default="ev68cb")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--iterations", type=int, default=3000)
    ap.add_argument("--polls", type=int, default=1024)
    ap.add_argument("--warm", type=int, default=WARM_CALLS)
    ap.add_argument("--no-audit", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=float, default=300)
    args = ap.parse_args()
    row = rows.ROWS[args.cpu]
    if row.name == "ev8n":
        print("codepage_mp: the image's PALcode is the 21264's; ev8n has its own")
        return 2
    assert P_BASE + args.iterations * PAGE <= 1 << 26, "memory.bits = 26"
    work = os.environ.get("ALPHABOX_WORK", os.path.join(HERE, "..", "..", "lab"))
    base = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    arms = [("normal", {})]
    if not args.no_audit:
        arms.append(("audit", {"ALPHABOX_JIT_NOPFLUSH": "2"}))
    ok = True
    print("== codepage_mp %s: two %s processors on the ES40, %d iterations, "
          "%d polls" % (args.label, row.name, args.iterations, args.polls))
    for name, env in arms:
        vals, emu, secs = run_once(args, row, os.path.join(base, name), env)
        if len(vals) != len(FIELDS):
            print("  %-6s no results (%.1f s; see %s)" % (name, secs, base))
            for ln in emu.splitlines()[-10:]:
                print("   |", ln)
            ok = False
            continue
        r = dict(zip(FIELDS, vals))
        first_old = r["first old"]
        first_old = -1 if first_old >> 63 else first_old
        audits = len(re.findall(r"NOPFLUSH AUDIT", emu))
        good = (r["iterations"] == args.iterations and
                r["in the run"] + r["late"] == args.iterations and
                r["new code"] == args.iterations and r["old code"] == 0 and
                r["other value"] == 0 and r["wrong before"] == 0 and
                audits == 0)
        print("  %-6s %.1f s: %d in the run, %d late; new code %d, old code %d"
              "%s, other %d, wrong before the change %d; the map missed a "
              "write %s%s: %s" % (
                  name, secs, r["in the run"], r["late"], r["new code"],
                  r["old code"],
                  " (first in iteration %d)" % first_old if first_old >= 0 else "",
                  r["other value"], r["wrong before"],
                  "%d time%s" % (audits, "" if audits == 1 else "s")
                  if name == "audit" else "(not audited)",
                  " (it prints at most 20 from each of its two places)" if audits >= 20 else "",
                  "ok" if good else "FAIL"))
        print("         the last iteration: the writer's run %d cycles, the "
              "executor's GO to CODE %d cycles (800 MHz)"
              % (r["run cycles"], r["executor cycles"]))
        ok = ok and good
    print("  CODEPAGE MP: %s" % ("PASS" if ok else "FAIL"))
    if ok and not args.keep:
        shutil.rmtree(base, ignore_errors=True)
    else:
        print("  run directory: %s" % base)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
