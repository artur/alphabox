#!/usr/bin/env python3
"""The Alphabox PALcode for the native 21464 (firmware/ev8, S12a phase 1)
on the ES47 in 21464 mode: 2 chips of 4 `ev8n` TPUs, booted from a
`rom.decompressed` holding the PALcode (built by firmware/ev8/build.py with
nada) and a test program in the console's place (docs/platforms/ev8-native.md,
sections 3 and S12a; docs/development.md, "The oracle").

    pal8_test.py <alphabox-binary> [label] [--port N] [--keep] [--nada PATH]

Every TPU resets at PAL_BASE + 0xB00; processor 0 (PID 0, TPU 0) enters the
test program as its console, the seven others park on the HWRPB's RXRDY
(N17). What is checked, written from the ARM's Alpha Linux PALcode
chapters and the IDS before the code (ARM and IDS pages as in
ev8-native.md):

  processor 0     the console's entry (a0 0 at power-up, a1 its WHAMI), PS 7
                  by rdps, whami 0 [ARM p. 566], wrval/rdval [pp. 557, 576],
                  swpipl's old IPL and rdps's PS for 0, 5 and 0xFF (a0<2:0>)
                  [pp. 555, 562]; wrent of entInt and entMM, a1 = 6 ignored
                  [p. 568], wrkgp [p. 571]; a wripir to itself [p. 570] at
                  IPL 7 pending in R_INT_STAT (N22) and not taken, at IPL 5
                  not taken, at IPL 4 taken at the next instruction: entInt
                  type 0 at IPL 5 with GP = KGP, the frame PS, PC, GP, a0-a2
                  [pp. 601-605], the bit cleared; rti restores a0-a2, GP, the
                  PS [p. 559]. Each TPU's R_INT_MASK holds its IPI bit.
  starting        each secondary in turn as Linux starts one (smp.c; ARM pp.
                  693-694, 725-726): CPU_restart and its data, CV and RC set,
                  "START\\r\\n" in the RX buffer, its RXRDY bit; it clears the
                  bit, sets BIP and clears RC, and starts at CPU_restart with
                  R27 the data, R26 and R25 zero, SP the HWPCB's KSP, IPL 7,
                  the HWPCB's FEN (an FP instruction runs), PCC, PTBR, USP,
                  UNIQUE, the HWRPB's VPTB, MCES 8, PCBB the slot [Table 27-9].
  the TB fills    on each secondary, from a hand-built three-level page table
                  with Linux's self-map at VPTB 0xFFFFFFFE00000000: a load
                  and a store (DTBM_SINGLE through VA_FORM, the VPTE page's
                  DTBM_DOUBLE walk), a call into a mapped page (ITB_MISS
                  through IVA_FORM); and entMM [p. 606] with VA, MMCSR [p. 593]
                  and the cause for: an invalid level 3 PTE on a load, a
                  store and an instruction fetch, a level 3 PTE with FOE, an
                  invalid level 2 PTE with KRE clear (ACV) and set (TNV), the
                  same on an instruction fetch (the double miss of ITB_MISS),
                  an invalid level 1 PTE, a valid level 2 PTE without KRE;
                  each frame's PC, PS, GP and saved a0-a2, and rti's return.
  IPIs            a wripir to each secondary in turn, waiting at IPL 0 in
                  QUIESCE, wakes it and no other: entInt type 0, IPL 5.
  halt            each secondary halts (CALL_PAL HALT at IPL 3): its slot's
                  halt PCBB, PC, PS, R25-R27 and reason 5 [pp. 634-635], and
                  it parks; one is started again from there and halts again;
                  processor 0 halts and re-enters the console with a0 = 5.
  the impure areas  (white box) each TPU's entries, KGP, PTBR, PCBB, MCES,
                  sysvalue, PS, USP, WHAMI = PID x 4 + TPU (N25), UNIQUE,
                  VPTB, its chip's register window ipe8(PID) (N18) and TPU bit.

Run output goes to $ALPHABOX_WORK/runs/<label> (kept on failure or --keep).
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
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "firmware", "ev8"))

import alpha_asm as A  # noqa: E402
import build as fw  # noqa: E402  (firmware/ev8/build.py)
import cpu_oracle  # noqa: E402
from alpha_asm import OP, R31, Asm, call_pal, mem, mfc  # noqa: E402

L = fw.layout()
KSEG = L["KSEG"]
SEG52 = 0xFFF8000000000000          # SPE52: VA<63:50> = 0x3FFE [IDS p. 688]
CHIPS, TPUS = 2, 4
PROCS = [4 * p + t for p in range(CHIPS) for t in range(TPUS)]
SECS = PROCS[1:]
SPEED_MHZ = 800
WAIT_S = 1.0                        # a bound on each wait, in seconds of RPCC

PAL = {"halt": 0x00, "wripir": 0x0D, "wrval": 0x31, "rdval": 0x32,
       "wrent": 0x34, "swpipl": 0x35, "rdps": 0x36, "wrkgp": 0x37,
       "whami": 0x3C, "rti": 0x3F}

# The test's memory (PID 0, below 2 MB, N28).
MAIN_PA = 0x40000            # processor 0's results
INTREC_PA = 0x40800          # + w x 0x80: each processor's entInt record
SEC_PA = 0x41000             # + w x 0x400: each secondary's results
FLAGS_PA = 0x44000
READY_PA = FLAGS_PA          # + w x 8
WOKEN_PA = FLAGS_PA + 0x100  # + w x 8
WATCH_PA = FLAGS_PA + 0x200  # + w x 0x40: each one's watched line
AGAIN_PA = FLAGS_PA + 0x600
SUMS_PA = FLAGS_PA + 0x800
STACK_PA = 0x50000           # + w x 0x2000
STACK_TOP = 0x1FC0
L1_PA, L2_PA, L3_PA = 0x60000, 0x62000, 0x64000
D0_PA, C0_PA, C1_PA = 0x66000, 0x68000, 0x6A000
UC_PA, UD_PA = 0x6C000, 0x6E000      # user mode's code and data
VPTB = 0xFFFFFFFE00000000    # Linux's: level 1 entry 1023 maps the table
PROC_OFF, PROC_SIZE = 0x200, 0x280     # [ARM p. 630]
SLOT_PRESENT = 0x1CC         # PL, PMV, PV, PP, PA [ARM p. 638]

# Values to recognise.
SYSVAL0 = 0x1234567890ABCDEF
KGP0, KGP_BASE = 0x6B67700000000000, 0x6B67700000001000
SENT_A0, SENT_A1, SENT_A2, SENT_GP = 0x5EA0, 0x5EA1, 0x5EA2, 0x5E29
H25, H26, H27 = 0x2500, 0x2600, 0x2700
RESTART_DATA, AGAIN_DATA = 0x5ECD0000, 0xA6A10000
DATA0, SENT_ST, ITB_VALUE = 0xD0D0D0D012345678, 0x57A7E5, 0x56781234
DATA1 = 0xD1D1D1D187654321
USP_BASE, UNIQUE_BASE, PCC_STEP = 0x7700, 0x55000, 0x1000

MAIN_FIELDS = ("a0 at power-up", "a1 at power-up", "rdps at power-up",
               "whami", "rdval after wrval", "swpipl(0) old", "rdps",
               "swpipl(5) old", "rdps", "swpipl(0xff) old", "rdps",
               "R_INT_STAT<3:0> after wripir(0) at IPL 7",
               "interrupts at IPL 7", "interrupts at IPL 5",
               "r0 after the interrupt", "r16 after rti", "r17 after rti",
               "r18 after rti", "r29 after rti", "interrupts at IPL 4",
               "rdps after rti", "R_INT_STAT<3:0> after the interrupt",
               "a0 after halt", "a1 after halt", "rdps after halt",
               "RXRDY at the end") + tuple(
                   "R_INT_MASK of %d" % w for w in PROCS)
M = {name: k for k, name in enumerate(MAIN_FIELDS)}
INT_FIELDS = ("count", "a0", "a1", "a2", "GP", "frame PS", "frame PC",
              "frame GP", "frame a0", "frame a1", "frame a2", "rdps",
              "R_INT_STAT, own bit", "frame address")
SEC_FIELDS = ("R27", "R26", "R25", "SP", "whami", "rdps", "rdval",
              "rdval after wrval", "FP instruction ran", "PCC", "load",
              "store and load", "ITB fill call", "entMM count",
              "slot flags at the ack", "RXRDY at the ack",
              "user mode: its load", "user mode: its store",
              "SP back from user mode", "rdps back from user mode",
              "KSP kept by rti")
S = {name: k for k, name in enumerate(SEC_FIELDS)}
MMREC_OFF = 0x100
MM_FIELDS = ("a0 (VA)", "a1 (MMCSR)", "a2 (cause)", "frame PC", "frame PS",
             "GP", "frame a0-a2, GP", "a0-a2, GP after rti")
# The faults, in order: kind, VA, MMCSR, cause [ARM pp. 593-594, 606]. The
# last is user mode's (below).
FAULTS = (("ld", 0x20004000, 0, 0), ("st", 0x20004000, 0, 1),
          ("jsr", 0x20006000, 0, -1), ("jsr", 0x2000A000, 3, -1),
          ("ld", 0x20800000, 1, 0), ("ld", 0x21000000, 0, 0),
          ("jsr", 0x21800000, 1, -1), ("ld", 0x200000000, 1, 0),
          ("st", 0x22000000, 1, 1))
USER_CODE_VA, USER_DATA_VA = 0x20008000, 0x2000C000
USER_FAULT = ("user ld", 0x20004000, 0, 0)
NFAULTS = len(FAULTS) + 1
SLOT_FIELDS = ("flags", "halt PCBB", "halt PC", "halt PS", "halt R25",
               "halt R26", "halt R27", "halt reason")
IMP_FIELDS = ("entInt", "entArith", "entMM", "entIF", "entUna", "entSys",
              "KGP", "PTBR", "PCBB", "MCES", "sysvalue", "PS", "KSP", "USP",
              "WHAMI", "UNIQUE", "VPTB", "register window", "TPU bit")
AGAIN_FIELDS = ("R27", "whami", "SP", "flag")

GIO_CNFG, GIO_DATA, GIO_ADDR, CMM_TX = 0x100, 0x110, 0x120, 4


def kseg(pa):
    return KSEG | pa


def csr8(n):
    """PID n's register window, ipe8(n) | 0x7FFC << 20 (N18, N20)."""
    return (~n & 0x1FFF) << 35 | 0x7FFC << 20


def slot_pa(w):
    return L["HWRPB_PA"] + PROC_OFF + w * PROC_SIZE


def impure_pa(w):
    return L["PAL_IMPURE"] + w * L["IMPURE_SIZE"]


def pte(pa, bits):
    return (pa >> 13) << 32 | bits


V, FOE, KRE, KWE, URE, UWE = 1, 8, 0x100, 0x1000, 0x800, 0x8000


def page_tables():
    """Three levels for 43-bit VA and 8 KB pages, Linux's layout: level 1
    entry 1023 maps the table itself (VPTB) [ARM pp. 582-583]."""
    l1, l2, l3 = bytearray(0x2000), bytearray(0x2000), bytearray(0x2000)

    def put(t, i, v):
        struct.pack_into("<Q", t, 8 * i, v)
    put(l1, 0, pte(L2_PA, V | KRE | KWE))
    put(l1, 1023, pte(L1_PA, V | KRE | KWE))
    put(l1, 1, 0)                                # 0x200000000: invalid, KRE 0
    put(l2, 0x40, pte(L3_PA, V | KRE | KWE))     # 0x20000000
    put(l2, 0x41, 0)                             # 0x20800000: invalid, KRE 0
    put(l2, 0x42, KRE)                           # 0x21000000: invalid, KRE 1
    put(l2, 0x43, 0)                             # 0x21800000: invalid, KRE 0
    put(l2, 0x44, pte(L3_PA, V))                 # 0x22000000: valid, no KRE
    put(l3, 0, pte(D0_PA, V | KRE | KWE | URE | UWE))   # 0x20000000
    put(l3, 1, pte(C0_PA, V | KRE))              # 0x20002000: code
    put(l3, 2, 0)                                # 0x20004000: invalid
    put(l3, 3, 0)                                # 0x20006000: invalid
    put(l3, 5, pte(C1_PA, V | KRE | FOE))        # 0x2000A000: FOE
    put(l3, 4, pte(UC_PA, V | KRE | URE))        # 0x20008000: user code
    put(l3, 6, pte(UD_PA, V | KRE | KWE | URE | UWE))   # 0x2000C000
    return [(L1_PA, l1), (L2_PA, l2), (L3_PA, l3)]


def hwrpb():
    """What the console would have built (S12b): the fields the PALcode
    reads, and a slot per processor with its HWPCB [ARM pp. 617-639]."""
    h = bytearray(PROC_OFF + len(PROCS) * PROC_SIZE)

    def q(off, v):
        struct.pack_into("<Q", h, off, v)
    q(0x00, L["HWRPB_PA"])
    q(L["HWRPB_ID"], L["HWRPB_ID_VALUE"])
    q(0x10, 5)                                   # revision
    q(L["HWRPB_VPTB"], VPTB)
    q(L["HWRPB_NR_PROC"], len(PROCS))
    q(L["HWRPB_PROC_SIZE"], PROC_SIZE)
    q(L["HWRPB_PROC_OFF"], PROC_OFF)
    for w in PROCS:
        s = PROC_OFF + w * PROC_SIZE
        q(s + L["SLOT_FLAGS"], SLOT_PRESENT)
        if w:
            q(s + L["PCB_KSP"], kseg(STACK_PA + 0x2000 * w + STACK_TOP))
            q(s + L["PCB_USP"], USP_BASE + w)
            q(s + L["PCB_PTBR"], L1_PA >> 13)
            q(s + L["PCB_PCC_ASN"], w << 32 | PCC_STEP * w)
            q(s + L["PCB_UNIQUE"], UNIQUE_BASE + w)
            q(s + L["PCB_FLAGS"], 1)             # FEN
    return h


def li64(r, value):
    """Five words that load any 64-bit value: LDAH, LDA, SLL 32, LDAH, LDA,
    so that a label's KSEG address can be filled in after the layout."""
    value &= (1 << 64) - 1
    sval = value - (1 << 64) if value >> 63 else value
    lo = value & 0xFFFFFFFF
    lo_s = lo - (1 << 32) if lo >> 31 else lo
    hi = (sval - lo_s) >> 32

    def split(v):
        low = v & 0xFFFF
        low = low - 0x10000 if low >= 0x8000 else low
        high = (v - low) >> 16
        assert -0x8000 <= high < 0x8000, hex(value)
        return high, low
    hh, hl = split(hi)
    lh, ll = split(lo_s)
    return [mem(OP["LDAH"], r, R31, hh), mem(OP["LDA"], r, r, hl),
            A.opl(0x12, 0x39, r, 32, r), mem(OP["LDAH"], r, r, lh),
            mem(OP["LDA"], r, r, ll)]


def build():
    """The test program at CONSOLE_PA, run at its KSEG address."""
    a = Asm(L["CONSOLE_PA"])
    addr_fixups = []             # (word index, register, label)
    n_labels = [0]

    def addr_of(reg, label):
        """reg <- the label's KSEG address (filled in at the end)."""
        addr_fixups.append((len(a.words), reg, label))
        for _ in range(5):
            a.emit(0)

    def fresh(stem):
        n_labels[0] += 1
        return "%s_%d" % (stem, n_labels[0])

    def ins(name, ra, b, rc, lit=False):
        a.emit(A.ins_op(name, ra, b, rc, lit))

    def ldq(ra, rb, d):
        a.emit(mem(OP["LDQ"], ra, rb, d))

    def stq(ra, rb, d):
        a.emit(mem(OP["STQ"], ra, rb, d))

    def lda(ra, rb, d):
        a.emit(mem(OP["LDA"], ra, rb, d))

    def pal(name):
        a.emit(call_pal(PAL[name]))

    def mb():
        a.emit(mfc(A.MISC["MB"]))

    def bsr(link, label):
        a.branch(OP["BSR"], link, label)

    def ret(link):
        a.emit(A.jmp(2, R31, link))

    def deadline(seconds):
        """r6 <- RPCC + seconds of cycles (marvel8_mp.py's bound)."""
        cycles = int(seconds * SPEED_MHZ * 10 ** 6)
        lo = cycles & 0xFFFF
        lo = lo - 0x10000 if lo >= 0x8000 else lo
        a.emit(mfc(A.MISC["RPCC"], 6))
        a.emit(mem(OP["LDAH"], 6, 6, (cycles - lo) >> 16))
        lda(6, 6, lo)

    def until(top):
        a.emit(mfc(A.MISC["RPCC"], 7))
        ins("SUBL", 6, 7, 7)
        a.branch(OP["BGT"], 7, top)

    def wait_nonzero(va):
        top, out = fresh("wnz"), fresh("wnz_out")
        a.li(5, va)
        deadline(WAIT_S)
        a.label(top)
        ldq(1, 5, 0)
        a.branch(OP["BNE"], 1, out)
        until(top)
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

    def dump(va, count):
        a.li(16, va)
        a.li(17, count)
        bsr(26, "print_hex")

    def chip_window(dst, pid_reg, tmp):
        """dst <- SEG52 | PID's register window, from the PID in pid_reg."""
        ins("ORNOT", R31, pid_reg, dst)
        ins("SLL", dst, 51, dst, lit=True)
        ins("SRL", dst, 16, dst, lit=True)
        a.li(tmp, SEG52 | 0x7FFC << 20)
        ins("BIS", dst, tmp, dst)

    # Every register but a0-a2 and GP (rti's) and SP: r0-r15, r19-r28.
    SAVED = list(range(0, 16)) + list(range(19, 29))

    def save_all():
        lda(30, 30, -0x100)
        for k, r in enumerate(SAVED):
            stq(r, 30, 8 * k)

    def restore_all():
        for k, r in enumerate(SAVED):
            ldq(r, 30, 8 * k)
        lda(30, 30, 0x100)

    FRAME = 0x100                # the PALcode's frame, above the save area

    # The console's entry: the PALcode's RET lands here (KSEG).
    a.branch(OP["BR"], R31, "console")

    # --- putc: r16 to the CMM's terminal through GIO (r12 PID 0's window;
    # r8; link r27) ---
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

    # --- print_hex: r16 = VA, r17 = quadwords; link r26 ---
    a.label("print_hex")
    ins("BIS", 16, 16, 5)
    ins("BIS", 17, 17, 9)
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
    ins("SUBQ", 9, 1, 9, lit=True)
    a.branch(OP["BGT"], 9, "ph_loop")
    ret(26)

    # --- entInt, every processor: the record, then rti (the frame at SP +
    # FRAME after the save area) ---
    a.label("ent_int")
    save_all()
    pal("whami")
    ins("BIS", 0, 0, 9)
    a.li(1, kseg(INTREC_PA))
    ins("SLL", 9, 7, 2, lit=True)
    ins("ADDQ", 1, 2, 1)
    ldq(2, 1, 0)
    ins("ADDQ", 2, 1, 2, lit=True)
    stq(2, 1, 0)
    for k, r in ((1, 16), (2, 17), (3, 18), (4, 29)):
        stq(r, 1, 8 * k)
    for k, off in ((5, 0), (6, 8), (7, 16), (8, 24), (9, 32), (10, 40)):
        ldq(2, 30, FRAME + off)
        stq(2, 1, 8 * k)
    pal("rdps")
    stq(0, 1, 8 * 11)
    ins("SRL", 9, 2, 3, lit=True)                # the PID
    chip_window(4, 3, 5)
    ldq(2, 4, L["R_INT_STAT"])
    ins("AND", 9, 3, 3, lit=True)
    lda(5, R31, 1)
    ins("SLL", 5, 3, 5)
    ins("AND", 2, 5, 2)
    stq(2, 1, 8 * 12)
    lda(2, 30, FRAME)
    stq(2, 1, 8 * 13)
    restore_all()
    pal("rti")

    # --- entMM, the secondaries: a record, resume at the address the test
    # left in r23 ---
    a.label("ent_mm")
    save_all()
    pal("whami")
    a.li(1, kseg(SEC_PA))
    ins("SLL", 0, 10, 2, lit=True)
    ins("ADDQ", 1, 2, 1)
    ldq(2, 1, 8 * S["entMM count"])
    ins("ADDQ", 2, 1, 3, lit=True)
    stq(3, 1, 8 * S["entMM count"])
    ins("SLL", 2, 6, 2, lit=True)
    ins("ADDQ", 1, 2, 1)
    lda(1, 1, MMREC_OFF)
    stq(16, 1, 0)
    stq(17, 1, 8)
    stq(18, 1, 16)
    ldq(2, 30, FRAME + 8)
    stq(2, 1, 24)
    ldq(2, 30, FRAME)
    stq(2, 1, 32)
    stq(29, 1, 40)
    lda(4, R31, 1)
    for off, val in ((24, SENT_A0), (32, SENT_A1), (40, SENT_A2),
                     (16, SENT_GP)):
        ldq(2, 30, FRAME + off)
        a.li(3, val)
        ins("CMPEQ", 2, 3, 2)
        ins("AND", 4, 2, 4)
    stq(4, 1, 48)
    ldq(2, 30, 8 * SAVED.index(23))              # the test's r23
    stq(2, 30, FRAME + 8)
    ldq(2, 30, FRAME)                            # from user mode: back to the
    ins("AND", 2, 8, 3, lit=True)                # kernel at IPL 7
    a.branch(OP["BEQ"], 3, "mm_kernel")
    lda(2, R31, 7)
    stq(2, 30, FRAME)
    a.label("mm_kernel")
    restore_all()
    pal("rti")

    # --- a secondary, started by processor 0 through RXRDY ---
    a.label("sec_entry")
    stq(27, 30, -8)
    stq(26, 30, -16)
    stq(25, 30, -24)
    pal("whami")
    ins("BIS", 0, 0, 9)                          # r9 = WHAMI
    a.li(20, kseg(SEC_PA))
    ins("SLL", 9, 10, 1, lit=True)
    ins("ADDQ", 20, 1, 20)                       # r20 = its results
    for k, off in ((S["R27"], -8), (S["R26"], -16), (S["R25"], -24)):
        ldq(1, 30, off)
        stq(1, 20, 8 * k)
    stq(30, 20, 8 * S["SP"])
    stq(9, 20, 8 * S["whami"])
    pal("rdps")
    stq(0, 20, 8 * S["rdps"])
    pal("rdval")
    stq(0, 20, 8 * S["rdval"])
    a.li(16, 0xABC00)
    ins("ADDQ", 16, 9, 16)
    pal("wrval")
    pal("rdval")
    stq(0, 20, 8 * S["rdval after wrval"])
    a.emit(A.fop(0x17, 0x020, R31, R31, 1))      # CPYS: FEN from the HWPCB
    lda(1, R31, 1)
    stq(1, 20, 8 * S["FP instruction ran"])
    a.emit(mfc(A.MISC["RPCC"], 1))
    ins("SRL", 1, 32, 2, lit=True)
    ins("ADDQ", 1, 2, 2)
    ins("ZAPNOT", 2, 0x0F, 2, lit=True)          # PCC_CNT + PCC_OFF, 32 bits
    stq(2, 20, 8 * S["PCC"])
    addr_of(16, "ent_mm")
    lda(17, R31, 2)
    pal("wrent")
    addr_of(16, "ent_int")
    lda(17, R31, 0)
    pal("wrent")
    a.li(16, KGP_BASE)
    ins("ADDQ", 16, 9, 16)
    pal("wrkgp")
    # The fills: DTBM_SINGLE (and the VPTE page's DTBM_DOUBLE), ITB_MISS.
    a.li(10, 0x20000000)
    ldq(1, 10, 0)
    stq(1, 20, 8 * S["load"])
    a.li(1, SENT_ST)
    stq(1, 10, 8)
    ldq(2, 10, 8)
    stq(2, 20, 8 * S["store and load"])
    a.li(10, 0x20002000)
    a.emit(A.jmp(1, 26, 10))                     # JSR r26, (r10)
    stq(0, 20, 8 * S["ITB fill call"])
    # The faults: entMM resumes at r23.
    for k, (kind, va, _, _) in enumerate(FAULTS):
        resume = "resume_%d" % k
        a.li(10, va)
        a.li(16, SENT_A0)
        a.li(17, SENT_A1)
        a.li(18, SENT_A2)
        a.li(29, SENT_GP)
        addr_of(23, resume)
        a.label("fault_pc_%d" % k)
        if kind == "ld":
            ldq(1, 10, 0)
        elif kind == "st":
            stq(R31, 10, 0)
        else:
            a.emit(A.jmp(1, 26, 10))
        a.label(resume)
        lda(4, R31, 1)
        for r, val in ((16, SENT_A0), (17, SENT_A1), (18, SENT_A2),
                       (29, SENT_GP)):
            a.li(3, val)
            ins("CMPEQ", r, 3, 2)
            ins("AND", 4, 2, 4)
        stq(4, 20, MMREC_OFF + 0x40 * k + 56)
    # User mode: rti with a frame of PS 8 [ARM p. 559] into a user page,
    # which loads and stores through a page of its own (a DTB miss in user
    # mode) and then loads from an invalid page: entMM from user mode, on
    # the kernel stack rti kept; the handler returns to the kernel at r23.
    a.li(10, USER_FAULT[1])
    a.li(13, USER_DATA_VA)
    addr_of(23, "user_back")
    lda(30, 30, -48)
    for off, val in ((0, 8), (8, USER_CODE_VA), (16, SENT_GP), (24, SENT_A0),
                     (32, SENT_A1), (40, SENT_A2)):
        a.li(1, val)
        stq(1, 30, off)
    pal("rti")
    a.label("user_back")
    stq(30, 20, 8 * S["SP back from user mode"])
    pal("rdps")
    stq(0, 20, 8 * S["rdps back from user mode"])
    stq(11, 20, 8 * S["user mode: its load"])
    a.li(1, USER_DATA_VA)
    ldq(2, 1, 8)
    stq(2, 20, 8 * S["user mode: its store"])
    lda(4, R31, 1)
    for r, val in ((16, SENT_A0), (17, SENT_A1), (18, SENT_A2),
                   (29, SENT_GP)):
        a.li(3, val)
        ins("CMPEQ", r, 3, 2)
        ins("AND", 4, 2, 4)
    stq(4, 20, MMREC_OFF + 0x40 * len(FAULTS) + 56)
    a.li(1, kseg(L["PAL_IMPURE"] + L["IMP_KSP"]))
    ins("SLL", 9, L["IMPURE_SHIFT"], 2, lit=True)
    ins("ADDQ", 1, 2, 1)
    ldq(2, 1, 0)
    stq(2, 20, 8 * S["KSP kept by rti"])
    # Ready; then wait for an IPI at IPL 0 in QUIESCE (arm, test, QUIESCE
    # [IDS p. 670]).
    a.li(1, kseg(READY_PA))
    ins("S8ADDQ", 9, 1, 1)
    lda(2, R31, 1)
    stq(2, 1, 0)
    mb()
    a.li(21, kseg(INTREC_PA))
    ins("SLL", 9, 7, 1, lit=True)
    ins("ADDQ", 21, 1, 21)
    ldq(19, 21, 0)
    a.li(15, kseg(WATCH_PA))
    ins("SLL", 9, 6, 1, lit=True)
    ins("ADDQ", 15, 1, 15)
    lda(16, R31, 0)
    pal("swpipl")
    a.label("ipi_wait")
    a.emit(mfc(A.MISC["LDQ_ARM"], 2, 15))
    ldq(1, 21, 0)
    ins("SUBQ", 1, 19, 1)
    a.branch(OP["BNE"], 1, "ipi_woke")
    a.emit(mfc(A.MISC["QUIESCE"]))
    a.branch(OP["BR"], R31, "ipi_wait")
    a.label("ipi_wait_end")
    a.label("ipi_woke")
    a.li(1, kseg(WOKEN_PA))
    ins("S8ADDQ", 9, 1, 1)
    lda(2, R31, 1)
    stq(2, 1, 0)
    mb()
    # Halt at IPL 3, with R25-R27 to find in the slot.
    lda(16, R31, 3)
    pal("swpipl")
    a.li(25, H25)
    ins("ADDQ", 25, 9, 25)
    a.li(26, H26)
    ins("ADDQ", 26, 9, 26)
    a.li(27, H27)
    ins("ADDQ", 27, 9, 27)
    pal("halt")
    a.label("sec_halted")
    a.branch(OP["BR"], R31, "sec_halted")

    # --- a secondary started again after its halt ---
    a.label("sec_again")
    ins("BIS", 27, 27, 9)
    pal("whami")
    a.li(1, kseg(AGAIN_PA))
    stq(9, 1, 0)
    stq(0, 1, 8)
    stq(30, 1, 16)
    mb()
    lda(2, R31, 1)
    stq(2, 1, 24)
    mb()
    pal("halt")
    a.label("again_halted")
    a.branch(OP["BR"], R31, "again_halted")

    # --- processor 0: the console ---
    a.label("console")
    a.li(20, kseg(MAIN_PA))
    a.li(30, kseg(STACK_PA + STACK_TOP))
    a.li(12, SEG52 | csr8(0))
    a.branch(OP["BNE"], 16, "warm")

    def mstore(reg, field):
        stq(reg, 20, 8 * M[field])

    mstore(16, "a0 at power-up")
    mstore(17, "a1 at power-up")
    pal("rdps")
    mstore(0, "rdps at power-up")
    pal("whami")
    mstore(0, "whami")
    lda(1, R31, 2)                               # GIO_CNFG: DIVISOR 1
    stq(1, 12, GIO_CNFG)
    mb()
    print_text("\n@@PAL8\n")
    a.li(16, SYSVAL0)
    pal("wrval")
    pal("rdval")
    mstore(0, "rdval after wrval")
    for ipl, names in ((0, ("swpipl(0) old", 6)), (5, ("swpipl(5) old", 8)),
                       (0xFF, ("swpipl(0xff) old", 10))):
        lda(16, R31, ipl)
        pal("swpipl")
        mstore(0, names[0])
        pal("rdps")
        stq(0, 20, 8 * names[1])
    addr_of(16, "ent_int")
    lda(17, R31, 0)
    pal("wrent")
    addr_of(16, "ent_mm")
    lda(17, R31, 2)
    pal("wrent")
    a.li(16, 0xBAD0)
    lda(17, R31, 6)                              # no such entry: ignored
    pal("wrent")
    a.li(16, KGP0)
    pal("wrkgp")
    # An IPI to itself: pending at IPL 7 and 5, taken at 4.
    a.li(21, kseg(INTREC_PA))
    ldq(19, 21, 0)
    lda(16, R31, 0)
    pal("wripir")
    mb()
    delay(1 << 14)
    ldq(1, 12, L["R_INT_STAT"])
    ins("AND", 1, 0xF, 1, lit=True)
    mstore(1, "R_INT_STAT<3:0> after wripir(0) at IPL 7")
    ldq(1, 21, 0)
    ins("SUBQ", 1, 19, 1)
    mstore(1, "interrupts at IPL 7")
    lda(16, R31, 5)
    pal("swpipl")
    delay(1 << 14)
    ldq(1, 21, 0)
    ins("SUBQ", 1, 19, 1)
    mstore(1, "interrupts at IPL 5")
    a.li(17, SENT_A1)
    a.li(18, SENT_A2)
    a.li(29, SENT_GP)
    lda(16, R31, 4)
    pal("swpipl")
    a.label("after_swpipl4")
    mstore(0, "r0 after the interrupt")
    mstore(16, "r16 after rti")
    mstore(17, "r17 after rti")
    mstore(18, "r18 after rti")
    mstore(29, "r29 after rti")
    ldq(1, 21, 0)
    ins("SUBQ", 1, 19, 1)
    mstore(1, "interrupts at IPL 4")
    pal("rdps")
    mstore(0, "rdps after rti")
    ldq(1, 12, L["R_INT_STAT"])
    ins("AND", 1, 0xF, 1, lit=True)
    mstore(1, "R_INT_STAT<3:0> after the interrupt")
    lda(16, R31, 7)
    pal("swpipl")

    def start(w, entry_label, data):
        """START to processor w, as Linux's secondary_cpu_start does."""
        a.li(9, kseg(slot_pa(w)))
        a.li(2, kseg(L["HWRPB_PA"]))
        addr_of(1, entry_label)
        stq(1, 2, L["HWRPB_RESTART"])
        a.li(1, data)
        stq(1, 2, L["HWRPB_RESTART_DATA"])
        ldq(1, 9, L["SLOT_FLAGS"])
        ins("BIS", 1, 0x22, 1, lit=True)         # CV and RC
        ins("BIC", 1, 1, 1, lit=True)            # BIP clear
        stq(1, 9, L["SLOT_FLAGS"])
        lda(1, R31, 7)
        a.emit(mem(OP["STL"], 1, 9, L["SLOT_RXLEN"]))
        a.li(1, L["START_CMD"])
        stq(1, 9, L["SLOT_RXBUF"])
        mb()
        top = fresh("setbit")
        a.label(top)                             # LDQ_L/STQ_C [ARM p. 694]
        a.emit(mem(OP["LDQ_L"], 1, 2, L["HWRPB_RXRDY"]))
        ins("BIS", 1, 1 << w, 1, lit=True)
        a.emit(mem(OP["STQ_C"], 1, 2, L["HWRPB_RXRDY"]))
        a.branch(OP["BEQ"], 1, top)
        mb()
        top, out = fresh("ack"), fresh("ack_out")
        deadline(WAIT_S)
        a.label(top)
        ldq(1, 9, L["SLOT_FLAGS"])
        a.branch(OP["BLBS"], 1, out)
        until(top)
        a.label(out)

    for w in SECS:
        start(w, "sec_entry", RESTART_DATA | w)
        a.li(3, kseg(SEC_PA + 0x400 * w))
        ldq(1, 9, L["SLOT_FLAGS"])
        stq(1, 3, 8 * S["slot flags at the ack"])
        ldq(1, 2, L["HWRPB_RXRDY"])
        stq(1, 3, 8 * S["RXRDY at the ack"])
        wait_nonzero(kseg(READY_PA + 8 * w))
    # An IPI to each in turn: it wakes, and no other.
    for k, w in enumerate(SECS):
        lda(16, R31, w)
        pal("wripir")
        wait_nonzero(kseg(WOKEN_PA + 8 * w))
        delay(1 << 14)
        a.li(3, kseg(WOKEN_PA))
        lda(4, R31, 0)
        for v in SECS:
            ldq(1, 3, 8 * v)
            ins("ADDQ", 4, 1, 4)
        a.li(3, kseg(SUMS_PA + 8 * k))
        stq(4, 3, 0)
    # Each halts: reason 5 in its slot.
    for w in SECS:
        top, out = fresh("halted"), fresh("halted_out")
        a.li(9, kseg(slot_pa(w)))
        deadline(WAIT_S)
        a.label(top)
        ldq(1, 9, L["SLOT_HALT_REASON"])
        ins("CMPEQ", 1, 5, 1, lit=True)
        a.branch(OP["BNE"], 1, out)
        until(top)
        a.label(out)
    # Processor 1 again, from its parking after the halt.
    start(1, "sec_again", AGAIN_DATA)
    wait_nonzero(kseg(AGAIN_PA + 24))
    top, out = fresh("again_halt"), fresh("again_halt_out")
    a.li(9, kseg(slot_pa(1)))
    addr_of(3, "again_halted")
    deadline(WAIT_S)
    a.label(top)
    ldq(1, 9, L["SLOT_HALT_PC"])
    ins("CMPEQ", 1, 3, 1)
    a.branch(OP["BNE"], 1, out)
    until(top)
    a.label(out)
    # Processor 0 halts: back to the console, a0 = 5.
    a.li(25, H25)
    a.li(26, H26)
    a.li(27, H27)
    pal("halt")
    a.label("after_halt0")
    a.branch(OP["BR"], R31, "after_halt0")

    a.label("warm")
    mstore(16, "a0 after halt")
    mstore(17, "a1 after halt")
    pal("rdps")
    mstore(0, "rdps after halt")
    a.li(2, kseg(L["HWRPB_PA"]))
    ldq(1, 2, L["HWRPB_RXRDY"])
    mstore(1, "RXRDY at the end")
    # Each TPU's R_INT_MASK (N22), PID p's window + 0x3F0000 + 0x40 x t:
    # read now, when every TPU has run (a TPU's reset may come late on a
    # loaded host).
    for w in PROCS:
        p, t = divmod(w, 4)
        a.li(3, SEG52 | csr8(p) | L["R_INT_MASK_TPU"] + 0x40 * t)
        ldq(1, 3, 0)
        mstore(1, "R_INT_MASK of %d" % w)
    print_text("\n@@BEGIN\n")
    dump(kseg(MAIN_PA), len(MAIN_FIELDS))
    print_text("@@\n")
    dump(kseg(INTREC_PA), len(INT_FIELDS))
    print_text("@@\n")
    dump(kseg(SUMS_PA), len(SECS))
    print_text("@@\n")
    dump(kseg(AGAIN_PA), len(AGAIN_FIELDS))
    for w in SECS:
        print_text("@@\n")
        dump(kseg(SEC_PA + 0x400 * w), MMREC_OFF // 8 + 8 * NFAULTS)
        print_text("@@\n")
        dump(kseg(INTREC_PA + 0x80 * w), len(INT_FIELDS))
    for w in PROCS:
        print_text("@@\n")
        dump(kseg(slot_pa(w) + L["SLOT_FLAGS"]), 1)
        dump(kseg(slot_pa(w) + L["SLOT_HALT_PCBB"]), len(SLOT_FIELDS) - 1)
    for w in PROCS:
        print_text("@@\n")
        dump(kseg(impure_pa(w)), len(IMP_FIELDS))
    print_text("@@END\n")
    a.label("done")
    a.branch(OP["BR"], R31, "done")

    words = a.finish()
    for i, reg, label in addr_fixups:
        words[i:i + 5] = li64(reg, kseg(a.labels[label]))
    assert len(words) * 4 + L["CONSOLE_PA"] <= MAIN_PA
    code = b"".join(struct.pack("<I", w) for w in words)
    lab = {name: kseg(pa) for name, pa in a.labels.items()}
    info = {"ent_int": lab["ent_int"], "ent_mm": lab["ent_mm"],
            "after_swpipl4": lab["after_swpipl4"],
            "after_halt0": lab["after_halt0"],
            "sec_halted": lab["sec_halted"],
            "again_halted": lab["again_halted"],
            "ipi_wait": (lab["ipi_wait"], lab["ipi_wait_end"]),
            "fault_pc": [lab["fault_pc_%d" % k] for k in range(len(FAULTS))]}
    return code, info


def mapped_pages():
    """The data page and the two code pages the page tables map."""
    d0 = bytearray(0x2000)
    struct.pack_into("<Q", d0, 0, DATA0)
    c0 = bytearray(0x2000)
    for i, w in enumerate((A.mem(OP["LDA"], 0, R31, ITB_VALUE & 0xFFFF),
                           A.mem(OP["LDAH"], 0, 0, ITB_VALUE >> 16),
                           A.jmp(2, R31, 26))):
        struct.pack_into("<I", c0, 4 * i, w)
    # User mode: a load and a store through a page of its own (r13), then a
    # load from an invalid page (r10), entMM from user mode.
    uc = bytearray(0x2000)
    for i, w in enumerate((A.mem(OP["LDQ"], 11, 13, 0),
                           A.mem(OP["STQ"], 11, 13, 8),
                           A.mem(OP["LDQ"], 1, 10, 0),
                           A.bra(OP["BR"], R31, -1))):
        struct.pack_into("<I", uc, 4 * i, w)
    ud = bytearray(0x2000)
    struct.pack_into("<Q", ud, 0, DATA1)
    return [(D0_PA, d0), (C0_PA, c0), (C1_PA, bytes(c0)), (UC_PA, uc),
            (UD_PA, ud)]


CFG = """sys0 = tsunami
{
  platform = "es47";
  memory.bits = 29;
  rom.srm = "absent.exe";
  rom.decompressed = "pal8.rom";
  rom.nvram = "cmm_nvram.bin";
%(cpus)s
  serial0 = serial { address = "127.0.0.1"; port = %(port)d; }
}
"""


def config(port):
    cpus = "\n".join("  cpu%d = ev8n { speed = %dM; tpus = %d; }"
                     % (c, SPEED_MHZ, TPUS) for c in range(CHIPS))
    return CFG % {"cpus": cpus, "port": port}


def run(binary, workdir, port, timeout):
    """Boot; read the CMM terminal until @@END; stop our own emulator."""
    with open(os.path.join(workdir, "es40.cfg"), "w") as f:
        f.write(config(port))
    out = open(os.path.join(workdir, "alphabox.out"), "wb")
    t0 = time.time()
    proc = subprocess.Popen([binary, "run"], cwd=workdir, stdout=out,
                            stderr=subprocess.STDOUT)
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
            while b"@@END" not in data and time.time() < deadline:
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    if proc.poll() is not None:
                        break
                    continue
                if not chunk:
                    break
                data += chunk
            sock.close()
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)     # our own emulator only
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        out.close()
    with open(os.path.join(workdir, "alphabox.out"), "rb") as f:
        emu = f.read().decode("latin-1")
    return data.decode("latin-1"), emu, time.time() - t0


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


def check(bufs, info, emu):
    """Every result against what the ARM and the IDS say it must be."""
    passes, fails = 0, []

    def ok(what, got, want=None, good=None):
        nonlocal passes
        if good is None:
            good = got == want
        if good:
            passes += 1
        else:
            fails.append("%s: %s, want %s" % (
                what, "%#x" % got if isinstance(got, int) else got,
                "%#x" % want if isinstance(want, int) else want))

    main, int0, sums, again = bufs[0], bufs[1], bufs[2], bufs[3]
    rest = bufs[4:]
    sec = {w: (rest[2 * k], rest[2 * k + 1]) for k, w in enumerate(SECS)}
    rest = rest[2 * len(SECS):]
    slots = {w: rest[k] for k, w in enumerate(PROCS)}
    imps = {w: rest[len(PROCS) + k] for k, w in enumerate(PROCS)}

    want_main = {
        "a0 at power-up": 0, "a1 at power-up": 0, "rdps at power-up": 7,
        "whami": 0, "rdval after wrval": SYSVAL0, "swpipl(0) old": 7,
        "swpipl(5) old": 0, "swpipl(0xff) old": 5,
        "R_INT_STAT<3:0> after wripir(0) at IPL 7": 1,
        "interrupts at IPL 7": 0, "interrupts at IPL 5": 0,
        "r0 after the interrupt": 5, "r16 after rti": 4,
        "r17 after rti": SENT_A1, "r18 after rti": SENT_A2,
        "r29 after rti": SENT_GP, "interrupts at IPL 4": 1,
        "rdps after rti": 4, "R_INT_STAT<3:0> after the interrupt": 0,
        "a0 after halt": 5, "a1 after halt": 0, "rdps after halt": 7,
        "RXRDY at the end": 0}
    want_main.update({"R_INT_MASK of %d" % w: 1 << (w & 3) for w in PROCS})
    for k, (ipl, name) in enumerate(((0, "rdps"), (5, "rdps"), (7, "rdps"))):
        idx = 6 + 2 * k
        ok("processor 0: rdps after swpipl(%d)" % ipl, main[idx], ipl)
    for name, k in M.items():
        if name in want_main:
            ok("processor 0: " + name, main[k], want_main[name])
    top0 = kseg(STACK_PA + STACK_TOP)
    want_int0 = {"count": 1, "a0": 0, "a1": 0, "a2": 0, "GP": KGP0,
                 "frame PS": 4, "frame PC": info["after_swpipl4"],
                 "frame GP": SENT_GP, "frame a0": 4, "frame a1": SENT_A1,
                 "frame a2": SENT_A2, "rdps": 5, "R_INT_STAT, own bit": 0,
                 "frame address": top0 - 48}
    for k, name in enumerate(INT_FIELDS):
        ok("processor 0 entInt: " + name, int0[k], want_int0[name])
    for k, w in enumerate(SECS):
        ok("TPUs woken after the IPI to %d" % w, sums[k], k + 1)
    for k, name in enumerate(AGAIN_FIELDS):
        want = {"R27": AGAIN_DATA, "whami": 1,
                "SP": kseg(STACK_PA + 0x2000 + STACK_TOP), "flag": 1}[name]
        ok("processor 1 started again: " + name, again[k], want)

    for w in SECS:
        res, intr = sec[w]
        top = kseg(STACK_PA + 0x2000 * w + STACK_TOP)
        want = {"R27": RESTART_DATA | w, "R26": 0, "R25": 0, "SP": top,
                "whami": w, "rdps": 7, "rdval": 0,
                "rdval after wrval": 0xABC00 + w, "FP instruction ran": 1,
                "load": DATA0, "store and load": SENT_ST,
                "ITB fill call": ITB_VALUE, "entMM count": NFAULTS,
                "slot flags at the ack": (SLOT_PRESENT | 0x20 | 1),
                "RXRDY at the ack": 0, "user mode: its load": DATA1,
                "user mode: its store": DATA1, "SP back from user mode": top,
                "rdps back from user mode": 7, "KSP kept by rti": top}
        for k, name in enumerate(SEC_FIELDS):
            if name == "PCC":
                d = (res[k] - PCC_STEP * w) & 0xFFFFFFFF
                ok("processor %d: PCC (count + offset - the HWPCB's)" % w,
                   d, "under 2^31", good=d < 1 << 31)
            else:
                ok("processor %d: %s" % (w, name), res[k], want[name])
        for f, (kind, va, mmcsr, cause) in enumerate(FAULTS + (USER_FAULT,)):
            rec = res[MMREC_OFF // 8 + 8 * f:MMREC_OFF // 8 + 8 * f + 8]
            if kind == "user ld":
                pc, ps = USER_CODE_VA + 8, 8
            else:
                pc = va if kind == "jsr" else info["fault_pc"][f]
                ps = 7
            want = (va, mmcsr, cause & (1 << 64) - 1, pc, ps, KGP_BASE + w,
                    1, 1)
            for i, name in enumerate(MM_FIELDS):
                ok("processor %d, entMM %d (%s %#x): %s"
                   % (w, f, kind, va, name), rec[i], want[i])
        lo, hi = info["ipi_wait"]
        want_int = {"count": 1, "a0": 0, "a1": 0, "a2": 0,
                    "GP": KGP_BASE + w, "frame PS": 0, "rdps": 5,
                    "R_INT_STAT, own bit": 0, "frame address": top - 48}
        for k, name in enumerate(INT_FIELDS):
            if name == "frame PC":
                ok("processor %d entInt: frame PC in the wait" % w, intr[k],
                   "%#x-%#x" % (lo, hi), good=lo <= intr[k] <= hi)
            elif name in want_int:
                ok("processor %d entInt: %s" % (w, name), intr[k],
                   want_int[name])

    for w in PROCS:
        s = slots[w]
        if w == 0:
            want = (SLOT_PRESENT, impure_pa(0) + L["IMP_CPCB"],
                    info["after_halt0"], 7, H25, H26, H27, 5)
        elif w == 1:
            want = (SLOT_PRESENT | 0x20 | 1, slot_pa(1), info["again_halted"],
                    7, 0, 0, AGAIN_DATA, 5)
        else:
            want = (SLOT_PRESENT | 0x20 | 1, slot_pa(w), info["sec_halted"],
                    3, H25 + w, H26 + w, H27 + w, 5)
        for k, name in enumerate(SLOT_FIELDS):
            ok("slot %d: %s" % (w, name), s[k], want[k])

    for w in PROCS:
        m = imps[w]
        p, t = divmod(w, 4)
        want = {"entInt": info["ent_int"], "entArith": 0,
                "entMM": info["ent_mm"], "entIF": 0, "entUna": 0,
                "entSys": 0, "KGP": KGP0 if w == 0 else KGP_BASE + w,
                "PTBR": 0 if w == 0 else L1_PA >> 13,
                "PCBB": impure_pa(0) + L["IMP_CPCB"] if w == 0 else
                slot_pa(w), "MCES": 0 if w == 0 else 8,
                "sysvalue": SYSVAL0 if w == 0 else 0xABC00 + w, "PS": 7,
                "KSP": 0 if w == 0 else
                kseg(STACK_PA + 0x2000 * w + STACK_TOP),
                "USP": 0 if w == 0 else USP_BASE + w, "WHAMI": w,
                "UNIQUE": 0 if w == 0 else UNIQUE_BASE + w,
                "VPTB": 0 if w == 0 else VPTB, "register window": csr8(p),
                "TPU bit": 1 << t}
        for k, name in enumerate(IMP_FIELDS):
            ok("impure area of %d: %s" % (w, name), m[k], want[name])

    started = sum(1 for ln in emu.splitlines() if "*** STARTING ***" in ln)
    ok("processors started at reset", started, len(PROCS))
    mism = [ln for ln in emu.splitlines() if "MISMATCH" in ln]
    ok("JIT_VERIFY MISMATCH lines", len(mism), 0)
    return passes, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="pal8")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=float, default=180)
    ap.add_argument("--nada", default=None)
    args = ap.parse_args()
    work = os.environ.get("ALPHABOX_WORK", os.path.join(REPO, "lab"))
    work = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    binary = os.path.abspath(args.binary)
    port = args.port or cpu_oracle.free_port()

    pal, _ = fw.build_pal(args.nada)
    code, info = build()
    img = fw.image(pal, L, code,
                   extra=page_tables() + mapped_pages() +
                   [(L["HWRPB_PA"], hwrpb())])
    fw.write_rom(os.path.join(work, "pal8.rom"), img, L)
    text, emu, secs = run(binary, work, port, args.timeout)
    with open(os.path.join(work, "serial.txt"), "w") as f:
        f.write(text)
    print("== the EV8 PALcode on the ES47, 2 chips of 4 TPUs: PALcode %#x "
          "bytes, test program %#x, %.1f s" % (len(pal), len(code), secs))
    bufs = parse(text)
    want_bufs = 4 + 2 * len(SECS) + 2 * len(PROCS)
    if not bufs or len(bufs) != want_bufs:
        print("  no results (%s blocks; see %s)"
              % (len(bufs) if bufs else 0, work))
        for ln in (text.splitlines() + emu.splitlines())[-15:]:
            print("   |", ln)
        print("  PAL8: FAIL")
        return 1
    passes, fails = check(bufs, info, emu)
    for f in fails:
        print("  FAIL " + f)
    vlines = [ln for ln in emu.splitlines() if "compiled-block execs" in ln]
    if vlines:
        print("  %s" % vlines[-1].strip())
    print("  PAL8: %s (PASS %d, FAIL %d)" % ("PASS" if not fails else "FAIL",
                                             passes, len(fails)))
    if not fails and not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("  run directory: %s" % work)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
