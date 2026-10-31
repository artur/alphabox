#!/usr/bin/env python3
"""The Alphabox PALcode for the native 21464 (firmware/ev8, S12a) on the
ES47 in 21464 mode: 2 chips of 4 `ev8n` TPUs, booted from a
`rom.decompressed` holding the PALcode (built by firmware/ev8/build.py with
nada) and a test program in the console's place (docs/platforms/ev8-native.md,
sections 3 and S12a; docs/development.md, "The oracle").

    pal8_test.py <alphabox-binary> [label] [--port N] [--keep] [--nada PATH]

Every TPU resets at PAL_BASE + 0xB00; processor 0 (PID 0, TPU 0) enters the
test program as its console, the seven others park on the HWRPB's RXRDY
(N17). What is checked, written from the ARM's Alpha Linux PALcode
chapters and the IDS before the code (ARM and IDS pages as in
ev8-native.md); every OS entry the test takes is logged, per processor,
with its a0 to a2, the frame's PC and PS, GP and the PS inside:

  processor 0     the console's entry (a0 0 at power-up, a1 its WHAMI), PS 7
                  by rdps, whami [ARM p. 566], wrval/rdval [pp. 557, 576],
                  swpipl and rdps [pp. 555, 562], wrent of the six entries
                  (a1 = 6 ignored) [p. 568], wrkgp [p. 571]; an IPI to
                  itself pending at IPL 7 and 5, taken at 4 (entInt type 0
                  at IPL 5, the frame, rti [p. 559]).
  the CALL_PALs   cflush, draina, swppal (1, N24), wrperfmon (0), rdmces and
                  wrmces [pp. 554, 572], wrusp and rdusp, wrunique and
                  rdunique, CALL_PAL 0x3B (N8: v0 the old VCTL, VEN a0<0>),
                  cserve 0x49 and 0x48 (the TOY through the CMM's byte window:
                  register B, the year and month, an NVRAM byte written and
                  read, a bad index, an unknown function), wrfen, clrfen,
                  wrvptptr, swpctx [p. 560] there and back (the old PCB's
                  KSP, USP, PCC and UNIQUE, the new SP, USP, UNIQUE, PCC,
                  PTBR, FEN, the ASN kept; TB entries of one ASN missed in
                  the other), tbi -2, -1 (an ASM entry kept), 1
                  (with imb), 2, 3 [p. 565], imb after rewriting code, wtint
                  [p. 579] idle and with an IPI pending, retsys and callsys
                  from user mode [pp. 558, 607], bpt, bugchk, gentrap, callsys
                  from kernel mode (bugchk), an unassigned code and a
                  privileged one from user mode (opDec).
  the entries     entIF [p. 605]: OPCDEC (an IFETCHB word in normal mode),
                  FEN (from kernel and user mode, re-executed after wrfen),
                  Tarantula's OPCDEC with VEN off, enabled lazily with 0x3B;
                  entArith [p. 602]: ADDQ/V, DIVT and DIVT/SU by zero (the
                  summary and the register mask); entUna [p. 607]: LDQ and
                  STL unaligned across a page (VA, opcode, register, PC the
                  next); entMM: a bad VA load (DFAULT), a jump to one
                  (BAD_JUMP_IVA), DTB ACV, FOR and FOW, user-mode ACV on
                  KSEG and a kernel page,
                  an instruction fetch's ACV (IACV), and S12a phase 1's TB
                  fills and faults; entInt: the interval timer (type 1, IPL
                  5, each tick cleared), the interrupt queue (type 3, IPL 4,
                  the vector Linux decodes from the IID, for an LSI and an
                  MSI) and a sym53c810's interrupt sent again by the IO7 after
                  the PALcode's end of interrupt as the IPL drops.
  starting        each secondary as Linux starts one (smp.c; ARM pp. 693-694,
                  725-726), the slot's PALcode fields [pp. 632-633].
  halt            CALL_PAL HALT (reason 5), a halt request in R_INT_REQ
                  (reason 1, OH), a kernel stack not quadword aligned (reason
                  2, with the PALcode's message on the terminal) [pp. 607,
                  635]; the HWPCB saved in the slot; one processor started
                  again; processor 0 back to the console with a0 = 5.
  the impure areas  (white box) each TPU's entries, KGP, PTBR, PCBB, MCES,
                  sysvalue, PS, KSP, USP, WHAMI = PID x 4 + TPU (N25), UNIQUE,
                  VPTB, its chip's register window (N18) and TPU bit; each
                  TPU's R_INT_MASK (N22).

The emulator runs with ALPHABOX_TRACE_QUIESCE=1, whose count shows that
processor 0's idle wtint slept to its timeout (docs/headless.md).

Run output goes to $ALPHABOX_WORK/runs/<label> (kept on failure or --keep).
Exit status 0 when everything holds.
"""
import argparse
import os
import re
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
W_HREQ, W_KSNV = 6, 7               # the secondaries that halt otherwise
SPEED_MHZ = 800
WAIT_S = 1.0                        # a bound on each wait, in seconds of RPCC
WTINT_LONG = 0xF0000                # an idle wtint that lasted the timeout
WTINT_SLEEPS = 32                   # (0xFFFF0 cycles, QT_WTINT), this often

PAL = {"halt": 0x00, "cflush": 0x01, "draina": 0x02, "cserve": 0x09,
       "swppal": 0x0A, "wripir": 0x0D, "rdmces": 0x10, "wrmces": 0x11,
       "wrfen": 0x2B, "wrvptptr": 0x2D, "swpctx": 0x30, "wrval": 0x31,
       "rdval": 0x32, "tbi": 0x33, "wrent": 0x34, "swpipl": 0x35,
       "rdps": 0x36, "wrkgp": 0x37, "wrusp": 0x38, "wrperfmon": 0x39,
       "rdusp": 0x3A, "vctl": 0x3B, "whami": 0x3C, "retsys": 0x3D,
       "wtint": 0x3E, "rti": 0x3F, "bpt": 0x80, "bugchk": 0x81,
       "callsys": 0x83, "imb": 0x86, "rdunique": 0x9E, "wrunique": 0x9F,
       "gentrap": 0xAA, "clrfen": 0xAE}

# The test's memory (PID 0, below 2 MB, N28).
MAIN_PA = 0x40000            # processor 0's results
INTREC_PA = 0x40800          # + w x 0x80: each processor's last entInt
MAIN2_PA = 0x41000           # processor 0's phase 2 results
FLAGS_PA = 0x44000
READY_PA = FLAGS_PA          # + w x 8
WOKEN_PA = FLAGS_PA + 0x100  # + w x 8
WATCH_PA = FLAGS_PA + 0x200  # + w x 0x40: each one's watched line
AGAIN_PA = FLAGS_PA + 0x600
SUMS_PA = FLAGS_PA + 0x800
TICKS_PA = FLAGS_PA + 0x900  # timer interrupts counted by entInt
DEVSYM_PA = FLAGS_PA + 0x940  # the sym53c810's interrupts
P1_PCB_PA = 0x45000          # processor 0's second context
UNA_PA = 0x47FF8             # the unaligned cases' data, across a page
CODEBUF_PA = 0x46000         # code rewritten for imb
P1_STACK_TOP = 0x49FC0
STACK_PA = 0x50000           # + w x 0x2000
STACK_TOP = 0x1FC0
L1_PA, L2_PA, L3_PA = 0x60000, 0x62000, 0x64000
D0_PA, C0_PA, C1_PA = 0x66000, 0x68000, 0x6A000
UC_PA, UD_PA = 0x6C000, 0x6E000      # user mode's code and data
LOG_PA = 0x70000             # + w x 0x1000: each processor's entries
T1_PA, T2_PA, A1_PA, A2_PA = 0x78000, 0x7A000, 0x7C000, 0x7E000
CX_PA, CY_PA, RO_PA, FORP_PA = 0x80000, 0x82000, 0x84000, 0x86000
FOWP_PA, KD_PA = 0x88000, 0x8A000
SEC_PA = 0x90000             # + w x 0x800: each secondary's results
SCRIPTS_PA = 0x98000         # the sym53c810's program
VPTB = 0xFFFFFFFE00000000    # Linux's: level 1 entry 1023 maps the table
PROC_OFF, PROC_SIZE = 0x200, 0x280     # [ARM p. 630]
SLOT_PRESENT = 0x1CC         # PL, PMV, PV, PP, PA [ARM p. 638]

# The pages through level 3 entry n, VA 0x20000000 + n x 0x2000.
VA_T, VA_A, VA_C = 0x20010000, 0x20012000, 0x20014000      # 8, 9, 10
VA_RO, VA_FOR, VA_FOW, VA_KD = 0x20016000, 0x20018000, 0x2001A000, 0x2001C000
BAD_VA = 0x0008000000000000  # not sign-extended from VA<42> [IDS p. 695]

# Values to recognise.
SYSVAL0 = 0x1234567890ABCDEF
KGP0, KGP_BASE = 0x6B67700000000000, 0x6B67700000001000
SENT_A0, SENT_A1, SENT_A2, SENT_GP = 0x5EA0, 0x5EA1, 0x5EA2, 0x5E29
H25, H26, H27 = 0x2500, 0x2600, 0x2700
RESTART_DATA, AGAIN_DATA = 0x5ECD0000, 0xA6A10000
DATA0, SENT_ST, ITB_VALUE = 0xD0D0D0D012345678, 0x57A7E5, 0x56781234
DATA1 = 0xD1D1D1D187654321
USP_BASE, UNIQUE_BASE, PCC_STEP = 0x7700, 0x55000, 0x1000
USP0, UNIQUE0 = 0x1111, 0x2222               # processor 0's console context
P1_USP, P1_UNIQUE, P1_PCC, P1_ASN = 0x1234, 0xC0FFEE, 0x40000000, 5
P1_USP2, P1_UNIQUE2 = 0x5678, 0xBEEF
LAZY = 0x7EC                 # entIF: the vector unit enabled lazily (r23)
SYS_RET = 0x5C5              # what entSys returns in v0
U_A0, U_A1, U_A2 = 0x1601, 0x1611, 0x1621  # callsys's arguments
T1V, T2V, A1V, A2V = 0x7171, 0x7272, 0xA1A1, 0xA2A2
CXV, CYV = 0xC0DE1, 0xC0DE2
NV_INDEX, NV_VALUE = 40, 0x5A
SYM_SLOT = 1                 # pci0.1: hose 0 (PID 0's IO7 port 0), slot 1
SYM_BAR = 0x10000000
SYM_LSI = 0 << 5 | SYM_SLOT << 2 | 0    # port 0, slot 1, INTA (Io7.hpp)
LSI_IID, MSI_IID = 0x23, 1 << 13 | 5    # IIDs of PE 0 written to the queue
PAL_REVISION = 0x0020000100020100  # layout.h's PAL_REVISION

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
MAIN2_FIELDS = (
    "swppal(0)", "swppal(0x8000)", "wrperfmon", "rdmces first",
    "rdmces after wrmces(0x18)", "rdmces after wrmces(7)",
    "rdusp after wrusp", "rdunique after wrunique", "0x3B(1) old",
    "0x3B(0) old", "0x3B(3) old", "0x3B(0) old after 3",
    "GET_TOY(11)", "PUT_TOY(40) v0", "GET_TOY(40)", "GET_TOY(9)",
    "GET_TOY(8)", "cserve 0x47", "GET_TOY(0x10c)", "GET_TOY(-1)",
    "PUT_TOY(0x10b) v0", "GET_TOY(0x10b)", "PCB FEN after wrfen(0)",
    "PCB FEN after the FEN fault", "FP after the FEN fault", "0x3B old in the lazy handler", "getvl ran",
    "0x3B(0) old after the lazy enable", "r1 kept by the unaligned LDQ",
    "r26 kept by the bad jump", "swpctx v0", "SP in P1", "rdusp in P1",
    "rdunique in P1", "PCC in P1", "PTBR in P1", "PCBB in P1",
    "tbi: load", "tbi: load after the PTE changed", "tbi: after tbi 2",
    "tbi: after the PTE restored", "tbi: after tbi -2", "ASM: load",
    "ASM: after the PTE changed and tbi -1", "ASM: after tbi -2",
    "ITB: call", "ITB: call after the PTE changed",
    "ITB: call after tbi 1 and imb", "tbi 3: load", "imb: first code",
    "imb: rewritten code", "ASN: load in P1", "ASN: call in P1",
    "swpctx back v0", "ASN: load back", "ASN: call back", "P1 PCB KSP", "P1 PCB USP",
    "P1 PCB PCC", "P1 PCB ASN", "P1 PCB UNIQUE", "SP back", "rdusp back",
    "rdunique back", "PTBR back", "wtint idle v0", "wtint idle cycles",
    "wtint with an IPI v0", "interrupts after wtint", "timer ticks",
    "R_INTER_TIM after the ticks", "R_INT_STAT<7:4> after the ticks",
    "queue bit with two IIDs", "queue interrupts", "sym interrupts",
    "sym ISTAT after", "queue bit at the end")
M2 = {name: k for k, name in enumerate(MAIN2_FIELDS)}
INT_FIELDS = ("count", "a0", "a1", "a2", "GP", "frame PS", "frame PC",
              "frame GP", "frame a0", "frame a1", "frame a2", "rdps",
              "R_INT_STAT, own bit", "frame address")
SEC_FIELDS = ("R27", "R26", "R25", "SP", "whami", "rdps", "rdval",
              "rdval after wrval", "FP instruction ran", "PCC", "load",
              "store and load", "ITB fill call", "entMM count",
              "slot flags at the ack", "RXRDY at the ack",
              "user mode: its load", "user mode: its store",
              "SP back from user mode", "rdps back from user mode",
              "KSP kept by rti", "user mode: callsys's v0",
              "user mode: rdunique", "user mode: FP after the FEN fault")
S = {name: k for k, name in enumerate(SEC_FIELDS)}
MMREC_OFF = 0x100
MM_FIELDS = ("a0 (VA)", "a1 (MMCSR)", "a2 (cause)", "frame PC", "frame PS",
             "GP", "frame a0-a2, GP", "a0-a2, GP after rti")
# The kernel's faults, in order: kind, VA, MMCSR, cause [ARM pp. 593-594,
# 606]: TNV 0, ACV 1, FOR 2, FOE 3, FOW 4.
FAULTS = (("ld", 0x20004000, 0, 0), ("st", 0x20004000, 0, 1),
          ("jsr", 0x20006000, 0, -1), ("jsr", 0x2000A000, 3, -1),
          ("ld", 0x20800000, 1, 0), ("ld", 0x21000000, 0, 0),
          ("jsr", 0x21800000, 1, -1), ("ld", 0x200000000, 1, 0),
          ("st", 0x22000000, 1, 1), ("st", VA_RO, 1, 1),
          ("ld", VA_FOR, 2, 0), ("st", VA_FOW, 4, 1))
USER_CODE_VA, USER_DATA_VA = 0x20008000, 0x2000C000
# User mode's faults, in order (the user program below): a load from KSEG,
# a load from a kernel page, a call into a kernel code page (IACV), and the
# last, which returns to the kernel.
USER_KSEG = KSEG | D0_PA
USER_FAULTS = (("user ld", USER_KSEG, 1, 0, 15), ("user ld", VA_KD, 1, 0, 16),
               ("user jsr", 0x20002000, 1, -1, 17),
               ("user ld", 0x20004000, 0, 0, 19))
NFAULTS = len(FAULTS) + len(USER_FAULTS)
SLOT_QW = 0x120 // 8
IMP_FIELDS = ("entInt", "entArith", "entMM", "entIF", "entUna", "entSys",
              "KGP", "PTBR", "PCBB", "MCES", "sysvalue", "PS", "KSP", "USP",
              "WHAMI", "UNIQUE", "VPTB", "register window", "TPU bit")
AGAIN_FIELDS = ("R27", "whami", "SP", "flag")
LOG_FIELDS = ("kind", "a0", "a1", "a2", "frame PC", "frame PS", "GP",
              "rdps inside")
KIND = {"I": 1, "M": 2, "F": 3, "A": 4, "U": 5, "S": 6}

GIO_CNFG, GIO_DATA, GIO_ADDR, CMM_TX = 0x100, 0x110, 0x120, 4
R_INT_STAT, R_INT_REQ, R_INT_QUEADD = 0x050, 0x070, 0x090
R_INTER_TIM = 0x0A0


def kseg(pa):
    return KSEG | pa


def csr8(n):
    """PID n's register window, ipe8(n) | 0x7FFC << 20 (N18, N20)."""
    return (~n & 0x1FFF) << 35 | 0x7FFC << 20


def io7_8(n, port):
    return (~n & 0x1FFF) << 35 | (~port & 7) << 32


def slot_pa(w):
    return L["HWRPB_PA"] + PROC_OFF + w * PROC_SIZE


def impure_pa(w):
    return L["PAL_IMPURE"] + w * L["IMPURE_SIZE"]


def pte(pa, bits):
    return (pa >> 13) << 32 | bits


V, FOR, FOW, FOE, ASM = 1, 2, 4, 8, 0x10
KRE, KWE, URE, UWE = 0x100, 0x1000, 0x800, 0x8000


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
    put(l3, 4, pte(UC_PA, V | KRE | URE))        # 0x20008000: user code
    put(l3, 5, pte(C1_PA, V | KRE | FOE))        # 0x2000A000: FOE
    put(l3, 6, pte(UD_PA, V | KRE | KWE | URE | UWE))   # 0x2000C000
    put(l3, 8, pte(T1_PA, V | KRE | KWE))        # VA_T: tbi
    put(l3, 9, pte(A1_PA, V | KRE | KWE | ASM))  # VA_A: tbiap keeps it
    put(l3, 10, pte(CX_PA, V | KRE))             # VA_C: tbisi
    put(l3, 11, pte(RO_PA, V | KRE | URE))       # VA_RO: read only
    put(l3, 12, pte(FORP_PA, V | KRE | KWE | FOR))
    put(l3, 13, pte(FOWP_PA, V | KRE | KWE | FOW))
    put(l3, 14, pte(KD_PA, V | KRE | KWE))       # VA_KD: kernel only
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


def p1_pcb():
    pcb = bytearray(0x80)
    for off, v in ((0x00, kseg(P1_STACK_TOP)), (0x08, P1_USP),
                   (0x10, L1_PA >> 13), (0x18, P1_ASN << 32 | P1_PCC),
                   (0x20, P1_UNIQUE), (0x28, 1)):
        struct.pack_into("<Q", pcb, off, v)
    return pcb


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


def returning(value):
    """A page of code that returns `value` in r0 to r26."""
    page = bytearray(0x2000)
    lo = value & 0xFFFF
    lo = lo - 0x10000 if lo >= 0x8000 else lo
    for i, w in enumerate((A.mem(OP["LDAH"], 0, R31, (value - lo) >> 16),
                           A.mem(OP["LDA"], 0, 0, lo), A.jmp(2, R31, 26))):
        struct.pack_into("<I", page, 4 * i, w)
    return page


def user_program():
    """User mode's code (r10 an invalid page, r12 a kernel code page, r13
    the user data page, r14 a KSEG address, r15 a kernel data page, r24
    the kernel's way back), one instruction a line, with its index."""
    def lda(ra, d):
        return A.mem(OP["LDA"], ra, R31, d)
    ld = OP["LDQ"]
    words = [
        A.mem(ld, 11, 13, 0),                   # 0: DTB miss, user mode
        A.mem(OP["STQ"], 11, 13, 8),            # 1
        lda(16, U_A0), lda(17, U_A1), lda(18, U_A2),   # 2-4
        call_pal(PAL["callsys"]),               # 5: entSys, retsys
        A.mem(OP["STQ"], 0, 13, 16),            # 6
        call_pal(PAL["rdunique"]),              # 7
        A.mem(OP["STQ"], 0, 13, 24),            # 8
        call_pal(PAL["imb"]),                   # 9: the PCB's IMB bit
        call_pal(PAL["clrfen"]),                # 10
        A.fop(0x17, 0x020, R31, R31, 2),        # 11: CPYS: FEN, again
        lda(1, 1),                              # 12
        A.mem(OP["STQ"], 1, 13, 32),            # 13
        call_pal(PAL["swpipl"]),                # 14: privileged: opDec
        A.mem(ld, 1, 14, 0),                    # 15: KSEG: ACV
        A.mem(ld, 1, 15, 0),                    # 16: kernel page: ACV
        A.jmp(1, 26, 12),                       # 17: JSR: IACV
        A.ins_op("BIS", 24, 24, 23),            # 18: the kernel's way back
        A.mem(ld, 1, 10, 0),                    # 19: TNV, back to the kernel
        A.bra(OP["BR"], R31, -1)]               # 20
    page = bytearray(0x2000)
    for i, w in enumerate(words):
        struct.pack_into("<I", page, 4 * i, w)
    return page


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

    def jsr(link, rb):
        a.emit(A.jmp(1, link, rb))

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

    def wait_at_least(va, n):
        """Spin, for at most WAIT_S, until the quadword at `va` is n or
        more (r1, r5-r7)."""
        top, out = fresh("wal"), fresh("wal_out")
        a.li(5, va)
        deadline(WAIT_S)
        a.label(top)
        ldq(1, 5, 0)
        ins("CMPLT", 1, n, 1, lit=True)
        a.branch(OP["BEQ"], 1, out)
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

    def log_event(kind):
        """In a handler after save_all: this processor's log gets kind, a0
        to a2, the frame's PC and PS, GP and rdps (r0-r5; r9 = WHAMI)."""
        pal("whami")
        ins("BIS", 0, 0, 9)
        a.li(1, kseg(LOG_PA))
        ins("SLL", 9, 12, 2, lit=True)
        ins("ADDQ", 1, 2, 1)
        ldq(3, 1, 0)
        ins("ADDQ", 3, 1, 4, lit=True)
        stq(4, 1, 0)
        ins("SLL", 3, 6, 3, lit=True)
        ins("ADDQ", 1, 3, 1)
        lda(2, R31, KIND[kind])
        stq(2, 1, 0x40)
        stq(16, 1, 0x48)
        stq(17, 1, 0x50)
        stq(18, 1, 0x58)
        ldq(2, 30, FRAME + 8)
        stq(2, 1, 0x60)
        ldq(2, 30, FRAME)
        stq(2, 1, 0x68)
        stq(29, 1, 0x70)
        pal("rdps")
        stq(0, 1, 0x78)

    def main2(reg, field):
        a.li(8, kseg(MAIN2_PA + 8 * M2[field]))
        stq(reg, 8, 0)

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
    a.branch(OP["BLE"], 9, "ph_done")
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
    a.label("ph_done")
    ret(26)

    # --- entInt, every processor: the log, the last-interrupt record; the
    # timer's ticks counted (stopped at the second, its bits cleared); the
    # sym53c810's interrupts counted (cleared at the second) ---
    a.label("ent_int")
    save_all()
    log_event("I")
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
    ldq(2, 4, R_INT_STAT)
    ins("AND", 9, 3, 3, lit=True)
    lda(5, R31, 1)
    ins("SLL", 5, 3, 5)
    ins("AND", 2, 5, 2)
    stq(2, 1, 8 * 12)
    lda(2, 30, FRAME)
    stq(2, 1, 8 * 13)
    # The timer (type 1): count; at the second tick stop chip 0's timer and
    # clear its tick bits.
    ins("CMPEQ", 16, 1, 2, lit=True)
    a.branch(OP["BEQ"], 2, "ei_not_tick")
    a.li(3, kseg(TICKS_PA))
    ldq(2, 3, 0)
    ins("ADDQ", 2, 1, 2, lit=True)
    stq(2, 3, 0)
    ins("CMPLT", 2, 2, 2, lit=True)
    a.branch(OP["BNE"], 2, "ei_done")
    a.li(3, SEG52 | csr8(0))
    stq(R31, 3, R_INTER_TIM)
    lda(2, R31, 0xF0)
    stq(2, 3, R_INT_STAT)
    mb()
    a.branch(OP["BR"], R31, "ei_done")
    a.label("ei_not_tick")
    # The sym53c810 (type 3, its vector): leave its interrupt asserted the
    # first time, clear it (a read of DSTAT) the second.
    ins("CMPEQ", 16, 3, 2, lit=True)
    a.branch(OP["BEQ"], 2, "ei_done")
    a.li(3, 0x800 + (SYM_LSI << 4))
    ins("CMPEQ", 17, 3, 2)
    a.branch(OP["BEQ"], 2, "ei_done")
    a.li(3, kseg(DEVSYM_PA))
    ldq(2, 3, 0)
    ins("ADDQ", 2, 1, 2, lit=True)
    stq(2, 3, 0)
    ins("CMPLT", 2, 2, 2, lit=True)
    a.branch(OP["BNE"], 2, "ei_done")
    a.li(3, SEG52 | io7_8(0, 0) + SYM_BAR)
    a.emit(mem(OP["LDBU"], 2, 3, 0x0C))         # DSTAT: clears SIR and DIP
    mb()
    a.label("ei_done")
    restore_all()
    pal("rti")

    # --- entMM: the log, the record (the secondaries), and the way on: with
    # r23 set, resume there (in the kernel at IPL 7 if the fault was in user
    # mode); with r23 clear, stay in user mode: a load or store skipped, a
    # call returned from (r26) ---
    a.label("ent_mm")
    save_all()
    log_event("M")
    ins("BIS", 9, 9, 0)
    a.branch(OP["BEQ"], 0, "mm_record_done")    # processor 0: no record
    a.li(1, kseg(SEC_PA))
    ins("SLL", 0, 11, 2, lit=True)
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
    a.label("mm_record_done")
    ldq(2, 30, 8 * SAVED.index(23))              # the test's r23
    a.branch(OP["BEQ"], 2, "mm_stay_user")
    stq(2, 30, FRAME + 8)
    ldq(2, 30, FRAME)
    ins("AND", 2, 8, 3, lit=True)
    a.branch(OP["BEQ"], 3, "mm_out")
    lda(2, R31, 7)
    stq(2, 30, FRAME)
    a.branch(OP["BR"], R31, "mm_out")
    a.label("mm_stay_user")
    ldq(2, 30, FRAME + 8)
    a.branch(OP["BLT"], 18, "mm_istream")
    lda(2, 2, 4)
    stq(2, 30, FRAME + 8)
    a.branch(OP["BR"], R31, "mm_out")
    a.label("mm_istream")
    ldq(2, 30, 8 * SAVED.index(26))
    stq(2, 30, FRAME + 8)
    a.label("mm_out")
    restore_all()
    pal("rti")

    # --- entIF: the log; FEN: FP on (wrfen) and the instruction again; an
    # opDec with r23 = LAZY: VEN on (CALL_PAL 0x3B, its old value kept) and
    # the instruction again ---
    a.label("ent_if")
    save_all()
    log_event("F")
    ins("CMPEQ", 16, 3, 2, lit=True)
    a.branch(OP["BEQ"], 2, "if_not_fen")
    lda(16, R31, 1)
    pal("wrfen")
    a.branch(OP["BR"], R31, "if_out")
    a.label("if_not_fen")
    ldq(2, 30, 8 * SAVED.index(23))
    a.li(3, LAZY)
    ins("CMPEQ", 2, 3, 2)
    a.branch(OP["BEQ"], 2, "if_out")
    lda(16, R31, 1)
    pal("vctl")
    main2(0, "0x3B old in the lazy handler")
    ldq(2, 30, FRAME + 8)
    lda(2, 2, -4)
    stq(2, 30, FRAME + 8)
    a.label("if_out")
    restore_all()
    pal("rti")

    # --- entArith and entUna: the log ---
    for name, kind in (("ent_arith", "A"), ("ent_una", "U")):
        a.label(name)
        save_all()
        log_event(kind)
        restore_all()
        pal("rti")

    # --- entSys: the log, v0 = SYS_RET, retsys ---
    a.label("ent_sys")
    save_all()
    log_event("S")
    a.li(2, SYS_RET)
    stq(2, 30, 8 * SAVED.index(0))
    restore_all()
    pal("retsys")

    def wrent_all():
        for k, label in enumerate(("ent_int", "ent_arith", "ent_mm",
                                   "ent_if", "ent_una", "ent_sys")):
            addr_of(16, label)
            lda(17, R31, k)
            pal("wrent")

    # --- a secondary, started by processor 0 through RXRDY ---
    a.label("sec_entry")
    stq(27, 30, -8)
    stq(26, 30, -16)
    stq(25, 30, -24)
    pal("whami")
    ins("BIS", 0, 0, 9)                          # r9 = WHAMI
    a.li(20, kseg(SEC_PA))
    ins("SLL", 9, 11, 1, lit=True)
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
    wrent_all()
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
    jsr(26, 10)
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
            jsr(26, 10)
        a.label(resume)
        lda(4, R31, 1)
        for r, val in ((16, SENT_A0), (17, SENT_A1), (18, SENT_A2),
                       (29, SENT_GP)):
            a.li(3, val)
            ins("CMPEQ", r, 3, 2)
            ins("AND", 4, 2, 4)
        stq(4, 20, MMREC_OFF + 0x40 * k + 56)
    # User mode: rti with a frame of PS 8 [ARM p. 559] into the user program
    # (user_program()), which ends with a fault that comes back here.
    a.li(10, 0x20004000)
    a.li(12, 0x20002000)
    a.li(13, USER_DATA_VA)
    a.li(14, USER_KSEG)
    a.li(15, VA_KD)
    addr_of(24, "user_back")
    lda(23, R31, 0)
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
    for off, field in ((8, "user mode: its store"),
                       (16, "user mode: callsys's v0"),
                       (24, "user mode: rdunique"),
                       (32, "user mode: FP after the FEN fault")):
        ldq(2, 1, off)
        stq(2, 20, 8 * S[field])
    lda(4, R31, 1)                               # rti restored user mode's
    for r, val in ((16, U_A0), (17, U_A1), (18, U_A2), (29, SENT_GP)):
        a.li(3, val)
        ins("CMPEQ", r, 3, 2)
        ins("AND", 4, 2, 4)
    stq(4, 20, MMREC_OFF + 0x40 * (NFAULTS - 1) + 56)
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
    # Its end: processor W_HREQ waits at IPL 0 for a halt request,
    # W_KSNV takes a bpt with its stack not quadword aligned, the others
    # halt at IPL 3 with R25-R27 to find in the slot.
    ins("CMPEQ", 9, W_HREQ, 1, lit=True)
    a.branch(OP["BNE"], 1, "sec_hreq")
    ins("CMPEQ", 9, W_KSNV, 1, lit=True)
    a.branch(OP["BNE"], 1, "sec_ksnv")
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
    a.label("sec_hreq")
    a.label("hreq_wait")
    a.emit(mfc(A.MISC["LDQ_ARM"], 2, 15))
    a.emit(mfc(A.MISC["QUIESCE"]))
    a.branch(OP["BR"], R31, "hreq_wait")
    a.label("hreq_wait_end")
    a.label("sec_ksnv")
    ins("BIS", 30, 4, 30, lit=True)
    pal("bpt")
    a.label("ksnv_after")
    a.branch(OP["BR"], R31, "ksnv_after")

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
    wrent_all()
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
    ldq(1, 12, R_INT_STAT)
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
    ldq(1, 12, R_INT_STAT)
    ins("AND", 1, 0xF, 1, lit=True)
    mstore(1, "R_INT_STAT<3:0> after the interrupt")
    lda(16, R31, 7)
    pal("swpipl")

    # ---- Phase 2, processor 0 (kernel mode, IPL 7) ----
    pal("cflush")
    pal("draina")
    for arg, field in ((0, "swppal(0)"), (0x8000, "swppal(0x8000)")):
        a.li(16, arg)
        pal("swppal")
        main2(0, field)
    pal("wrperfmon")
    main2(0, "wrperfmon")
    pal("rdmces")
    main2(0, "rdmces first")
    for arg, field in ((0x18, "rdmces after wrmces(0x18)"),
                       (0x07, "rdmces after wrmces(7)")):
        lda(16, R31, arg)
        pal("wrmces")
        pal("rdmces")
        main2(0, field)
    a.li(16, USP0)
    pal("wrusp")
    pal("rdusp")
    main2(0, "rdusp after wrusp")
    a.li(16, UNIQUE0)
    pal("wrunique")
    pal("rdunique")
    main2(0, "rdunique after wrunique")
    for arg, field in ((1, "0x3B(1) old"), (0, "0x3B(0) old"),
                       (3, "0x3B(3) old"), (0, "0x3B(0) old after 3")):
        lda(16, R31, arg)
        pal("vctl")
        main2(0, field)
    for fn, idx, data, field in ((0x49, 11, 0, "GET_TOY(11)"),
                                 (0x48, NV_INDEX, NV_VALUE, "PUT_TOY(40) v0"),
                                 (0x49, NV_INDEX, 0, "GET_TOY(40)"),
                                 (0x49, 9, 0, "GET_TOY(9)"),
                                 (0x49, 8, 0, "GET_TOY(8)"),
                                 (0x47, 0, 0, "cserve 0x47"),
                                 (0x49, 0x10C, 0, "GET_TOY(0x10c)"),
                                 (0x49, -1, 0, "GET_TOY(-1)"),
                                 (0x48, 0x10B, 0x3C, "PUT_TOY(0x10b) v0"),
                                 (0x49, 0x10B, 0, "GET_TOY(0x10b)")):
        lda(16, R31, fn)
        a.li(17, idx)
        a.li(18, data)
        pal("cserve")
        main2(0, field)
    # entIF: opDec (an unassigned CALL_PAL, an IFETCHB word in normal mode),
    # bpt, bugchk, gentrap, callsys in kernel mode.
    lda(23, R31, 0)
    a.emit(call_pal(0x05))
    a.label("p0_after_cp05")
    a.emit(A.ifetchb())
    a.label("p0_after_ifetchb")
    for name in ("bpt", "bugchk", "gentrap", "callsys"):
        pal(name)
        a.label("p0_after_" + name)
    # FEN: FP off, a CPYS faults, the handler turns it on, it runs again.
    pcb0 = kseg(impure_pa(0) + L["IMP_CPCB"] + L["PCB_FLAGS"])
    lda(16, R31, 0)
    pal("wrfen")
    a.li(3, pcb0)
    ldq(1, 3, 0)
    main2(1, "PCB FEN after wrfen(0)")
    a.label("p0_fen_pc")
    a.emit(A.fop(0x17, 0x020, R31, R31, 1))
    a.li(3, pcb0)
    ldq(1, 3, 0)
    main2(1, "PCB FEN after the FEN fault")
    lda(1, R31, 1)
    main2(1, "FP after the FEN fault")
    # Tarantula with VEN off: opDec; enabled lazily by the handler.
    a.li(23, LAZY)
    lda(1, R31, 0x55)
    a.emit(A.vc("getvl", rc=1))
    a.label("p0_after_getvl")
    lda(23, R31, 0)
    ins("CMPEQ", 1, 0x55, 1, lit=True)
    ins("XOR", 1, 1, 1, lit=True)
    main2(1, "getvl ran")
    lda(16, R31, 0)
    pal("vctl")
    main2(0, "0x3B(0) old after the lazy enable")
    # entArith: ADDQ/V, DIVT and DIVT/SU by zero.
    a.li(1, 0x4000000000000000)
    ins("ADDQ/V", 1, 1, 2)                       # 2^62 + 2^62: overflow
    a.label("p0_after_addqv")
    a.li(1, 0x3FF0000000000000)                  # 1.0
    a.emit(A.fop(0x14, 0x024, 1, R31, 1))        # ITOFT r1, f1
    a.emit(A.fop(0x16, 0x0A3, 1, R31, 3))        # DIVT f1, f31, f3
    a.label("p0_after_divt")
    a.emit(A.fop(0x16, 0x5A3, 1, R31, 4))        # DIVT/SU f1, f31, f4
    a.label("p0_after_divtsu")
    # entUna: LDQ and STL unaligned, across a page: Alphabox traps only
    # those (docs/cpu-fidelity.md, "Unaligned data accesses do not trap").
    a.li(3, kseg(UNA_PA))
    a.li(1, 0x0BAD)
    ldq(1, 3, 4)
    a.label("p0_after_ldq_una")
    main2(1, "r1 kept by the unaligned LDQ")
    a.emit(mem(OP["STL"], 2, 3, 6))
    a.label("p0_after_stl_una")
    # entMM: a load from a bad VA (DFAULT), a jump to one (BAD_JUMP_IVA,
    # the jump not completing: r26 kept).
    a.li(3, BAD_VA)
    addr_of(23, "p0_after_badld")
    a.label("p0_badld_pc")
    ldq(1, 3, 0)
    a.label("p0_after_badld")
    a.li(26, 0x2626)
    addr_of(23, "p0_after_badjump")
    a.label("p0_badjump_pc")
    jsr(26, 3)
    a.label("p0_after_badjump")
    main2(26, "r26 kept by the bad jump")
    lda(23, R31, 0)
    # wrvptptr, then a second context (swpctx) with the page tables.
    a.li(16, VPTB)
    pal("wrvptptr")
    a.li(16, P1_PCB_PA)
    pal("swpctx")
    main2(0, "swpctx v0")
    main2(30, "SP in P1")
    pal("rdusp")
    main2(0, "rdusp in P1")
    pal("rdunique")
    main2(0, "rdunique in P1")
    a.emit(mfc(A.MISC["RPCC"], 1))
    ins("SRL", 1, 32, 2, lit=True)
    ins("ADDQ", 1, 2, 2)
    ins("ZAPNOT", 2, 0x0F, 2, lit=True)
    main2(2, "PCC in P1")
    a.li(3, kseg(impure_pa(0)))
    ldq(1, 3, L["IMP_PTBR"])
    main2(1, "PTBR in P1")
    ldq(1, 3, L["IMP_PCBB"])
    main2(1, "PCBB in P1")

    def l3_set(index, value):
        a.li(3, kseg(L3_PA + 8 * index))
        a.li(4, value)
        stq(4, 3, 0)
        mb()

    def load_to(va, field):
        a.li(3, va)
        ldq(1, 3, 0)
        main2(1, field)

    def tbi(kind, va=0):
        a.li(16, kind)
        a.li(17, va)
        pal("tbi")

    load_to(VA_T, "tbi: load")
    l3_set(8, pte(T2_PA, V | KRE | KWE))
    load_to(VA_T, "tbi: load after the PTE changed")
    tbi(2, VA_T)
    load_to(VA_T, "tbi: after tbi 2")
    l3_set(8, pte(T1_PA, V | KRE | KWE))
    load_to(VA_T, "tbi: after the PTE restored")
    tbi(-2)
    load_to(VA_T, "tbi: after tbi -2")
    load_to(VA_A, "ASM: load")
    l3_set(9, pte(A2_PA, V | KRE | KWE | ASM))
    tbi(-1)
    load_to(VA_A, "ASM: after the PTE changed and tbi -1")
    tbi(-2)
    load_to(VA_A, "ASM: after tbi -2")
    l3_set(9, pte(A1_PA, V | KRE | KWE | ASM))
    a.li(3, VA_C)
    jsr(26, 3)
    main2(0, "ITB: call")
    l3_set(10, pte(CY_PA, V | KRE))
    a.li(3, VA_C)
    jsr(26, 3)
    main2(0, "ITB: call after the PTE changed")
    tbi(1, VA_C)
    pal("imb")
    a.li(3, VA_C)
    jsr(26, 3)
    main2(0, "ITB: call after tbi 1 and imb")
    l3_set(10, pte(CX_PA, V | KRE))
    tbi(1, VA_C)
    l3_set(8, pte(T2_PA, V | KRE | KWE))
    tbi(3, VA_T)
    load_to(VA_T, "tbi 3: load")
    l3_set(8, pte(T1_PA, V | KRE | KWE))
    tbi(3, VA_T)
    # imb: code rewritten in memory, run again.
    for value, field in ((0x111, "imb: first code"),
                         (0x222, "imb: rewritten code")):
        a.li(3, kseg(CODEBUF_PA))
        a.li(4, A.mem(OP["LDA"], 0, R31, value))
        a.emit(mem(OP["STL"], 4, 3, 0))
        a.li(4, A.jmp(2, R31, 26))
        a.emit(mem(OP["STL"], 4, 3, 4))
        mb()
        pal("imb")
        jsr(26, 3)
        main2(0, field)
    # The ASN: entries filled at ASN 5 for VA_T and VA_C, then both PTEs
    # changed with no tbi; back at the console's ASN 0 neither entry may
    # hit [ARM p. 583], so the new pages are read and called.
    tbi(-2)
    load_to(VA_T, "ASN: load in P1")
    a.li(3, VA_C)
    jsr(26, 3)
    main2(0, "ASN: call in P1")
    l3_set(8, pte(T2_PA, V | KRE | KWE))
    l3_set(10, pte(CY_PA, V | KRE))
    a.li(16, P1_USP2)
    pal("wrusp")
    a.li(16, P1_UNIQUE2)
    pal("wrunique")
    # Back to the console's context, given the same page table (swpctx
    # loads PTBR and never saves it [ARM p. 560]).
    a.li(3, kseg(impure_pa(0) + L["IMP_CPCB"]))
    a.li(4, L1_PA >> 13)
    stq(4, 3, L["PCB_PTBR"])
    mb()
    a.li(16, impure_pa(0) + L["IMP_CPCB"])
    pal("swpctx")
    main2(0, "swpctx back v0")
    load_to(VA_T, "ASN: load back")
    a.li(3, VA_C)
    jsr(26, 3)
    main2(0, "ASN: call back")
    l3_set(8, pte(T1_PA, V | KRE | KWE))
    l3_set(10, pte(CX_PA, V | KRE))
    tbi(-2)
    a.li(3, kseg(P1_PCB_PA))
    for off, field in ((0x00, "P1 PCB KSP"), (0x08, "P1 PCB USP"),
                       (0x18, "P1 PCB PCC"), (0x20, "P1 PCB UNIQUE")):
        ldq(1, 3, off)
        if off == 0x18:
            ins("ZAPNOT", 1, 0x0F, 1, lit=True)
        main2(1, field)
    ldq(1, 3, 0x18)
    ins("SRL", 1, 32, 1, lit=True)
    main2(1, "P1 PCB ASN")
    main2(30, "SP back")
    pal("rdusp")
    main2(0, "rdusp back")
    pal("rdunique")
    main2(0, "rdunique back")
    a.li(3, kseg(impure_pa(0)))
    ldq(1, 3, L["IMP_PTBR"])
    main2(1, "PTBR back")
    # wtint: idle at IPL 7 (it sleeps, then returns); with an IPI pending
    # (it returns, the IPI taken once the IPL drops). The ARM lets wtint
    # return at once [p. 579], and a QUIESCE may fail [IDS p. 671]:
    # Alphabox's JIT fails one on a newly watched page until every other
    # processor has dropped its cached translations (AlphaCPU::quiesce),
    # which a loaded host may let them do late. So again, for at most
    # WAIT_S, until WTINT_SLEEPS of them have lasted the timeout; a host
    # that runs this processor late fakes a few, which is why it is more
    # than one, and why check() reads the emulator's count of QUIESCEs
    # ended by the timeout too. The longest is checked, every v0 ORed.
    top, out, short = fresh("wtint"), fresh("wtint_out"), fresh("wtint_s")
    lda(11, R31, 0)                              # the long ones
    lda(13, R31, 0)                              # the longest sleep
    lda(14, R31, 0)                              # every v0, ORed
    deadline(WAIT_S)
    a.label(top)
    a.emit(mfc(A.MISC["RPCC"], 9))
    lda(16, R31, 0)
    pal("wtint")
    a.emit(mfc(A.MISC["RPCC"], 10))
    ins("BIS", 14, 0, 14)
    ins("SUBQ", 10, 9, 10)
    ins("ZAPNOT", 10, 0x0F, 10, lit=True)
    ins("CMPULT", 13, 10, 1)
    ins("CMOVNE", 1, 10, 13)
    a.li(1, WTINT_LONG)
    ins("CMPULT", 10, 1, 1)
    a.branch(OP["BNE"], 1, short)
    ins("ADDQ", 11, 1, 11, lit=True)
    ins("CMPULT", 11, WTINT_SLEEPS, 1, lit=True)
    a.branch(OP["BEQ"], 1, out)
    a.label(short)
    until(top)
    a.label(out)
    main2(14, "wtint idle v0")
    main2(13, "wtint idle cycles")
    a.li(21, kseg(INTREC_PA))
    ldq(19, 21, 0)
    lda(16, R31, 0)
    pal("wripir")
    mb()
    lda(16, R31, 0)
    pal("wtint")
    main2(0, "wtint with an IPI v0")
    lda(16, R31, 0)
    pal("swpipl")
    a.label("p0_after_wtint_ipl0")
    lda(16, R31, 7)
    pal("swpipl")
    ldq(1, 21, 0)
    ins("SUBQ", 1, 19, 1)
    main2(1, "interrupts after wtint")
    # The interval timer: chip 0's at ITV 7; two ticks at IPL 0 (entInt
    # stops it at the second).
    lda(1, R31, 7)
    stq(1, 12, R_INTER_TIM)
    mb()
    lda(16, R31, 0)
    pal("swpipl")
    wait_at_least(kseg(TICKS_PA), 2)
    lda(16, R31, 7)
    pal("swpipl")
    a.li(3, kseg(TICKS_PA))
    ldq(1, 3, 0)
    main2(1, "timer ticks")
    ldq(1, 12, R_INTER_TIM)
    main2(1, "R_INTER_TIM after the ticks")
    ldq(1, 12, R_INT_STAT)
    ins("AND", 1, 0xF0, 1, lit=True)
    main2(1, "R_INT_STAT<7:4> after the ticks")
    # The interrupt queue: an LSI's and an MSI's IID (PE 0) at IPL 7, then
    # IPL 0: two device interrupts, one at a time.
    for iid in (LSI_IID, MSI_IID):
        a.li(1, iid)
        stq(1, 12, R_INT_QUEADD)
        mb()
    ldq(1, 12, R_INT_STAT)
    ins("SRL", 1, 12, 1, lit=True)
    ins("AND", 1, 1, 1, lit=True)
    main2(1, "queue bit with two IIDs")
    a.li(3, kseg(LOG_PA))
    ldq(19, 3, 0)
    lda(16, R31, 0)
    pal("swpipl")
    top = fresh("q2")
    out = fresh("q2_out")
    deadline(WAIT_S)
    a.label(top)
    a.li(3, kseg(LOG_PA))
    ldq(1, 3, 0)
    ins("SUBQ", 1, 19, 1)
    ins("CMPLT", 1, 2, 1, lit=True)
    a.branch(OP["BEQ"], 1, out)
    until(top)
    a.label(out)
    lda(16, R31, 7)
    pal("swpipl")
    a.li(3, kseg(LOG_PA))
    ldq(1, 3, 0)
    ins("SUBQ", 1, 19, 1)
    main2(1, "queue interrupts")
    # The sym53c810 in pci0.1: PCI window 0 of port 0 direct over the first
    # 1 GB, its registers at SYM_BAR, its SIR interrupt (DIEN), LSI 4 to PID
    # 0; the SCRIPTS' INT at IPL 7, then IPL 0: the IO7 sends it, entInt
    # leaves it asserted, rti's drop of the IPL sends the PALcode's end of
    # interrupt, the IO7 sends it again, entInt clears it.
    a.li(5, SEG52 | io7_8(0, 0) + 0xFF800000)
    lda(1, R31, 1)
    stq(1, 5, 0x1000)                            # POx_WBASE0
    a.li(1, 0x3FF00000)
    stq(1, 5, 0x1100)                            # POx_WMASK0
    stq(R31, 5, 0x1200)                          # POx_TBASE0
    a.li(5, SEG52 | io7_8(0, 0) + 0xFE000000 + (SYM_SLOT << 11))
    a.li(1, SYM_BAR)
    a.emit(mem(OP["STL"], 1, 5, 0x14))           # BAR1 (memory)
    lda(1, R31, 0x0006)
    a.emit(mem(OP["STL"], 1, 5, 0x04))           # COMMAND
    mb()
    a.li(5, SEG52 | io7_8(0, 0) + SYM_BAR)
    lda(1, R31, 0x04)
    a.emit(mem(OP["STB"], 1, 5, 0x39))           # DIEN: SIR
    a.li(3, SEG52 | io7_8(0, 7) + 0xFF800000 + 0x310000 + SYM_LSI * 0x40)
    a.li(1, 1 << 24)                             # LSI_CTL: enabled, PID 0
    stq(1, 3, 0)
    mb()
    a.li(1, SCRIPTS_PA)
    a.emit(mem(OP["STL"], 1, 5, 0x2C))           # DSP: the SCRIPTS run
    mb()
    delay(1 << 16)
    lda(16, R31, 0)
    pal("swpipl")
    wait_at_least(kseg(DEVSYM_PA), 2)
    delay(1 << 16)                               # time for a third, if any
    lda(16, R31, 7)
    pal("swpipl")
    a.li(3, kseg(DEVSYM_PA))
    ldq(1, 3, 0)
    main2(1, "sym interrupts")
    a.li(5, SEG52 | io7_8(0, 0) + SYM_BAR)
    a.emit(mem(OP["LDBU"], 1, 5, 0x14))          # ISTAT
    main2(1, "sym ISTAT after")
    ldq(1, 12, R_INT_STAT)
    ins("SRL", 1, 12, 1, lit=True)
    ins("AND", 1, 1, 1, lit=True)
    main2(1, "queue bit at the end")

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
        a.li(3, kseg(SEC_PA + 0x800 * w))
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

    def wait_reason(w, reason):
        top, out = fresh("halted"), fresh("halted_out")
        a.li(9, kseg(slot_pa(w)))
        deadline(WAIT_S)
        a.label(top)
        ldq(1, 9, L["SLOT_HALT_REASON"])
        ins("CMPEQ", 1, reason, 1, lit=True)
        a.branch(OP["BNE"], 1, out)
        until(top)
        a.label(out)

    for w in SECS:
        if w not in (W_HREQ, W_KSNV):
            wait_reason(w, 5)
    # A halt request to W_HREQ (R_INT_REQ<11:8>, N22), then W_KSNV's halt.
    p, t = divmod(W_HREQ, 4)
    a.li(3, SEG52 | csr8(p) | R_INT_REQ)
    a.li(1, 1 << (8 + t))
    stq(1, 3, 0)
    mb()
    wait_reason(W_HREQ, 1)
    wait_reason(W_KSNV, 2)
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
    print_text("@@\n")
    dump(kseg(MAIN2_PA), len(MAIN2_FIELDS))
    print_text("@@\n")
    dump(kseg(P1_PCB_PA), 6)
    for w in SECS:
        print_text("@@\n")
        dump(kseg(SEC_PA + 0x800 * w), MMREC_OFF // 8 + 8 * NFAULTS)
        print_text("@@\n")
        dump(kseg(INTREC_PA + 0x80 * w), len(INT_FIELDS))
    for w in PROCS:
        print_text("@@\n")
        dump(kseg(slot_pa(w)), SLOT_QW)
    for w in PROCS:
        print_text("@@\n")
        dump(kseg(impure_pa(w)), len(IMP_FIELDS))
    for w in PROCS:
        # Each log: its count, then its entries (print_hex prints none for
        # a count of 0).
        print_text("@@\n")
        a.li(3, kseg(LOG_PA + 0x1000 * w))
        ldq(17, 3, 0)
        ins("SLL", 17, 3, 17, lit=True)
        a.li(16, kseg(LOG_PA + 0x1000 * w + 0x40))
        bsr(26, "print_hex")
    print_text("@@END\n")
    a.label("done")
    a.branch(OP["BR"], R31, "done")

    words = a.finish()
    for i, reg, label in addr_fixups:
        words[i:i + 5] = li64(reg, kseg(a.labels[label]))
    assert len(words) * 4 + L["CONSOLE_PA"] <= MAIN_PA
    code = b"".join(struct.pack("<I", w) for w in words)
    lab = {name: kseg(pa) for name, pa in a.labels.items()}
    return code, lab


def data_pages():
    """The pages the page tables map, the second context's PCB, the
    unaligned cases' data, the sym53c810's SCRIPTS."""
    out = []
    for pa, v in ((D0_PA, DATA0), (UD_PA, DATA1), (T1_PA, T1V), (T2_PA, T2V),
                  (A1_PA, A1V), (A2_PA, A2V), (RO_PA, 0x5050),
                  (FORP_PA, 0xF0F0), (FOWP_PA, 0xF1F1), (KD_PA, 0x4B4B)):
        page = bytearray(0x2000)
        struct.pack_into("<Q", page, 0, v)
        out.append((pa, page))
    out += [(C0_PA, returning(ITB_VALUE)), (C1_PA, returning(ITB_VALUE)),
            (CX_PA, returning(CXV)), (CY_PA, returning(CYV)),
            (UC_PA, user_program()), (P1_PCB_PA, p1_pcb()),
            (UNA_PA, bytes(range(1, 17))),
            (SCRIPTS_PA, struct.pack("<II", 0x98080000, 0x00000000))]
    return out


CFG = """sys0 = tsunami
{
  platform = "es47";
  memory.bits = 29;
  rom.srm = "absent.exe";
  rom.decompressed = "pal8.rom";
  rom.nvram = "cmm_nvram.bin";
%(cpus)s
  serial0 = serial { address = "127.0.0.1"; port = %(port)d; }
  pci0.%(sym)d = sym53c810 { }
}
"""


def config(port):
    cpus = "\n".join("  cpu%d = ev8n { speed = %dM; tpus = %d; }"
                     % (c, SPEED_MHZ, TPUS) for c in range(CHIPS))
    return CFG % {"cpus": cpus, "port": port, "sym": SYM_SLOT}


def run(binary, workdir, port, timeout):
    """Boot; read the CMM terminal until @@END; stop our own emulator."""
    with open(os.path.join(workdir, "es40.cfg"), "w") as f:
        f.write(config(port))
    out = open(os.path.join(workdir, "alphabox.out"), "wb")
    t0 = time.time()
    env = dict(os.environ, ALPHABOX_TRACE_QUIESCE="1")
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


N_BLOCKS = 6 + 2 * len(SECS) + 3 * len(PROCS)
MASK64 = (1 << 64) - 1


def check(bufs, lab, emu, text):
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

    main, int0, sums, again, main2, p1pcb = bufs[:6]
    rest = bufs[6:]
    sec = {w: (rest[2 * k], rest[2 * k + 1]) for k, w in enumerate(SECS)}
    rest = rest[2 * len(SECS):]
    slots = {w: rest[k] for k, w in enumerate(PROCS)}
    imps = {w: rest[len(PROCS) + k] for k, w in enumerate(PROCS)}
    logs = {w: rest[2 * len(PROCS) + k] for k, w in enumerate(PROCS)}

    def mask(t):
        return (1 << t | 1 << (4 + t) | 1 << (8 + t) |
                (1 << 12 if t == 0 else 0))

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
    want_main.update({"R_INT_MASK of %d" % w: mask(w & 3) for w in PROCS})
    for k, ipl in enumerate((0, 5, 7)):
        ok("processor 0: rdps after swpipl(%d)" % ipl, main[6 + 2 * k], ipl)
    for name, k in M.items():
        if name in want_main:
            ok("processor 0: " + name, main[k], want_main[name])
    top0 = kseg(STACK_PA + STACK_TOP)
    # The last of processor 0's eight interrupts: the sym53c810's second.
    want_int0 = {"count": 8, "a0": 3, "a1": 0x800 + (SYM_LSI << 4),
                 "GP": KGP0, "R_INT_STAT, own bit": 0}
    for k, name in enumerate(INT_FIELDS):
        if name in want_int0:
            ok("processor 0 last entInt: " + name, int0[k], want_int0[name])
    for k, w in enumerate(SECS):
        ok("TPUs woken after the IPI to %d" % w, sums[k], k + 1)
    for k, name in enumerate(AGAIN_FIELDS):
        want = {"R27": AGAIN_DATA, "whami": 1,
                "SP": kseg(STACK_PA + 0x2000 + STACK_TOP), "flag": 1}[name]
        ok("processor 1 started again: " + name, again[k], want)

    # Phase 2, processor 0.
    year, month = time.localtime().tm_year % 100, time.localtime().tm_mon
    want2 = {"swppal(0)": 1, "swppal(0x8000)": 1, "wrperfmon": 0,
             "rdmces first": 0, "rdmces after wrmces(0x18)": 0x18,
             "rdmces after wrmces(7)": 0, "rdusp after wrusp": USP0,
             "rdunique after wrunique": UNIQUE0, "0x3B(1) old": 0,
             "0x3B(0) old": 1, "0x3B(3) old": 0, "0x3B(0) old after 3": 1,
             "GET_TOY(11)": 0x02, "PUT_TOY(40) v0": 0,
             "GET_TOY(40)": NV_VALUE, "GET_TOY(9)": year, "GET_TOY(8)": month,
             "cserve 0x47": MASK64, "GET_TOY(0x10c)": MASK64,
             "GET_TOY(-1)": MASK64,
             "PUT_TOY(0x10b) v0": 0, "GET_TOY(0x10b)": 0x3C,
             "PCB FEN after wrfen(0)": 0, "PCB FEN after the FEN fault": 1,
             "FP after the FEN fault": 1, "0x3B old in the lazy handler": 0,
             "getvl ran": 1, "0x3B(0) old after the lazy enable": 1,
             "r1 kept by the unaligned LDQ": 0x0BAD,
             "r26 kept by the bad jump": 0x2626,
             "swpctx v0": impure_pa(0) + L["IMP_CPCB"],
             "SP in P1": kseg(P1_STACK_TOP), "rdusp in P1": P1_USP,
             "rdunique in P1": P1_UNIQUE, "PTBR in P1": L1_PA >> 13,
             "PCBB in P1": P1_PCB_PA, "tbi: load": T1V,
             "tbi: load after the PTE changed": T1V, "tbi: after tbi 2": T2V,
             "tbi: after the PTE restored": T2V, "tbi: after tbi -2": T1V,
             "ASM: load": A1V, "ASM: after the PTE changed and tbi -1": A1V,
             "ASM: after tbi -2": A2V, "ITB: call": CXV,
             "ITB: call after the PTE changed": CXV,
             "ITB: call after tbi 1 and imb": CYV, "tbi 3: load": T2V,
             "imb: first code": 0x111, "imb: rewritten code": 0x222,
             "ASN: load in P1": T1V, "ASN: call in P1": CXV,
             "ASN: load back": T2V, "ASN: call back": CYV,
             "swpctx back v0": P1_PCB_PA, "P1 PCB KSP": kseg(P1_STACK_TOP),
             "P1 PCB USP": P1_USP2, "P1 PCB ASN": P1_ASN,
             "P1 PCB UNIQUE": P1_UNIQUE2, "SP back": top0,
             "rdusp back": USP0, "rdunique back": UNIQUE0, "PTBR back": L1_PA >> 13,
             "wtint idle v0": 0, "wtint with an IPI v0": 0,
             "interrupts after wtint": 1, "timer ticks": 2,
             "R_INTER_TIM after the ticks": 0,
             "R_INT_STAT<7:4> after the ticks": 0,
             "queue bit with two IIDs": 1, "queue interrupts": 2,
             "sym interrupts": 2, "queue bit at the end": 0}
    for name, k in M2.items():
        got = main2[k]
        if name == "PCC in P1":
            d = (got - P1_PCC) & 0xFFFFFFFF
            ok("processor 0: PCC in P1 (count + offset - the PCB's)", d,
               "under 2^31", good=d < 1 << 31)
        elif name == "P1 PCB PCC":
            d = (got - P1_PCC) & 0xFFFFFFFF
            ok("processor 0: P1's PCC saved by swpctx", d, "under 2^31",
               good=d < 1 << 31)
        elif name == "wtint idle cycles":
            ok("processor 0: wtint idle slept", got, ">= %#x" % WTINT_LONG,
               good=got >= WTINT_LONG)
        elif name == "sym ISTAT after":
            ok("processor 0: sym ISTAT<DIP> after", got & 1, 0)
        else:
            ok("processor 0: " + name, got, want2[name])
    ok("P1's PCB PTBR unchanged", p1pcb[2], L1_PA >> 13)
    ok("P1's PCB FEN", p1pcb[5], 1)

    # Each processor's log against the entries it must hold, in order.
    def log_entries(buf):
        return [buf[i:i + 8] for i in range(0, len(buf), 8)]

    def want_log(w, entries, got):
        ok("processor %d: entries logged" % w, len(got), len(entries))
        for i, (want, have) in enumerate(zip(entries, got)):
            for f, (wv, hv) in enumerate(zip(want, have)):
                if wv is None:
                    continue
                if callable(wv):
                    ok("processor %d, entry %d: %s" % (w, i, LOG_FIELDS[f]),
                       hv, "in range", good=wv(hv))
                else:
                    ok("processor %d, entry %d: %s" % (w, i, LOG_FIELDS[f]),
                       hv, wv & MASK64)

    I, MM, F, AR, U, SY = (KIND[k] for k in "IMFAUS")
    p0 = [
        (I, 0, 0, 0, lab["after_swpipl4"], 4, KGP0, 5),
        (F, 4, None, None, lab["p0_after_cp05"], 7, KGP0, 7),
        (F, 4, None, None, lab["p0_after_ifetchb"], 7, KGP0, 7),
        (F, 0, None, None, lab["p0_after_bpt"], 7, KGP0, 7),
        (F, 1, None, None, lab["p0_after_bugchk"], 7, KGP0, 7),
        (F, 2, None, None, lab["p0_after_gentrap"], 7, KGP0, 7),
        (F, 1, None, None, lab["p0_after_callsys"], 7, KGP0, 7),
        (F, 3, None, None, lab["p0_fen_pc"], 7, KGP0, 7),
        (F, 4, None, None, lab["p0_after_getvl"], 7, KGP0, 7),
        (AR, 0x40, 1 << 2, None, lab["p0_after_addqv"], 7, KGP0, 7),
        (AR, 0x04, 1 << 35, None, lab["p0_after_divt"], 7, KGP0, 7),
        (AR, 0x05, 1 << 36, None, lab["p0_after_divtsu"], 7, KGP0, 7),
        (U, kseg(UNA_PA + 4), 0x29, 1, lab["p0_after_ldq_una"], 7, KGP0, 7),
        (U, kseg(UNA_PA + 6), 0x2C, 2, lab["p0_after_stl_una"], 7, KGP0,
         7),
        (MM, BAD_VA, 1, 0, lab["p0_badld_pc"], 7, KGP0, 7),
        (MM, BAD_VA, 1, -1, lab["p0_badjump_pc"], 7, KGP0, 7),
        (I, 0, 0, 0, lab["p0_after_wtint_ipl0"], 0, KGP0, 5),
        (I, 1, None, None, None, 0, KGP0, 5),
        (I, 1, None, None, None, 0, KGP0, 5),
        (I, 3, 0x800 + (LSI_IID << 4), None, None, 0, KGP0, 4),
        (I, 3, 0x1000 + (5 << 4), None, None, 0, KGP0, 4),
        (I, 3, 0x800 + (SYM_LSI << 4), None, None, 0, KGP0, 4),
        (I, 3, 0x800 + (SYM_LSI << 4), None, None, 0, KGP0, 4)]
    want_log(0, p0, log_entries(logs[0]))

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
                "rdps back from user mode": 7, "KSP kept by rti": top,
                "user mode: callsys's v0": SYS_RET,
                "user mode: rdunique": UNIQUE_BASE + w,
                "user mode: FP after the FEN fault": 1}
        for k, name in enumerate(SEC_FIELDS):
            if name == "PCC":
                d = (res[k] - PCC_STEP * w) & 0xFFFFFFFF
                ok("processor %d: PCC (count + offset - the HWPCB's)" % w,
                   d, "under 2^31", good=d < 1 << 31)
            else:
                ok("processor %d: %s" % (w, name), res[k], want[name])
        recs = list(FAULTS) + [f[:4] for f in USER_FAULTS]
        for f, (kind, va, mmcsr, cause) in enumerate(recs):
            rec = res[MMREC_OFF // 8 + 8 * f:MMREC_OFF // 8 + 8 * f + 8]
            if kind.startswith("user"):
                index = USER_FAULTS[f - len(FAULTS)][4]
                pc = (va if kind == "user jsr" else
                      USER_CODE_VA + 4 * index)
                ps = 8
            else:
                pc = va if kind == "jsr" else lab["fault_pc_%d" % f]
                ps = 7
            # User mode's frames hold its own a0-a2 (U_A*), not the
            # kernel's sentinels; only the last user fault is checked after
            # rti (back in the kernel).
            user = kind.startswith("user")
            want_rec = [va, mmcsr, cause & MASK64, pc, ps, KGP_BASE + w,
                        0 if user else 1,
                        1 if (not user or f == NFAULTS - 1) else None]
            for i, name in enumerate(MM_FIELDS):
                if want_rec[i] is not None:
                    ok("processor %d, entMM %d (%s %#x): %s"
                       % (w, f, kind, va, name), rec[i], want_rec[i])
        lo, hi = lab["ipi_wait"], lab["ipi_wait_end"]
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
        # The log: the kernel's faults, then user mode's entries, the IPI;
        # W_KSNV's bpt never reaches the OS (the halt comes first).
        upc = USER_CODE_VA
        entries = [(MM, va, mmcsr, cause, None, 7, KGP_BASE + w, 7)
                   for (_, va, mmcsr, cause) in FAULTS]
        entries += [
            (SY, U_A0, U_A1, U_A2, upc + 4 * 6, 8, KGP_BASE + w, 0),
            (F, 3, None, None, upc + 4 * 11, 8, KGP_BASE + w, 0),
            (F, 4, None, None, upc + 4 * 15, 8, KGP_BASE + w, 0),
            (MM, USER_KSEG, 1, 0, upc + 4 * 15, 8, KGP_BASE + w, 0),
            (MM, VA_KD, 1, 0, upc + 4 * 16, 8, KGP_BASE + w, 0),
            (MM, 0x20002000, 1, -1, 0x20002000, 8, KGP_BASE + w, 0),
            (MM, 0x20004000, 0, 0, upc + 4 * 19, 8, KGP_BASE + w, 0),
            (I, 0, 0, 0, lambda pc: lo <= pc <= hi, 0, KGP_BASE + w, 5)]
        want_log(w, entries, log_entries(logs[w]))

    for w in PROCS:
        s = slots[w]
        q = {off: s[off // 8] for off in range(0, 0x120, 8)}
        if w == 0:
            want = {0x80: SLOT_PRESENT,
                    0xE8: impure_pa(0) + L["IMP_CPCB"],
                    0xF0: lab["after_halt0"], 0xF8: 7, 0x100: H25,
                    0x108: H26, 0x110: H27, 0x118: 5}
        else:
            top = kseg(STACK_PA + 0x2000 * w + STACK_TOP)
            want = {0x80: SLOT_PRESENT | 0x20 | 1, 0xE8: slot_pa(w),
                    0x88: L["PAL_MEM_LEN"], 0x90: L["PAL_SCR_LEN"],
                    0x98: L["PAL_MEM_PA"], 0xA0: L["PAL_SCR_PA"],
                    0xA8: PAL_REVISION, 0x08: USP_BASE + w,
                    0x20: UNIQUE_BASE + w, 0x28: 1 | 1 << 32}
            if w == 1:
                want.update({0xF0: lab["again_halted"], 0xF8: 7,
                             0x100: 0, 0x108: 0, 0x110: AGAIN_DATA,
                             0x118: 5, 0x00: top})
            elif w == W_HREQ:
                want.update({0x80: SLOT_PRESENT | 0x20 | 1 | 0x10,
                             0xF8: 0, 0x118: 1, 0x00: top})
            elif w == W_KSNV:
                want.update({0xF0: lab["ksnv_after"], 0xF8: 0, 0x118: 2,
                             0x00: top | 4})
            else:
                want.update({0xF0: lab["sec_halted"], 0xF8: 3,
                             0x100: H25 + w, 0x108: H26 + w,
                             0x110: H27 + w, 0x118: 5, 0x00: top})
        for off, v in sorted(want.items()):
            ok("slot %d +%#x" % (w, off), q[off], v)
        if w == W_HREQ:
            lo, hi = lab["sec_hreq"], lab["hreq_wait_end"]
            ok("slot %d: halt PC in the wait" % w, q[0xF0],
               "%#x-%#x" % (lo, hi), good=lo <= q[0xF0] <= hi)
        if w:
            d = (q[0x18] - PCC_STEP * w) & 0xFFFFFFFF
            ok("slot %d: the PCC saved at the halt" % w, d, "under 2^31",
               good=d < 1 << 31)

    for w in PROCS:
        m = imps[w]
        p, t = divmod(w, 4)
        ents = [lab[n] for n in ("ent_int", "ent_arith", "ent_mm", "ent_if",
                                 "ent_una", "ent_sys")]
        want = {"entInt": ents[0], "entArith": ents[1], "entMM": ents[2],
                "entIF": ents[3], "entUna": ents[4], "entSys": ents[5],
                "KGP": KGP0 if w == 0 else KGP_BASE + w,
                "PTBR": L1_PA >> 13,
                "PCBB": impure_pa(0) + L["IMP_CPCB"] if w == 0 else
                slot_pa(w), "MCES": 0 if w == 0 else 8,
                "sysvalue": SYSVAL0 if w == 0 else 0xABC00 + w, "PS": 7,
                "KSP": 0 if w == 0 else
                kseg(STACK_PA + 0x2000 * w + STACK_TOP),
                "USP": USP0 if w == 0 else USP_BASE + w, "WHAMI": w,
                "UNIQUE": UNIQUE0 if w == 0 else UNIQUE_BASE + w,
                "VPTB": VPTB, "register window": csr8(p),
                "TPU bit": 1 << t}
        for k, name in enumerate(IMP_FIELDS):
            ok("impure area of %d: %s" % (w, name), m[k], want[name])

    msg = ("%%PAL8-F-HALT, CPU %016x, reason %016x" % (W_KSNV, 2))
    ok("the PALcode's message for processor %d's halt" % W_KSNV,
       "absent", msg, good=msg in text.replace("\r", ""))
    # The idle wtint's QUIESCE, counted by the emulator
    # (ALPHABOX_TRACE_QUIESCE): processor 0 sleeps nowhere else to the
    # timeout, and a host that ran it late could fake the RPCC check.
    qline = [ln for ln in emu.splitlines()
             if ln.startswith("%CPU-I-QUIESCE: CPU0 ")]
    qto = re.search(r"timeout (\d+)", qline[0]) if qline else None
    ok("processor 0: QUIESCEs ended by the timeout (wtint idle)",
       int(qto.group(1)) if qto else "no report", ">= 1",
       good=bool(qto) and int(qto.group(1)) >= 1)
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
    code, lab = build()
    img = fw.image(pal, L, code,
                   extra=page_tables() + data_pages() +
                   [(L["HWRPB_PA"], hwrpb())])
    fw.write_rom(os.path.join(work, "pal8.rom"), img, L)
    text, emu, secs = run(binary, work, port, args.timeout)
    with open(os.path.join(work, "serial.txt"), "w") as f:
        f.write(text)
    print("== the EV8 PALcode on the ES47, 2 chips of 4 TPUs: PALcode %#x "
          "bytes, test program %#x, %.1f s" % (len(pal), len(code), secs))
    bufs = parse(text)
    if not bufs or len(bufs) != N_BLOCKS:
        print("  no results (%s blocks of %d; see %s)"
              % (len(bufs) if bufs else 0, N_BLOCKS, work))
        for ln in (text.splitlines() + emu.splitlines())[-15:]:
            print("   |", ln)
        print("  PAL8: FAIL")
        return 1
    passes, fails = check(bufs, lab, emu, text)
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
