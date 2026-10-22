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
EXC_ADDR, and anything it cannot resume from (ITB miss, IACV, machine
check, double miss, reset) ends the run through the native `fatal` path.
"""
import struct

import alpha_asm as A
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
    com.branch(OP["BEQ"], P22, "resume")
    for v in FATAL_VECTORS:
        com.emit(lda(P22, P4, -v))
        com.branch(OP["BEQ"], P22, "fatal")
    com.emit(opl(0x10, 0x20, P5, 4, P5))   # fault or trap: skip the instruction
    com.label("resume")
    com.emit(hw_ret(P5))
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
    blocks = [init, com, cp]
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
        else:
            e.emit(lda(P4, R31, off))
            e.emit(bra(OP["BR"], R31, (cp.base - (e.here + 4)) // 4))
        assert len(e.words) <= 0x40 // 4, hex(func)
        blocks.append(e)

    # The body of CALL_PAL 02: the user-mode translations, then CM = user.
    for va, pte, itb in (
            (USER_CODE_VA, USER_PA | 0xF00 | 0x10, True),
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
    """

    def __init__(self, passes, blocks=True):
        self.passes = passes
        # Every case starts a block of its own (block()): without that a
        # group is one straight run of thousands of instructions, which the
        # JIT records only from the last dispatch batch's boundary to the next
        # branch, and the "compiled" last pass ran 8% of the cases compiled.
        self.blocks = blocks
        self.asm = Asm(CODE_PA)
        self.user = Asm(USER_CODE_VA)
        self.pool = {}          # value -> index
        self.results = []       # (case, field, expect, mask, known)
        self.res_base = 0       # byte offset R28 currently points at
        self.case = None
        self.expected_traps = 0
        self.scratch = kseg(SCR_PA)
        self.trap_record = kseg(TR_PA)

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

    def begin(self, name):
        self.case = name
        if self.blocks:
            self.block()

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
        a.li(R_UART, UART_VA)
        emit_print_text(a, "\n@@FATAL\n")
        a.li(16, kseg(TR_PA))
        a.li(17, 7)
        a.branch(OP["BSR"], 18, "print_hex")
        a.emit(call_pal(0x00))

        a.label("main")
        a.li(R_TR, kseg(TR_PA))
        a.li(R_UART, UART_VA)
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
        pal = build_pal(TR_PA, kseg(a.labels["main"]), kseg(a.labels["fatal"]))
        for addr, w in pal.items():
            struct.pack_into("<I", img, addr, w)
        for i, w in enumerate(code):
            struct.pack_into("<I", img, CODE_PA + 4 * i, w)
        for v, i in self.pool.items():
            struct.pack_into("<Q", img, POOL_PA + 8 * i, v)
        for i, w in enumerate(self.user.finish()):
            struct.pack_into("<I", img, USER_PA + 4 * i, w)
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
