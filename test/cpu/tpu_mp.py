#!/usr/bin/env python3
"""The native EV8's TPUs: a four-TPU boot image (docs/platforms/ev8-native.md,
S11, N17; docs/development.md, "The oracle").

    tpu_mp.py <alphabox-binary> [label] [--port N] [--keep]

One `ev8n` chip (four TPUs, the row's default) on the ES40 board, through
the ALPHABOX_CPU_ANY_BOARD test hook, as the CPU oracle runs one; the image
replaces the console (image.py's EV8 PALcode, build_pal8). Every TPU enters
the reset entry, MILD_RESET at PAL_BASE + 0xB00 [N11, N17], records the IPRs
as reset left them and the entry it came through in its own trap record,
and TPUs 1-3 park there with LDQ_ARM and QUIESCE (QUIESCE_TIMEOUT<WATCH_EN>
set, no timeout). TPU 0 writes CPU_CNFG, a per-chip IPR, and VCTL, a
per-TPU one, then releases TPUs 1-3 by storing a start address in each one's
mailbox: the store wakes the TPU [IDS pp. 511, 513]. Each TPU then reads
TPU_CNFG<TPU_ID>, CPU_BASE, CPU_CNFG, VCTL and PAL_TEMP1 in PALmode and
stores them, with its reset entry, in a slot of its own; TPU 0 prints all
four. Expected [IDS pp. 477-481, 488]: TPU_ID 0-3, CPU_BASE the chip's PID
(0, N6), CPU_CNFG the value TPU 0 wrote on every TPU, VCTL TPU 0's alone,
each TPU's own trap record, and 0xB00 on all four. Exit status 0 when all
hold.
"""
import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import alpha_asm as A  # noqa: E402
import cpu_oracle  # noqa: E402
import image  # noqa: E402
from alpha_asm import OP, R31, Asm, call_pal, mem, mfc, opl, opr  # noqa: E402
from image import kseg  # noqa: E402

TPUS = 4
DATA_PA = 0x1E8000           # done words (+0x100), the slots (+0x400)
DONE, SLOTS, SLOT = 0x100, 0x400, 0x40
FIELDS = ("TPU_CNFG", "CPU_BASE", "CPU_CNFG", "VCTL", "PAL_TEMP1",
          "reset entry")
CPU_CNFG_VALUE = 0x123

CFG = """sys0 = tsunami
{
  memory.bits = 26;
  rom.srm = "absent.exe";
  rom.decompressed = "oracle.rom";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  exit_on_pal_halt = true;

  cpu0 = ev8n
  {
    speed = 800M;
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


def snippet(words):
    """Words for the EV8 PALcode's CALL_PAL 91, which returns after them."""
    return list(words) + image.PAL8_RETURN


def build():
    I = A.IPR8
    mf, mt = A.hw_mfpr8, A.hw_mtpr8
    a = Asm(image.CODE_PA)
    snips = Asm(image.PAL8_SNIP_PA)
    D = 20                                      # r20: the data page
    a.branch(OP["BR"], R31, "main")
    image.emit_print_hex(a)
    a.label("fatal")
    a.li(image.R_UART, image.UART_VA8)
    image.emit_print_text(a, "\n@@FATAL\n")
    a.li(16, kseg(image.TR_PA))
    a.li(17, 7)
    a.branch(OP["BSR"], 18, "print_hex")
    a.emit(call_pal(0x00))

    # What every TPU reads in PALmode: TPU_CNFG, CPU_BASE, CPU_CNFG, VCTL,
    # PAL_TEMP1 into r1-r5.
    read_pa = snips.here
    for w in snippet([mf(1, I["tpu_cnfg"]), mf(2, I["cpu_base"]),
                      mf(3, I["cpu_cnfg"]), mf(4, I["vctl"]),
                      mf(5, I["pal_temp1"])]):
        snips.emit(w)
    # TPU 0: CPU_CNFG (the chip's) and VCTL (its own).
    set_pa = snips.here
    for w in snippet([mem(OP["LDA"], 1, R31, CPU_CNFG_VALUE),
                      mt(1, I["cpu_cnfg"]), mem(OP["LDA"], 1, R31, 1),
                      mt(1, I["vctl"]), A.ifetchb()]):
        snips.emit(w)

    def record():
        """r1-r5 and the reset entry into this TPU's slot; r6 = TPU_ID."""
        a.li(16, read_pa)
        a.emit(call_pal(0x91))
        a.emit(opl(0x12, 0x34, 1, 56, 6))       # SRL: TPU_ID
        a.emit(opl(0x12, 0x39, 6, 6, 7))        # SLL: x SLOT (0x40)
        a.li(8, kseg(DATA_PA + SLOTS))
        a.emit(opr(0x10, 0x20, 8, 7, 8))
        for k, r in enumerate((1, 2, 3, 4, 5)):
            a.emit(mem(OP["STQ"], r, 8, 8 * k))
        a.emit(opl(0x12, 0x39, 6, 10, 7))       # x TR8_BLOCK (0x400)
        a.li(9, kseg(image.TR_PA))
        a.emit(opr(0x10, 0x20, 9, 7, 9))
        a.emit(mem(OP["LDQ"], 1, 9, image.TR8_RESET_ENTRY))
        a.emit(mem(OP["STQ"], 1, 8, 8 * 5))

    # TPUs 1-3, released: record, say done, park again.
    a.label("start")
    record()
    a.li(D, kseg(DATA_PA))
    a.emit(mfc(A.MISC["MB"]))
    a.emit(opl(0x12, 0x39, 6, 3, 7))            # x 8
    a.emit(opr(0x10, 0x20, D, 7, 7))
    a.emit(mem(OP["LDA"], 1, R31, 1))
    a.emit(mem(OP["STQ"], 1, 7, DONE))
    a.label("idle")
    a.emit(mfc(A.MISC["LDQ_ARM"], 1, 7))
    a.emit(mfc(A.MISC["QUIESCE"]))
    a.branch(OP["BR"], R31, "idle")

    # TPU 0.
    a.label("main")
    a.li(image.R_UART, image.UART_VA8)
    a.li(16, set_pa)
    a.emit(call_pal(0x91))
    a.li(D, kseg(DATA_PA))
    for t in range(1, TPUS):                    # clear the done words
        a.emit(mem(OP["STQ"], R31, D, DONE + 8 * t))
    a.emit(mfc(A.MISC["MB"]))
    a.li(1, kseg(a.labels["start"]))
    a.li(2, kseg(image.TR_PA + image.TR8_MAILBOX))
    for t in range(1, TPUS):                    # release TPUs 1-3
        a.emit(mem(OP["STQ"], 1, 2, 0x40 * t))
    record()
    a.li(D, kseg(DATA_PA))
    a.li(3, 1 << 26)                            # a bound on the wait
    a.label("wait")
    a.emit(opl(0x10, 0x29, 3, 1, 3))            # SUBQ
    a.branch(OP["BEQ"], 3, "print")
    for t in range(1, TPUS):
        a.emit(mem(OP["LDQ"], 1, D, DONE + 8 * t))
        a.branch(OP["BEQ"], 1, "wait")
    a.label("print")
    image.emit_print_text(a, "\n@@BEGIN\n")
    a.li(16, kseg(DATA_PA + SLOTS))
    a.li(17, TPUS * SLOT // 8)
    a.branch(OP["BSR"], 18, "print_hex")
    image.emit_print_text(a, "@@END\n")
    a.emit(call_pal(0x00))
    code = a.finish()

    img = bytearray(image.IMAGE_TOP)
    pal = image.build_pal8(image.TR_PA, kseg(a.labels["main"]),
                           kseg(a.labels["fatal"]))
    for addr, w in pal.items():
        struct.pack_into("<I", img, addr, w)
    for i, w in enumerate(code):
        struct.pack_into("<I", img, image.CODE_PA + 4 * i, w)
    for i, w in enumerate(snips.finish()):
        struct.pack_into("<I", img, image.PAL8_SNIP_PA + 4 * i, w)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="tpu-mp")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=float, default=60)
    args = ap.parse_args()
    work = os.environ.get("ALPHABOX_WORK", os.path.join(HERE, "..", "..", "lab"))
    workdir = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    image.write_rom(os.path.join(workdir, "oracle.rom"), build())
    port = args.port or cpu_oracle.free_port()
    env = {"ALPHABOX_CPU_ANY_BOARD": "1"}
    orig = cpu_oracle.CFG
    cpu_oracle.CFG = CFG
    text, emu, secs = cpu_oracle.run(os.path.abspath(args.binary), workdir,
                                     "ev8n", port, env, args.timeout)
    cpu_oracle.CFG = orig
    with open(os.path.join(workdir, "serial.txt"), "w") as f:
        f.write(text)
    print("== tpu_mp %s: one ev8n chip of %d TPUs on the ES40, %.1f s"
          % (args.label, TPUS, secs))
    vals = []
    i = text.find("@@BEGIN")
    if i >= 0:
        for ln in text[i:].replace("\r", "").split("\n")[1:]:
            ln = ln.strip()
            if ln == "@@END":
                break
            if ln:
                vals.append(int(ln, 16))
    if len(vals) != TPUS * SLOT // 8:
        print("  no results (see %s)" % workdir)
        for ln in emu.splitlines()[-10:]:
            print("   |", ln)
        print("  TPU MP: FAIL")
        return 1
    ok = True
    started = sum(1 for ln in emu.splitlines() if "*** STARTING ***" in ln)
    for t in range(TPUS):
        got = vals[t * SLOT // 8:t * SLOT // 8 + len(FIELDS)]
        want = (t << 56, 0, CPU_CNFG_VALUE, 1 if t == 0 else 0,
                image.TR_PA + image.TR8_BLOCK * t, 0xB00)
        bad = [(f, g, w) for f, g, w in zip(FIELDS, got, want) if g != w]
        print("  TPU %d: %s: %s" % (t, ", ".join(
            "%s %#x" % (f, g) for f, g in zip(FIELDS, got)),
            "ok" if not bad else "FAIL " + ", ".join(
                "%s want %#x" % (f, w) for f, g, w in bad)))
        ok = ok and not bad
    print("  processors started: %d (every TPU at reset, N17)" % started)
    ok = ok and started == TPUS
    print("  TPU MP: %s" % ("PASS" if ok else "FAIL"))
    if ok and not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print("  run directory: %s" % workdir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
