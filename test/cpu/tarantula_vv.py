#!/usr/bin/env python3
"""Tarantula's operate and control instructions in kernels, each checked
against scalar code run in the same image on the same inputs: a boot image
of its own (docs/platforms/ev8.md, S7; docs/development.md, "The oracle").

    tarantula_vv.py <alphabox-binary> [label] [--cpu ROW] [--port N] [--keep]

One processor of the row (default ev8; ev8n too) on the ES40 board, through
the ALPHABOX_CPU_ANY_BOARD test hook, as the CPU oracle runs one; the image
replaces the console (image.py's PALcode, the 21264's or the EV8's). It sets
VCTL<VEN> (on the `ev8` row the CSR at offset 0x3F0000 of its window, on
`ev8n` the IPR 0xAC) and runs four kernels:

  1 daxpy        y <- a x + y, 128 T elements: VSMULT, VVADDT
  2 dot product  x . y over 128 elements, T and quadword: VVMULT and VVMULQ,
                 then the sum by scalar code over the products
  3 masked update  y <- y + 7 where x > 25, 100 elements: VSCMPGT, SETVM,
                 VSADDQ under the mask, VVMRG to merge (masked-off elements
                 are UNPREDICTABLE, ev8-isa.md 4.3)
  4 strip-mined  z <- x k + y over 300 quadwords, SETVL giving each pass
                 its length (128, 128, 44): VSMULQ, VVADDQ

How values get in and out. S7 has no vector loads or stores (S8), and the
sheet moves no element between vector and scalar registers (point 13).
In: the VS route the sheet gives. The image builds $vr30 = 0, 1, ..., 127
with SETVL, a broadcast (VSBIS from $vr31), SETVM (which clears every mask
bit at or above vl) and VVMRG, then puts element i in with a one-element
mask: VSCMPEQ $vr30, i, then SETVM, then VSMRG of the scalar. Out: the
ALPHABOX_TARANTULA_PEEK test hook (docs/headless.md), quadword loads from
the register window; the dot product's reduction is scalar code over those
loads, since the sheet has neither an element move (GETVL, GETVS and GETVM
give no element) nor a way to move an element to another position for a
reduction by halving.

The image counts the elements where the vector result and the scalar one
differ, and sums both; this script checks the counts are 0, the scalar sums
against its own (Python) computation, that the strip-mined loop took three
passes, and that no exception was taken. Exit status 0 when all hold.
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
import rows  # noqa: E402
import tfloat as T  # noqa: E402
from alpha_asm import OP, R31, Asm, call_pal, fop, jmp, mem, opl, opr  # noqa: E402
from image import kseg  # noqa: E402

M64 = (1 << 64) - 1
WIN_PA = 0xFFFFFC00000
VCTL_PA = WIN_PA + 0x3F0000
PEEK_PA = WIN_PA + 0x3F8000

DATA_PA = 0x1E8000            # results, inputs (past the EV8 PAL's snippets)
N1, N3, N4 = 128, 100, 300
A_DAXPY = 2.5
K4 = 0x9E3779B97F4A7C15
# Quadword offsets from DATA_PA: the results the image prints, then the
# arrays, 512 quadwords each.
RES = 0
NRES = 4 * 5 + 2              # 4 per kernel, the iota's check, the traps
X_F, Y_F, X_Q, Y_Q, X_M, Y_M, X_S, Y_S, OUT_V, OUT_S, PROD, IOTA = (
    64 + k * 512 for k in range(12))
KERNELS = ("daxpy", "dot product (T)", "dot product (Q)", "masked update",
           "strip-mined loop")


def inputs():
    xf = [T.bits(1.0 + i / 7.0) for i in range(N1)]
    yf = [T.bits(3.0 - i / 11.0) for i in range(N1)]
    xq = [((i * 0x0123456789ABCDEF) ^ (i << 40)) & M64 for i in range(N1)]
    yq = [(0xFEDCBA9876543210 + i * 977) & M64 for i in range(N1)]
    xm = [(i * 37) % 50 for i in range(N3)]
    ym = [i * 1000 for i in range(N3)]
    xs = [(i * 6364136223846793005 + 1) & M64 for i in range(N4)]
    ys = [(i ** 3 + 0x5555) & M64 for i in range(N4)]
    return xf, yf, xq, yq, xm, ym, xs, ys


def expected():
    """What the scalar code must compute (Python's own arithmetic)."""
    import struct as st

    def f(v):
        return st.unpack("<d", st.pack("<Q", v))[0]
    xf, yf, xq, yq, xm, ym, xs, ys = inputs()
    daxpy = [T.bits(A_DAXPY * f(x) + f(y)) for x, y in zip(xf, yf)]
    dot_t = 0.0
    for x, y in zip(xf, yf):
        dot_t = dot_t + f(x) * f(y)
    dot_q = sum(x * y for x, y in zip(xq, yq)) & M64
    masked = [(y + 7) if x > 25 else y for x, y in zip(xm, ym)]
    strip = [(x * K4 + y) & M64 for x, y in zip(xs, ys)]
    return {"daxpy": sum(daxpy) & M64, "dot product (T)": T.bits(dot_t),
            "dot product (Q)": dot_q, "masked update": sum(masked) & M64,
            "strip-mined loop": sum(strip) & M64}


def lda(ra, rb, d):
    return mem(OP["LDA"], ra, rb, d)


def ldq(ra, rb, d):
    return mem(OP["LDQ"], ra, rb, d)


def stq(ra, rb, d):
    return mem(OP["STQ"], ra, rb, d)


def ldt(fa, rb, d):
    return mem(OP["LDT"], fa, rb, d)


def stt(fa, rb, d):
    return mem(OP["STT"], fa, rb, d)


def addq(ra, rb, rc):
    return opr(0x10, 0x20, ra, rb, rc)


def build(ev8n):
    a = Asm(image.CODE_PA)
    seg = image.SEG52 if ev8n else image.SEG48
    uart = image.UART_VA8 if ev8n else image.UART_VA
    D = 20                    # r20: the data area
    W = 21                    # r21: the register window
    a.branch(OP["BR"], R31, "main")
    image.emit_print_hex(a)
    a.label("fatal")
    a.li(image.R_UART, uart)
    image.emit_print_text(a, "\n@@FATAL\n")
    a.li(16, kseg(image.TR_PA))
    a.li(17, 7)
    a.branch(OP["BSR"], 18, "print_hex")
    a.emit(call_pal(0x00))

    # insert_N: $vrN element i <- the quadword at r2 + 8 i, i < r3 (vl
    # covers r3); clobbers r4-r6, the mask and $vr29. The one-element mask:
    # VSCMPEQ $vr30 (the iota), i -> $vr29, then SETVM; then VSMRG.
    for n in (1, 2, 5, 6):
        a.label("insert_%d" % n)
        a.emit(lda(4, R31, 0))
        a.label("ins_loop_%d" % n)
        a.emit(A.vs("vscmpeq", 30, 4, 29))
        a.emit(A.vc("setvm", rb=29))
        a.emit(ldq(5, 2, 0))
        a.emit(A.vs("vsmrg", n, 5, n))
        a.emit(opl(0x10, 0x20, 4, 1, 4))
        a.emit(lda(2, 2, 8))
        a.emit(opr(0x10, 0x4D, 4, 3, 6))           # CMPLT r4,r3,r6
        a.branch(OP["BNE"], 6, "ins_loop_%d" % n)
        a.emit(jmp(2, R31, 26))
    # extract: r3 elements of $vr(r1) to r2 (the window, quadword loads).
    a.label("extract")
    a.emit(opl(0x12, 0x39, 1, 10, 4))          # SLL r1,10,r4
    a.emit(addq(W, 4, 4))
    a.label("ext_loop")
    a.emit(ldq(5, 4, 0))
    a.emit(stq(5, 2, 0))
    a.emit(lda(4, 4, 8))
    a.emit(lda(2, 2, 8))
    a.emit(opl(0x10, 0x29, 3, 1, 3))
    a.branch(OP["BGT"], 3, "ext_loop")
    a.emit(jmp(2, R31, 26))
    # compare: r3 quadwords at r1 and r2: r0 <- how many differ, r7 <- the
    # sum of r1's, r8 <- the sum of r2's.
    a.label("compare")
    a.emit(lda(0, R31, 0))
    a.emit(lda(7, R31, 0))
    a.emit(lda(8, R31, 0))
    a.label("cmp_loop")
    a.emit(ldq(4, 1, 0))
    a.emit(ldq(5, 2, 0))
    a.emit(addq(7, 4, 7))
    a.emit(addq(8, 5, 8))
    a.emit(opr(0x10, 0x2D, 4, 5, 6))           # CMPEQ
    a.emit(opl(0x11, 0x40, 6, 1, 6))           # XOR #1: 1 where they differ
    a.emit(addq(0, 6, 0))
    a.emit(lda(1, 1, 8))
    a.emit(lda(2, 2, 8))
    a.emit(opl(0x10, 0x29, 3, 1, 3))
    a.branch(OP["BGT"], 3, "cmp_loop")
    a.emit(jmp(2, R31, 26))

    def call(label):
        a.branch(OP["BSR"], 26, label)

    def addr(r, off):
        a.li(r, kseg(DATA_PA + 8 * off))

    def result(k, regs):
        """Store r0, r7, r8 (or `regs`) as kernel k's results."""
        for j, r in enumerate(regs):
            a.emit(stq(r, D, 8 * (RES + 4 * k + j)))

    a.label("main")
    a.li(image.R_UART, uart)
    a.li(D, kseg(DATA_PA))
    a.li(W, seg | PEEK_PA)
    # VEN = 1.
    if ev8n:
        snips = Asm(image.PAL8_SNIP_PA)
        for w in [lda(1, R31, 1), A.hw_mtpr8(1, A.IPR8["vctl"]), A.ifetchb()] \
                + image.PAL8_RETURN:
            snips.emit(w)
        a.li(16, image.PAL8_SNIP_PA)
        a.emit(call_pal(0x91))
    else:
        snips = None
        a.li(1, seg | VCTL_PA)
        a.emit(lda(2, R31, 1))
        a.emit(stq(2, 1, 0))
    # The FPCR: rounding normal, INE's status set and its trap disabled, so
    # that neither the scalar code nor the vector code traps for an inexact
    # result (an exception's status bit, once set, needs no trap to set it).
    a.li(1, (T.NORMAL << 58) | (1 << 62) | T.status_bit(T.INE))
    a.emit(stq(1, D, 8 * (RES + 60)))
    a.emit(ldt(1, D, 8 * (RES + 60)))
    a.emit(fop(0x17, 0x024, 1, 1, 1))          # MT_FPCR
    # The iota: $vr30[i] = i. $vr30 <- 0; then for k = 0..126: vm <- the
    # first k + 1 elements (SETVL k + 1, a broadcast of 1, SETVM, which
    # clears the mask at and above vl), and at vl 128 VVMRG $vr28 (= $vr30
    # + 1), $vr30 -> $vr30: element i is incremented once for each k < i.
    a.emit(A.vc("setvl", lit=128))
    a.emit(A.vs("vsbis", 31, 0, 30, lit=0))
    a.emit(lda(9, R31, 1))
    a.label("iota")
    a.emit(A.vc("setvl", rb=9))
    a.emit(A.vs("vsbis", 31, 0, 27, lit=1))
    a.emit(A.vc("setvm", rb=27))
    a.emit(A.vc("setvl", lit=128))
    a.emit(A.vs("vsaddq", 30, 0, 28, lit=1))
    a.emit(A.vv("vvmrg", 28, 30, 30))
    a.emit(opl(0x10, 0x20, 9, 1, 9))
    a.emit(opl(0x10, 0x4D, 9, 128, 10))        # CMPLT r9,#128
    a.branch(OP["BNE"], 10, "iota")
    # The iota itself, out through the window, against 0..127.
    a.li(1, 30)
    addr(2, OUT_V)
    a.emit(lda(3, R31, 128))
    call("extract")
    addr(1, OUT_V)
    addr(2, IOTA)
    a.emit(lda(3, R31, 128))
    call("compare")
    a.emit(stq(0, D, 8 * (RES + 20)))

    # 1: daxpy, T, 128 elements.
    a.emit(A.vc("setvl", lit=N1))
    for vr, off in ((1, X_F), (2, Y_F)):
        addr(2, off)
        a.emit(lda(3, R31, N1))
        call("insert_%d" % vr)
    addr(1, RES + 61)                          # a
    a.emit(ldt(9, 1, 0))
    a.emit(A.vs("vsmult", 1, 9, 3))
    a.emit(A.vv("vvaddt", 3, 2, 2))
    a.li(1, 2)
    addr(2, OUT_V)
    a.emit(lda(3, R31, N1))
    call("extract")
    addr(10, X_F)
    addr(11, Y_F)
    addr(12, OUT_S)
    a.emit(lda(13, R31, N1))
    a.label("daxpy_s")
    a.emit(ldt(1, 10, 0))
    a.emit(ldt(2, 11, 0))
    a.emit(fop(0x16, 0x0A2, 9, 1, 1))          # MULT f9,f1,f1
    a.emit(fop(0x16, 0x0A0, 1, 2, 1))          # ADDT f1,f2,f1
    a.emit(stt(1, 12, 0))
    for r in (10, 11, 12):
        a.emit(lda(r, r, 8))
    a.emit(opl(0x10, 0x29, 13, 1, 13))
    a.branch(OP["BGT"], 13, "daxpy_s")
    addr(1, OUT_V)
    addr(2, OUT_S)
    a.emit(lda(3, R31, N1))
    call("compare")
    result(0, (0, 7, 8))

    # 2: dot product, T: VVMULT, the products out, summed in order; the
    # scalar loop sums x * y in the same order. $vr2 holds daxpy's result:
    # y goes in again.
    addr(2, Y_F)
    a.emit(lda(3, R31, N1))
    call("insert_2")
    a.emit(A.vv("vvmult", 1, 2, 4))
    a.li(1, 4)
    addr(2, PROD)
    a.emit(lda(3, R31, N1))
    call("extract")
    addr(10, PROD)
    a.emit(lda(13, R31, N1))
    a.emit(fop(0x17, 0x020, 31, 31, 3))        # CPYS: f3 <- 0.0
    a.label("dot_v")
    a.emit(ldt(1, 10, 0))
    a.emit(fop(0x16, 0x0A0, 3, 1, 3))          # ADDT
    a.emit(lda(10, 10, 8))
    a.emit(opl(0x10, 0x29, 13, 1, 13))
    a.branch(OP["BGT"], 13, "dot_v")
    addr(10, X_F)
    addr(11, Y_F)
    a.emit(lda(13, R31, N1))
    a.emit(fop(0x17, 0x020, 31, 31, 4))
    a.label("dot_s")
    a.emit(ldt(1, 10, 0))
    a.emit(ldt(2, 11, 0))
    a.emit(fop(0x16, 0x0A2, 1, 2, 1))
    a.emit(fop(0x16, 0x0A0, 4, 1, 4))
    a.emit(lda(10, 10, 8))
    a.emit(lda(11, 11, 8))
    a.emit(opl(0x10, 0x29, 13, 1, 13))
    a.branch(OP["BGT"], 13, "dot_s")
    a.emit(stt(3, D, 8 * (RES + 62)))
    a.emit(stt(4, D, 8 * (RES + 63)))
    a.emit(ldq(7, D, 8 * (RES + 62)))
    a.emit(ldq(8, D, 8 * (RES + 63)))
    a.emit(opr(0x10, 0x2D, 7, 8, 0))
    a.emit(opl(0x11, 0x40, 0, 1, 0))           # 1 if they differ
    result(1, (0, 7, 8))
    # 2b: dot product, quadwords: VVMULQ, the sum by ADDQ.
    for vr, off in ((1, X_Q), (2, Y_Q)):
        addr(2, off)
        a.emit(lda(3, R31, N1))
        call("insert_%d" % vr)
    a.emit(A.vv("vvmulq", 1, 2, 4))
    a.li(1, 4)
    addr(2, PROD)
    a.emit(lda(3, R31, N1))
    call("extract")
    addr(10, PROD)
    a.emit(lda(13, R31, N1))
    a.emit(lda(7, R31, 0))
    a.label("dotq_v")
    a.emit(ldq(1, 10, 0))
    a.emit(addq(7, 1, 7))
    a.emit(lda(10, 10, 8))
    a.emit(opl(0x10, 0x29, 13, 1, 13))
    a.branch(OP["BGT"], 13, "dotq_v")
    addr(10, X_Q)
    addr(11, Y_Q)
    a.emit(lda(13, R31, N1))
    a.emit(lda(8, R31, 0))
    a.label("dotq_s")
    a.emit(ldq(1, 10, 0))
    a.emit(ldq(2, 11, 0))
    a.emit(opr(0x13, 0x20, 1, 2, 1))           # MULQ
    a.emit(addq(8, 1, 8))
    a.emit(lda(10, 10, 8))
    a.emit(lda(11, 11, 8))
    a.emit(opl(0x10, 0x29, 13, 1, 13))
    a.branch(OP["BGT"], 13, "dotq_s")
    a.emit(opr(0x10, 0x2D, 7, 8, 0))
    a.emit(opl(0x11, 0x40, 0, 1, 0))
    result(2, (0, 7, 8))

    # 3: masked update, 100 elements.
    a.emit(A.vc("setvl", lit=N3))
    for vr, off in ((5, X_M), (6, Y_M)):
        addr(2, off)
        a.emit(lda(3, R31, N3))
        call("insert_%d" % vr)
    a.emit(A.vs("vscmpgt", 5, 0, 7, lit=25))
    a.emit(A.vc("setvm", rb=7))
    a.emit(A.vs("vsaddq", 6, 0, 8, m=True, lit=7))
    a.emit(A.vv("vvmrg", 6, 8, 6))             # vm ? $vr8 : $vr6
    a.li(1, 6)
    addr(2, OUT_V)
    a.emit(lda(3, R31, N3))
    call("extract")
    addr(10, X_M)
    addr(11, Y_M)
    addr(12, OUT_S)
    a.emit(lda(13, R31, N3))
    a.label("mask_s")
    a.emit(ldq(1, 10, 0))
    a.emit(ldq(2, 11, 0))
    a.emit(opl(0x10, 0x4D, 1, 26, 3))          # CMPLT r1,#26: not > 25
    a.branch(OP["BNE"], 3, "mask_s_keep")
    a.emit(opl(0x10, 0x20, 2, 7, 2))
    a.label("mask_s_keep")
    a.emit(stq(2, 12, 0))
    for r in (10, 11, 12):
        a.emit(lda(r, r, 8))
    a.emit(opl(0x10, 0x29, 13, 1, 13))
    a.branch(OP["BGT"], 13, "mask_s")
    addr(1, OUT_V)
    addr(2, OUT_S)
    a.emit(lda(3, R31, N3))
    call("compare")
    result(3, (0, 7, 8))

    # 4: strip-mined, 300 quadwords: SETVL r(remaining) gives this pass's
    # length; r9 counts the passes.
    a.li(14, N4)                               # remaining
    a.emit(lda(15, R31, 0))                    # done
    a.emit(lda(9, R31, 0))
    a.label("strip")
    a.emit(A.vc("setvl", rb=14, rc=17))        # r17 <- this pass's vl
    for vr, off in ((1, X_S), (2, Y_S)):
        addr(2, off)
        a.emit(opl(0x12, 0x39, 15, 3, 4))      # done x 8
        a.emit(addq(2, 4, 2))
        a.emit(opr(0x11, 0x20, 17, 17, 3))
        call("insert_%d" % vr)
    addr(1, RES + 59)
    a.emit(ldq(18, 1, 0))                      # k
    a.emit(A.vs("vsmulq", 1, 18, 3))
    a.emit(A.vv("vvaddq", 3, 2, 3))
    a.li(1, 3)
    addr(2, OUT_V)
    a.emit(opl(0x12, 0x39, 15, 3, 4))
    a.emit(addq(2, 4, 2))
    a.emit(opr(0x11, 0x20, 17, 17, 3))
    call("extract")
    a.emit(addq(15, 17, 15))
    a.emit(opr(0x10, 0x29, 14, 17, 14))        # SUBQ remaining, vl
    a.emit(opl(0x10, 0x20, 9, 1, 9))
    a.branch(OP["BGT"], 14, "strip")
    a.emit(stq(9, D, 8 * (RES + 4 * 4 + 3)))   # the passes
    addr(10, X_S)
    addr(11, Y_S)
    addr(12, OUT_S)
    a.li(13, N4)
    a.label("strip_s")
    a.emit(ldq(1, 10, 0))
    a.emit(ldq(2, 11, 0))
    a.emit(opr(0x13, 0x20, 1, 18, 1))
    a.emit(addq(1, 2, 1))
    a.emit(stq(1, 12, 0))
    for r in (10, 11, 12):
        a.emit(lda(r, r, 8))
    a.emit(opl(0x10, 0x29, 13, 1, 13))
    a.branch(OP["BGT"], 13, "strip_s")
    addr(1, OUT_V)
    addr(2, OUT_S)
    a.li(3, N4)
    call("compare")
    result(4, (0, 7, 8))
    # The trap record's total: nothing may have trapped (the MT_FPCR of the
    # native EV8 traps by design, and is counted out by the script).
    a.li(1, kseg(image.TR_PA))
    a.emit(ldq(1, 1, image.TR_TOTAL))
    a.emit(stq(1, D, 8 * (RES + 21)))
    image.emit_print_text(a, "\n@@BEGIN\n")
    a.li(16, kseg(DATA_PA + 8 * RES))
    a.li(17, NRES)
    a.branch(OP["BSR"], 18, "print_hex")
    image.emit_print_text(a, "@@END\n")
    a.emit(call_pal(0x00))
    code = a.finish()

    img = bytearray(image.IMAGE_TOP)
    pal = (image.build_pal8 if ev8n else image.build_pal)(
        image.TR_PA, kseg(a.labels["main"]), kseg(a.labels["fatal"]))
    for ad, w in pal.items():
        struct.pack_into("<I", img, ad, w)
    for i, w in enumerate(code):
        struct.pack_into("<I", img, image.CODE_PA + 4 * i, w)
    if snips:
        for i, w in enumerate(snips.finish()):
            struct.pack_into("<I", img, image.PAL8_SNIP_PA + 4 * i, w)
    xf, yf, xq, yq, xm, ym, xs, ys = inputs()
    for off, vals in ((X_F, xf), (Y_F, yf), (X_Q, xq), (Y_Q, yq),
                      (X_M, xm), (Y_M, ym), (X_S, xs), (Y_S, ys)):
        for i, v in enumerate(vals):
            struct.pack_into("<Q", img, DATA_PA + 8 * (off + i), v)
    for i in range(128):
        struct.pack_into("<Q", img, DATA_PA + 8 * (IOTA + i), i)
    struct.pack_into("<Q", img, DATA_PA + 8 * (RES + 59), K4)
    struct.pack_into("<Q", img, DATA_PA + 8 * (RES + 61), T.bits(A_DAXPY))
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="tarantula-vv")
    ap.add_argument("--cpu", default="ev8", choices=("ev8", "ev8n"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=float, default=120)
    args = ap.parse_args()
    ev8n = rows.ROWS[args.cpu].family == "EV8"
    work = os.environ.get("ALPHABOX_WORK", os.path.join(HERE, "..", "..", "lab"))
    workdir = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    image.write_rom(os.path.join(workdir, "oracle.rom"), build(ev8n))
    port = args.port or cpu_oracle.free_port()
    env = {"ALPHABOX_CPU_ANY_BOARD": "1", "ALPHABOX_TARANTULA_PEEK": "1"}
    text, emu, secs = cpu_oracle.run(os.path.abspath(args.binary), workdir,
                                     args.cpu, port, env, args.timeout)
    with open(os.path.join(workdir, "serial.txt"), "w") as f:
        f.write(text)
    print("== tarantula_vv %s: one %s on the ES40, %.1f s" % (
        args.label, args.cpu, secs))
    vals = []
    i = text.find("@@BEGIN")
    if i >= 0:
        for ln in text[i:].replace("\r", "").split("\n")[1:]:
            ln = ln.strip()
            if ln == "@@END":
                break
            if ln:
                vals.append(int(ln, 16))
    if len(vals) != NRES:
        print("  no results (see %s)" % workdir)
        for ln in emu.splitlines()[-10:]:
            print("   |", ln)
        print("  TARANTULA VV: FAIL")
        return 1
    want = expected()
    ok = True
    for k, name in enumerate(KERNELS):
        bad, vsum, ssum = vals[4 * k:4 * k + 3]
        if name.startswith("dot"):
            good = bad == 0 and ssum == want[name]
            print("  %-17s vector %016x, scalar %016x, expected %016x: %s" % (
                name, vsum, ssum, want[name], "ok" if good else "FAIL"))
        else:
            good = bad == 0 and ssum == want[name] and vsum == ssum
            print("  %-17s %d elements differ; sums vector %016x, scalar "
                  "%016x, expected %016x: %s" % (
                      name, bad, vsum, ssum, want[name],
                      "ok" if good else "FAIL"))
        ok = ok and good
    print("  the iota built by VC and VS: %d elements of 128 differ from i: %s"
          % (vals[20], "ok" if vals[20] == 0 else "FAIL"))
    ok = ok and vals[20] == 0
    passes = vals[4 * 4 + 3]
    print("  strip-mined passes: %d (expected 3: 128, 128, 44)" % passes)
    traps = vals[21] - (1 if ev8n else 0)      # ev8n: MT_FPCR's own trap
    print("  exceptions taken: %d (expected 0)" % traps)
    ok = ok and passes == 3 and traps == 0
    print("  TARANTULA VV: %s" % ("PASS" if ok else "FAIL"))
    if ok and not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print("  run directory: %s" % workdir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
