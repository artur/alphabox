"""The conformance image: a minimal PALcode, a driver loop, the cases and
a UART printer, laid out as the decompressed console image the emulator
loads instead of firmware (System.cpp load_decompressed_console: entry PC,
PAL_BASE, the low 2 MB of memory, the tag "ALPHDC01").

Machine state the image sets up (21264/EV68CB HRM chapter 5):
  I_CTL   SDE[1] (PALshadow registers, as every PALcode runs), IC_EN,
          SPE<1> (the 43-bit KSEG superpage fffffc00.00000000 for code)
  M_CTL   SPE<2,1>: data through the same KSEG, and the 48-bit superpage
          (VA<47:46> = 2) that reaches the I/O space for the UART
  VA_CTL  VA_48 (data addresses are checked as 48-bit)
  IER_CM  zero: kernel mode, every interrupt masked
  PCTX    FPE set, ASN 0
  CC      offset 0x12345678, counter enabled
and the ITB and DTB are emptied, so that every access in kernel mode is a
superpage access; only the user-mode cases (CALL_PAL 02) fill the TBs.

The PALcode records every exception in a trap record (TR) and resumes:
faults and traps at EXC_ADDR + 4 (skipping the instruction), interrupts at
EXC_ADDR after clearing SIRR, IER and ASTRR (only the JIT cases raise
any), an IACV on the kernel-only code page in kernel mode at R17, and
anything it cannot resume from (ITB miss, any other IACV, machine check,
double miss, reset) ends the run through the native `fatal` path.

On the native EV8 (the `ev8n` row) the image has a PALcode of the 21464's
own interface instead, with the same services and trap record
(build_pal8, below).
"""
import contextlib
import struct

import alpha_asm as A
import rows
from alpha_asm import (IPR, OP, R31, Asm, bra, call_pal, hw_ld, hw_mfpr,
                       hw_mtpr, hw_ret, hw_st, jmp, mem, opl, opr)

KSEG = 0xFFFFFC0000000000          # SPE<1>: VA<47:41> = 7E -> PA<40:0>
SEG48 = 0xFFFF800000000000         # SPE<2>: VA<47:46> = 2 -> PA<43:0>
UART_PA = 0x801FC0003F8            # Tsunami PCI0 I/O + COM1 THR
UART_VA = SEG48 | UART_PA

PAL_PA = 0x100000                  # PAL_BASE (not 0x8000: that is the native VMS PAL)
CODE_PA = 0x108000                 # driver, printer and cases
CODE_TOP = 0x1C0000
POOL_PA = 0x1C0000                 # literal pool, R27 = its KSEG address + 32 KB
POOL_ENTRIES = 8192
TR_PA = 0x1D0000                   # trap record
SCR_PA = 0x1D2000                  # three 8 KB scratch pages for the memory cases
USER_PA = 0x1D8000                 # one 8 KB page of user-mode code
# User mode (CALL_PAL 02 installs these translations, HRM 5.2.3/5.3.2):
USER_CODE_VA = 0x20000000          # ITB: USER_PA, read in every mode
USER_DATA_VA = 0x20002000          # DTB: scratch page 2, read/write in every mode
USER_KONLY_VA = 0x20004000         # DTB: scratch page 2, kernel read/write only
USER_FAULT_VA = 0x20006000         # DTB: scratch page 2, every mode, FOR and FOW set
KCODE_PA = 0x1DA000                # one 8 KB page of code only kernel mode may run
KCODE_VA = 0x20008000              # ITB: KCODE_PA, KRE only
JPAL_SCR_PA = SCR_PA + 0x5F00      # scratch page 2: the PAL service's HW_ST/HW_LD
IMAGE_TOP = 0x200000               # the decompressed image holds 2 MB
BUF_A_PA = 0x400000                # first pass: always interpreted
BUF_B_PA = 0x800000                # the other passes: compiled on a JIT lane
# The loader patches these words of the ES40 console (Es40.cpp); stay off them.
CONSOLE_PATCHES = (0x14248, 0x14288, 0x142C8, 0x68320, 0x8BB78, 0x8BC0C, 0x8BC94)

TRAP_COUNT = "trap count"
TRAPS_IN_PASS = "traps in the pass"

# Registers the driver reserves; cases use R0-R23.
R_TR, R_PASS, R_SCR, R_POOL, R_RES, R_UART = 24, 25, 26, 27, 28, 29

# Trap record (TR) layout, quadword offsets in bytes.
TR_COUNT, TR_VEC, TR_EXC_ADDR, TR_EXC_SUM, TR_MM_STAT, TR_VA, TR_TOTAL = (
    0, 8, 16, 24, 32, 40, 48)

# Exception entry offsets (HRM Table 6-8).
VECTORS = {
    0x100: "DTBM_DOUBLE_3", 0x180: "DTBM_DOUBLE_4", 0x200: "FEN",
    0x280: "UNALIGN", 0x300: "DTBM_SINGLE", 0x380: "DFAULT", 0x400: "OPCDEC",
    0x480: "IACV", 0x500: "MCHK", 0x580: "ITB_MISS", 0x600: "ARITH",
    0x680: "INTERRUPT", 0x700: "MT_FPCR", 0x780: "RESET",
}
FATAL_VECTORS = (0x100, 0x180, 0x480, 0x500, 0x580, 0x780)

# PALmode shadow registers (R4-R7, R20-R23 with I_CTL[SDE<1>] set).
P4, P5, P6, P7, P20, P21, P22, P23 = 4, 5, 6, 7, 20, 21, 22, 23


def kseg(pa):
    return KSEG | pa


def lda(ra, rb, d):
    return mem(OP["LDA"], ra, rb, d)


def ldq(ra, rb, d):
    return mem(OP["LDQ"], ra, rb, d)


def stq(ra, rb, d):
    return mem(OP["STQ"], ra, rb, d)


def stb(ra, rb, d):
    return mem(OP["STB"], ra, rb, d)


def bis(ra, rb, rc):
    return opr(0x11, 0x20, ra, rb, rc)


def build_pal(tr_pa, main_va, fatal_va):
    """The PALcode, PAL_BASE-relative, as one 32 KB block of words."""
    words = {}

    # Reset entry (PAL_BASE + 0): jump to the initialisation code.
    init = Asm(PAL_PA + 0x4000)
    init.li(1, 0x100096)                   # IC_EN=3, SPE<1>, SDE<1>, bit 20
    init.emit(hw_mtpr(1, IPR["I_CTL"]))
    init.li(1, 0xC)                        # M_CTL SPE<2,1>
    init.emit(hw_mtpr(1, IPR["M_CTL"]))
    init.li(1, 0x2)                        # VA_CTL VA_48
    init.emit(hw_mtpr(1, IPR["VA_CTL"]))
    init.emit(hw_mtpr(R31, IPR["IER_CM"]))  # kernel, interrupts off
    init.li(1, 0x4)                        # PCTX: ASN 0, AST 0, FPE 1
    init.emit(hw_mtpr(1, IPR["PCTX"] | 0x1F))
    init.emit(hw_mtpr(R31, IPR["DTB_ASN0"]))
    init.emit(hw_mtpr(R31, IPR["DTB_ASN1"]))
    init.emit(hw_mtpr(R31, IPR["ITB_IA"]))
    init.emit(hw_mtpr(R31, IPR["DTB_IA"]))
    init.li(1, 0x1234567800000000)         # CC<63:32> offset
    init.emit(hw_mtpr(1, IPR["CC"]))
    init.li(1, 1 << 32)                    # CC_CTL: counter 0, enabled
    init.emit(hw_mtpr(1, IPR["CC_CTL"]))
    init.li(P20, tr_pa)                    # p20: the trap record, physical
    init.li(1, main_va)
    init.emit(hw_ret(1))

    # Common exception handler: p4 holds the vector offset.
    com = Asm(PAL_PA + 0x4800)
    com.emit(hw_mfpr(P5, IPR["EXC_ADDR"]))
    com.emit(hw_mfpr(P6, IPR["EXC_SUM"]))
    com.emit(hw_mfpr(P7, IPR["MM_STAT"]))
    com.emit(hw_mfpr(P21, IPR["VA"]))
    com.emit(hw_ld(P22, P20, TR_COUNT))
    com.emit(opl(0x10, 0x20, P22, 1, P22))
    com.emit(hw_st(P22, P20, TR_COUNT))
    com.emit(hw_ld(P22, P20, TR_TOTAL))
    com.emit(opl(0x10, 0x20, P22, 1, P22))
    com.emit(hw_st(P22, P20, TR_TOTAL))
    com.emit(hw_st(P4, P20, TR_VEC))
    com.emit(hw_st(P5, P20, TR_EXC_ADDR))
    com.emit(hw_st(P6, P20, TR_EXC_SUM))
    com.emit(hw_st(P7, P20, TR_MM_STAT))
    com.emit(hw_st(P21, P20, TR_VA))
    com.emit(lda(P22, P4, -0x680))         # an interrupt resumes where it hit
    com.branch(OP["BEQ"], P22, "intr")
    # An IACV on the kernel-only code page (the JIT cases' user-mode fetch
    # from it) returns to kernel mode at R17; any other IACV is fatal.
    com.emit(lda(P22, P4, -0x480))
    com.branch(OP["BNE"], P22, "not_iacv")
    com.emit(opl(0x12, 0x34, P5, 13, P22))  # SRL EXC_ADDR,#13
    com.li(P21, KCODE_VA >> 13)
    com.emit(opr(0x10, 0x2D, P22, P21, P22))  # CMPEQ
    com.branch(OP["BEQ"], P22, "fatal")
    com.emit(hw_mtpr(R31, IPR["CM"]))     # kernel mode
    com.emit(hw_ret(17, stall=True))
    com.label("not_iacv")
    for v in FATAL_VECTORS:
        com.emit(lda(P22, P4, -v))
        com.branch(OP["BEQ"], P22, "fatal")
    com.emit(opl(0x10, 0x20, P5, 4, P5))   # fault or trap: skip the instruction
    com.label("resume")
    com.emit(hw_ret(P5))
    # Interrupt: no request stays pending and nothing stays enabled (the JIT
    # cases raise a software interrupt or a kernel AST, then return here).
    com.label("intr")
    com.emit(hw_mtpr(R31, IPR["SIRR"]))
    com.emit(hw_mtpr(R31, IPR["IER"]))
    com.emit(hw_mtpr(R31, IPR["PCTX"] | 0x04))  # ASTRR
    com.emit(hw_ret(P5, stall=True))
    com.label("fatal")
    com.li(P5, fatal_va)
    com.emit(hw_ret(P5))

    # CALL_PAL entries (HRM 6.8.1): privileged at 0x2000 + func*0x40,
    # unprivileged at 0x3000 + (func & 0x3f)*0x40. Every entry not given a
    # service below counts itself in the trap record (vector 0x2000+off,
    # EXC_ADDR = the linkage register) and returns.
    cp = Asm(PAL_PA + 0x4C00)
    cp.emit(hw_ld(P22, P20, TR_COUNT))
    cp.emit(opl(0x10, 0x20, P22, 1, P22))
    cp.emit(hw_st(P22, P20, TR_COUNT))
    cp.emit(hw_ld(P22, P20, TR_TOTAL))
    cp.emit(opl(0x10, 0x20, P22, 1, P22))
    cp.emit(hw_st(P22, P20, TR_TOTAL))
    cp.emit(hw_st(P4, P20, TR_VEC))
    cp.emit(hw_st(P23, P20, TR_EXC_ADDR))
    cp.emit(hw_ret(P23))

    umode = Asm(PAL_PA + 0x5000)
    # CALL_PAL 8A: arithmetic on the PALshadow and the main registers, a
    # physical store and load, a branch and a CMOV, all in PALmode (the JIT
    # compiles PALmode blocks with R4-R7/R20-R23 mapped to the shadow bank).
    #   p4 = R16 + 1; p5 = p4 * 3; p6 = p5 XOR R17; HW_ST p6; p21 = HW_LD;
    #   R0 = p21 + p4, + 7 unless R16 <u R17;
    #   R1 = R16 <u R17 ? 4 * p4 + p5 : p6
    jpal = Asm(PAL_PA + 0x5800)
    jpal.emit(opl(0x10, 0x20, 16, 1, P4))      # ADDQ R16,#1,p4
    jpal.emit(opl(0x13, 0x20, P4, 3, P5))      # MULQ p4,#3,p5
    jpal.emit(opr(0x11, 0x40, P5, 17, P6))     # XOR p5,R17,p6
    jpal.li(P7, JPAL_SCR_PA)
    jpal.emit(hw_st(P6, P7, 0))
    jpal.emit(hw_ld(P21, P7, 0))
    jpal.emit(opr(0x10, 0x20, P21, P4, 0))     # ADDQ p21,p4,R0
    jpal.emit(opr(0x10, 0x1D, 16, 17, P22))    # CMPULT R16,R17,p22
    jpal.emit(bra(OP["BNE"], P22, 1))
    jpal.emit(opl(0x10, 0x20, 0, 7, 0))        # ADDQ R0,#7,R0
    jpal.emit(opr(0x10, 0x22, P4, P5, 1))      # S4ADDQ p4,p5,R1
    jpal.emit(opr(0x11, 0x24, P22, P6, 1))     # CMOVEQ p22,p6,R1
    jpal.emit(hw_ret(P23))
    blocks = [init, com, cp, jpal]
    for off in VECTORS:
        v = Asm(PAL_PA + off)
        v.emit(lda(P4, R31, off))
        v.emit(bra(OP["BR"], R31, (com.base - (v.here + 4)) // 4))
        blocks.append(v)
    for func in list(range(0x00, 0x40)) + list(range(0x80, 0xC0)):
        off = 0x2000 + ((func & 0x80) << 5) + ((func & 0x3F) << 6)
        e = Asm(PAL_PA + off)
        if func == 0x00:                   # HALT: exit_on_pal_halt ends the run
            e.emit(bra(OP["BR"], R31, -1))
        elif func == 0x01:                 # service: FPE <- R16<0>
            e.emit(opl(0x12, 0x39, 16, 2, P22))
            e.emit(hw_mtpr(P22, IPR["PCTX"] | A.PCTX_FPE))
            e.emit(hw_ret(P23))
        elif func == 0x02:                 # service: user mode at R16
            e.emit(bra(OP["BR"], R31, (umode.base - (e.here + 4)) // 4))
        elif func == 0x84:                 # service: kernel mode at R17
            e.emit(hw_mtpr(R31, IPR["CM"]))
            e.emit(hw_ret(17))
        elif func == 0x83:                 # service: R0 <- linkage, R1 <- EXC_ADDR
            e.emit(bis(P23, P23, 0))
            e.emit(hw_mfpr(1, IPR["EXC_ADDR"]))
            e.emit(hw_ret(P23))
        # The JIT cases' services (cases.py, group jit*). An update of an
        # Ibox IPR returns with HW_RET/STALL, so that it "affects all
        # instructions following the return path" (HRM D.8, Restriction 11).
        elif func == 0x85:                 # SIRR <- R16, IER <- R17
            e.emit(hw_mtpr(16, IPR["SIRR"]))
            e.emit(hw_mtpr(17, IPR["IER"]))
            e.emit(hw_ret(P23, stall=True))
        elif func == 0x86:                 # PCTX ASTER, ASTRR <- R16, IER <- R17
            e.emit(hw_mtpr(16, IPR["PCTX"] | 0x06))
            e.emit(hw_mtpr(17, IPR["IER"]))
            e.emit(hw_ret(P23, stall=True))
        elif func == 0x87:                 # I_CTL <- R16, return to R17
            e.emit(hw_mtpr(16, IPR["I_CTL"]))
            e.emit(hw_ret(17, stall=True))
        elif func == 0x88:                 # R0 <- R4 of PALmode's bank, return to R17
            e.emit(bis(4, 4, 0))
            e.emit(hw_ret(17))
        elif func == 0x89:                 # PALshadow R4 <- R16
            e.emit(bis(16, 16, P4))
            e.emit(hw_ret(P23))
        elif func == 0x8A:                 # the arithmetic above
            e.emit(bra(OP["BR"], R31, (jpal.base - (e.here + 4)) // 4))
        elif func == 0x8C:                 # IER <- R17
            e.emit(hw_mtpr(17, IPR["IER"]))
            e.emit(hw_ret(P23, stall=True))
        elif func in (0x8D, 0x8E):         # 8D: HW_STL R17 -> phys R16; both IC_FLUSH
            if func == 0x8D:
                e.emit(hw_st(17, 16, 0, quad=False))
            # HRM D.20: a conditional branch between IC_FLUSH and the
            # HW_RET/STALL, which the flush waits for.
            e.emit(hw_mtpr(R31, IPR["IC_FLUSH"]))
            e.emit(bra(OP["BNE"], R31, 0))
            e.emit(hw_ret(P23, stall=True))
        else:
            e.emit(lda(P4, R31, off))
            e.emit(bra(OP["BR"], R31, (cp.base - (e.here + 4)) // 4))
        assert len(e.words) <= 0x40 // 4, hex(func)
        blocks.append(e)

    # The body of CALL_PAL 02: the user-mode translations, then CM = user.
    for va, pte, itb in (
            (USER_CODE_VA, USER_PA | 0xF00 | 0x10, True),
            (KCODE_VA, KCODE_PA | 0x100 | 0x10, True),
            (USER_DATA_VA, ((SCR_PA + 0x4000) >> 13) << 32 | 0xFF10, False),
            (USER_KONLY_VA, ((SCR_PA + 0x4000) >> 13) << 32 | 0x1110, False),
            (USER_FAULT_VA, ((SCR_PA + 0x4000) >> 13) << 32 | 0xFF16, False)):
        umode.li(P4, va)
        umode.li(P5, pte)
        if itb:
            umode.emit(hw_mtpr(P4, IPR["ITB_TAG"]))
            umode.emit(hw_mtpr(P5, IPR["ITB_PTE"]))
        else:
            for ipr in ("DTB_TAG0", "DTB_TAG1"):
                umode.emit(hw_mtpr(P4, IPR[ipr]))
            for ipr in ("DTB_PTE0", "DTB_PTE1"):
                umode.emit(hw_mtpr(P5, IPR[ipr]))
    umode.emit(lda(P6, R31, 3 << 3))   # CM = user
    umode.emit(hw_mtpr(P6, IPR["CM"]))
    umode.emit(hw_ret(16))
    blocks.append(umode)

    # The reset entry itself.
    entry = Asm(PAL_PA)
    entry.emit(bra(OP["BR"], R31, (init.base - (PAL_PA + 4)) // 4))
    blocks.append(entry)

    for b in blocks:
        for i, w in enumerate(b.finish()):
            a = b.base + 4 * i
            assert a not in words, hex(a)
            words[a] = w
    return words


# --- The EV8 PALcode: the same services on the 21464's own interface ------------
#
# For the `ev8n` row (docs/platforms/ev8-native.md, S11): the 21464's IPRs
# (alpha_asm.IPR8), HW_MFPR's Rc, RET for every return, IFETCHB after the
# IPR writes of a flow (Restriction 1), and two shadow registers, S0 and S1,
# in place of the 21264's eight, so the handlers spill R1-R5 into the trap
# record's save area. Every TPU enters the reset entry (0xB00, N17); TPU 0
# goes to the driver, TPUs 1-3 park on a mailbox of their own with LDQ_ARM
# and QUIESCE (WATCH_EN set, no timeout) and, given an address there, start
# at it in kernel mode with their own trap record (TR_PA + TR8_BLOCK x TPU).
# The machine state is the 21264 image's: kernel mode, interrupts off, FP
# on, ASN 0, TPU group 0, 43-bit VA and 8 KB pages, the code in SPE43 (the
# KSEG), data through SPE52 (the UART: SEG52), SPE43 and SPE32, the cycle
# counter's offset 0x12345678, both TBs empty; and QUIESCE_TIMEOUT 0xFFFF0
# cycles with both enables (the quiesce group's timeout, QUIESCE8_TIMEOUT).
#
# The trap record keeps the 21264 image's fields, so that the cases compare
# what they compare on every row: TR_EXC_ADDR is the restart PC the PALcode
# returns through (DTBMS_RET_ADDR for the DTB single miss), TR_EXC_SUM the
# 21264's form (REG<12:8>: the instruction's Rc for ARITH, Ra otherwise;
# INT<7>). The 21464's own values follow it (TR8_*). An ARITH or MT_FPCR
# trap resumes at EXC_ADDR, which is the next instruction [IDS p. 485];
# BAD_JUMP_IVA and DTBM_SINGLE(_CONS) after the instruction, as every fault;
# an ITB miss in the probe region (PROBE_VA, 64 KB) returns to R17 in
# kernel mode, as an IACV on the kernel-only code page does.

SEG52 = 0xFFF8000000000000          # SPE52: VA<63:50> = 0x3FFE -> PA<47:0>
SEG32 = 0xFFFFFFFF80000000          # SPE32: VA<63:30> = 0x3FFFFFFFE -> PA<29:0>
UART_VA8 = SEG52 | UART_PA
TR8_EXC_ADDR, TR8_EXC_SUM, TR8_DTBMS, TR8_VA_FORM, TR8_IVA_FORM = (
    56, 64, 72, 80, 88)
TR8_SAVE = 0x80                     # R1-R5 while a handler runs
TR8_BLOCK = 0x400                   # TPU t's trap record: TR_PA + t x 0x400
TR8_MAILBOX = 0x1000                # TPU t's start word: + 0x40 x t
PAL8_SNIP_PA = 0x1E0000             # PALmode snippets (CALL_PAL 91)
PAL8_SNIP_TOP = 0x200000
PROBE_VA = 0x20010000               # ITB probes: a miss here returns to R17
QUIESCE8_TIMEOUT = 0xFFFF0          # QUIESCE_TIMEOUT<19:4>, its largest
I_CTL8 = 2 << 24                    # SPE43
M_CTL8 = 7 << 24                    # SPE52, SPE43, SPE32
# The entries the PALcode does not return from (chapter 22's).
FATAL8 = (0x000, 0x080, 0x500, 0x780, 0x980, 0xA00, 0xA80, 0xB00, 0xB80)
# The reset record (in each TPU's trap record): these IPRs as the reset
# left them, then the entry the TPU came through.
RESET_IPRS = ("i_ctl", "m_ctl", "i_mode", "m_mode", "i_pctx", "m_pctx",
              "ier_cm", "sirr", "isum", "cc", "quiesce_timeout", "pal_base",
              "pal_temp1", "pal_temp2", "vctl", "tpu_cnfg", "cpu_cnfg",
              "cpu_base", "dc_ctl", "dc_stat", "ic_stat", "exc_addr",
              "exc_sum", "m_stat", "va", "dtbms_ret_addr")
TR8_RESET = 0x200
TR8_RESET_ENTRY = TR8_RESET + 8 * len(RESET_IPRS)
VECTORS8 = dict(VECTORS)
VECTORS8.update({0x100: "DTBM_DOUBLE", 0x180: "DTBM_DOUBLE_ALT",
                 0x780: "IMCHK", 0x800: "DTBM_SINGLE_CONS",
                 0x880: "ITB_MISS_CONS", 0x900: "BAD_JUMP_IVA",
                 0x980: "FAST_RESET", 0xA00: "WAKEUP", 0xA80: "TPU_RESTART",
                 0xB80: "DST_NXM"})


def li8(asm, ra, rb, value):
    """asm.li for a register named `ra` as destination and `rb` as operand
    B: a shadow register's two numbers (S0: 24 and 22), or the same number
    twice for any other register."""
    value &= (1 << 64) - 1
    sval = value - (1 << 64) if value >> 63 else value

    def add32(base_b, v):
        lo = v & 0xFFFF
        lo = lo - 0x10000 if lo >= 0x8000 else lo
        hi = (v - lo) >> 16
        assert -0x8000 <= hi < 0x8000
        if hi:
            asm.emit(mem(OP["LDAH"], ra, base_b, hi))
            base_b = rb
        if lo or base_b != rb:
            asm.emit(mem(OP["LDA"], ra, base_b, lo))

    if -0x80000000 <= sval < 0x7FFF8000:
        add32(R31, sval)
        return
    lo = value & 0xFFFFFFFF
    lo_s = lo - (1 << 32) if lo >> 31 else lo
    hi = (sval - lo_s) >> 32
    add32(R31, hi)
    asm.emit(opl(0x12, 0x39, ra, 32, ra))   # SLL: Ra reads as operand A
    if lo_s:
        add32(rb, lo_s)


def build_pal8(tr_pa, main_va, fatal_va, by_chip=False, mailbox_pa=None):
    """The EV8 PALcode, PAL_BASE-relative, as {address: word}.

    With `by_chip` (a machine of several chips, test/cpu/marvel8_mp.py),
    each TPU's number is CPU_BASE x 4 + TPU_ID, the WHAMI of
    docs/platforms/ev8-native.md N25: its trap record is tr_pa + that x
    TR8_BLOCK, its start word mailbox_pa + that x 0x40, and only number 0
    goes on to `main_va`; the others park. Without it the number is TPU_ID,
    as on the one chip of tpu_mp.py and the oracle."""
    if mailbox_pa is None:
        mailbox_pa = tr_pa + TR8_MAILBOX
    I = A.IPR8
    mf, mt = A.hw_mfpr8, A.hw_mtpr8
    S0A, S1A, S0B, S1B = A.S0A, A.S1A, A.S0B, A.S1B
    words = {}

    def ld(ra, disp):           # HW_LDQ/P ra <- [S0 + disp]
        return A.hw_ld8(ra, S0B, disp)

    def st(ra, disp):
        return A.hw_st8(ra, S0B, disp)

    def save(a, regs):
        for k, r in enumerate(regs):
            a.emit(st(r, TR8_SAVE + 8 * k))

    def restore(a, regs):
        for k, r in enumerate(regs):
            a.emit(ld(r, TR8_SAVE + 8 * k))

    def bump(a, disp):          # [S0 + disp] += 1, through R1
        a.emit(ld(1, disp))
        a.emit(opl(0x10, 0x20, 1, 1, 1))
        a.emit(st(1, disp))

    # Reset (every TPU): the machine state, then TPU 0 to the driver.
    init = Asm(PAL_PA + 0x4000)
    # This TPU's trap record (R1), and in it the IPRs as reset left them
    # and the entry taken (S1, set by the entry's first instruction).
    init.emit(mf(2, I["tpu_cnfg"]))
    init.emit(opl(0x12, 0x34, 2, 56, 2))    # SRL: TPU_ID
    if by_chip:                             # + CPU_BASE x 4 (N25)
        init.emit(mf(3, I["cpu_base"]))
        init.emit(opl(0x12, 0x39, 3, 2, 3))
        init.emit(opr(0x10, 0x20, 2, 3, 2))
    init.emit(opl(0x12, 0x39, 2, 10, 3))    # SLL: x TR8_BLOCK (0x400)
    li8(init, 1, 1, tr_pa)
    init.emit(opr(0x10, 0x20, 1, 3, 1))
    for k, name in enumerate(RESET_IPRS):
        init.emit(mf(4, I[name]))
        init.emit(A.hw_st8(4, 1, TR8_RESET + 8 * k))
    init.emit(A.hw_st8(S1A, 1, TR8_RESET_ENTRY))
    init.emit(mt(1, I["pal_temp1"]))
    li8(init, 4, 4, I_CTL8)
    init.emit(mt(4, I["i_ctl"]))
    li8(init, 4, 4, M_CTL8)
    init.emit(mt(4, I["m_ctl"]))
    init.emit(mt(R31, I["ier_cm"]))         # kernel mode, interrupts off
    init.emit(mt(R31, I["sirr"]))
    init.emit(mt(R31, I["m_mode"]))
    init.emit(mt(R31, I["i_mode"]))
    li8(init, 1, 1, 1 << 19)                # FP_ENABLE, ASN 0, group 0
    init.emit(mt(1, I["i_pctx"]))
    init.emit(mt(R31, I["m_pctx"]))
    init.emit(mem(OP["LDA"], 1, R31, 0x10))  # all groups, the write pointer
    init.emit(mt(1, I["itb_im"]))
    init.emit(mt(1, I["dtb_im"]))
    li8(init, 1, 1, 0x1234567800000001)     # CC: offset, enabled, count 0
    init.emit(mt(1, I["cc"]))
    li8(init, 1, 1, QUIESCE8_TIMEOUT | 3)
    init.emit(mt(1, I["quiesce_timeout"]))
    init.emit(A.ifetchb())
    init.branch(OP["BNE"], 2, "park")
    li8(init, 1, 1, main_va)
    init.emit(A.ret(1))                     # normal mode: Rb<1:0> = 00
    # TPUs 1-3: LDQ_ARM on the mailbox, QUIESCE until it holds an address
    # (WATCH_EN only: no timeout, N17), then start there.
    init.label("park")
    init.emit(opl(0x12, 0x39, 2, 6, 3))     # x 0x40
    li8(init, 4, 4, kseg(mailbox_pa))
    init.emit(opr(0x10, 0x20, 4, 3, 4))
    init.emit(mem(OP["LDA"], 1, R31, 2))
    init.emit(mt(1, I["quiesce_timeout"]))
    init.emit(A.ifetchb())
    init.label("park_loop")
    init.emit(A.mfc(A.MISC["LDQ_ARM"], 5, 4))
    init.branch(OP["BNE"], 5, "go")
    init.emit(A.mfc(A.MISC["QUIESCE"]))
    init.branch(OP["BR"], R31, "park_loop")
    init.label("go")
    li8(init, 1, 1, QUIESCE8_TIMEOUT | 3)
    init.emit(mt(1, I["quiesce_timeout"]))
    init.emit(A.ifetchb())
    init.emit(A.ret(5))

    # The common exception handler: S1 = the vector offset.
    com = Asm(PAL_PA + 0x4800)
    com.emit(mf(S0A, I["pal_temp1"]))
    save(com, [1, 2, 3, 4, 5])
    bump(com, TR_COUNT)
    bump(com, TR_TOTAL)
    com.emit(opr(0x11, 0x20, R31, S1B, 1))  # BIS: r1 = vector
    com.emit(st(1, TR_VEC))
    for reg, ipr, disp in ((1, "exc_addr", TR8_EXC_ADDR),
                           (2, "dtbms_ret_addr", TR8_DTBMS),
                           (3, "exc_sum", TR8_EXC_SUM),
                           (4, "va_form", TR8_VA_FORM),
                           (4, "iva_form", TR8_IVA_FORM),
                           (4, "m_stat", TR_MM_STAT),
                           (4, "va", TR_VA)):
        com.emit(mf(reg, I[ipr]))
        com.emit(st(reg, disp))
    # The restart PC: DTBMS_RET_ADDR for the DTB single miss, else EXC_ADDR.
    for v in (0x300, 0x800):
        com.emit(mem(OP["LDA"], 5, S1B, -v))
        com.branch(OP["BEQ"], 5, "dtbm")
    com.branch(OP["BR"], R31, "restart")
    com.label("dtbm")
    com.emit(opr(0x11, 0x20, R31, 2, 1))
    com.label("restart")
    com.emit(st(1, TR_EXC_ADDR))
    # EXC_SUM in the 21264's form: <6:0>, INT<13> -> <7>, REG<12:8>.
    com.emit(opl(0x11, 0x00, 3, 0x7F, 2))
    com.emit(opl(0x12, 0x34, 3, 13, 4))
    com.emit(opl(0x11, 0x00, 4, 1, 4))
    com.emit(opl(0x12, 0x39, 4, 7, 4))
    com.emit(opr(0x11, 0x20, 2, 4, 2))
    com.emit(opl(0x12, 0x34, 3, 24, 4))     # Ra
    com.emit(mem(OP["LDA"], 5, S1B, -0x600))
    com.branch(OP["BNE"], 5, "not_arith")
    com.emit(opl(0x12, 0x34, 3, 14, 4))     # Rc
    com.label("not_arith")
    com.emit(opl(0x11, 0x00, 4, 31, 4))
    com.emit(opl(0x12, 0x39, 4, 8, 4))
    com.emit(opr(0x11, 0x20, 2, 4, 2))
    com.emit(st(2, TR_EXC_SUM))
    com.emit(mem(OP["LDA"], 5, S1B, -0x680))
    com.branch(OP["BEQ"], 5, "intr")
    com.emit(mem(OP["LDA"], 5, S1B, -0x480))
    com.branch(OP["BEQ"], 5, "iacv")
    for v in (0x580, 0x880):
        com.emit(mem(OP["LDA"], 5, S1B, -v))
        com.branch(OP["BEQ"], 5, "itbmiss")
    for v in FATAL8:
        com.emit(mem(OP["LDA"], 5, S1B, -v))
        com.branch(OP["BEQ"], 5, "fatal")
    for v in (0x600, 0x700):                # restart at the next instruction
        com.emit(mem(OP["LDA"], 5, S1B, -v))
        com.branch(OP["BEQ"], 5, "resume")
    com.emit(opl(0x10, 0x20, 1, 4, 1))      # a fault: skip the instruction
    com.label("resume")
    com.emit(opr(0x11, 0x20, R31, 1, S1A))  # S1 = the restart PC
    restore(com, [1, 2, 3, 4, 5])
    com.emit(A.ret(S1B))
    com.label("intr")                       # nothing stays pending or enabled
    com.emit(mt(R31, I["sirr"]))
    com.emit(mt(R31, I["ier_cm"]))
    com.emit(A.ifetchb())
    com.branch(OP["BR"], R31, "resume")
    com.label("iacv")                       # the kernel-only code page
    com.emit(opl(0x12, 0x34, 1, 13, 2))
    li8(com, 3, 3, KCODE_VA >> 13)
    com.emit(opr(0x10, 0x2D, 2, 3, 2))      # CMPEQ
    com.branch(OP["BEQ"], 2, "fatal")
    com.emit(mt(R31, I["i_mode_cur"]))      # kernel mode
    com.emit(A.ifetchb())
    com.branch(OP["BR"], R31, "to_r17")
    com.label("itbmiss")                    # the probe region
    com.emit(opl(0x12, 0x34, 1, 16, 2))
    li8(com, 3, 3, PROBE_VA >> 16)
    com.emit(opr(0x10, 0x2D, 2, 3, 2))
    com.branch(OP["BEQ"], 2, "fatal")
    com.label("to_r17")
    com.emit(opr(0x11, 0x20, R31, 17, S1A))
    restore(com, [1, 2, 3, 4, 5])
    com.emit(A.ret(S1B))
    com.label("fatal")
    li8(com, 1, 1, fatal_va)
    com.emit(A.ret(1))

    # CALL_PAL entries [IDS p. 499]: privileged at 0x2000 + func x 0x40,
    # unprivileged at 0x3000 + (func & 0x3f) x 0x40; S1 holds the return PC.
    # One not given a service counts itself (vector 0x2000 + offset,
    # EXC_ADDR the return PC) and returns.
    cp = Asm(PAL_PA + 0x4C00)               # R1 (saved) = the offset
    cp.emit(st(1, TR_VEC))
    bump(cp, TR_COUNT)
    bump(cp, TR_TOTAL)
    cp.emit(opr(0x11, 0x20, R31, S1B, 1))
    cp.emit(st(1, TR_EXC_ADDR))
    restore(cp, [1])
    cp.emit(A.ret(S1B))

    # CALL_PAL 02: the user-mode translations (the 21264 image's, in the
    # 21464's PTE forms: the PFN in <63:32>, VALID <0> in the DTB's), then
    # CM = user, at R16.
    umode = Asm(PAL_PA + 0x5000)
    for va, pte, itb in (
            (USER_CODE_VA, (USER_PA >> 13) << 32 | 0xF00 | 0x10, True),
            (KCODE_VA, (KCODE_PA >> 13) << 32 | 0x100 | 0x10, True),
            (USER_DATA_VA, ((SCR_PA + 0x4000) >> 13) << 32 | 0xFF11, False),
            (USER_KONLY_VA, ((SCR_PA + 0x4000) >> 13) << 32 | 0x1111, False),
            (USER_FAULT_VA, ((SCR_PA + 0x4000) >> 13) << 32 | 0xFF17, False)):
        li8(umode, S0A, S0B, va)
        if itb:
            umode.emit(mt(S0B, I["itb_tag"]))
        else:
            umode.emit(mt(S0B, I["dtb_tag0"]))
            umode.emit(mt(S0B, I["dtb_tag1"]))
        li8(umode, S0A, S0B, pte)
        if itb:
            umode.emit(mt(S0B, I["itb_pte"]))
        else:
            umode.emit(mt(S0B, I["dtb_pte0"]))
            umode.emit(mt(S0B, I["dtb_pte1"]))
        umode.emit(A.ifetchb())
    umode.emit(mem(OP["LDA"], S0A, R31, 3 << 3))  # CM = user
    umode.emit(mt(S0B, I["i_mode_cur"]))
    umode.emit(A.ifetchb())
    umode.emit(A.ret(16))
    blocks = [init, com, cp, umode]

    for off in VECTORS8:
        if off == 0xB00:
            continue
        v = Asm(PAL_PA + off)
        v.emit(mem(OP["LDA"], S1A, R31, off))
        v.emit(bra(OP["BR"], R31, (com.base - (v.here + 4)) // 4))
        blocks.append(v)
    reset = Asm(PAL_PA + 0xB00)             # MILD_RESET, power-up (N11)
    reset.emit(mem(OP["LDA"], S1A, R31, 0xB00))
    reset.emit(bra(OP["BR"], R31, (init.base - (reset.here + 4)) // 4))
    entry0 = Asm(PAL_PA)                    # the SWPPAL/SROM entry
    entry0.emit(mem(OP["LDA"], S1A, R31, 0))
    entry0.emit(bra(OP["BR"], R31, (init.base - (entry0.here + 4)) // 4))
    blocks += [reset, entry0]
    for func in list(range(0x00, 0x40)) + list(range(0x80, 0xC0)):
        off = 0x2000 + ((func & 0x80) << 5) + ((func & 0x3F) << 6)
        e = Asm(PAL_PA + off)
        if func == 0x00:                   # HALT: exit_on_pal_halt ends the run
            e.emit(bra(OP["BR"], R31, -1))
        elif func == 0x01:                 # FP_ENABLE <- R16<0>
            e.emit(opl(0x12, 0x39, 16, 19, S0A))
            e.emit(mt(S0B, I["i_pctx_fpe"]))
            e.emit(A.ifetchb())
            e.emit(A.ret(S1B))
        elif func == 0x02:                 # user mode at R16
            e.emit(bra(OP["BR"], R31, (umode.base - (e.here + 4)) // 4))
        elif func == 0x84:                 # kernel mode at R17
            e.emit(mt(R31, I["i_mode_cur"]))
            e.emit(A.ifetchb())
            e.emit(A.ret(17))
        elif func == 0x83:                 # R0 <- S1 (the return), R1 <- EXC_ADDR
            e.emit(opr(0x11, 0x20, R31, S1B, 0))
            e.emit(mf(1, I["exc_addr"]))
            e.emit(A.ret(S1B))
        elif func == 0x85:                 # SIRR <- R16, IER_CM <- R17
            e.emit(mt(16, I["sirr"]))
            e.emit(mt(17, I["ier_cm"]))
            e.emit(A.ifetchb())
            e.emit(A.ret(S1B))
        elif func == 0x8C:                 # IER_CM <- R17
            e.emit(mt(17, I["ier_cm"]))
            e.emit(A.ifetchb())
            e.emit(A.ret(S1B))
        elif func in (0x8D, 0x8E):         # 8D: HW_STL R17 -> phys R16; IC_FLUSH
            if func == 0x8D:
                e.emit(A.hw_st8(17, 16, 0, length=2))
            e.emit(mt(R31, I["ic_flush"]))
            e.emit(A.ifetchb())
            e.emit(A.ret(S1B))
        elif func == 0x91:                 # a PALmode snippet at physical R16
            e.emit(mt(S1B, I["pal_temp2"]))
            e.emit(opl(0x11, 0x20, 16, 1, S0A))   # BIS: R16 | PALmode
            e.emit(A.ret(S0B))
        else:
            e.emit(mf(S0A, I["pal_temp1"]))
            e.emit(st(1, TR8_SAVE))
            e.emit(mem(OP["LDA"], 1, R31, off))
            e.emit(bra(OP["BR"], R31, (cp.base - (e.here + 4)) // 4))
        assert len(e.words) <= 0x40 // 4, hex(func)
        blocks.append(e)

    for b in blocks:
        for i, w in enumerate(b.finish()):
            a = b.base + 4 * i
            assert a not in words, hex(a)
            words[a] = w
    return words


# How a PALmode snippet (CALL_PAL 91) returns: S1 from PAL_TEMP2, RET.
PAL8_RETURN = [A.hw_mfpr8(A.S1A, A.IPR8["pal_temp2"]), A.ret(A.S1B)]


def emit_print_hex(asm):
    """print_hex: R16 = address, R17 = quadword count, R18 = return.
    Each quadword as 16 hex digits and a newline, through COM1 THR."""
    asm.label("print_hex")
    asm.label("ph_loop")
    asm.emit(ldq(1, 16, 0))
    asm.emit(lda(2, R31, 60))
    asm.label("ph_nib")
    asm.emit(opr(0x12, 0x34, 1, 2, 3))      # SRL r1,r2,r3
    asm.emit(opl(0x11, 0x00, 3, 15, 3))     # AND r3,#15,r3
    asm.emit(opl(0x10, 0x1D, 3, 10, 4))     # CMPULT r3,#10,r4
    asm.emit(opl(0x10, 0x20, 3, 48, 5))     # '0' + n
    asm.emit(opl(0x10, 0x20, 3, 87, 6))     # 'a' - 10 + n
    asm.emit(opr(0x11, 0x24, 4, 6, 5))      # CMOVEQ r4,r6,r5
    asm.emit(stb(5, R_UART, 0))
    asm.emit(opl(0x10, 0x29, 2, 4, 2))      # SUBQ r2,#4,r2
    asm.branch(OP["BGE"], 2, "ph_nib")
    asm.emit(lda(5, R31, 10))
    asm.emit(stb(5, R_UART, 0))
    asm.emit(opl(0x10, 0x20, 16, 8, 16))
    asm.emit(opl(0x10, 0x29, 17, 1, 17))
    asm.branch(OP["BGT"], 17, "ph_loop")
    asm.emit(jmp(0, R31, 18))


def emit_print_text(asm, text):
    """Inline: print a short constant string."""
    for ch in text.encode():
        asm.emit(lda(5, R31, ch))
        asm.emit(stb(5, R_UART, 0))


class Builder:
    """Lays the cases out and holds the literal pool and the result slots.

    A case calls begin(), emits its instructions through `asm`, loads
    operands with load() and stores what it wants checked with store();
    each stored quadword is one result, compared under `mask` with the
    value the manual gives. `known` is (value, reference[, scope]) for a
    documented divergence: that value is reported as KNOWN rather than
    FAIL. Scope "compiled" accepts it from the last pass only (a divergence
    of the JIT's compiled code); the default, "all", from either.

    The cases are for one processor row (rows.py). A case runs only where
    the row has what it needs: the extension of every word it emits
    (rows.word_needs), and whatever it asks for with need(). Elsewhere
    everything the case emitted is taken back and its results are counted
    in `skipped`; part() does the same for a span inside a case. So a case
    that may be skipped must leave nothing a later case relies on.
    """

    def __init__(self, passes, blocks=True, row=None):
        self.passes = passes
        self.row = row or rows.ROWS[rows.BOARD_ROW]
        self.skipped = []       # (case or part, why, results not run)
        self._spans = []        # the open case, then its open parts
        # Every case starts a block of its own (block()): without that a
        # group is one straight run of thousands of instructions, which the
        # JIT records only from the last dispatch batch's boundary to the next
        # branch, and the "compiled" last pass ran 8% of the cases compiled.
        self.blocks = blocks
        self.asm = Asm(CODE_PA)
        self.user = Asm(USER_CODE_VA)
        self.kcode = Asm(KCODE_VA)
        self.pool = {}          # value -> index
        self.results = []       # (case, field, expect, mask, known)
        self.res_base = 0       # byte offset R28 currently points at
        self.case = None
        self.expected_traps = 0
        self.scratch = kseg(SCR_PA)
        self.trap_record = kseg(TR_PA)
        # The native EV8 runs the EV8 PALcode (build_pal8), and reaches the
        # UART through its 52-bit superpage.
        self.ev8n = self.row.family == "EV8"
        self.uart_va = UART_VA8 if self.ev8n else UART_VA
        self.pal8 = Asm(PAL8_SNIP_PA)   # PALmode snippets (pal8_call)

    def va(self):
        """The virtual address of the next instruction."""
        return kseg(self.asm.here)

    def user_snippet(self, words):
        """Place instructions in the user-mode page; returns their VA.
        They run at USER_CODE_VA + offset, entered by CALL_PAL 02."""
        va = self.user.here
        for w in words:
            self.user.emit(w)
        assert self.user.here - USER_CODE_VA <= 0x2000, "user page full"
        return va

    def kcode_snippet(self, words):
        """Place instructions in the kernel-only code page (ITB: KRE only,
        installed by CALL_PAL 02); returns their VA."""
        va = self.kcode.here
        for w in words:
            self.kcode.emit(w)
        assert self.kcode.here - KCODE_VA <= 0x2000, "kernel code page full"
        return va

    def start(self):
        """The next case begins here, before its begin(): what a case emits
        ahead of the block begin() starts (operand loads at the end of the
        previous block) is still its own, and goes with it if skipped."""
        self._close_all()
        self._open(None)

    def begin(self, name):
        if len(self._spans) != 1 or self._spans[0]["name"] is not None:
            self._close_all()
            self._open(name)
        self._spans[0]["name"] = name
        self.case = name
        if self.blocks:
            self.block()

    @contextlib.contextmanager
    def part(self, name):
        """A span of the current case that is skipped on its own when the
        row lacks what it needs, the rest of the case still running."""
        assert self._spans, "a part outside a case"
        self._open(name)
        depth = len(self._spans)
        yield
        assert len(self._spans) == depth
        self._close()

    def need(self, what):
        """The case (or part) being emitted needs `what` of the row
        (rows.py: an extension, rows.Unused(word), rows.UNUSED_MISC_NOP)."""
        assert self._spans, "a need outside a case"
        self._spans[-1]["needs"].add(what)

    def _open(self, name):
        self._spans.append({
            "name": name, "needs": set(), "asm": len(self.asm.words),
            "fixups": len(self.asm.fixups), "user": len(self.user.words),
            "kcode": len(self.kcode.words), "results": len(self.results),
            "pool": len(self.pool), "traps": self.expected_traps,
            "pal8": len(self.pal8.words),
            "res_base": self.res_base})

    def _close(self):
        s = self._spans.pop()
        needs = set(s["needs"])
        for asm, n in ((self.asm, s["asm"]), (self.user, s["user"]),
                       (self.kcode, s["kcode"])):
            needs.update(filter(None, map(rows.word_needs, asm.words[n:])))
        why = sorted(filter(None, (self.row.lacks(n) for n in needs)))
        if not why:
            return
        # Take back everything the span emitted and declared.
        here = self.asm.base + 4 * s["asm"]
        del self.asm.words[s["asm"]:]
        del self.asm.fixups[s["fixups"]:]
        for lab in [k for k, v in self.asm.labels.items() if v >= here]:
            del self.asm.labels[lab]
        del self.user.words[s["user"]:]
        del self.kcode.words[s["kcode"]:]
        self.skipped.append((s["name"], "; ".join(why),
                             len(self.results) - s["results"]))
        del self.results[s["results"]:]
        self.pool = {v: i for v, i in self.pool.items() if i < s["pool"]}
        del self.pal8.words[s["pal8"]:]
        self.expected_traps = s["traps"]
        self.res_base = s["res_base"]

    def _close_all(self):
        while self._spans:
            self._close()

    def block(self):
        """End the block here: BR R31 over one word. The JIT records a block
        from a dispatch to the first instruction whose successor is not the
        next word (a BR to the next instruction does not end it), so the
        instructions after this start a block that is the same in every pass
        and compiles once it has run compile_after times (16); a span that a
        dispatch batch's budget cuts is not recorded at all. The word skipped
        is opcode 01, which would show as an unexpected OPCDEC if run."""
        self.asm.emit(bra(OP["BR"], R31, 1))
        self.asm.emit(0x01 << 26)

    def emit(self, word):
        return self.asm.emit(word)

    def pal8_snippet(self, words):
        """PALmode code for the EV8 PALcode's CALL_PAL 91, which runs it at
        its physical address in PALmode; it returns to the instruction after
        the CALL_PAL (PAL8_RETURN). Returns the address."""
        pa = self.pal8.here
        for w in list(words) + PAL8_RETURN:
            self.pal8.emit(w)
        return pa

    def pal8_call(self, words):
        """Run `words` in PALmode now (the native EV8's image only): R16
        is the snippet's address, the others are the case's. Elsewhere
        the case is skipped (rows.NATIVE8), and this is taken back with it.
        Returns the snippet's address and the CALL_PAL's VA."""
        pa = self.pal8_snippet(words)
        self.li(16, pa)
        at = self.va()
        self.emit(call_pal(0x91))
        return pa, at

    def li(self, reg, value):
        self.asm.li(reg, value)

    def load(self, reg, value):
        value &= (1 << 64) - 1
        if value not in self.pool:
            self.pool[value] = len(self.pool)
        assert len(self.pool) <= POOL_ENTRIES, "literal pool full"
        self.asm.emit(ldq(reg, R_POOL, 8 * self.pool[value] - 32768))

    def slot(self, field, expect, mask=(1 << 64) - 1, known=None):
        """Reserve the next result quadword and return its displacement from
        R28, moving R28 on first when the slot is out of reach."""
        off = 8 * len(self.results)
        if off - self.res_base > 32760:
            self.asm.emit(mem(OP["LDAH"], R_RES, R_RES, 1))
            self.res_base += 0x10000
        if known is not None:
            scope = known[2] if len(known) > 2 else "all"
            known = (known[0] & mask, known[1], scope)
            if known[0] == expect & mask:
                known = None        # the divergence does not show in this field
        self.results.append((self.case, field, expect & mask, mask, known))
        return off - self.res_base

    def store(self, reg, field, expect, mask=(1 << 64) - 1, known=None):
        d = self.slot(field, expect, mask, known)
        self.asm.emit(stq(reg, R_RES, d))

    def trap_clear(self):
        """Zero the trap record before the instruction under test, so a
        field read after it is this instruction's or zero."""
        for off in (TR_COUNT, TR_VEC, TR_EXC_ADDR, TR_EXC_SUM, TR_VA):
            self.asm.emit(stq(R31, R_TR, off))

    def trap_check(self, vector=None, exc_addr=None, exc_sum=None,
                   exc_sum_mask=0x1FFF, va=None, known=None):
        """Store the trap record after the instruction under test: the
        count, and, when a trap is expected, the vector, EXC_ADDR and the
        masked EXC_SUM and VA asked for. `known` = (count, reference[,
        scope]) for a documented divergence in whether the trap is taken."""
        count = 0 if vector is None else 1
        # The 21464's restart PC for ARITH and MT_FPCR is the next
        # instruction's, which EXC_ADDR holds [IDS pp. 485, 630].
        if self.ev8n and vector in (0x600, 0x700) and exc_addr is not None:
            exc_addr += 4
        self.expected_traps += count

        self.asm.emit(ldq(0, R_TR, TR_COUNT))
        self.store(0, TRAP_COUNT, count, known=known)
        if vector is None:
            return
        kz = None if known is None else (0,) + tuple(known[1:])
        self.asm.emit(ldq(0, R_TR, TR_VEC))
        self.store(0, "trap vector", vector, known=kz)
        if exc_addr is not None:
            self.asm.emit(ldq(0, R_TR, TR_EXC_ADDR))
            self.store(0, "EXC_ADDR", exc_addr, known=kz)
        if exc_sum is not None:
            self.asm.emit(ldq(0, R_TR, TR_EXC_SUM))
            self.store(0, "EXC_SUM", exc_sum, mask=exc_sum_mask, known=kz)
        if va is not None:
            self.asm.emit(ldq(0, R_TR, TR_VA))
            self.store(0, "VA", va, known=kz)

    def build(self, emit_cases):
        """Assemble everything; returns the 2 MB memory image."""
        a = self.asm
        a.branch(OP["BR"], R31, "main")
        emit_print_hex(a)

        a.label("fatal")                   # an exception nothing can resume
        a.li(R_UART, self.uart_va)
        emit_print_text(a, "\n@@FATAL\n")
        a.li(16, kseg(TR_PA))
        a.li(17, 7)
        a.branch(OP["BSR"], 18, "print_hex")
        a.emit(call_pal(0x00))

        a.label("main")
        a.li(R_TR, kseg(TR_PA))
        a.li(R_UART, self.uart_va)
        a.emit(bis(R31, R31, R_PASS))
        a.label("pass")
        a.li(R_POOL, kseg(POOL_PA) + 32768)
        a.li(R_SCR, kseg(SCR_PA))
        a.li(R_RES, kseg(BUF_A_PA))
        a.li(1, kseg(BUF_B_PA))
        a.emit(opr(0x11, 0x26, R_PASS, 1, R_RES))   # CMOVNE pass,BUF_B,R28
        a.emit(stq(R31, R_TR, TR_TOTAL))
        self.res_base = 0

        emit_cases(self)
        self._close_all()

        self.case = "harness"
        a.emit(ldq(0, R_TR, TR_TOTAL))
        # The driver checks this against the sum of the cases' own trap
        # counts: a trap outside any case's window is one nobody expected.
        self.store(0, TRAPS_IN_PASS, self.expected_traps)
        a.emit(opl(0x10, 0x20, R_PASS, 1, R_PASS))
        assert self.passes < 256
        a.emit(opl(0x10, 0x4D, R_PASS, self.passes, 1))   # CMPLT
        a.branch(OP["BNE"], 1, "pass")

        nres = len(self.results)
        emit_print_text(a, "\n@@BEGIN %d %d\n" % (nres, self.passes))
        for buf in (BUF_A_PA, BUF_B_PA):
            a.li(16, kseg(buf))
            a.li(17, nres)
            a.branch(OP["BSR"], 18, "print_hex")
            emit_print_text(a, "@@\n")
        emit_print_text(a, "@@END\n")
        a.emit(call_pal(0x00))
        code = a.finish()
        assert CODE_PA + 4 * len(code) <= CODE_TOP, "code too large"
        assert 8 * nres <= BUF_B_PA - BUF_A_PA

        img = bytearray(IMAGE_TOP)
        pal = (build_pal8 if self.ev8n else build_pal)(
            TR_PA, kseg(a.labels["main"]), kseg(a.labels["fatal"]))
        # What the cases kept, the row has; anything else is the image's own
        # (print_hex's STB is BWX), and a row without it cannot be tested.
        lacks = sorted({self.row.lacks(n) for n in filter(None, map(
            rows.word_needs, list(code) + list(pal.values()) + self.user.words
            + self.kcode.words))} - {None})
        if lacks:
            raise ValueError("the image itself needs what row %s lacks: %s"
                             % (self.row.name, "; ".join(lacks)))
        for addr, w in pal.items():
            struct.pack_into("<I", img, addr, w)
        for i, w in enumerate(code):
            struct.pack_into("<I", img, CODE_PA + 4 * i, w)
        for v, i in self.pool.items():
            struct.pack_into("<Q", img, POOL_PA + 8 * i, v)
        for i, w in enumerate(self.user.finish()):
            struct.pack_into("<I", img, USER_PA + 4 * i, w)
        for i, w in enumerate(self.kcode.finish()):
            struct.pack_into("<I", img, KCODE_PA + 4 * i, w)
        assert PAL8_SNIP_PA + 4 * len(self.pal8.words) <= PAL8_SNIP_TOP
        for i, w in enumerate(self.pal8.finish()):
            struct.pack_into("<I", img, PAL8_SNIP_PA + 4 * i, w)
        for p in CONSOLE_PATCHES:
            assert img[p:p + 4] == bytes(4), "a console patch lands on the image"
        self.code_words = len(code)
        return img


TAG = b"ALPHDC01"


def write_rom(path, img):
    """The decompressed-console file: entry PC (PALmode), PAL_BASE, the
    image, the tag (System.cpp load_decompressed_console)."""
    with open(path, "wb") as f:
        f.write(struct.pack("<QQ", PAL_PA | 1, PAL_PA))
        f.write(img)
        f.write(TAG)
