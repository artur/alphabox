#!/usr/bin/env python3
"""The Marvel machines in 21464 mode: a boot image on the ES47, ES80 and
GS1280 with `ev8n` processors (docs/platforms/ev8-native.md, section 2 and
S11b; docs/development.md, "The oracle").

    marvel8_mp.py <alphabox-binary> [label] [--layout L ...] [--port N]
                  [--keep]

Each layout runs one image (image.py's EV8 PALcode, build_pal8, numbering
each TPU CPU_BASE x 4 + TPU_ID, N25), loaded as the board's decompressed
console (rom.decompressed: no console runs in 21464 mode), on its board row
with no test hook:

  es47    2 chips of 4 TPUs   (8 processors)
  es80    8 chips of 4 TPUs   (32: the most a machine has)
  gs1280  16 chips of 2 TPUs  (32: PIDs 0-15 on the torus)

Every TPU enters MILD_RESET (N17); TPU 0 of chip 0 runs the test, the
others park until it starts them. What is checked, from the packet's
sources before the code:

  GIO (2.5, N23)   DONE reads 1 at reset; a write with GIO_CNFG's divisor
                   zero does not complete, one written after a divisor does;
                   GIO_DATA holds 32 bits; a read of the CMM's register 0;
                   the CMM's terminal echoes a character typed at it.
  the map (N18)    every processor reads its own chip's R_WHOAMI and the
                   next chip's through ipe8(PID); TPU 0 reads chip 0's and
                   chip 1's through Linux's KSEG form (SPE43); each chip's
                   memory at ((n & 3) | (n & 0x3fc) << 1) << 34 holds what
                   its TPU 0 wrote there.
  PIDs (N19)       CPU_BASE is the chip's dense PID, TPU_CNFG the TPU,
                   R_WHOAMI the coordinates EW <9:5>, NS <4:0>; R_ROUT's
                   entries to itself and to PID 1 in the IDS's layout.
  interrupts (N22) TPU 0's R_INT_MASK at 0x060 and 0x3F0000; an IPI to each
                   TPU in turn (1 << t in its chip's R_INT_REQ) wakes it,
                   from QUIESCE or a wait loop, and no other; R_INT_REQ sets
                   only its own fields; R_INT_STAT written with ones clears.
  the timer (N22)  one timer per chip sets a tick bit for each TPU, <7:4>,
                   each cleared on its own; every TPU takes two ticks.
  the queue (N21)  four IIDs fill R_INT_QUEADD's queue, a fifth is refused;
                   an IO7 interrupt (a sym53c810's SCRIPTS INT, LSI 4 of
                   hose 0) arriving at the full queue is refused and resent
                   by the IO7 once one entry is taken (its trace,
                   ALPHABOX_TRACE_IO7, names both); the queue then reads
                   the three left and the IO7's IID, and empties.

Then the configurations that must be refused, each checked for its
reason: a GS1280 of nine chips of four TPUs (36 processors, more than 32),
a machine mixing `ev7` and `ev8n`, a 21464-mode machine with no boot image.
Exit status 0 when everything holds.
"""
import argparse
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import alpha_asm as A  # noqa: E402
import cpu_oracle  # noqa: E402
import image  # noqa: E402
from alpha_asm import OP, R31, Asm, call_pal, mem, mfc, opl, opr  # noqa: E402
from image import kseg  # noqa: E402

LAYOUTS = {"es47": (2, 4), "es80": (8, 4), "gs1280": (16, 2)}

IMAGE_SIZE = 0x480000        # the Marvel rows' decompressed console
TR8_PA = 0x200000            # trap records: + WHAMI x 0x400
MAILBOX_PA = 0x210000        # start words: + WHAMI x 0x40
WATCH_PA = 0x212000          # each processor's watched line: + w x 0x40
READY_PA = 0x214000          # + w x 8
WOKEN_PA = 0x214200
TDONE_PA = 0x214400
GO_PA = 0x214600             # the timer phase's start, a line of its own
SLOT_PA = 0x218000           # + w x 0x100
MAIN_PA = 0x21C000           # TPU 0's results
SUMS_PA = 0x21D000           # after the k-th IPI: how many TPUs woke
SCRIPTS_PA = 0x220000        # the SCSI controller's program
SIG_OFF = 0x2000             # each chip's word at its memory base + this
SIG = 0x51500000
NTICKS = 2
BOUND = 1 << 22              # a bound on each wait, in loop turns

# Each processor's slot, quadwords.
SLOT_FIELDS = ("TPU_CNFG", "CPU_BASE", "WHOAMI own", "WHOAMI next",
               "memory word", "R_INT_MASK", "IPI interrupts", "IPI vector",
               "IPI stat", "IPI stat cleared", "ticks", "own tick bit")
(S_TPU, S_BASE, S_WHO, S_WHO2, S_MEM, S_MASK, S_IPI_N, S_IPI_VEC,
 S_IPI_STAT, S_IPI_STAT2, S_TICKS, S_TICK_OWN) = range(len(SLOT_FIELDS))

MAIN_FIELDS = ("GIO_DATA at reset", "GIO_DATA, clock stopped",
               "GIO_DATA, clock running", "GIO_DATA, 64-bit write",
               "GIO read of CMM reg 0", "R_WHOAMI PID 0 (KSEG)",
               "R_WHOAMI PID 1 (KSEG)", "R_ROUT[0] of PID 0",
               "R_ROUT[1] of PID 0", "self IPI interrupts",
               "R_INT_REQ 0x40010 -> R_INT_STAT", "ticks <7:4> unmasked",
               "after clearing TPU 0's", "queue bit, 4 IIDs",
               "SCSI ISTAT", "R_INT_QUE head", "R_INT_QUE 2", "R_INT_QUE 3",
               "R_INT_QUE 4", "R_INT_QUE 5 (the IO7's)", "R_INT_QUE empty",
               "queue bit, empty", "echoed character")
(M_GIO_RESET, M_GIO_STOPPED, M_GIO_DONE, M_GIO_WIDE, M_GIO_STATUS, M_WHO0,
 M_WHO1, M_ROUTE0, M_ROUTE1, M_SELF_IPI, M_REQ, M_TICK_ALL, M_TICK_CLR,
 M_Q_STAT, M_ISTAT, M_Q_HEAD, M_Q2, M_Q3, M_Q4, M_Q5, M_Q_EMPTY,
 M_Q_STAT_EMPTY, M_ECHO) = range(len(MAIN_FIELDS))
M_SIG = len(MAIN_FIELDS)     # + p: chip p's memory word, read by TPU 0
ECHO_CHAR = "Z"

# Rbox registers (ev8-native.md 2.3).
R_WHOAMI, R_INT_STAT, R_INT_MASK, R_INT_REQ, R_INT_QUE, R_INT_QUEADD = (
    0x030, 0x050, 0x060, 0x070, 0x080, 0x090)
R_INTER_TIM, R_ROUT, R_INT_MASK_TPU = 0x0A0, 0x2000, 0x3F0000
GIO_CNFG, GIO_DATA, GIO_ADDR = 0x100, 0x110, 0x120
CMM_TX, CMM_RX = 4, 5

SYM_SLOT = 1                 # pci0.1: hose 0 (PID 0's IO7 port 0), slot 1
SYM_BAR = 0x10000000
SYM_LSI = 0 << 5 | SYM_SLOT << 2 | 0    # port 0, slot 1, INTA (Io7.hpp)


def ipe8(n):
    return (~n & 0x1FFF) << 35


def csr8(n):
    return ipe8(n) | 0x7FFC << 20


def io7_8(n, port):
    return ipe8(n) | (~port & 7) << 32


def mem_base(n):
    return ((n & 3) | (n & 0x3FC) << 1) << 34


def seg52(pa):
    return image.SEG52 | pa


def kseg41(pa):
    """Linux's KSEG form, IDENT_ADDR | PA<40:0>, which SPE43 maps back with
    VA<40> copied into PA<47:41> [IDS pp. 688-689]."""
    return image.KSEG | (pa & ((1 << 41) - 1))


def coordinates(layout, c):
    if layout == "gs1280":
        return (c & 1) | ((c >> 3) & 1) << 1, (c >> 1) & 3
    return c, 0


def present(layout):
    chips, tpus = LAYOUTS[layout]
    return [4 * p + t for p in range(chips) for t in range(tpus)]


def build(layout):
    chips, tpus = LAYOUTS[layout]
    procs = present(layout)
    others = [w for w in procs if w]
    I = A.IPR8
    mf, mt = A.hw_mfpr8, A.hw_mtpr8
    a = Asm(image.CODE_PA)
    snips = Asm(image.PAL8_SNIP_PA)

    def ins(name, ra, b, rc, lit=False):
        a.emit(A.ins_op(name, ra, b, rc, lit))

    def ldq(ra, rb, d):
        a.emit(mem(OP["LDQ"], ra, rb, d))

    def stq(ra, rb, d):
        a.emit(mem(OP["STQ"], ra, rb, d))

    def lda(ra, rb, d):
        a.emit(mem(OP["LDA"], ra, rb, d))

    def mb():
        a.emit(mfc(A.MISC["MB"]))

    def bsr(link, label):
        a.branch(OP["BSR"], link, label)

    def ret(link):
        a.emit(A.jmp(2, R31, link))

    n_labels = [0]

    def fresh(stem):
        n_labels[0] += 1
        return "%s_%d" % (stem, n_labels[0])

    def wait_nonzero(va):
        """Spin, bounded, until the quadword at `va` is not zero (r5, r6)."""
        top, out = fresh("wnz"), fresh("wnz_out")
        a.li(5, va)
        a.li(6, BOUND)
        a.label(top)
        ldq(1, 5, 0)
        a.branch(OP["BNE"], 1, out)
        ins("SUBQ", 6, 1, 6, lit=True)
        a.branch(OP["BGT"], 6, top)
        a.label(out)

    def delay(turns):
        top = fresh("delay")
        a.li(6, turns)
        a.label(top)
        ins("SUBQ", 6, 1, 6, lit=True)
        a.branch(OP["BGT"], 6, top)

    def print_text(text):
        for ch in text.encode():
            lda(16, R31, ch)
            bsr(27, "putc")

    def gio_read(reg):
        """r1 = GIO_DATA after a read of the CMM's register `reg` (r12 the
        window of PID 0)."""
        top, top2 = fresh("gr"), fresh("gr2")
        a.label(top)
        ldq(8, 12, GIO_DATA)
        a.branch(OP["BGE"], 8, top)            # DONE <63>: the port idle
        lda(8, R31, reg << 1 | 1)              # GIO_ADDR: START_READ
        stq(8, 12, GIO_ADDR)
        a.label(top2)
        ldq(1, 12, GIO_DATA)
        a.branch(OP["BGE"], 1, top2)

    # PALmode snippets (CALL_PAL 91).
    sn_ident = snips.here
    for w in [mf(1, I["tpu_cnfg"]), mf(2, I["cpu_base"])] + image.PAL8_RETURN:
        snips.emit(w)
    sn_qt = snips.here                          # QUIESCE_TIMEOUT <- r17
    for w in [mt(17, I["quiesce_timeout"]), A.ifetchb()] + image.PAL8_RETURN:
        snips.emit(w)

    a.branch(OP["BR"], R31, "main")

    # --- putc: r16 to the CMM's terminal through GIO (r12 PID 0's window,
    # clobbers r8; link r27) ---
    a.label("putc")
    a.label("pc_w1")
    ldq(8, 12, GIO_DATA)
    a.branch(OP["BGE"], 8, "pc_w1")
    lda(8, R31, CMM_TX << 1)
    stq(8, 12, GIO_ADDR)
    stq(16, 12, GIO_DATA)
    a.label("pc_w2")
    ldq(8, 12, GIO_DATA)
    a.branch(OP["BGE"], 8, "pc_w2")
    ret(27)

    # --- print_hex: r16 = VA, r17 = quadwords, link r26 (r1-r6, r16, r18) ---
    a.label("print_hex")
    ins("BIS", 16, 16, 5)
    ins("BIS", 17, 17, 6)
    a.label("ph_loop")
    ldq(1, 5, 0)
    lda(2, R31, 60)
    a.label("ph_nib")
    ins("SRL", 1, 2, 3)
    ins("AND", 3, 15, 3, lit=True)
    ins("CMPULT", 3, 10, 4, lit=True)
    ins("ADDQ", 3, 48, 16, lit=True)
    ins("ADDQ", 3, 87, 18, lit=True)
    ins("CMOVEQ", 4, 18, 16)
    bsr(27, "putc")
    ins("SUBQ", 2, 4, 2, lit=True)
    a.branch(OP["BGE"], 2, "ph_nib")
    lda(16, R31, 10)
    bsr(27, "putc")
    ins("ADDQ", 5, 8, 5, lit=True)
    ins("SUBQ", 6, 1, 6, lit=True)
    a.branch(OP["BGT"], 6, "ph_loop")
    ret(26)

    a.label("fatal")
    a.li(12, seg52(csr8(0)))
    lda(1, R31, 2)
    stq(1, 12, GIO_CNFG)
    print_text("\n@@FATAL\n")
    a.li(16, kseg(TR8_PA))
    a.li(17, 7)
    bsr(26, "print_hex")
    a.emit(call_pal(0x00))

    # --- setup, every processor (link r26): r9 = w, r10 = PID, r11 = TPU,
    # r12 its chip's window (SPE52), r13 its slot, r14 its trap record, r15
    # its watched line; the identity, the windows, the memory word, its
    # interrupt mask, QUIESCE without a timeout ---
    a.label("setup")
    a.li(16, sn_ident)
    a.emit(call_pal(0x91))
    ins("SRL", 1, 56, 11, lit=True)             # TPU_ID
    ins("SLL", 2, 54, 10, lit=True)             # CPU_BASE<9:0>
    ins("SRL", 10, 54, 10, lit=True)
    ins("S4ADDQ", 10, 11, 9)                    # w = PID x 4 + TPU (N25)
    a.li(13, kseg(SLOT_PA))
    ins("SLL", 9, 8, 3, lit=True)
    ins("ADDQ", 13, 3, 13)
    stq(1, 13, 8 * S_TPU)
    stq(2, 13, 8 * S_BASE)
    a.li(14, kseg(TR8_PA))
    ins("SLL", 9, 10, 3, lit=True)
    ins("ADDQ", 14, 3, 14)
    a.li(15, kseg(WATCH_PA))
    ins("SLL", 9, 6, 3, lit=True)
    ins("ADDQ", 15, 3, 15)
    # The chip's window: SEG52 | ipe8(PID) | 0x7FFC << 20.
    ins("ORNOT", R31, 10, 1)
    ins("SLL", 1, 51, 1, lit=True)
    ins("SRL", 1, 16, 1, lit=True)              # (~PID & 0x1fff) << 35
    a.li(2, seg52(0x7FFC << 20))
    ins("BIS", 1, 2, 12)
    ldq(3, 12, R_WHOAMI)
    stq(3, 13, 8 * S_WHO)
    # The next chip's window.
    ins("ADDQ", 10, 1, 4, lit=True)
    a.li(5, chips)
    ins("CMPEQ", 4, 5, 5)
    ins("CMOVNE", 5, R31, 4)
    ins("ORNOT", R31, 4, 1)
    ins("SLL", 1, 51, 1, lit=True)
    ins("SRL", 1, 16, 1, lit=True)
    ins("BIS", 1, 2, 1)
    ldq(3, 1, R_WHOAMI)
    stq(3, 13, 8 * S_WHO2)
    # TPU 0: a word in the chip's own memory, read back.
    a.branch(OP["BNE"], 11, "no_sig")
    ins("AND", 10, 3, 1, lit=True)
    ins("SRL", 10, 2, 2, lit=True)
    ins("SLL", 2, 3, 2, lit=True)               # (PID & 0x3fc) << 1
    ins("BIS", 1, 2, 1)
    ins("SLL", 1, 34, 1, lit=True)
    a.li(2, kseg(SIG_OFF))
    ins("BIS", 1, 2, 5)
    a.li(3, SIG)
    ins("ADDQ", 3, 10, 3)
    stq(3, 5, 0)
    mb()
    ldq(4, 5, 0)
    stq(4, 13, 8 * S_MEM)
    a.label("no_sig")
    # Its mask: its IPI bit and its tick bit. TPU 0 writes R_INT_MASK at
    # 0x060 and reads it at 0x3F0000; TPU t at 0x3F0000 + 0x40 x t.
    lda(3, R31, 1)
    ins("SLL", 3, 11, 3)
    ins("SLL", 3, 4, 4, lit=True)
    ins("BIS", 3, 4, 3)
    a.li(6, R_INT_MASK_TPU)
    ins("ADDQ", 12, 6, 6)
    ins("SLL", 11, 6, 7, lit=True)
    ins("ADDQ", 6, 7, 5)
    a.branch(OP["BNE"], 11, "mask_t")
    stq(3, 12, R_INT_MASK)
    a.branch(OP["BR"], R31, "mask_back")
    a.label("mask_t")
    stq(3, 5, 0)
    a.label("mask_back")
    mb()
    ldq(4, 5, 0)
    stq(4, 13, 8 * S_MASK)
    lda(17, R31, 2)                             # WATCH_EN, no timeout
    a.li(16, sn_qt)
    a.emit(call_pal(0x91))
    ret(26)

    # --- the timer phase, every processor (link r26) ---
    a.label("timer_phase")
    a.label("tp_go")
    a.li(2, kseg(GO_PA))
    a.emit(mfc(A.MISC["LDQ_ARM"], 1, 2))
    a.branch(OP["BNE"], 1, "tp_start")
    a.emit(mfc(A.MISC["QUIESCE"]))
    a.branch(OP["BR"], R31, "tp_go")
    a.label("tp_start")
    lda(19, R31, 0)
    lda(21, R31, 0)
    ldq(20, 14, image.TR_COUNT)
    a.label("tp_loop")
    a.li(17, 1 << 35)                           # IER_CM: level 2, the timer
    a.emit(call_pal(0x8C))
    # Arm, test, QUIESCE [IDS p. 670]: armed after the test, a tick taken at
    # the LDQ_ARM (the PALcode counts it and disables it) would leave the
    # QUIESCE nothing to wake it. One taken at the QUIESCE returns to it with
    # the flag clear: an interrupt to the TPU clears it [IDS p. 513].
    a.label("tp_wait")
    a.emit(mfc(A.MISC["LDQ_ARM"], 2, 15))
    ldq(1, 14, image.TR_COUNT)
    ins("SUBQ", 1, 20, 1)
    a.branch(OP["BNE"], 1, "tp_tick")
    a.emit(mfc(A.MISC["QUIESCE"]))
    a.branch(OP["BR"], R31, "tp_wait")
    a.label("tp_tick")
    ins("ADDQ", 20, 1, 20)
    ins("ADDQ", 19, 1, 19, lit=True)
    ldq(3, 12, R_INT_STAT)
    lda(4, R31, 0x10)
    ins("SLL", 4, 11, 4)                        # 1 << (4 + t)
    ins("AND", 3, 4, 5)
    ins("CMPULT", R31, 5, 5)
    ins("ADDQ", 21, 5, 21)
    stq(4, 12, R_INT_STAT)                      # clear its own tick
    mb()
    ins("CMPULT", 19, NTICKS, 1, lit=True)
    a.branch(OP["BNE"], 1, "tp_loop")
    stq(19, 13, 8 * S_TICKS)
    stq(21, 13, 8 * S_TICK_OWN)
    a.li(2, kseg(TDONE_PA))
    ins("S8ADDQ", 9, 2, 2)
    lda(1, R31, 1)
    stq(1, 2, 0)
    mb()
    ret(26)

    # --- every processor but TPU 0 of chip 0, released by it ---
    a.label("start")
    bsr(26, "setup")
    ldq(20, 14, image.TR_COUNT)
    a.li(17, 1 << 36)                           # IER_CM: level 3, the IPI
    a.emit(call_pal(0x8C))
    a.li(2, kseg(READY_PA))
    ins("S8ADDQ", 9, 2, 2)
    lda(1, R31, 1)
    stq(1, 2, 0)
    mb()
    ins("AND", 9, 7, 1, lit=True)               # w % 8 == 1: a wait loop
    ins("CMPEQ", 1, 1, 1, lit=True)
    a.branch(OP["BNE"], 1, "ipi_spin")
    a.label("ipi_q")                            # arm, test, QUIESCE, as tp_wait
    a.emit(mfc(A.MISC["LDQ_ARM"], 2, 15))
    ldq(1, 14, image.TR_COUNT)
    ins("SUBQ", 1, 20, 1)
    a.branch(OP["BNE"], 1, "ipi_woke")
    a.emit(mfc(A.MISC["QUIESCE"]))
    a.branch(OP["BR"], R31, "ipi_q")
    a.label("ipi_spin")
    ldq(1, 14, image.TR_COUNT)
    ins("SUBQ", 1, 20, 1)
    a.branch(OP["BEQ"], 1, "ipi_spin")
    a.label("ipi_woke")
    stq(1, 13, 8 * S_IPI_N)
    ldq(2, 14, image.TR_VEC)
    stq(2, 13, 8 * S_IPI_VEC)
    ldq(3, 12, R_INT_STAT)
    ins("AND", 3, 0xF, 3, lit=True)
    stq(3, 13, 8 * S_IPI_STAT)
    lda(4, R31, 1)
    ins("SLL", 4, 11, 4)
    stq(4, 12, R_INT_STAT)                      # clear its own IPI
    mb()
    ldq(3, 12, R_INT_STAT)
    ins("AND", 3, 0xF, 3, lit=True)
    stq(3, 13, 8 * S_IPI_STAT2)
    a.li(2, kseg(WOKEN_PA))
    ins("S8ADDQ", 9, 2, 2)
    lda(1, R31, 1)
    stq(1, 2, 0)
    mb()
    bsr(26, "timer_phase")
    a.label("park")
    a.emit(mfc(A.MISC["LDQ_ARM"], 2, 15))
    a.emit(mfc(A.MISC["QUIESCE"]))
    a.branch(OP["BR"], R31, "park")

    # --- TPU 0 of chip 0 ---
    a.label("main")
    bsr(26, "setup")
    a.li(20, kseg(MAIN_PA))

    def mstore(reg, field):
        stq(reg, 20, 8 * field)

    # GIO (N23): DONE at reset; the clock stopped, then running; 32 bits.
    ldq(1, 12, GIO_DATA)
    mstore(1, M_GIO_RESET)
    lda(1, R31, CMM_TX << 1)
    stq(1, 12, GIO_ADDR)
    lda(1, R31, ord("#"))
    stq(1, 12, GIO_DATA)
    mb()
    delay(1 << 12)
    ldq(1, 12, GIO_DATA)
    mstore(1, M_GIO_STOPPED)
    lda(1, R31, 2)                              # DIVISOR 1
    stq(1, 12, GIO_CNFG)
    mb()
    ldq(1, 12, GIO_DATA)
    mstore(1, M_GIO_DONE)
    a.li(1, 0xABCD000000000000 | ord("A"))
    stq(1, 12, GIO_DATA)
    mb()
    ldq(1, 12, GIO_DATA)
    mstore(1, M_GIO_WIDE)
    gio_read(0)
    mstore(1, M_GIO_STATUS)
    print_text("\n@@MARVEL8 %s\n" % layout)

    # The map through Linux's KSEG arithmetic, and the route table.
    for va, field in ((kseg41(csr8(0) | R_WHOAMI), M_WHO0),
                      (kseg41(csr8(1) | R_WHOAMI), M_WHO1),
                      (kseg41(csr8(0) | R_ROUT), M_ROUTE0),
                      (kseg41(csr8(0) | R_ROUT + 0x10), M_ROUTE1)):
        a.li(3, va)
        ldq(1, 3, 0)
        mstore(1, field)

    # Release the others and wait for them.
    a.li(1, kseg(a.labels["start"]))
    a.li(2, kseg(MAILBOX_PA))
    for w in others:
        stq(1, 2, 0x40 * w)
    mb()
    for w in others:
        wait_nonzero(kseg(READY_PA + 8 * w))
    for p in range(chips):
        a.li(3, kseg(mem_base(p) + SIG_OFF))
        ldq(1, 3, 0)
        mstore(1, M_SIG + p)

    # An IPI to each TPU in turn (N22): 1 << t in its chip's R_INT_REQ.
    for k, w in enumerate(others):
        p, t = divmod(w, 4)
        a.li(3, seg52(csr8(p) | R_INT_REQ))
        lda(1, R31, 1 << t)
        stq(1, 3, 0)
        mb()
        wait_nonzero(kseg(WOKEN_PA + 8 * w))
        delay(1 << 14)                          # time for a wrong one to wake
        a.li(3, kseg(WOKEN_PA))
        lda(4, R31, 0)
        for v in others:
            ldq(1, 3, 8 * v)
            ins("ADDQ", 4, 1, 4)
        a.li(3, kseg(SUMS_PA + 8 * k))
        stq(4, 3, 0)

    # TPU 0's own: R_INT_REQ bit 0 of chip 0, its mask, IER_CM level 3.
    ldq(19, 14, image.TR_COUNT)
    a.li(17, 1 << 36)
    a.emit(call_pal(0x8C))
    lda(1, R31, 1)
    stq(1, 12, R_INT_REQ)
    mb()
    a.li(6, BOUND)
    a.label("self_ipi")
    ldq(1, 14, image.TR_COUNT)
    ins("SUBQ", 1, 19, 1)
    a.branch(OP["BNE"], 1, "self_ipi_done")
    ins("SUBQ", 6, 1, 6, lit=True)
    a.branch(OP["BGT"], 6, "self_ipi")
    a.label("self_ipi_done")
    mstore(1, M_SELF_IPI)
    lda(1, R31, 1)
    stq(1, 12, R_INT_STAT)
    # R_INT_REQ sets only its own fields: a tick bit (4) is not one.
    stq(R31, 12, R_INT_MASK)
    a.li(1, 0x40010)
    stq(1, 12, R_INT_REQ)
    mb()
    ldq(1, 12, R_INT_STAT)
    mstore(1, M_REQ)
    a.li(1, 0x40000)
    stq(1, 12, R_INT_STAT)
    mb()

    # The interval timer (N22): chip 0's ticks with every TPU's interrupt
    # off, all four bits; TPU 0 clears its own.
    lda(1, R31, 7)
    stq(1, 12, R_INTER_TIM)
    mb()
    a.li(6, BOUND)
    a.label("tick_all")
    ldq(1, 12, R_INT_STAT)
    ins("AND", 1, 0xF0, 1, lit=True)
    ins("CMPEQ", 1, 0xF0, 2, lit=True)
    a.branch(OP["BNE"], 2, "tick_all_done")
    ins("SUBQ", 6, 1, 6, lit=True)
    a.branch(OP["BGT"], 6, "tick_all")
    a.label("tick_all_done")
    mstore(1, M_TICK_ALL)
    stq(R31, 12, R_INTER_TIM)                   # stopped while it looks
    mb()
    lda(1, R31, 0x10)
    stq(1, 12, R_INT_STAT)
    mb()
    ldq(1, 12, R_INT_STAT)
    ins("AND", 1, 0xF0, 1, lit=True)
    mstore(1, M_TICK_CLR)
    lda(1, R31, 0xE0)
    stq(1, 12, R_INT_STAT)
    lda(1, R31, 0x11)
    stq(1, 12, R_INT_MASK)                      # its mask again
    # Every chip's timer, and every TPU takes its ticks.
    lda(1, R31, 7)
    for p in range(chips):
        a.li(3, seg52(csr8(p) | R_INTER_TIM))
        stq(1, 3, 0)
    mb()
    a.li(2, kseg(GO_PA))
    lda(1, R31, 1)
    stq(1, 2, 0)
    mb()
    bsr(26, "timer_phase")
    a.li(20, kseg(MAIN_PA))                     # timer_phase's r20
    for w in others:
        wait_nonzero(kseg(TDONE_PA + 8 * w))
    for p in range(chips):
        a.li(3, seg52(csr8(p) | R_INTER_TIM))
        stq(R31, 3, 0)
    mb()
    lda(17, R31, 0)
    a.emit(call_pal(0x8C))                      # interrupts off
    stq(R31, 12, R_INT_MASK)
    lda(1, R31, 0xF0)
    stq(1, 12, R_INT_STAT)
    mb()

    # The queue (N21): four IIDs, a fifth refused.
    for iid in (0x111, 0x222, 0x333, 0x444, 0x555):
        a.li(1, iid)
        stq(1, 12, R_INT_QUEADD)
        mb()
    ldq(1, 12, R_INT_STAT)
    a.li(2, 1 << 12)
    ins("AND", 1, 2, 1)
    mstore(1, M_Q_STAT)
    # The SCSI controller in pci0.1: PCI window 0 of port 0 direct-mapped
    # over the first 1 GB, its registers at SYM_BAR, memory and bus master
    # on, its SIR interrupt enabled (DIEN), LSI 4 enabled to PID 0.
    a.li(5, seg52(io7_8(0, 0) + 0xFF800000))
    lda(1, R31, 1)
    stq(1, 5, 0x1000)                           # POx_WBASE0
    a.li(1, 0x3FF00000)
    stq(1, 5, 0x1100)                           # POx_WMASK0
    stq(R31, 5, 0x1200)                         # POx_TBASE0
    a.li(5, seg52(io7_8(0, 0) + 0xFE000000 + (SYM_SLOT << 11)))
    a.li(1, SYM_BAR)
    a.emit(mem(OP["STL"], 1, 5, 0x14))          # BAR1 (memory)
    lda(1, R31, 0x0006)
    a.emit(mem(OP["STL"], 1, 5, 0x04))          # COMMAND
    mb()
    a.li(5, seg52(io7_8(0, 0) + SYM_BAR))
    lda(1, R31, 0x04)
    a.emit(mem(OP["STB"], 1, 5, 0x39))          # DIEN: SIR
    a.li(3, seg52(io7_8(0, 7) + 0xFF800000 + 0x310000 + SYM_LSI * 0x40))
    a.li(1, 1 << 24)                            # LSI_CTL: enabled, PID 0
    stq(1, 3, 0)
    mb()
    a.li(1, SCRIPTS_PA)
    a.emit(mem(OP["STL"], 1, 5, 0x2C))          # DSP: the SCRIPTS run
    mb()
    a.li(6, BOUND)
    a.label("sym_wait")
    a.emit(mem(OP["LDBU"], 1, 5, 0x14))         # ISTAT: DIP
    a.branch(OP["BLBS"], 1, "sym_done")
    ins("SUBQ", 6, 1, 6, lit=True)
    a.branch(OP["BGT"], 6, "sym_wait")
    a.label("sym_done")
    mstore(1, M_ISTAT)
    delay(1 << 16)                              # the line reaches the IO7
    ldq(1, 12, R_INT_QUE)
    mstore(1, M_Q_HEAD)
    stq(R31, 12, R_INT_QUE)                     # taken: the IO7 resends
    mb()
    for field in (M_Q2, M_Q3, M_Q4, M_Q5):
        ldq(1, 12, R_INT_QUE)
        mstore(1, field)
        stq(R31, 12, R_INT_QUE)
        mb()
    ldq(1, 12, R_INT_QUE)
    mstore(1, M_Q_EMPTY)
    ldq(1, 12, R_INT_STAT)
    a.li(2, 1 << 12)
    ins("AND", 1, 2, 1)
    mstore(1, M_Q_STAT_EMPTY)

    # The terminal: a character typed at the CMM's terminal, echoed.
    print_text("@@ECHO?\n")
    a.li(19, 1 << 26)
    a.label("echo_wait")
    gio_read(0)                                 # ST_RXREADY <8>
    ins("SRL", 1, 8, 2, lit=True)
    a.branch(OP["BLBS"], 2, "echo_got")
    ins("SUBQ", 19, 1, 19, lit=True)
    a.branch(OP["BGT"], 19, "echo_wait")
    lda(16, R31, ord("?"))
    a.branch(OP["BR"], R31, "echo_show")
    a.label("echo_got")
    gio_read(CMM_RX)
    ins("AND", 1, 0xFF, 16, lit=True)
    a.label("echo_show")
    mstore(16, M_ECHO)
    ins("BIS", 16, 16, 19)
    lda(16, R31, ord("["))
    bsr(27, "putc")
    ins("BIS", 19, 19, 16)
    bsr(27, "putc")
    lda(16, R31, ord("]"))
    bsr(27, "putc")

    nmain = M_SIG + chips
    print_text("\n@@BEGIN\n")
    a.li(16, kseg(MAIN_PA))
    a.li(17, nmain)
    bsr(26, "print_hex")
    print_text("@@\n")
    a.li(16, kseg(SUMS_PA))
    a.li(17, len(others))
    bsr(26, "print_hex")
    for w in procs:
        print_text("@@\n")
        a.li(16, kseg(SLOT_PA + 0x100 * w))
        a.li(17, len(SLOT_FIELDS))
        bsr(26, "print_hex")
    print_text("@@END\n")
    a.emit(call_pal(0x00))
    code = a.finish()
    assert image.CODE_PA + 4 * len(code) <= image.CODE_TOP

    img = bytearray(IMAGE_SIZE)
    pal = image.build_pal8(TR8_PA, kseg(a.labels["main"]),
                           kseg(a.labels["fatal"]), by_chip=True,
                           mailbox_pa=MAILBOX_PA)
    for addr, w in pal.items():
        struct.pack_into("<I", img, addr, w)
    for i, w in enumerate(code):
        struct.pack_into("<I", img, image.CODE_PA + 4 * i, w)
    for i, w in enumerate(snips.finish()):
        struct.pack_into("<I", img, image.PAL8_SNIP_PA + 4 * i, w)
    # The SCRIPTS: INT 0 (SIR), which stops them and interrupts.
    for i, w in enumerate((0x98080000, 0x00000000)):
        struct.pack_into("<I", img, SCRIPTS_PA + 4 * i, w)
    return img, nmain


def config(layout, port, chips=None, tpus=None, cpu="ev8n",
           image_name="marvel8.rom", mixed=False):
    c, t = LAYOUTS[layout]
    chips = c if chips is None else chips
    tpus = t if tpus is None else tpus
    lines = ["sys0 = tsunami", "{", '  platform = "%s";' % layout,
             "  memory.bits = 29;", '  rom.srm = "absent.exe";']
    if image_name:
        lines.append('  rom.decompressed = "%s";' % image_name)
    lines += ['  rom.nvram = "cmm_nvram.bin";', "  exit_on_pal_halt = true;"]
    for i in range(chips):
        row = "ev7" if mixed and i == 1 else cpu
        opt = " tpus = %d;" % tpus if row == "ev8n" else ""
        lines.append("  cpu%d = %s { speed = 800M;%s }" % (i, row, opt))
    lines += ['  serial0 = serial { address = "127.0.0.1"; port = %d; }'
              % port, "  pci0.%d = sym53c810 { }" % SYM_SLOT, "}", ""]
    return "\n".join(lines)


def run(binary, workdir, cfg, port, timeout):
    """Boot; type ECHO_CHAR at the prompt; return (serial, output, s, rc)."""
    with open(os.path.join(workdir, "es40.cfg"), "w") as f:
        f.write(cfg)
    out = open(os.path.join(workdir, "alphabox.out"), "wb")
    t0 = time.time()
    # The IO7's trace says when the full queue refused its IID and when it
    # was taken on being sent again.
    env = dict(os.environ, ALPHABOX_TRACE_IO7="1")
    proc = subprocess.Popen([binary, "run"], cwd=workdir, stdout=out,
                            stderr=subprocess.STDOUT, env=env)
    data = bytearray()
    try:
        sock = None
        deadline = t0 + timeout
        while sock is None and time.time() < deadline:
            if proc.poll() is not None:
                break
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            except OSError:
                time.sleep(0.05)
        if sock is not None:
            sock.settimeout(1.0)
            typed = False
            while (b"@@END" not in data and b"@@FATAL" not in data and
                   time.time() < deadline):
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    if proc.poll() is not None:
                        break
                    continue
                if not chunk:
                    break
                data += chunk
                if not typed and b"@@ECHO?" in data:
                    sock.sendall(ECHO_CHAR.encode())
                    typed = True
            sock.close()
        try:
            proc.wait(timeout=max(5.0, deadline - time.time()))
        except subprocess.TimeoutExpired:
            proc.send_signal(signal.SIGTERM)   # our own emulator only
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    finally:
        out.close()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    with open(os.path.join(workdir, "alphabox.out"), "rb") as f:
        emu = f.read().decode("latin-1")
    return data.decode("latin-1"), emu, time.time() - t0, proc.returncode


def parse(text):
    i = text.find("@@BEGIN")
    if i < 0:
        return None
    bufs, cur = [], []
    for ln in text[i:].replace("\r", "").split("\n")[1:]:
        ln = ln.strip()
        if ln in ("@@", "@@END"):
            bufs.append(cur)
            cur = []
            if ln == "@@END":
                return bufs
            continue
        if ln:
            try:
                cur.append(int(ln, 16))
            except ValueError:
                return None
    return None


def expected_main(layout, nmain):
    chips, _ = LAYOUTS[layout]
    done = 1 << 63
    ns0, ew0 = coordinates(layout, 0)
    ns1, ew1 = coordinates(layout, 1)
    e = {
        M_GIO_RESET: done, M_GIO_STOPPED: ord("#"),
        M_GIO_DONE: done | ord("#"), M_GIO_WIDE: done | ord("A"),
        M_WHO0: ew0 << 5 | ns0, M_WHO1: ew1 << 5 | ns1,
        # Every access, CAD, the destination's coordinates; to PID 1 one
        # hop North (NS + 1, IND 0) on every layout.
        M_ROUTE0: 0xFF | ns0 << 16 | ew0 << 12,
        M_ROUTE1: 0xFF | ns1 << 16 | ew1 << 12,
        M_SELF_IPI: 1, M_REQ: 0x40000, M_TICK_ALL: 0xF0, M_TICK_CLR: 0xE0,
        M_Q_STAT: 1 << 12, M_ISTAT: None, M_Q_HEAD: 1 << 24 | 0x111,
        M_Q2: 1 << 24 | 0x222, M_Q3: 1 << 24 | 0x333, M_Q4: 1 << 24 | 0x444,
        M_Q5: 1 << 24 | (0 << 14 | SYM_LSI), M_Q_EMPTY: 0,
        M_Q_STAT_EMPTY: 0, M_ECHO: ord(ECHO_CHAR),
    }
    for p in range(chips):
        e[M_SIG + p] = SIG + p
    return e


def check_layout(binary, layout, work, port, timeout, keep):
    chips, tpus = LAYOUTS[layout]
    procs = present(layout)
    others = [w for w in procs if w]
    workdir = os.path.join(work, layout)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    img, nmain = build(layout)
    image.write_rom(os.path.join(workdir, "marvel8.rom"), img)
    text, emu, secs, rc = run(binary, workdir, config(layout, port), port,
                              timeout)
    with open(os.path.join(workdir, "serial.txt"), "w") as f:
        f.write(text)
    print("== %s: %d chips of %d TPUs in 21464 mode, %.1f s"
          % (layout, chips, tpus, secs))
    bufs = parse(text)
    if not bufs or len(bufs) != 2 + len(procs):
        print("  no results (see %s)" % workdir)
        for ln in (text.splitlines() + emu.splitlines())[-12:]:
            print("   |", ln)
        return 0, 1
    main, sums, slots = bufs[0], bufs[1], bufs[2:]
    passes = fails = 0

    def check(what, got, want, ok=None):
        nonlocal passes, fails
        good = ok if ok is not None else got == want
        if good:
            passes += 1
        else:
            fails += 1
            print("  FAIL %s: %s, want %s" % (
                what, "%#x" % got if isinstance(got, int) else got,
                "%#x" % want if isinstance(want, int) else want))

    e = expected_main(layout, nmain)
    for k in range(nmain):
        name = MAIN_FIELDS[k] if k < M_SIG else "memory word of PID %d" % (
            k - M_SIG)
        got = main[k] if k < len(main) else None
        if k == M_GIO_STATUS:
            # DONE and the CMM's status in the low 32 bits, nothing above.
            check(name, got, "DONE, <62:32> clear",
                  ok=got is not None and got >> 32 == 1 << 31)
        elif k == M_ISTAT:
            check(name, got, "DIP <0> set", ok=got is not None and got & 1)
        else:
            check(name, got, e[k])
    for k, s in enumerate(sums):
        check("TPUs woken after the IPI to %d" % others[k], s, k + 1)
    for w, slot in zip(procs, slots):
        p, t = divmod(w, 4)
        ns, ew = coordinates(layout, p)
        nns, new = coordinates(layout, (p + 1) % chips)
        want = {S_TPU: t << 56, S_BASE: p, S_WHO: ew << 5 | ns,
                S_WHO2: new << 5 | nns, S_MEM: SIG + p if t == 0 else 0,
                S_MASK: (1 << t) | (1 << (4 + t)), S_TICKS: NTICKS,
                S_TICK_OWN: NTICKS}
        if w:
            want.update({S_IPI_N: 1, S_IPI_VEC: 0x680, S_IPI_STAT: 1 << t,
                         S_IPI_STAT2: 0})
        for f, name in enumerate(SLOT_FIELDS):
            if f in want:
                check("w%d (PID %d TPU %d) %s" % (w, p, t, name), slot[f],
                      want[f])
    if ("[%s]" % ECHO_CHAR) not in text:
        check("the echo on the terminal", "absent", "[%s]" % ECHO_CHAR,
              ok=False)
    else:
        passes += 1
    started = sum(1 for ln in emu.splitlines() if "*** STARTING ***" in ln)
    check("processors started at reset", started, len(procs))
    for what in ("LSI %d: refused by PID 0's full interrupt queue" % SYM_LSI,
                 "LSI %d: sent again, taken by PID 0" % SYM_LSI):
        check("the IO7: %s" % what, "absent", "in the trace",
              ok=what in emu)
    # A JIT_VERIFY lane: no compiled block may differ from the interpreter.
    mism = [ln for ln in emu.splitlines() if "MISMATCH" in ln]
    check("JIT_VERIFY MISMATCH lines", len(mism), 0)
    vlines = [ln for ln in emu.splitlines() if "compiled-block execs" in ln]
    if vlines:
        print("  %s" % vlines[-1].strip())
    print("  %d results: PASS %d, FAIL %d" % (passes + fails, passes, fails))
    if not fails and not keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print("  run directory: %s" % workdir)
    return passes, fails


def check_refusals(binary, work, port):
    """Configurations refused, each with its reason."""
    cases = (
        ("gs1280, 9 chips of 4 TPUs", config("gs1280", port, chips=9,
                                              tpus=4),
         "Alphabox runs at most 32"),
        ("es47, ev8n with ev7", config("es47", port, mixed=True),
         "processors of different families"),
        ("es47 in 21464 mode, no boot image", config("es47", port,
                                                      image_name=None),
         "starts from a boot image"),
    )
    passes = fails = 0
    for what, cfg, reason in cases:
        workdir = os.path.join(work, "refused")
        shutil.rmtree(workdir, ignore_errors=True)
        os.makedirs(workdir)
        with open(os.path.join(workdir, "es40.cfg"), "w") as f:
            f.write(cfg)
        p = subprocess.run([binary, "run"], cwd=workdir, capture_output=True,
                           timeout=60)
        out = (p.stdout + p.stderr).decode("latin-1")
        ok = p.returncode != 0 and reason in out
        print("  refused: %s: %s" % (what, "ok" if ok else "FAIL (rc %d)"
                                     % p.returncode))
        if not ok:
            for ln in out.splitlines()[-5:]:
                print("   |", ln)
        passes, fails = passes + ok, fails + (not ok)
        shutil.rmtree(workdir, ignore_errors=True)
    return passes, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="marvel8-mp")
    ap.add_argument("--layout", action="append", choices=sorted(LAYOUTS))
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=float, default=180)
    args = ap.parse_args()
    work = os.environ.get("ALPHABOX_WORK", os.path.join(HERE, "..", "..", "lab"))
    work = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    binary = os.path.abspath(args.binary)
    port = args.port or cpu_oracle.free_port()
    total_p = total_f = 0
    for layout in args.layout or ("es47", "es80", "gs1280"):
        p, f = check_layout(binary, layout, work, port, args.timeout,
                            args.keep)
        total_p, total_f = total_p + p, total_f + f
    p, f = check_refusals(binary, work, port)
    total_p, total_f = total_p + p, total_f + f
    print("  MARVEL8 MP: %s (PASS %d, FAIL %d)" % (
        "PASS" if not total_f else "FAIL", total_p, total_f))
    if not total_f and not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if not total_f else 1


if __name__ == "__main__":
    sys.exit(main())
