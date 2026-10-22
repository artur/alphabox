"""A small Alpha assembler for the conformance images (test/cpu).

Instruction formats are the Alpha Architecture Reference Manual's (4th
edition, chapter 3.3: memory, branch, operate, floating-point operate and
PALcode formats); opcode and function numbers are its appendix C. The
PALcode-reserved formats (HW_MFPR, HW_MTPR, HW_LD, HW_ST, HW_RET) and the
internal processor register numbers are the 21264/EV68CB Hardware Reference
Manual's (chapter 6.4 and Table 5-1).

Nothing here knows what an instruction does: that is aarm.py's job.
"""

R31 = 31

# --- formats (ARM 3.3) ------------------------------------------------------


def mem(op, ra, rb, disp):
    """Memory format: opcode, Ra, Rb, 16-bit signed displacement."""
    assert -0x8000 <= disp < 0x8000, disp
    return (op << 26) | (ra << 21) | (rb << 16) | (disp & 0xFFFF)


def mfc(func, ra=R31, rb=R31):
    """Memory format with a function code in the displacement (opcode 18)."""
    return (0x18 << 26) | (ra << 21) | (rb << 16) | (func & 0xFFFF)


def opr(op, func, ra, rb, rc):
    """Operate format, register Rb."""
    return (op << 26) | (ra << 21) | (rb << 16) | (func << 5) | rc


def opl(op, func, ra, lit, rc):
    """Operate format, 8-bit literal in place of Rb."""
    assert 0 <= lit < 256, lit
    return (op << 26) | (ra << 21) | (lit << 13) | (1 << 12) | (func << 5) | rc


def fop(op, func, fa, fb, fc):
    """Floating-point operate format (11-bit function)."""
    return (op << 26) | (fa << 21) | (fb << 16) | (func << 5) | fc


def bra(op, ra, disp):
    """Branch format: 21-bit signed displacement in instructions."""
    assert -(1 << 20) <= disp < (1 << 20), disp
    return (op << 26) | (ra << 21) | (disp & 0x1FFFFF)


def jmp(kind, ra, rb, hint=0):
    """Memory format jump (opcode 1A): kind 0 JMP, 1 JSR, 2 RET, 3 JSR_CO."""
    return (0x1A << 26) | (ra << 21) | (rb << 16) | (kind << 14) | (hint & 0x3FFF)


def call_pal(func):
    return func & 0x3FFFFFF


# --- PALcode-reserved formats (21264 HRM 6.4) -------------------------------


def hw_mfpr(ra, ipr):
    return (0x19 << 26) | (ra << 21) | (ra << 16) | ((ipr & 0xFF) << 8)


def hw_mtpr(rb, ipr):
    return (0x1D << 26) | (rb << 21) | (rb << 16) | ((ipr & 0xFF) << 8)


def hw_ld(ra, rb, disp, typ=0, quad=True):
    """HW_LD: TYPE<15:13> (0 physical), LEN<12> (1 quadword), DISP<11:0>."""
    assert -0x800 <= disp < 0x800
    return ((0x1B << 26) | (ra << 21) | (rb << 16) | (typ << 13) |
            ((1 if quad else 0) << 12) | (disp & 0xFFF))


def hw_st(ra, rb, disp, typ=0, quad=True):
    assert -0x800 <= disp < 0x800
    return ((0x1F << 26) | (ra << 21) | (rb << 16) | (typ << 13) |
            ((1 if quad else 0) << 12) | (disp & 0xFFF))


def hw_ret(rb, stall=False):
    """HW_RET: TYPE<15:14> = 10 (RET), STALL<13>."""
    return ((0x1E << 26) | (R31 << 21) | (rb << 16) | (2 << 14) |
            ((1 if stall else 0) << 13))


# Internal processor registers (21264/EV68CB HRM Table 5-1).
IPR = {
    "ITB_TAG": 0x00, "ITB_PTE": 0x01, "ITB_IAP": 0x02, "ITB_IA": 0x03,
    "ITB_IS": 0x04, "EXC_ADDR": 0x06, "IVA_FORM": 0x07, "CM": 0x09,
    "IER": 0x0A, "IER_CM": 0x0B, "SIRR": 0x0C, "ISUM": 0x0D,
    "HW_INT_CLR": 0x0E, "EXC_SUM": 0x0F, "PAL_BASE": 0x10, "I_CTL": 0x11,
    "IC_FLUSH_ASM": 0x12, "IC_FLUSH": 0x13, "PCTR_CTL": 0x14, "I_STAT": 0x16,
    "DTB_TAG0": 0x20, "DTB_PTE0": 0x21, "DTB_IS0": 0x24, "DTB_ASN0": 0x25,
    "DTB_ALTMODE": 0x26, "MM_STAT": 0x27, "M_CTL": 0x28, "DC_CTL": 0x29,
    "DC_STAT": 0x2A, "PCTX": 0x40, "DTB_TAG1": 0xA0, "DTB_PTE1": 0xA1,
    "DTB_IAP": 0xA2, "DTB_IA": 0xA3, "DTB_IS1": 0xA4, "DTB_ASN1": 0xA5,
    "CC": 0xC0, "CC_CTL": 0xC1, "VA": 0xC2, "VA_FORM": 0xC3, "VA_CTL": 0xC4,
}
# PCTX field selects (index bits <4:0>): ASN, ASTER, ASTRR, PPCE, FPE.
PCTX_FPE = 0x10

# --- opcodes and function codes (ARM appendix C) ----------------------------

OP = {
    "LDA": 0x08, "LDAH": 0x09, "LDBU": 0x0A, "LDQ_U": 0x0B, "LDWU": 0x0C,
    "STW": 0x0D, "STB": 0x0E, "STQ_U": 0x0F,
    "LDF": 0x20, "LDG": 0x21, "LDS": 0x22, "LDT": 0x23,
    "STF": 0x24, "STG": 0x25, "STS": 0x26, "STT": 0x27,
    "LDL": 0x28, "LDQ": 0x29, "LDL_L": 0x2A, "LDQ_L": 0x2B,
    "STL": 0x2C, "STQ": 0x2D, "STL_C": 0x2E, "STQ_C": 0x2F,
    "BR": 0x30, "FBEQ": 0x31, "FBLT": 0x32, "FBLE": 0x33, "BSR": 0x34,
    "FBNE": 0x35, "FBGE": 0x36, "FBGT": 0x37,
    "BLBC": 0x38, "BEQ": 0x39, "BLT": 0x3A, "BLE": 0x3B, "BLBS": 0x3C,
    "BNE": 0x3D, "BGE": 0x3E, "BGT": 0x3F,
}

# Integer operate: mnemonic -> (opcode, function).
IOP = {
    # 10: integer arithmetic
    "ADDL": (0x10, 0x00), "S4ADDL": (0x10, 0x02), "SUBL": (0x10, 0x09),
    "S4SUBL": (0x10, 0x0B), "CMPBGE": (0x10, 0x0F), "S8ADDL": (0x10, 0x12),
    "S8SUBL": (0x10, 0x1B), "CMPULT": (0x10, 0x1D), "ADDQ": (0x10, 0x20),
    "S4ADDQ": (0x10, 0x22), "SUBQ": (0x10, 0x29), "S4SUBQ": (0x10, 0x2B),
    "CMPEQ": (0x10, 0x2D), "S8ADDQ": (0x10, 0x32), "S8SUBQ": (0x10, 0x3B),
    "CMPULE": (0x10, 0x3D), "ADDL/V": (0x10, 0x40), "SUBL/V": (0x10, 0x49),
    "CMPLT": (0x10, 0x4D), "ADDQ/V": (0x10, 0x60), "SUBQ/V": (0x10, 0x69),
    "CMPLE": (0x10, 0x6D),
    # 11: logical and conditional move
    "AND": (0x11, 0x00), "BIC": (0x11, 0x08), "CMOVLBS": (0x11, 0x14),
    "CMOVLBC": (0x11, 0x16), "BIS": (0x11, 0x20), "CMOVEQ": (0x11, 0x24),
    "CMOVNE": (0x11, 0x26), "ORNOT": (0x11, 0x28), "XOR": (0x11, 0x40),
    "CMOVLT": (0x11, 0x44), "CMOVGE": (0x11, 0x46), "EQV": (0x11, 0x48),
    "AMASK": (0x11, 0x61), "CMOVLE": (0x11, 0x64), "CMOVGT": (0x11, 0x66),
    "IMPLVER": (0x11, 0x6C),
    # 12: shifts and byte manipulation
    "MSKBL": (0x12, 0x02), "EXTBL": (0x12, 0x06), "INSBL": (0x12, 0x0B),
    "MSKWL": (0x12, 0x12), "EXTWL": (0x12, 0x16), "INSWL": (0x12, 0x1B),
    "MSKLL": (0x12, 0x22), "EXTLL": (0x12, 0x26), "INSLL": (0x12, 0x2B),
    "ZAP": (0x12, 0x30), "ZAPNOT": (0x12, 0x31), "MSKQL": (0x12, 0x32),
    "SRL": (0x12, 0x34), "EXTQL": (0x12, 0x36), "SLL": (0x12, 0x39),
    "INSQL": (0x12, 0x3B), "SRA": (0x12, 0x3C), "MSKWH": (0x12, 0x52),
    "INSWH": (0x12, 0x57), "EXTWH": (0x12, 0x5A), "MSKLH": (0x12, 0x62),
    "INSLH": (0x12, 0x67), "EXTLH": (0x12, 0x6A), "MSKQH": (0x12, 0x72),
    "INSQH": (0x12, 0x77), "EXTQH": (0x12, 0x7A),
    # 13: multiply
    "MULL": (0x13, 0x00), "MULQ": (0x13, 0x20), "UMULH": (0x13, 0x30),
    "MULL/V": (0x13, 0x40), "MULQ/V": (0x13, 0x60),
    # 1C: BWX sign extension, CIX, MVI (and FTOIx, below)
    "SEXTB": (0x1C, 0x00), "SEXTW": (0x1C, 0x01), "CTPOP": (0x1C, 0x30),
    "PERR": (0x1C, 0x31), "CTLZ": (0x1C, 0x32), "CTTZ": (0x1C, 0x33),
    "UNPKBW": (0x1C, 0x34), "UNPKBL": (0x1C, 0x35), "PKWB": (0x1C, 0x36),
    "PKLB": (0x1C, 0x37), "MINSB8": (0x1C, 0x38), "MINSW4": (0x1C, 0x39),
    "MINUB8": (0x1C, 0x3A), "MINUW4": (0x1C, 0x3B), "MAXUB8": (0x1C, 0x3C),
    "MAXUW4": (0x1C, 0x3D), "MAXSB8": (0x1C, 0x3E), "MAXSW4": (0x1C, 0x3F),
}

# Integer <-> floating register moves (FIX): (opcode, function).
FTOI = {"FTOIT": (0x1C, 0x70), "FTOIS": (0x1C, 0x78)}
ITOF = {"ITOFS": (0x14, 0x004), "ITOFF": (0x14, 0x014), "ITOFT": (0x14, 0x024)}
CPYS = (0x17, 0x020)   # CPYS Fa,Fb,Fc; CPYS F31,F31,F31 is FNOP

# Miscellaneous (opcode 18) function codes.
MISC = {
    "TRAPB": 0x0000, "EXCB": 0x0400, "MB": 0x4000, "WMB": 0x4400,
    "FETCH": 0x8000, "FETCH_M": 0xA000, "RPCC": 0xC000, "RC": 0xE000,
    "ECB": 0xE800, "RS": 0xF000, "WH64": 0xF800, "WH64EN": 0xFC00,
}


def ins_op(name, ra, b, rc, lit=False):
    op, fn = IOP[name]
    return opl(op, fn, ra, b, rc) if lit else opr(op, fn, ra, b, rc)


class Asm:
    """Instructions at a fixed physical address, with forward labels."""

    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
        self.fixups = []   # (index, label, kind)

    @property
    def here(self):
        return self.base + 4 * len(self.words)

    def emit(self, word):
        self.words.append(word & 0xFFFFFFFF)
        return len(self.words) - 1

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.here

    def branch(self, op, ra, target):
        """A branch to a label (resolved at finish)."""
        i = self.emit(bra(op, ra, 0))
        self.fixups.append((i, target))
        return i

    def finish(self):
        for i, target in self.fixups:
            dest = self.labels[target]
            disp = (dest - (self.base + 4 * (i + 1))) // 4
            self.words[i] = (self.words[i] & ~0x1FFFFF) | (disp & 0x1FFFFF)
        return self.words

    # Load a constant without touching memory: LDA/LDAH for anything that
    # is a sign-extended 32-bit value, else the high half, SLL 32 and the
    # low half added with LDAH/LDA. Arbitrary operands come from the
    # literal pool instead (an LDQ), so this is for addresses and setup.
    def li(self, r, value):
        value &= (1 << 64) - 1
        sval = value - (1 << 64) if value >> 63 else value
        if -0x80000000 <= sval < 0x7FFF8000:
            self._add32(r, R31, sval)
            return
        lo = value & 0xFFFFFFFF
        lo_s = lo - (1 << 32) if lo >> 31 else lo
        hi = (sval - lo_s) >> 32
        assert -0x80000000 <= hi < 0x7FFF8000 and lo_s < 0x7FFF8000, hex(value)
        self._add32(r, R31, hi)
        self.emit(opl(0x12, 0x39, r, 32, r))  # SLL r,32,r
        if lo_s:
            self._add32(r, r, lo_s)

    def _add32(self, r, base, v):
        """r = base + v, v a signed 32-bit value below 0x7fff8000."""
        lo = v & 0xFFFF
        lo = lo - 0x10000 if lo >= 0x8000 else lo
        hi = (v - lo) >> 16
        assert -0x8000 <= hi < 0x8000
        if hi:
            self.emit(mem(OP["LDAH"], r, base, hi))
            base = r
        if lo or base != r:
            self.emit(mem(OP["LDA"], r, base, lo))
