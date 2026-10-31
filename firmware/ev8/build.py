#!/usr/bin/env python3
# Alphabox Alpha Emulator
# Copyright (C) 2026 Artur Goulão
# Website: https://github.com/artur/alphabox
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version. See LICENSE at the top of the repository.
"""Build the EV8 firmware: the Alphabox PALcode for the native 21464 (the
`ev8n` row), and a `rom.decompressed` that starts an ES47, ES80 or GS1280
in 21464 mode with it (docs/platforms/ev8-native.md, sections 3.5 and 3.6,
N27 and N28; S12a).

    build.py [--nada PATH] [--console FILE] [--out PATH] [--size N]

The PALcode is pal.c, nada assembler source: nada main 9152b521
(lab/tools/nada/NADA-COMMIT.txt), invoked as

    nada -t alpha-none -mcpu=ev8 -mpal -n -f flat -B 0xfffffffffffffff0

A flat image starts with nada's `br $31, _start`, padded to 16 bytes, and
the code follows; linked 16 bytes below PAL_BASE (0), every label is its
physical address and every `.align` counts from PAL_BASE. This script drops
those 16 bytes, checks the layout (each entry and CALL_PAL slot starts with
`lda $31, offset($31)`, a no-op naming its own offset), and places the
PALcode at physical 0 of the image. A console, if given (a flat binary),
goes at CONSOLE_PA. Without one, processor 0's first instruction there is a
zero word, CALL_PAL HALT, whose halt re-enters the same place: until S12b
writes the console, the image serves tools that add their own, as
test/cpu/pal8_test.py does.

The output is the decompressed-console file CSystem::load_decompressed_console
reads: the entry PC (PAL_BASE | 1), PAL_BASE, the board's console bytes
(0x480000 on the Marvel rows), the tag ALPHDC01. On an `ev8n` machine every
TPU then resets at PAL_BASE + 0xB00. Nothing is committed but the sources;
the default output is <ALPHABOX_WORK or lab>/firmware-ev8/rom.decompressed.

nada assembles only the bare `ret` (`ret $31, ($ra), 1`), not RET through
another register: the PALcode's RET $31, (S1), (S0) and (R6) are words, which
this script checks against test/cpu/alpha_asm.py's ret() (S12a, a form the
agreement of ev8-native.md 3.5 implies and nada does not take).
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "test", "cpu"))

import alpha_asm as A  # noqa: E402

SOURCE = os.path.join(HERE, "pal.c")
LAYOUT = os.path.join(HERE, "layout.h")
NADA_COMMIT = "9152b521cfa37aa88d7afb41bd52cff9fc2967de"
NADA_SHA256 = "47c4e6509ce8e09a6d0707d585c4a7cd7d7f039bd6406c2cc4a1c82973138914"
LOAD_ADDRESS = 0xFFFFFFFFFFFFFFF0     # PAL_BASE - 16: nada's branch drops off
CONSOLE_BYTES = 0x480000              # the Marvel rows' decompressed console
TAG = b"ALPHDC01"


def work_dir():
    return os.path.abspath(os.environ.get("ALPHABOX_WORK",
                                          os.path.join(REPO, "lab")))


def default_nada():
    return os.environ.get("NADA", os.path.join(work_dir(), "tools", "nada",
                                               "nada"))


def layout():
    """layout.h's #define lines that name a number, as a dict."""
    out = {}
    with open(LAYOUT) as f:
        for ln in f:
            m = re.match(r"#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\b", ln)
            if m:
                out[m.group(1)] = int(m.group(2), 0)
    return out


def check_layout(L):
    """What layout.h writes out that follows from its other values."""
    def need(cond, what):
        if not cond:
            raise SystemExit("layout.h: " + what)
    need(L["KSEG_CONSOLE"] == L["KSEG"] | L["CONSOLE_PA"], "KSEG_CONSOLE")
    need(L["KSEG_RXRDY"] == L["KSEG"] | (L["HWRPB_PA"] + L["HWRPB_RXRDY"]),
         "KSEG_RXRDY")
    need(L["IMPURE_SIZE"] == 1 << L["IMPURE_SHIFT"], "IMPURE_SHIFT")
    need(L["PAL_IMPURE"] + L["MAX_WHAMI"] * L["IMPURE_SIZE"] <= L["HWRPB_PA"],
         "the impure areas run into the HWRPB")
    need(L["PAL_TEXT_TOP"] <= L["PAL_DATA"] <= L["PAL_IMPURE"], "PAL_DATA")
    need(L["CONSOLE_PA"] < L["FIRMWARE_TOP"] <= 0x200000, "FIRMWARE_TOP")
    for ipl, eien in ((0, 0x3F), (4, 0x2D), (5, 0x21), (6, 0x20)):
        need(L["IER_IPL%d" % ipl] == eien << 33, "IER_IPL%d" % ipl)
    need(L["START_CMD"] == int.from_bytes(b"START\r\n", "little"), "START_CMD")
    need(L["HWRPB_ID_VALUE"] == int.from_bytes(b"HWRPB\0\0\0", "little"),
         "HWRPB_ID_VALUE")
    need(L["SEL_IPI"] == (5 + 1) << 8 | L["IMP_ENTINT"], "SEL_IPI")
    need(L["SEL_DEVICE"] == (4 + 1) << 8 | L["IMP_ENTINT"], "SEL_DEVICE")
    for sel, ent in (("SEL_ARITH", "IMP_ENTARITH"), ("SEL_MM", "IMP_ENTMM"),
                     ("SEL_IF", "IMP_ENTIF"), ("SEL_UNA", "IMP_ENTUNA"),
                     ("SEL_SYS", "IMP_ENTSYS")):
        need(L[sel] == L[ent], sel)
    need(L["IMP_CPCB"] + 16 * 8 <= L["IMPURE_SIZE"], "IMP_CPCB")
    need(L["KSEG_GIO_LOCK"] == L["KSEG"] | L["PAL_GIO_LOCK"], "KSEG_GIO_LOCK")
    need(L["PAL_DATA"] <= L["PAL_GIO_LOCK"] and
         L["PAL_GIO_LOCK"] + 0x40 * (L["MAX_WHAMI"] // 4) <= L["PAL_IMPURE"],
         "PAL_GIO_LOCK")
    need(L["CMM_TOY_NV"] == 0x4b98 - 12, "CMM_TOY_NV")
    need(L["PAL_MEM_PA"] == L["PAL_BASE"] and
         L["PAL_MEM_LEN"] == L["PAL_IMPURE"] - L["PAL_BASE"], "PAL_MEM_LEN")
    need(L["PAL_SCR_PA"] == L["PAL_IMPURE"] and
         L["PAL_SCR_LEN"] == L["MAX_WHAMI"] * L["IMPURE_SIZE"], "PAL_SCR_LEN")
    need(L["FLAG_PALOK"] == 0x1c0, "FLAG_PALOK")


def check_ret_words():
    """pal.c's RET words are alpha_asm's RET $31, (S1), (S0) and (R6)."""
    with open(SOURCE) as f:
        src = f.read()
    for name, rb in (("RET_S1", A.S1B), ("RET_S0", A.S0B), ("RET_R6", 6)):
        m = re.search(r'#define %s "\s*\.long (0x[0-9a-fA-F]+)' % name, src)
        if not m or int(m.group(1), 16) != A.ret(rb):
            raise SystemExit("pal.c: %s is not RET $31, ($%d) (%#x)"
                             % (name, rb, A.ret(rb)))


def assemble(nada=None, keep=None):
    """Run nada on pal.c; the PALcode's bytes from PAL_BASE."""
    nada = nada or default_nada()
    if not os.path.exists(nada):
        raise SystemExit("no nada at %s (--nada, or NADA)" % nada)
    with open(nada, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    if sha != NADA_SHA256:
        print("build.py: warning: %s is not nada %s (sha256 %s)"
              % (nada, NADA_COMMIT[:8], sha), file=sys.stderr)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(keep or tmp, "pal.img")
        cmd = [nada, "-t", "alpha-none", "-mcpu=ev8", "-mpal", "-n",
               "-f", "flat", "-B", "%#x" % LOAD_ADDRESS, "-o", out, SOURCE]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            raise SystemExit("nada failed:\n" + p.stdout + p.stderr)
        with open(out, "rb") as f:
            img = f.read()
    head = struct.unpack_from("<I", img, 0)[0]
    if head >> 21 != (0x30 << 5 | 31):          # BR R31, _start
        raise SystemExit("nada's image does not start with br $31, _start")
    return img[16:]


def marker(off):
    return A.mem(A.OP["LDA"], A.R31, A.R31, off)


def slots():
    """Every entry (0x000-0xF80) and CALL_PAL slot (0x2000-0x3FC0)."""
    return (list(range(0x000, 0x1000, 0x80)) +
            [0x2000 + ((f & 0x80) << 5) + ((f & 0x3F) << 6)
             for f in list(range(0x00, 0x40)) + list(range(0x80, 0xC0))])


def check_pal(pal, L):
    if len(pal) > L["PAL_TEXT_TOP"]:
        raise SystemExit("the PALcode is %#x bytes, past PAL_TEXT_TOP %#x"
                         % (len(pal), L["PAL_TEXT_TOP"]))
    bad = [off for off in slots()
           if off + 4 > len(pal) or
           struct.unpack_from("<I", pal, off)[0] != marker(off)]
    if bad:
        raise SystemExit("PALcode layout: no marker at %s"
                         % ", ".join("%#x" % b for b in bad[:8]))


def build_pal(nada=None):
    """The checked PALcode, and layout.h's values."""
    L = layout()
    check_layout(L)
    check_ret_words()
    pal = assemble(nada)
    check_pal(pal, L)
    return pal, L


def image(pal, L, console=None, extra=(), size=CONSOLE_BYTES):
    """The console bytes: the PALcode at 0, `console` at CONSOLE_PA, and
    each (pa, bytes) of `extra`."""
    img = bytearray(size)
    img[0:len(pal)] = pal
    parts = [(L["CONSOLE_PA"], console)] if console else []
    for pa, data in parts + list(extra):
        if pa < L["PAL_TEXT_TOP"] or pa + len(data) > size:
            raise SystemExit("%#x bytes at %#x: outside the image"
                             % (len(data), pa))
        img[pa:pa + len(data)] = data
    return img


def write_rom(path, img, L):
    """The decompressed-console file (System.cpp, load_decompressed_console)."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack("<QQ", L["PAL_BASE"] | 1, L["PAL_BASE"]))
        f.write(img)
        f.write(TAG)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nada", default=None)
    ap.add_argument("--console", help="a flat binary placed at CONSOLE_PA")
    ap.add_argument("--out", default=os.path.join(work_dir(), "firmware-ev8",
                                                  "rom.decompressed"))
    ap.add_argument("--size", type=lambda s: int(s, 0), default=CONSOLE_BYTES)
    args = ap.parse_args()
    pal, L = build_pal(args.nada)
    console = None
    if args.console:
        with open(args.console, "rb") as f:
            console = f.read()
    write_rom(args.out, image(pal, L, console, size=args.size), L)
    words = struct.unpack("<%dI" % (len(pal) // 4), pal)
    print("PALcode %#x bytes, %d instructions; %s"
          % (len(pal), sum(1 for w in words if w), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
