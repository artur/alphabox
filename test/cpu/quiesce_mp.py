#!/usr/bin/env python3
"""LDx_ARM and QUIESCE across processors and DMA: a two-processor boot
image (docs/platforms/ev8.md, S3; docs/development.md, "The oracle").

    quiesce_mp.py <alphabox-binary> [label] [--cpu ROW] [--port N] [--keep]

Two processors of the row (default ev8) on the ES40 board, through the
ALPHABOX_CPU_ANY_BOARD test hook, as the CPU oracle runs one; the image
replaces the console (image.py's PALcode and loader). Processor 0 starts
processor 1 through the dual-port RAM's start word (es40/DPR.cpp), as the
ES40's console does; both run the same image, and the first to arrive is
the releaser. With the quiesce timeout set long (ALPHABOX_QUIESCE_TIMEOUT,
2**28 cycles: 336 ms at 800 MHz), processor 1 waits on a lock the IDS's
way [21464 IDS Rev 1.1k, PDF p. 674] while processor 0, after a delay of
2**24 cycles (21 ms), releases it:

  1 store      processor 0 stores 0 to the lock
  2 STx_C      processor 0 stores 0 with LDQ_L/STQ_C
  3 timeout    nobody writes: one LDQ_ARM and QUIESCE, which last the timeout
  4 interrupt  processor 1 enables the interprocessor interrupt and
               QUIESCEs; processor 0 sends it (the Cchip's MISC<IPREQ>)
  5 DMA        a SCSI controller's SCRIPTS Memory Move (a sym53c810 in
               pci0.3, PCI window 0 direct-mapped) writes 0 to the lock

Each scenario but the timeout must end after the delay and well before the
timeout, and the emulator's own count (ALPHABOX_TRACE_QUIESCE) must name
the cause: woken by store 1, STx_C 1, DMA 1, interrupt 1, timeout 1.
Exit status 0 when everything holds.
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
from alpha_asm import OP, R31, Asm, call_pal, mem, mfc, opl, opr  # noqa: E402
from image import kseg  # noqa: E402

TIMEOUT = 1 << 28        # ALPHABOX_QUIESCE_TIMEOUT, cycles
DELAY = 1 << 24          # processor 0's wait before it releases, cycles

DATA_PA = 0x1E0000       # the shared page: one 64-byte line each
LOCK, WHO, ALIVE, GO, READY, DONE = 0x000, 0x040, 0x080, 0x0C0, 0x100, 0x140
RES = 0x400              # results: 4 quadwords per scenario
SCRIPTS_PA = 0x1E8000    # the SCSI controller's program, and its source
ZERO_PA = 0x1E8100       # eight zero bytes the Memory Move copies

DPR_START1 = 0x80110000000 + (0x3428 << 6)   # DPR byte 0x3428: start CPU 1
CCHIP_MISC = 0x801A0000080
PCHIP0 = 0x80180000000                       # WSBA0 +0, WSM0 +0x100, TBA0 +0x200
PCI0_CFG = 0x801FE000000                     # type 0: device << 11 | register
PCI0_MEM = 0x80000000000
SYM_DEV = 3                                  # pci0.3
SYM_BAR = 0x10000000                         # its registers, PCI memory
SYM_DSP = 0x2C

SCENARIOS = ("store", "STx_C", "timeout", "interrupt", "DMA")

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
    speed = 800M;
  }
  cpu1 = %(cpu)s
  {
    speed = 800M;
  }

  serial0 = serial
  {
    address = "127.0.0.1";
    port = %(port)d;
  }

  pci0.3 = sym53c810
  {
  }

  pci0.7 = ali
  {
  }
}
"""


def seg48(pa):
    return image.SEG48 | pa


def build():
    a = Asm(image.CODE_PA)
    D = 20                                      # r20: the shared page
    a.branch(OP["BR"], R31, "main")
    image.emit_print_hex(a)
    a.label("fatal")
    a.li(image.R_UART, image.UART_VA)
    image.emit_print_text(a, "\n@@FATAL\n")
    a.li(16, kseg(image.TR_PA))
    a.li(17, 7)
    a.branch(OP["BSR"], 18, "print_hex")
    a.emit(call_pal(0x00))

    def wait_eq(reg_off, k, tag):
        """Spin until the quadword at reg_off of the shared page is k."""
        a.label(tag)
        a.emit(mem(OP["LDQ"], 1, D, reg_off))
        a.emit(opl(0x10, 0x29, 1, k, 1))         # SUBQ r1,#k,r1
        a.branch(OP["BNE"], 1, tag)

    def delay(tag):
        """Spin for DELAY cycles of RPCC."""
        a.emit(mfc(A.MISC["RPCC"], 2))
        a.label(tag)
        a.emit(mfc(A.MISC["RPCC"], 3))
        a.emit(opr(0x10, 0x29, 3, 2, 3))         # SUBQ
        a.emit(opl(0x12, 0x31, 3, 0x0F, 3))      # ZAPNOT: 32-bit count
        a.emit(mem(OP["LDAH"], 4, R31, DELAY >> 16))
        a.emit(opr(0x10, 0x1D, 3, 4, 3))         # CMPULT r3,r4,r3
        a.branch(OP["BNE"], 3, tag)

    # --- both processors -------------------------------------------------
    a.label("main")
    a.li(image.R_UART, image.UART_VA)
    a.li(D, kseg(DATA_PA))
    a.emit(mem(OP["LDQ"], 1, D, WHO))
    a.branch(OP["BNE"], 1, "secondary")
    a.emit(mem(OP["LDA"], 1, R31, 1))
    a.emit(mem(OP["STQ"], 1, D, WHO))
    a.emit(mfc(A.MISC["MB"]))

    # --- processor 0: the releaser ---------------------------------------
    # PCI window 0 direct-mapped over the first 1 GB, so that the SCSI
    # controller's DMA reaches memory; its registers at SYM_BAR; I/O,
    # memory and bus-master enables.
    a.li(5, seg48(PCHIP0))
    a.li(1, 0x00000001)                          # WSBA0: base 0, enabled
    a.emit(mem(OP["STQ"], 1, 5, 0x000))
    a.li(1, 0x3FF00000)                          # WSM0: 1 GB
    a.emit(mem(OP["STQ"], 1, 5, 0x100))
    a.emit(mem(OP["STQ"], R31, 5, 0x200))        # TBA0: 0
    a.li(5, seg48(PCI0_CFG + (SYM_DEV << 11)))
    a.li(1, SYM_BAR)
    a.emit(mem(OP["STL"], 1, 5, 0x14))           # BAR1 (memory)
    a.li(1, 0x0007)
    a.emit(mem(OP["STL"], 1, 5, 0x04))           # COMMAND
    a.emit(mfc(A.MISC["MB"]))
    # Start processor 1.
    a.li(5, seg48(DPR_START1))
    a.emit(mem(OP["STQ"], R31, 5, 0))
    wait_eq(ALIVE, 1, "w_alive")
    for k, name in enumerate(SCENARIOS, 1):
        a.emit(mem(OP["LDA"], 1, R31, 1))
        a.emit(mem(OP["STQ"], 1, D, LOCK))       # locked
        a.emit(mfc(A.MISC["MB"]))
        a.emit(mem(OP["LDA"], 1, R31, k))
        a.emit(mem(OP["STQ"], 1, D, GO))
        wait_eq(READY, k, "w_ready%d" % k)
        delay("delay%d" % k)
        if name == "store":
            a.emit(mem(OP["STQ"], R31, D, LOCK))
        elif name == "STx_C":
            a.label("stc")
            a.emit(mem(OP["LDQ_L"], 1, D, LOCK))
            a.emit(mem(OP["LDA"], 1, R31, 0))
            a.emit(mem(OP["STQ_C"], 1, D, LOCK))
            a.branch(OP["BEQ"], 1, "stc")
        elif name == "interrupt":
            a.li(5, seg48(CCHIP_MISC))
            a.li(1, 0x1000 << 1)                 # IPREQ for processor 1
            a.emit(mem(OP["STQ"], 1, 5, 0))
        elif name == "DMA":
            a.li(5, seg48(PCI0_MEM + SYM_BAR))
            a.li(1, SCRIPTS_PA)
            a.emit(mem(OP["STL"], 1, 5, SYM_DSP))  # starts the SCRIPTS
        a.emit(mfc(A.MISC["MB"]))
        wait_eq(DONE, k, "w_done%d" % k)
    image.emit_print_text(a, "\n@@BEGIN %d\n" % (4 * len(SCENARIOS)))
    a.li(16, kseg(DATA_PA + RES))
    a.li(17, 4 * len(SCENARIOS))
    a.branch(OP["BSR"], 18, "print_hex")
    image.emit_print_text(a, "@@END\n")
    a.emit(call_pal(0x00))

    # --- processor 1: the sleeper -----------------------------------------
    LDQ_ARM = mfc(A.MISC["LDQ_ARM"], 0, D)       # LDQ_ARM r0,(r20): the lock
    QUIESCE = mfc(A.MISC["QUIESCE"])
    a.label("secondary")
    a.emit(mem(OP["LDA"], 1, R31, 1))
    a.emit(mem(OP["STQ"], 1, D, ALIVE))
    for k, name in enumerate(SCENARIOS, 1):
        res = RES + 32 * (k - 1)
        wait_eq(GO, k, "w_go%d" % k)
        if name == "interrupt":
            a.li(17, 1 << (33 + 3))              # IER: EIEN<3>, the IPI
            a.emit(call_pal(0x8C))
            a.li(21, kseg(image.TR_PA))
            a.emit(mem(OP["STQ"], R31, 21, image.TR_COUNT))
        a.emit(mem(OP["LDA"], 9, R31, 0))        # r9: QUIESCEs executed
        a.emit(mfc(A.MISC["RPCC"], 6))
        a.emit(mem(OP["LDA"], 1, R31, k))
        a.emit(mem(OP["STQ"], 1, D, READY))
        if name in ("timeout", "interrupt"):
            a.emit(LDQ_ARM)
            a.emit(QUIESCE)
            a.emit(opl(0x10, 0x20, 9, 1, 9))
        else:
            # The IDS's loop [p. 674]: arm, test, QUIESCE, test again.
            a.label("busy%d" % k)
            a.emit(LDQ_ARM)
            a.branch(OP["BEQ"], 0, "got%d" % k)
            a.emit(QUIESCE)
            a.emit(opl(0x10, 0x20, 9, 1, 9))
            a.emit(mem(OP["LDQ"], 0, D, LOCK))
            a.branch(OP["BEQ"], 0, "got%d" % k)
            a.branch(OP["BR"], R31, "busy%d" % k)
            a.label("got%d" % k)
        a.emit(mfc(A.MISC["RPCC"], 7))
        a.emit(opr(0x10, 0x29, 7, 6, 7))
        a.emit(opl(0x12, 0x31, 7, 0x0F, 7))
        a.emit(mem(OP["STQ"], 7, D, res))        # the wait, in cycles
        a.emit(mem(OP["STQ"], 9, D, res + 8))    # QUIESCEs executed
        if name == "interrupt":
            a.li(5, kseg(image.TR_PA))
            a.emit(mem(OP["LDQ"], 1, 5, image.TR_COUNT))
            a.emit(mem(OP["STQ"], 1, D, res + 16))   # interrupts taken
            a.emit(mem(OP["LDQ"], 1, 5, image.TR_VEC))
            a.emit(mem(OP["STQ"], 1, D, res + 24))   # their vector
            a.li(5, seg48(CCHIP_MISC))
            a.li(1, 0x100 << 1)                  # IPINTR: clear it
            a.emit(mem(OP["STQ"], 1, 5, 0))
        a.emit(mfc(A.MISC["MB"]))
        a.emit(mem(OP["LDA"], 1, R31, k))
        a.emit(mem(OP["STQ"], 1, D, DONE))
    a.label("idle")
    a.branch(OP["BR"], R31, "idle")
    code = a.finish()

    img = bytearray(image.IMAGE_TOP)
    pal = image.build_pal(image.TR_PA, kseg(a.labels["main"]),
                          kseg(a.labels["fatal"]))
    for addr, w in pal.items():
        struct.pack_into("<I", img, addr, w)
    for i, w in enumerate(code):
        struct.pack_into("<I", img, image.CODE_PA + 4 * i, w)
    # The SCRIPTS: Memory Move 8 bytes ZERO -> LOCK, then INT (a stop).
    for i, w in enumerate((0xC0000008, ZERO_PA, DATA_PA + LOCK,
                           0x98080000, 0x00000000)):
        struct.pack_into("<I", img, SCRIPTS_PA + 4 * i, w)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="quiesce-mp")
    ap.add_argument("--cpu", default="ev8")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=float, default=60)
    args = ap.parse_args()
    row = rows.ROWS[args.cpu]
    if not row.own_instruction(mfc(A.MISC["QUIESCE"])):
        print("quiesce_mp: row %s has no QUIESCE" % row.name)
        return 2
    work = os.environ.get("ALPHABOX_WORK", os.path.join(HERE, "..", "..", "lab"))
    workdir = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    image.write_rom(os.path.join(workdir, "oracle.rom"), build())
    port = args.port or cpu_oracle.free_port()
    env = {"ALPHABOX_CPU_ANY_BOARD": "1", "ALPHABOX_TRACE_QUIESCE": "1",
           "ALPHABOX_QUIESCE_TIMEOUT": str(TIMEOUT)}
    # cpu_oracle.run boots its one-processor configuration: ours instead.
    orig = cpu_oracle.CFG
    cpu_oracle.CFG = CFG
    text, emu, secs = cpu_oracle.run(os.path.abspath(args.binary), workdir,
                                     row.name, port, env, args.timeout)
    cpu_oracle.CFG = orig
    with open(os.path.join(workdir, "serial.txt"), "w") as f:
        f.write(text)
    print("== quiesce_mp %s: two %s processors on the ES40, %.1f s"
          % (args.label, row.name, secs))
    ok = True
    i = text.find("@@BEGIN")
    vals = []
    if i >= 0:
        for ln in text[i:].replace("\r", "").split("\n")[1:]:
            ln = ln.strip()
            if ln == "@@END":
                break
            if ln:
                vals.append(int(ln, 16))
    if len(vals) != 4 * len(SCENARIOS):
        print("  no results (see %s)" % workdir)
        for ln in emu.splitlines()[-10:]:
            print("   |", ln)
        print("  QUIESCE MP: FAIL")
        return 1
    for k, name in enumerate(SCENARIOS):
        wait, n, traps, vec = vals[4 * k:4 * k + 4]
        if name == "timeout":
            good = wait >= TIMEOUT and n == 1
            want = ">= %d (the timeout)" % TIMEOUT
        else:
            good = DELAY <= wait < TIMEOUT and n >= 1
            want = "from %d (the delay) to below %d (the timeout)" % (DELAY, TIMEOUT)
        if name == "interrupt":
            good = good and traps == 1 and vec == 0x680
        print("  %-9s waited %10d cycles (%6.1f ms), %d QUIESCE%s%s: %s (%s)" % (
            name, wait, wait / 800e3, n, "" if n == 1 else "s",
            ", interrupt taken %d at %#x" % (traps, vec) if name == "interrupt" else "",
            "ok" if good else "FAIL", want))
        ok = ok and good
    rep = [ln for ln in emu.splitlines() if "%CPU-I-QUIESCE: CPU1" in ln]
    if not rep:
        print("  no %CPU-I-QUIESCE report from CPU1")
        ok = False
    else:
        print("  " + rep[-1].strip())
        m = re.search(r"woken by store (\d+), STx_C (\d+), DMA (\d+), "
                      r"interrupt (\d+), timeout (\d+), other (\d+)", rep[-1])
        got = tuple(int(x) for x in m.groups()) if m else None
        if got != (1, 1, 1, 1, 1, 0):
            print("  wake causes %s, want store 1, STx_C 1, DMA 1, interrupt 1, "
                  "timeout 1, other 0" % (got,))
            ok = False
    print("  QUIESCE MP: %s" % ("PASS" if ok else "FAIL"))
    if ok and not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print("  run directory: %s" % workdir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
