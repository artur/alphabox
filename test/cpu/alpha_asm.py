"""A small Alpha assembler for the conformance images (test/cpu).

Instruction formats are the Alpha Architecture Reference Manual's (4th
edition, chapter 3.3: memory, branch, operate, floating-point operate and
PALcode formats); opcode and function numbers are its appendix C. The
PALcode-reserved formats (HW_MFPR, HW_MTPR, HW_LD, HW_ST, HW_RET) and the
internal processor register numbers are the 21264/EV68CB Hardware Reference
Manual's (chapter 6.4 and Table 5-1).

The 21464's (EV8's) additions are its Internal Design Specification's (Rev
1.1k), as docs/platforms/ev8-isa.md fixes them for Alphabox and the nada
toolchain: CMOV2, FCMOV2, MAP_ALIGN, CCB, LDL_ARM, LDQ_ARM and QUIESCE, the
1C short-vector SIMD set (SIMD1C); and
its own PALcode forms and IPR names, as docs/platforms/ev8-native.md 3.5
gives them (hw_mfpr8, hw_mtpr8, hw_ld8, hw_st8, ifetchb, ret, IPR8).

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

# --- the 21464's PALcode forms (IDS pp. 569-574, 650; ev8-native.md 3.5) ----


def hw_mfpr8(rc, ipr, rclass=0):
    """HW_MFPR: Rclass <24:21>, index <12:5>, the destination Rc <4:0>."""
    return ((0x19 << 26) | ((rclass & 0xF) << 21) | ((ipr & 0xFF) << 5) |
            (rc & 0x1F))


def hw_mtpr8(rb, ipr, rclass=0, wclass=0):
    """HW_MTPR: Rclass <24:21>, the source Rb <20:16>, index <12:5>, Wclass
    <4:0> (bubble, valid, class)."""
    return ((0x1D << 26) | ((rclass & 0xF) << 21) | ((rb & 0x1F) << 16) |
            ((ipr & 0xFF) << 5) | (wclass & 0x1F))


# HW_LD/HW_ST types <15:13> and lengths <12:11> [IDS p. 570].
HW8_TYPE = {"p": 0, "pte": 2, "v": 4, "wchk": 5, "a": 6, "wchk_a": 7}
HW8_LEN = {"b": 0, "w": 1, "l": 2, "q": 3}


def hw_ld8(ra, rb, disp, typ=0, length=3):
    """HW_LD: type <15:13>, length <12:11>, an 11-bit signed displacement."""
    assert -0x400 <= disp < 0x400, disp
    return ((0x1B << 26) | (ra << 21) | (rb << 16) | ((typ & 7) << 13) |
            ((length & 3) << 11) | (disp & 0x7FF))


def hw_st8(ra, rb, disp, typ=0, length=3):
    assert -0x400 <= disp < 0x400, disp
    return ((0x1F << 26) | (ra << 21) | (rb << 16) | ((typ & 7) << 13) |
            ((length & 3) << 11) | (disp & 0x7FF))


def ifetchb():
    """IFETCHB: opcode 1E, the other bits zero [IDS p. 650]."""
    return 0x1E << 26


def ret(rb, ra=R31, hint=0):
    """RET: in PALmode the new mode is Rb<1:0> (00 normal, 01 PALmode, 11
    SuperPALmode) [IDS p. 573]."""
    return jmp(2, ra, rb, hint)


# The shadow registers by position [IDS p. 580]: S0 and S1 are named R24
# and R25 as operand A or the destination, R22 and R23 as operand B.
S0A, S1A, S0B, S1B = 24, 25, 22, 23

# The 21464's IPR names (ev8-native.md 3.5): a field-select variant is a
# name of its own; cpu_base, vctl, ier_cm, sirr, isum and hw_int_clr are
# Alphabox's indexes (N6-N8).
IPR8 = {
    "i_ctl": 0x83, "i_ctl_vpte": 0x81, "i_ctl_spe": 0x82,
    "i_mode": 0x8B, "i_mode_cur": 0x89, "i_mode_con": 0x8A,
    "i_pctx": 0x97, "i_pctx_asn": 0x91, "i_pctx_grp": 0x92,
    "i_pctx_fpe": 0x94, "exc_sum": 0xA0, "exc_addr": 0xA1, "iva_form": 0xA3,
    "dtbms_ret_addr": 0xA6, "dtbms_ret_addr_blk": 0xA7, "pal_base": 0xA8,
    "pal_temp1": 0xA9, "pal_temp2": 0xAA, "cpu_base": 0xAB, "vctl": 0xAC,
    "ier_cm": 0xB0, "sirr": 0xB1, "isum": 0xB2, "hw_int_clr": 0xB3,
    "cc": 0xBB, "cc_offset": 0xB9, "cc_count": 0xBA, "itb_im": 0xC0,
    "itb_is": 0xC2, "itb_pte": 0xC4, "itb_tag": 0xC6, "cpu_cnfg": 0xC8,
    "ic_stat": 0xC9, "ic_flush": 0xCC, "ic_flush_asm": 0xCD,
    "tpu_cnfg": 0xD8,
    "m_ctl": 0x03, "m_ctl_vpte": 0x01, "m_ctl_spe": 0x02, "m_mode": 0x0F,
    "m_mode_cur": 0x09, "m_mode_con": 0x0A, "m_mode_alt": 0x0C,
    "m_pctx": 0x13, "m_pctx_asn": 0x11, "m_pctx_grp": 0x12, "m_stat": 0x20,
    "va": 0x21, "va_form": 0x23, "quiesce_timeout": 0x38, "dtb_im": 0x40,
    "dtb_is": 0x42, "dtb_pte0": 0x44, "dtb_pte1": 0x45, "dtb_tag0": 0x46,
    "dtb_tag1": 0x47, "dc_ctl": 0x48, "dc_stat": 0x49,
}

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
    # the 21464's second half of a split CMOV (ev8-isa.md 3.2)
    "CMOV2": (0x11, 0x68),
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

# The 21464's short-vector SIMD set under opcode 1C (ev8-isa.md 3.3):
# mnemonic -> function. In the "zz" blocks the size is the low two bits,
# 00 UB8, 01 UW4, 10 SB8, 11 SW4, the order agreed with nada.
SIMD1C = {"CMPWGE": 0x04, "CMPLGE": 0x05, "PERMB8": 0x08, "GPKBLB4": 0x09,
          "PKUWB8": 0x28, "PKULW4": 0x29, "PKSWB8": 0x2A, "PKSLW4": 0x2B,
          "UPKUBW4": 0x2C, "UPKUWL2": 0x2D, "UPKSBW4": 0x2E, "UPKSWL2": 0x2F,
          "TMULUSB8": 0x54, "TMULUSW4": 0x55, "VMULLUW4": 0x59,
          "VMULHUW4": 0x5B, "VSRB8": 0x60, "VSRW4": 0x61, "VSRAB8": 0x62,
          "VSRAW4": 0x63, "VSRL2": 0x64, "VSRAL2": 0x66, "VSLB8": 0x68,
          "VSLW4": 0x69, "VSLL2": 0x6C}
for _base, _stem in ((0x10, "VADD"), (0x18, "VSUB"), (0x20, "VMINMAX"),
                     (0x40, "TADD"), (0x44, "TSUB"), (0x48, "TABSERR"),
                     (0x4C, "TSQERR"), (0x50, "TMUL")):
    for _zz, _sfx in enumerate(("UB8", "UW4", "SB8", "SW4")):
        SIMD1C[_stem + _sfx] = _base + _zz
for _base, _stem in ((0x14, "VADD"), (0x1C, "VSUB"), (0x24, "VMINMAX")):
    SIMD1C[_stem + "UL2"] = _base
    SIMD1C[_stem + "SL2"] = _base + 2
IOP.update({_n: (0x1C, _f) for _n, _f in SIMD1C.items()})

# Integer <-> floating register moves (FIX): (opcode, function).
FTOI = {"FTOIT": (0x1C, 0x70), "FTOIS": (0x1C, 0x78)}
ITOF = {"ITOFS": (0x14, 0x004), "ITOFF": (0x14, 0x014), "ITOFT": (0x14, 0x024)}
CPYS = (0x17, 0x020)   # CPYS Fa,Fb,Fc; CPYS F31,F31,F31 is FNOP
FCMOV2 = (0x17, 0x068)  # the 21464's FCMOV second half (ev8-isa.md 3.2)
MAP_ALIGN = opr(0x11, 0x40, R31, R31, R31)  # XOR R31,R31,R31 (ev8-isa.md 3.2)

# Miscellaneous (opcode 18) function codes; the last four are the 21464's
# (ev8-isa.md 3.1).
MISC = {
    "TRAPB": 0x0000, "EXCB": 0x0400, "MB": 0x4000, "WMB": 0x4400,
    "FETCH": 0x8000, "FETCH_M": 0xA000, "RPCC": 0xC000, "RC": 0xE000,
    "ECB": 0xE800, "RS": 0xF000, "WH64": 0xF800, "WH64EN": 0xFC00,
    "LDL_ARM": 0xB000, "LDQ_ARM": 0xB400, "QUIESCE": 0xB800, "CCB": 0xEC00,
}


# --- Tarantula (docs/platforms/ev8-isa.md, section 4) --------------------------
#
# The integer operate layout for VV (01), VS (02) and VC (05): Va or Ra
# <25:21>, Vb, Rb or Fb <20:16>, <15:13> SBZ in the register form, L <12>
# (an 8-bit literal in <20:13>), M <11> (under mask), the 6-bit function
# <10:5>, Vc or Rc <4:0>. `sbz` and the literal flag can be forced, for the
# cases of the field rules.

VV_FNC = {
    "vvaddq": 0x00, "vvsubq": 0x01, "vvmulq": 0x02, "vvumulh": 0x03,
    "vvand": 0x04, "vvbic": 0x05, "vvbis": 0x06, "vvornot": 0x07,
    "vvxor": 0x08, "vveqv": 0x09, "vvsll": 0x0A, "vvsrl": 0x0B, "vvsra": 0x0C,
    "vvmrg": 0x0D, "vvcmpeq": 0x10, "vvcmpne": 0x11, "vvcmplt": 0x12,
    "vvcmple": 0x13, "vvcmpgt": 0x14, "vvcmpge": 0x15, "vvcmpult": 0x16,
    "vvcmpule": 0x17, "vvcmpugt": 0x18, "vvcmpuge": 0x19,
    "vvaddt": 0x20, "vvsubt": 0x21, "vvmult": 0x22, "vvdivt": 0x23,
    "vvsqrtt": 0x24, "vvcmpteq": 0x25, "vvcmptlt": 0x26, "vvcmptle": 0x27,
    "vvcmptun": 0x28, "vvcmptne": 0x29, "vvcmptgt": 0x2A, "vvcmptge": 0x2B,
    "vvcvtqt": 0x2C, "vvcvttq": 0x2D, "vvcvttqc": 0x2E, "vvcpys": 0x30,
    "vvcpysn": 0x31, "vvcpyse": 0x32,
}
# VS has every VV code but the unary ones (24, 2C, 2D, 2E).
VS_FNC = {"vs" + k[2:]: v for k, v in VV_FNC.items()
          if v not in (0x24, 0x2C, 0x2D, 0x2E)}
VC_FNC = {"setvl": 0x00, "setvs": 0x01, "setvm": 0x02, "getvl": 0x04,
          "getvs": 0x05, "getvm": 0x06}


def vop(op, fnc, a, b, c, m=False, lit=None, sbz=0):
    """A Tarantula operate-format word: opcode `op`, fields a <25:21>,
    b <20:16> (or the literal <20:13> with L set), c <4:0>."""
    w = (op << 26) | ((a & 31) << 21) | ((1 if m else 0) << 11) | \
        ((fnc & 0x3F) << 5) | (c & 31)
    if lit is not None:
        assert 0 <= lit < 256, lit
        return w | (lit << 13) | (1 << 12)
    return w | ((b & 31) << 16) | ((sbz & 7) << 13)


def vv(name, va, vb, vc, m=False):
    return vop(0x01, VV_FNC[name], va, vb, vc, m)


def vs(name, va, rb, vc, m=False, lit=None):
    return vop(0x02, VS_FNC[name], va, rb, vc, m, lit)


def vc(name, rb=R31, rc=R31, lit=None, ra=R31):
    return vop(0x05, VC_FNC[name], ra, rb, rc, False, lit)


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
