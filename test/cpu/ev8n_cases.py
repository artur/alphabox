"""The native EV8's cases (group ev8n): the 21464's own privileged interface
on the `ev8n` row (docs/platforms/ev8-native.md, S11 and section 6), from
its Internal Design Specification, Rev 1.1k (IDS, by PDF page), and the
decisions N1-N31 of ev8-native.md.

The image runs the EV8 PALcode (image.build_pal8) on this row. A case
reaches PALmode through its CALL_PAL 91, which runs a snippet of PALmode
code (image.Builder.pal8_call) and returns after the CALL_PAL; the rest is
normal-mode kernel code, which on a JIT lane's last pass runs compiled
(PALmode code on this row never does). Registers R0-R23 are the case's; in
PALmode R24 and R25 as operand A or destination, and R22 and R23 as operand
B, are the shadow registers S0 and S1 [IDS p. 580], which CALL_PAL 91
itself uses. Every case needs rows.NATIVE8, so the other rows skip them.
"""
import alpha_asm as A
from alpha_asm import (OP, R31, S0A, S0B, S1A, S1B, bra, call_pal, jmp, mem,
                       mfc, opl, opr)
import image
import rows
from image import R_SCR, R_TR, TR_COUNT, TR_VA, TR_VEC, kseg

M64 = (1 << 64) - 1
I = A.IPR8
mf, mt = A.hw_mfpr8, A.hw_mtpr8

DATA_PA = 0x1DE000          # an 8 KB page of the cases' own
PROBE_PA = 0x1DC000         # the ITB probes' code: LDA r20,#1; RET (r17)
PA64 = 0x2000000            # 64 KB and larger pages: past the image
UNMAPPED = 0x10000000       # no translation anywhere
PAL_PA = image.PAL_PA
PTE_K = 0x1101              # DTB PTE: KWE, KRE, VALID
NOP = opr(0x11, 0x20, R31, R31, R31)   # BIS r31,r31,r31: CMOV2's placeholder


def bis(ra, rb, rc):
    return opr(0x11, 0x20, ra, rb, rc)


def lda(ra, rb, d):
    return mem(OP["LDA"], ra, rb, d)


def ldq(ra, rb, d):
    return mem(OP["LDQ"], ra, rb, d)


def stq(ra, rb, d):
    return mem(OP["STQ"], ra, rb, d)


class Sn:
    """A PALmode snippet: words, with labels for branches."""

    def __init__(self):
        self.a = A.Asm(0)

    def e(self, *words):
        for w in words:
            self.a.emit(w)
        return self

    def li(self, r, v):
        self.a.li(r, v)
        return self

    def s0(self, v):            # S0 <- v
        image.li8(self.a, S0A, S0B, v)
        return self

    def idx(self):
        return len(self.a.words)

    def words(self):
        return self.a.finish()


def pte8(pa, flags):
    """A DTB or ITB PTE: the PFN in <63:32>, with 8 KB pages [IDS p. 504]."""
    return ((pa >> 13) << 32) | flags


def pte64(pa, flags):
    return ((pa >> 16) << 32) | flags


def dtb_fill(sn, va, pte):
    """The DTB writer block: both tags, both PTEs (N9: one entry)."""
    sn.s0(va).e(mt(S0B, I["dtb_tag0"]), mt(S0B, I["dtb_tag1"]))
    sn.s0(pte).e(mt(S0B, I["dtb_pte0"]), mt(S0B, I["dtb_pte1"]), A.ifetchb())
    return sn


def itb_fill(sn, va, pte):
    sn.s0(va).e(mt(S0B, I["itb_tag"]))
    sn.s0(pte).e(mt(S0B, I["itb_pte"]), A.ifetchb())
    return sn


def ctx(sn, grp, asn, itb=False):
    """The TPU group and ASN of the DTB (M_PCTX) or the ITB (I_PCTX's ASN
    and TPU_GRP selects, 0x93), as PALcode writes them [pp. 492, 509]."""
    sn.s0((grp << 16) | (asn << 8))
    sn.e(mt(S0B, 0x93 if itb else I["m_pctx"]), A.ifetchb())
    return sn


def call(b, sn):
    return b.pal8_call(sn.words())


def stored(b, reg, field, expect, mask=M64):
    b.store(reg, field, expect & mask, mask=mask)


def tr(b, off, field, expect, mask=M64):
    b.emit(ldq(0, R_TR, off))
    b.store(0, field, expect & mask, mask=mask)


def regs_of(word):
    """EXC_SUM's register fields [IDS pp. 485-486]."""
    return ((((word >> 21) & 31) << 24) | (((word >> 16) & 31) << 19) |
            ((word & 31) << 14))


def begin(b, name):
    b.begin("ev8n: " + name)
    b.need(rows.NATIVE8)


# --- the reset (N17) and the IPRs at their reset values -----------------------

RESET = {"i_ctl": 0, "m_ctl": 0, "i_mode": 0, "m_mode": 0, "i_pctx": 0,
         "m_pctx": 0, "ier_cm": 0, "sirr": 0, "isum": 0, "cc": 0,
         "quiesce_timeout": 0x2800, "pal_base": PAL_PA, "pal_temp1": 0,
         "pal_temp2": 0, "vctl": 0, "tpu_cnfg": 0, "cpu_cnfg": 0,
         "cpu_base": 0, "dc_ctl": 3 << 48, "dc_stat": 0, "ic_stat": 0,
         "exc_addr": 0, "exc_sum": 0, "m_stat": 0, "va": 0,
         "dtbms_ret_addr": 0}


def g_reset(b):
    begin(b, "reset: the entry (MILD_RESET 0xB00) and the IPRs it leaves")
    tr(b, image.TR8_RESET_ENTRY, "the reset entry", 0xB00)
    for k, name in enumerate(image.RESET_IPRS):
        tr(b, image.TR8_RESET + 8 * k, "%s at reset" % name, RESET[name])


# --- the IPRs: every index, its select bits, read-only and W1C fields ----------

def rw_case(b, name, steps, restore):
    """Write each (index, value) and read the register back (at `read`,
    the all-fields index) into R1, R2, ..; then the restore writes."""
    begin(b, "IPR %s: writes by field select, read back" % name)
    sn = Sn()
    for k, (index, value, read, expect, mask) in enumerate(steps):
        sn.li(17, value).e(mt(17, index), A.ifetchb(), mf(1 + k, read))
    for index, value in restore:
        sn.li(17, value).e(mt(17, index))
    sn.e(A.ifetchb())
    call(b, sn)
    for k, (index, value, read, expect, mask) in enumerate(steps):
        stored(b, 1 + k, "%s: write %#x <- %#x, read %#x" % (
            name, index, value, read), expect, mask)


CTL_VPTE = 0x000ffffe00000000
CTL_RED, CTL_SPE, CTL_BIG, CTL_DBL, CTL_SIZES = (
    1 << 27, 7 << 24, 1 << 23, 1 << 22, 3 << 20)


def g_iprs(b):
    ones = M64
    pat = 0x5555555555555555
    # I_CTL [IDS pp. 489-490]: 01 VPTE_BASE, 10 SUPERPAGE, VA_SIZE,
    # PAGE_SIZE, 11 all (REDUCED_PT with 11 only, as the select lists it).
    ic_all = CTL_VPTE | CTL_RED | CTL_SPE | CTL_SIZES
    rw_case(b, "I_CTL", [
        (0x83, ones, 0x83, ic_all, M64),
        (0x81, 0, 0x83, ic_all & ~CTL_VPTE, M64),
        (0x82, 0, 0x80, CTL_RED, M64),
        (0x80, ones, 0x81, CTL_RED, M64),
        (0x83, pat, 0x82, pat & ic_all, M64),
    ], [(0x83, image.I_CTL8)])
    mc_all = CTL_VPTE | CTL_RED | CTL_SPE | CTL_BIG | CTL_DBL | CTL_SIZES
    rw_case(b, "M_CTL", [
        (0x03, ones, 0x03, mc_all, M64),
        (0x01, 0, 0x03, mc_all & ~CTL_VPTE, M64),
        (0x02, 0, 0x00, CTL_RED | CTL_DBL, M64),
        (0x00, ones, 0x01, CTL_RED | CTL_DBL, M64),
        (0x03, pat, 0x02, pat & mc_all, M64),
    ], [(0x03, image.M_CTL8)])
    # I_MODE: 01 CURRENT, 10 CONSOLE; M_MODE: 001 CURRENT, 010 CONSOLE,
    # 100 ALT [pp. 490-491, 507-508]. CURRENT is one field with IER_CM's CM
    # (N7): each write shows in all three.
    rw_case(b, "I_MODE, M_MODE, IER_CM<CM> (N7)", [
        (0x8B, ones, 0x8B, 0x38, M64),
        (0x89, 0, 0x8B, 0x20, M64),
        (0x8A, 0, 0x0F, 0, M64),
        (0x89, 1 << 3, 0x0F, 1 << 3, M64),
        (0x0F, ones, 0x0F, 0xF8, M64),
        (0x0C, 0, 0x0F, 0x38, M64),
        (0x0A, 0, 0x0F, 0x18, M64),
        (0x09, 2 << 3, 0x8B, 2 << 3, M64),
        (0x0B, 0, I["ier_cm"], 0, M64),
        (0x08, ones, 0x0F, 0, M64),
        (I["ier_cm"], 1 << 3, 0x09, 1 << 3, M64),
    ], [(0x0F, 0), (0x8B, 0), (I["ier_cm"], 0)])
    # I_PCTX: 001 ASN, 010 TPU_GRP, 100 FP_ENABLE; M_PCTX: 01 ASN, 10
    # TPU_GRP [pp. 491-492, 508-509].
    ip_all = (1 << 19) | (3 << 16) | (0xFF << 8)
    rw_case(b, "I_PCTX", [
        (0x97, ones, 0x97, ip_all, M64),
        (0x91, 0, 0x90, ip_all & ~(0xFF << 8), M64),
        (0x92, 0, 0x93, 1 << 19, M64),
        (0x94, 0, 0x95, 0, M64),
        (0x93, ones, 0x96, (3 << 16) | (0xFF << 8), M64),
        (0x90, 0, 0x97, (3 << 16) | (0xFF << 8), M64),
    ], [(0x97, 1 << 19)])
    rw_case(b, "M_PCTX", [
        (0x13, ones, 0x13, (3 << 16) | (0xFF << 8), M64),
        (0x11, 0x1200, 0x10, (3 << 16) | (0x12 << 8), M64),
        (0x12, 0, 0x11, 0x12 << 8, M64),
        (0x10, ones, 0x12, 0x12 << 8, M64),
    ], [(0x13, 0)])
    # CC: 01 OFFSET and ENABLE, 10 COUNT, 11 all; <3:1> RAZ [pp. 482-483].
    # The count runs: its bits are not compared.
    rw_case(b, "CC (OFFSET, ENABLE)", [
        (0xB9, 0xABCDEF01FFFFFFFF, 0xBB, 0xABCDEF0100000001,
         0xFFFFFFFF0000000F),
        (0xB9, 0x0000000100000000, 0xB8, 0x0000000100000000,
         0xFFFFFFFF0000000F),
    ], [(0xB9, 0x1234567800000001)])
    # COUNT <31:4>, every 16th cycle; disabled it stands still.
    begin(b, "IPR CC: COUNT written, and still while disabled")
    sn = Sn()
    sn.li(17, 0x1234567800000000).e(mt(17, 0xB9))       # disabled
    sn.li(17, 0x00012340).e(mt(17, 0xBA), A.ifetchb())
    sn.e(mf(1, 0xBB), lda(5, R31, 100), opl(0x10, 0x29, 5, 1, 5),
         bra(OP["BGT"], 5, -2), mf(2, 0xBB))
    sn.li(17, 0x1234567800000001).e(mt(17, 0xB9), A.ifetchb())
    call(b, sn)
    stored(b, 1, "CC disabled: OFFSET, COUNT, ENABLE", 0x1234567800012340)
    stored(b, 2, "CC disabled: the same later", 0x1234567800012340)
    # PAL_BASE<47:15> (N6), PAL_TEMP1, PAL_TEMP2, VCTL (N8), QUIESCE_TIMEOUT,
    # CPU_CNFG<24:0> (CHIP_ID read-only), TPU_CNFG<7:4> (TPU_ID read-only),
    # DC_CTL<55:48> [pp. 487-489, 498-501, 510-511].
    begin(b, "IPRs: stored fields, read-only fields")
    sn = Sn()
    sn.e(mf(15, I["pal_temp1"]), mf(14, I["pal_temp2"]))
    for k, (ipr, value, restore) in enumerate((
            ("pal_base", M64, PAL_PA), ("pal_temp1", 0x123456789ABCDEF0, None),
            ("pal_temp2", 0x0FEDCBA987654321, None), ("vctl", M64, 0),
            ("quiesce_timeout", M64, image.QUIESCE8_TIMEOUT | 3),
            ("cpu_cnfg", M64, 0), ("tpu_cnfg", M64, 0),
            ("dc_ctl", M64, 3 << 48))):
        sn.li(17, value).e(mt(17, I[ipr]), A.ifetchb(), mf(1 + k, I[ipr]))
        if restore is not None:
            sn.li(17, restore).e(mt(17, I[ipr]))
    sn.e(mt(15, I["pal_temp1"]), mt(14, I["pal_temp2"]), A.ifetchb())
    call(b, sn)
    for k, (name, expect) in enumerate((
            ("PAL_BASE <- ~0", 0x0000FFFFFFFF8000),
            ("PAL_TEMP1", 0x123456789ABCDEF0), ("PAL_TEMP2", 0x0FEDCBA987654321),
            ("VCTL <- ~0", 1), ("QUIESCE_TIMEOUT <- ~0", 0xFFFF3),
            ("CPU_CNFG <- ~0 (CHIP_ID 0, read-only)", 0x1FFFFFF),
            ("TPU_CNFG <- ~0 (TPU_ID 0, read-only)", 0xF0),
            ("DC_CTL <- ~0", 0x00FF000000000000))):
        stored(b, 1 + k, name, expect)
    # Read-only and implicitly written registers ignore HW_MTPR; IC_STAT and
    # DC_STAT are write-1-to-clear and nothing sets them; CPU_BASE is the
    # chip's number (N6), 0. The performance registers read zero.
    begin(b, "IPRs: read-only, W1C and performance registers")
    sn = Sn()
    names = ("exc_sum", "exc_addr", "iva_form", "dtbms_ret_addr", "cpu_base",
             "isum", "m_stat", "va", "va_form", "ic_stat", "dc_stat")
    for k, ipr in enumerate(names):
        sn.e(mf(1, I[ipr])).li(17, M64).e(mt(17, I[ipr]), A.ifetchb(),
                                          mf(2, I[ipr]),
                                          opr(0x11, 0x40, 1, 2, 3 + k))
    sn.e(mf(14, I["cpu_base"]), mf(15, I["ic_stat"]), mf(17, I["dc_stat"]),
         mf(18, 0xE0), mf(19, 0xF7), mf(20, 0x60), mf(21, 0x71))
    call(b, sn)
    for k, ipr in enumerate(names):
        stored(b, 3 + k, "%s unchanged by HW_MTPR (XOR)" % ipr.upper(), 0)
    for r, nm in ((14, "CPU_BASE"), (15, "IC_STAT"), (17, "DC_STAT"),
                  (18, "IAGG_EVENT0 (0xE0)"), (19, "PR3_TIMELINE3 (0xF7)"),
                  (20, "MAGG_EVENT0 (0x60)"), (21, "PR1_DMISS_INFO (0x71)")):
        stored(b, r, nm, 0)
    # IER_CM, SIRR, ISUM, HW_INT_CLR (N7): the 21264's layouts without
    # ASTEN; ISUM shows what is requested and enabled. All cleared again
    # before the return, where an enabled request would be taken.
    begin(b, "IPRs: IER_CM, SIRR, ISUM (N7)")
    sn = Sn()
    sn.li(17, M64 & ~(3 << 3)).e(mt(17, I["ier_cm"]), mf(1, I["ier_cm"]))
    sn.e(mt(R31, I["ier_cm"]))
    sn.li(17, M64).e(mt(17, I["sirr"]), mf(2, I["sirr"]), mf(3, I["isum"]))
    sn.li(17, 1 << 15).e(mt(17, I["ier_cm"]), mf(4, I["isum"]))
    sn.li(17, M64).e(mt(17, I["hw_int_clr"]), mf(5, I["isum"]))
    sn.e(mt(R31, I["sirr"]), mt(R31, I["ier_cm"]), A.ifetchb(),
         mf(6, I["isum"]))
    call(b, sn)
    stored(b, 1, "IER_CM <- ~0 but CM: EIEN..SIEN, no ASTEN", 0x7FFFFFC000)
    stored(b, 2, "SIRR <- ~0: SIR<28:14>", 0x1FFFC000)
    stored(b, 3, "ISUM, nothing enabled", 0)
    stored(b, 4, "ISUM, SIEN<15> (level 2)", 1 << 15)
    stored(b, 5, "ISUM after HW_INT_CLR (software requests stay)", 1 << 15)
    stored(b, 6, "ISUM, all cleared", 0)
    # The indexes the cases above leave: DTBMS_RET_ADDR's second (0xA7, the
    # one that also blocks loads and stores), IC_FLUSH_ASM, M_MODE's 101 and
    # 110 selects, and every performance register.
    begin(b, "IPRs: DTBMS_RET_ADDR at 0xA7, IC_FLUSH_ASM, M_MODE 0x0D/0x0E")
    sn = Sn().e(mf(1, 0xA6), mf(2, 0xA7), opr(0x11, 0x40, 1, 2, 3),
                mt(R31, I["ic_flush_asm"]), A.ifetchb())
    sn.li(17, M64).e(mt(17, 0x0D), mf(4, 0x0F), mt(R31, 0x0F))
    sn.li(17, M64).e(mt(17, 0x0E), mf(5, 0x0F), mt(R31, 0x0F), A.ifetchb())
    call(b, sn)
    stored(b, 3, "DTBMS_RET_ADDR 0xA6 XOR 0xA7", 0)
    stored(b, 4, "M_MODE <- ~0 at 0x0D: ALT and CURRENT", 0xD8)
    stored(b, 5, "M_MODE <- ~0 at 0x0E: ALT and CONSOLE", 0xE0)
    perf = ([0xE0, 0xE1, 0xE2, 0xE3] + list(range(0xE8, 0xEE)) +
            list(range(0xF0, 0xF8)) + [0x60, 0x61, 0x62, 0x63, 0x68, 0x6A,
                                       0x6B, 0x6C, 0x70, 0x71])
    begin(b, "IPRs: every performance register reads zero, a write too")
    sn = Sn().e(bis(R31, R31, 1))
    for k in perf:
        sn.li(17, M64).e(mt(17, k), mf(2, k), opr(0x11, 0x20, 1, 2, 1))
    call(b, sn)
    stored(b, 1, "the %d performance registers ORed" % len(perf), 0)
    # HW_MFPR's destination is Rc <4:0> (N3), whatever Rclass <24:21> holds;
    # HW_MTPR's source Rb <20:16>, whatever the classes [IDS pp. 571-573].
    begin(b, "HW_MFPR Rc and HW_MTPR Rb with reader and writer classes")
    b.load(7, 0x7777777777777777)
    b.load(6, 1)
    sn = Sn().e(mf(5, I["pal_base"], rclass=0xF),
                mt(6, I["vctl"], rclass=0x9, wclass=0x1F), A.ifetchb(),
                mf(8, I["vctl"], rclass=0x8), mt(R31, I["vctl"]))
    call(b, sn)
    stored(b, 5, "HW_MFPR Rc (Rclass 0xF)", PAL_PA)
    stored(b, 7, "r7 (not a destination)", 0x7777777777777777)
    stored(b, 8, "VCTL written from Rb", 1)


# --- HW_LD and HW_ST -----------------------------------------------------------

def g_hwldst(b):
    """Types <15:13>, lengths <12:11>, the 11-bit signed displacement, the
    address forced to the data size, the data of LDBU, LDWU, LDL, LDQ
    [IDS pp. 569-570]."""
    L = A.HW8_LEN
    T = A.HW8_TYPE
    ld, st = A.hw_ld8, A.hw_st8
    q = 0x8123456789ABCDEF
    begin(b, "HW_LD/HW_ST physical: each length, displacements, alignment")
    for r, v in ((2, q), (3, 0xFEDCBA98), (4, 0x1234ABCD), (5, 0x1FF)):
        b.load(r, v)
    sn = Sn().li(1, DATA_PA + 0x100).li(10, DATA_PA + 0x500)
    sn.e(st(2, 1, 0, T["p"], L["q"]), st(3, 1, 8, T["p"], L["l"]),
         st(4, 1, 0x10, T["p"], L["w"]), st(5, 1, 0x18, T["p"], L["b"]),
         ld(6, 1, 0, T["p"], L["q"]), ld(7, 1, 8, T["p"], L["l"]),
         ld(8, 1, 0x10, T["p"], L["w"]), ld(9, 1, 0x18, T["p"], L["b"]),
         ld(11, 10, -0x400, T["p"], L["q"]),          # 0x500 - 0x400
         ld(12, 1, 3, T["p"], L["q"]),                # forced to 0x100
         ld(13, 1, 0xB, T["p"], L["l"]),              # forced to 0x108
         ld(14, 1, 0x11, T["p"], L["w"]),             # forced to 0x110
         st(2, 1, 0x27, T["p"], L["w"]),              # forced to 0x126
         st(2, 10, -0x3E7, T["p"], L["l"]),           # 0x500-0x3E7 -> 0x118
         ld(15, 1, 0x18, T["p"], L["q"]))
    call(b, sn)
    stored(b, 6, "HW_LDQ/P", q)
    stored(b, 7, "HW_LDL/P (sign-extended)", 0xFFFFFFFFFEDCBA98)
    stored(b, 8, "HW_LDW/P (zero-extended)", 0xABCD)
    stored(b, 9, "HW_LDB/P", 0xFF)
    stored(b, 11, "HW_LDQ/P, displacement -0x400", q)
    stored(b, 12, "HW_LDQ/P at +3: forced to the quadword", q)
    stored(b, 13, "HW_LDL/P at +0xB: forced to the longword",
           0xFFFFFFFFFEDCBA98)
    stored(b, 14, "HW_LDW/P at +0x11: forced to the word", 0xABCD)
    b.li(1, kseg(DATA_PA))
    b.emit(mem(OP["LDWU"], 2, 1, 0x126))
    stored(b, 2, "HW_STW/P at +0x27: the word at +0x26", q & 0xFFFF)
    stored(b, 15, "HW_STL/P, displacement -0x3E7 (forced): the quadword at "
                  "+0x118", 0x89ABCDEF)
    # The virtual forms, in kernel mode through the KSEG (SPE43).
    begin(b, "HW_LD/HW_ST virtual: V, WrChk, Alt, WrChk/Alt, PTE")
    b.load(2, q)
    va = kseg(DATA_PA + 0x200)
    sn = Sn().li(1, va)
    sn.e(st(2, 1, 0, T["v"], L["q"]), ld(3, 1, 0, T["v"], L["q"]),
         ld(4, 1, 0, T["wchk"], L["q"]), ld(5, 1, 4, T["a"], L["l"]),
         ld(6, 1, 0, T["wchk_a"], L["w"]), ld(7, 1, 0, T["pte"], L["q"]),
         st(R31, 1, 8, T["a"], L["q"]), ld(8, 1, 8, T["v"], L["q"]))
    call(b, sn)
    stored(b, 3, "HW_LDQ/V", q)
    stored(b, 4, "HW_LDQ/WrChk", q)
    stored(b, 5, "HW_LDL/Alt at +4", 0xFFFFFFFF81234567)
    stored(b, 6, "HW_LDW/WrChk/Alt", 0xCDEF)
    stored(b, 7, "HW_LDQ/PTE", q)
    stored(b, 8, "HW_STQ/Alt then HW_LDQ/V", 0)
    # Alt with M_MODE<ALT> = user: the superpage is kernel-only, DFAULT with
    # M_STAT<ACV> and OPCODE 1B [pp. 507, 510].
    begin(b, "HW_LDQ/Alt with ALT = user: DFAULT")
    b.load(2, 0x5A5A5A5A5A5A5A5A)
    sn = Sn().li(1, va).li(17, 3 << 6).e(mt(17, I["m_mode_alt"]), A.ifetchb())
    k = sn.idx()
    sn.e(ld(2, 1, 0, T["a"], L["q"]), mt(R31, I["m_mode_alt"]), A.ifetchb())
    pa = b.pal8.here
    b.trap_clear()
    call(b, sn)
    b.trap_check(vector=0x380, exc_addr=pa + 4 * k + 1, va=va,
                 exc_sum=2 << 8, exc_sum_mask=0x1F00)
    tr(b, image.TR_MM_STAT, "M_STAT", (0x1B << 4) | 2, 0xFFF)
    stored(b, 2, "r2 (not written)", 0x5A5A5A5A5A5A5A5A)
    # The codes the 21464 does not have (N4: 001 and 011) and the load-only
    # types on a store raise OPCDEC, in PALmode too.
    for nm, word in (("HW_LD type 001", ld(1, 31, 0, 1, L["q"])),
                     ("HW_LD type 011", ld(1, 31, 0, 3, L["q"])),
                     ("HW_ST type 001", st(R31, 31, 0, 1, L["q"])),
                     ("HW_ST type 010 (PTE)", st(R31, 31, 0, 2, L["q"])),
                     ("HW_ST type 101 (WrChk)", st(R31, 31, 0, 5, L["q"])),
                     ("HW_ST type 111 (WrChk/Alt)", st(R31, 31, 0, 7, L["q"]))):
        begin(b, "%s raises OPCDEC (N4)" % nm)
        pa = b.pal8.here
        b.trap_clear()
        call(b, Sn().e(word))
        b.trap_check(vector=0x400, exc_addr=pa + 1)
    # A HW_LD/PTE that misses: the double miss (no VA written [p. 511]),
    # DTBM_DOUBLE, or DTBM_DOUBLE_ALT with M_CTL<DBL_ALT> (N12).
    for alt in (False, True):
        begin(b, "HW_LD/PTE miss: DTBM_DOUBLE%s" % ("_ALT" if alt else ""))
        b.li(1, UNMAPPED)
        b.trap_clear()
        b.emit(ldq(2, 1, 0))                       # VA = UNMAPPED
        sn = Sn().li(1, UNMAPPED + 0x2000)
        if alt:
            sn.li(17, image.M_CTL8 | CTL_DBL).e(mt(17, I["m_ctl"]),
                                                A.ifetchb())
        k = sn.idx()
        sn.e(ld(3, 1, 0, T["pte"], L["q"]))
        if alt:
            sn.li(17, image.M_CTL8).e(mt(17, I["m_ctl"]), A.ifetchb())
        pa = b.pal8.here
        call(b, sn)
        b.expected_traps += 2
        tr(b, TR_COUNT, "trap count", 2)
        tr(b, TR_VEC, "trap vector", 0x180 if alt else 0x100)
        tr(b, image.TR8_EXC_ADDR, "EXC_ADDR", pa + 4 * k + 1)
        tr(b, TR_VA, "VA (the single miss's)", UNMAPPED)


# --- modes, jumps, RET, CALL_PAL, the entries ---------------------------------

def g_modes(b):
    """PC<1:0>: 00 normal, 01 PALmode, 11 SuperPALmode [IDS pp. 484-485,
    573]; RET's transitions, Table 17-5 [p. 574]; N1, N2."""
    begin(b, "RET and JSR in PALmode: PALmode, SuperPALmode, back")
    b.load(20, 0)
    sn = Sn()
    a = sn.a
    b0 = sn.idx()
    sn.e(bra(OP["BR"], 1, 0))               # r1 = PC+4 | 01 (N2)
    sn.e(lda(2, 1, 4 * 3))                  # word b0+4
    sn.e(A.ret(2, ra=3))                    # stays in PALmode; r3 = link
    sn.e(lda(20, R31, 0x666))               # skipped
    b4 = sn.idx()
    sn.e(bra(OP["BR"], 4, 0))               # r4 (PALmode)
    sn.e(lda(5, 4, 4 * 4), opl(0x11, 0x20, 5, 2, 5))   # word b4+5, | 2
    sn.e(A.ret(5))                          # to SuperPALmode
    sn.e(lda(20, R31, 0x667))               # skipped
    b8 = sn.idx()
    sn.e(bra(OP["BR"], 6, 0))               # r6 (SuperPALmode)
    sn.e(mf(7, I["pal_base"]))              # PALmode's instructions work
    sn.e(lda(8, 6, 4 * 5), opl(0x11, 0x08, 8, 2, 8))   # word b8+6, & ~2
    sn.e(jmp(1, 9, 8))                      # JSR to PALmode; r9 = link
    sn.e(lda(20, R31, 0x668))               # skipped
    sn.e(bra(OP["BR"], 10, 0))              # r10 (PALmode)
    pa = b.pal8.here
    call(b, sn)
    stored(b, 1, "BR in PALmode: link with mode 01", pa + 4 * (b0 + 1) + 1)
    stored(b, 3, "RET in PALmode: link with mode 01", pa + 4 * (b0 + 3) + 1)
    stored(b, 4, "after RET to 01: PALmode", pa + 4 * (b4 + 1) + 1)
    stored(b, 6, "after RET to 11: SuperPALmode", pa + 4 * (b8 + 1) + 3)
    stored(b, 7, "HW_MFPR in SuperPALmode", PAL_PA)
    stored(b, 9, "JSR in SuperPALmode: link with mode 11",
           pa + 4 * (b8 + 5) + 3)
    stored(b, 10, "after JSR to 01: PALmode", pa + 4 * (b8 + 7) + 1)
    stored(b, 20, "the skipped words", 0)
    # A CALL_PAL from SuperPALmode keeps PC<1> [IDS p. 499] and S1 gets the
    # mode (inference, 1.3): the image's counting entry records S1 and
    # returns through it, to SuperPALmode.
    begin(b, "CALL_PAL and OPCDEC in SuperPALmode: S1, EXC_ADDR mode 11")
    sn = Sn()
    b0 = sn.idx()
    sn.e(bra(OP["BR"], 1, 0), lda(2, 1, 4 * 3), opl(0x11, 0x20, 2, 2, 2),
         A.ret(2))                          # word b0+4, SuperPALmode
    k = sn.idx()
    sn.e(call_pal(0x09), bra(OP["BR"], 3, 0))
    pa = b.pal8.here
    b.trap_clear()
    call(b, sn)
    b.trap_check(vector=0x2000 + (0x09 << 6), exc_addr=pa + 4 * (k + 1) + 3)
    stored(b, 3, "back in SuperPALmode after the CALL_PAL",
           pa + 4 * (k + 2) + 3)
    sn = Sn()
    sn.e(bra(OP["BR"], 1, 0), lda(2, 1, 4 * 3), opl(0x11, 0x20, 2, 2, 2),
         A.ret(2))
    k = sn.idx()
    sn.e(1 << 26, bra(OP["BR"], 3, 0))      # opcode 01: OPCDEC
    pa = b.pal8.here
    b.trap_clear()
    call(b, sn)
    b.trap_check(vector=0x400, exc_addr=pa + 4 * k + 3)
    stored(b, 3, "resumed in SuperPALmode", pa + 4 * (k + 2) + 3)
    # A RET to 10 is taken as normal mode [decision]; the snippet returns
    # through S1 | 2.
    begin(b, "RET from PALmode with Rb<1:0> = 10: normal mode")
    sn = Sn()
    for w in image.PAL8_RETURN[:-1]:
        sn.e(w)
    sn.e(opl(0x11, 0x20, S1A, 2, S1A))       # BIS S1,#2,S1
    words = sn.words()
    pa = b.pal8.here
    for w in words:
        b.pal8.emit(w)
    b.pal8.emit(A.ret(S1B))
    b.li(16, pa)
    b.emit(call_pal(0x91))
    at = b.va()
    b.emit(bra(OP["BR"], 1, 0))
    stored(b, 1, "BR after the RET: normal mode, PC<1:0> = 00", at + 4)
    # In normal mode Rb<1:0> are ignored: a RET cannot raise the mode
    # [p. 573]; its Ra gets the next PC.
    begin(b, "RET in normal mode with Rb<1:0> = 01: stays normal")
    b.emit(bra(OP["BR"], 1, 0))
    at = b.va()
    b.emit(lda(2, 1, 4 * 4))
    b.emit(opl(0x11, 0x20, 2, 1, 2))
    b.emit(A.ret(2, ra=3))
    b.emit(lda(20, R31, 0x669))
    b.emit(bra(OP["BR"], 4, 0))
    stored(b, 3, "RET's link in normal mode", at + 12)
    stored(b, 4, "after the RET: normal mode", at + 20)
    # BAD_JUMP_IVA: a jump outside the virtual address space, at the jump,
    # which does not complete (Ra unwritten); EXC_ADDR and EXC_SUM [pp. 629,
    # 633]. SEG52 is no superpage of the I-stream here (I_CTL<SPE<2>> clear)
    # and not a 43-bit address.
    for nm, target in (("bit 63 alone", 0x8000000000000000),
                       ("SEG52, not enabled in I_CTL", image.SEG52 | 0x100000)):
        begin(b, "BAD_JUMP_IVA: JSR to %s" % nm)
        b.load(1, target)
        b.load(2, 0x2222)
        b.trap_clear()
        at = b.va()
        word = jmp(1, 2, 1)
        b.emit(word)
        b.trap_check(vector=0x900, exc_addr=at, exc_sum=2 << 8,
                     exc_sum_mask=0x1F00)
        tr(b, image.TR8_EXC_SUM, "EXC_SUM (the 21464's)", regs_of(word))
        stored(b, 2, "Ra (not written)", 0x2222)
    begin(b, "BAD_JUMP_IVA: RET from PALmode to a malformed address")
    sn = Sn().li(1, 0x8000000000000000)
    k = sn.idx()
    sn.e(A.ret(1))
    pa = b.pal8.here
    b.trap_clear()
    call(b, sn)
    b.trap_check(vector=0x900, exc_addr=pa + 4 * k + 1)


def g_shadow(b):
    """The shadow registers' table in every position [IDS p. 580]: operand A
    and the destination R24, R25 -> S0, S1; operand B R22, R23 -> S0, S1;
    the legacy CMOV's Rc not replaced; the native CMOV's split, Restriction
    13 and the same for R22 [inference, 1.3]."""
    begin(b, "shadow registers: every position, legacy and native CMOV")
    b.load(22, 0x2222222222222222)
    b.load(23, 0x2323232323232323)
    b.load(11, 0x2424)
    b.load(15, 0x2525)
    sn = Sn()
    sn.e(bis(R31, 24, 1), bis(R31, 25, 2),          # B: arch R24, R25
         lda(24, R31, 0x5050), lda(25, R31, 0x5151),  # dest: S0, S1
         bis(24, R31, 3), bis(25, R31, 4),          # A: S0, S1
         bis(R31, 22, 5), bis(R31, 23, 6),          # B: S0, S1
         bis(22, R31, 7), bis(23, R31, 8),          # A: arch R22, R23
         lda(22, R31, 0x2244),                      # dest: arch R22
         bis(R31, 22, 9), bis(22, R31, 10))
    # Legacy CMOV (no placeholder after it) writes the architectural R24.
    sn.e(opr(0x11, 0x24, R31, 11, 24), bis(R31, 24, 12), bis(24, R31, 13))
    # Native, not taken: Rc's destination S0 gets Rc's operand B, R24.
    sn.e(opr(0x11, 0x26, R31, 11, 24), NOP, bis(24, R31, 14))
    # Native, taken: S0 <- Rb.
    sn.e(opr(0x11, 0x24, R31, 15, 24), NOP, bis(24, R31, 17))
    # Native with Rc = R22, not taken: the architectural R22 gets S0.
    sn.e(opr(0x11, 0x26, R31, 11, 22), NOP, bis(22, R31, 18))
    # The architectural R24 and R25 back (legacy CMOVs), for the driver.
    sn.e(opr(0x11, 0x24, R31, 1, 24), bis(R31, R31, 19),
         opr(0x11, 0x24, R31, 2, 25), bis(R31, R31, 19))
    call(b, sn)
    stored(b, 3, "S0 as operand A (R24)", 0x5050)
    stored(b, 4, "S1 as operand A (R25)", 0x5151)
    stored(b, 5, "S0 as operand B (R22)", 0x5050)
    stored(b, 6, "S1 as operand B (R23)", 0x5151)
    stored(b, 7, "R22 as operand A: the architectural one",
           0x2222222222222222)
    stored(b, 8, "R23 as operand A", 0x2323232323232323)
    stored(b, 9, "S0 after writing R22", 0x5050)
    stored(b, 10, "R22 as destination: the architectural one", 0x2244)
    stored(b, 12, "legacy CMOV wrote the architectural R24", 0x2424)
    stored(b, 13, "... and S0 is unchanged", 0x5050)
    stored(b, 14, "native CMOV R24, not taken: S0 <- the architectural R24",
           0x2424)
    stored(b, 17, "native CMOV R24, taken: S0 <- Rb", 0x2525)
    stored(b, 18, "native CMOV R22, not taken: R22 <- S0 [inference]",
           0x2525)
    stored(b, 22, "R22 in normal mode", 0x2525)
    stored(b, 23, "R23 in normal mode", 0x2323232323232323)
    # CALL_PAL writes S1 with the return PC and the caller's mode
    # [inference, 1.3]: CALL_PAL 91 keeps it in PAL_TEMP2.
    begin(b, "CALL_PAL writes S1: the return PC, mode 00")
    pa, at = call(b, Sn().e(mf(1, I["pal_temp2"])))
    stored(b, 1, "S1 (PAL_TEMP2) inside CALL_PAL 91", at + 4)


def g_entries(b):
    """Each entry: its offset, the IPRs it writes (Table 22-3 [IDS p. 633]),
    the restart PC (Table 22-2 [pp. 629-633])."""
    T = A.HW8_TYPE
    # ARITH: EXC_ADDR the next instruction, EXC_SUM the 21464's.
    begin(b, "ARITH: EXC_ADDR and EXC_SUM")
    b.load(1, 0x7FFFFFFFFFFFFFFF)
    b.trap_clear()
    at = b.va()
    word = opl(0x10, 0x60, 1, 1, 2)         # ADDQ/V r1,#1,r2
    b.emit(word)
    b.trap_check(vector=0x600, exc_addr=at, exc_sum=0xC0 | (2 << 8))
    tr(b, image.TR8_EXC_ADDR, "EXC_ADDR (the next instruction)", at + 4)
    tr(b, image.TR8_EXC_SUM, "EXC_SUM: INT<13>, IOV<6>, registers",
       (1 << 13) | (1 << 6) | regs_of(word))
    # DTBM_SINGLE writes DTBMS_RET_ADDR, not EXC_ADDR [p. 483].
    for nm, word, mstat in (("LDQ", ldq(2, 1, 8), 0x29 << 4),
                            ("STQ", stq(2, 1, 16), (0x2D << 4) | 1)):
        begin(b, "DTBM_SINGLE (%s): DTBMS_RET_ADDR, EXC_SUM, VA, VA_FORM, "
                 "M_STAT; EXC_ADDR kept" % nm)
        b.li(1, UNMAPPED)
        b.trap_clear()
        last = b.va()
        b.emit(1 << 26)                     # OPCDEC: EXC_ADDR = last
        at = b.va()
        b.emit(word)
        b.expected_traps += 2
        tr(b, TR_COUNT, "trap count", 2)
        tr(b, TR_VEC, "trap vector", 0x300)
        tr(b, image.TR8_DTBMS, "DTBMS_RET_ADDR", at)
        tr(b, image.TR8_EXC_ADDR, "EXC_ADDR (the OPCDEC's)", last)
        tr(b, image.TR8_EXC_SUM, "EXC_SUM", regs_of(word))
        va = UNMAPPED + (word & 0xFFFF)
        tr(b, TR_VA, "VA", va)
        tr(b, image.TR8_VA_FORM, "VA_FORM", (va >> 10) & 0x1FFFFFFF8)
        tr(b, image.TR_MM_STAT, "M_STAT", mstat, 0xFFF)
    # The DTB miss in PALmode: DTBMS_RET_ADDR has mode 01.
    begin(b, "DTBM_SINGLE from PALmode: DTBMS_RET_ADDR mode 01")
    sn = Sn().li(1, UNMAPPED)
    k = sn.idx()
    sn.e(A.hw_ld8(2, 1, 0, T["v"]))
    pa = b.pal8.here
    b.trap_clear()
    call(b, sn)
    b.trap_check(vector=0x300, exc_addr=pa + 4 * k + 1, va=UNMAPPED)
    tr(b, image.TR8_DTBMS, "DTBMS_RET_ADDR", pa + 4 * k + 1)
    tr(b, image.TR_MM_STAT, "M_STAT (opcode 1B)", 0x1B << 4, 0xFFF)
    # In console mode the DTB and ITB misses take the _CONS entries
    # [pp. 491, 508].
    begin(b, "DTBM_SINGLE_CONS in console mode")
    b.li(1, UNMAPPED)
    call(b, Sn().li(17, 1 << 5).e(mt(17, I["m_mode_con"]), A.ifetchb()))
    b.trap_clear()
    at = b.va()
    b.emit(ldq(2, 1, 0))
    b.trap_check(vector=0x800, exc_addr=at, va=UNMAPPED)
    call(b, Sn().e(mt(R31, I["m_mode_con"]), A.ifetchb()))
    for cons in (False, True):
        begin(b, "ITB_MISS%s: EXC_ADDR, IVA_FORM" % ("_CONS" if cons else ""))
        vpte = 0xFFFFFFFE00000000
        sn = Sn().li(17, vpte).e(mt(17, I["i_ctl_vpte"]))
        if cons:
            sn.li(17, 1 << 5).e(mt(17, I["i_mode_con"]))
        sn.e(A.ifetchb())
        call(b, sn)
        b.li(1, image.PROBE_VA + 0x8000)
        b.trap_clear()
        b.emit(jmp(1, 17, 1))               # JSR r17: the probe's return
        b.trap_check(vector=0x880 if cons else 0x580,
                     exc_addr=image.PROBE_VA + 0x8000)
        tr(b, image.TR8_IVA_FORM, "IVA_FORM", vpte |
           (((image.PROBE_VA + 0x8000) >> 10) & 0x1FFFFFFF8))
        call(b, Sn().e(mt(R31, I["i_ctl_vpte"]), mt(R31, I["i_mode_con"]),
                       A.ifetchb()))
    # A bad VA sign is DFAULT with ACV (N12), with VA and EXC_SUM.
    begin(b, "DFAULT: a bad VA sign (ACV)")
    b.load(1, 0x8000000000000000)
    b.trap_clear()
    at = b.va()
    word = ldq(2, 1, 0)
    b.emit(word)
    b.trap_check(vector=0x380, exc_addr=at, va=0x8000000000000000,
                 exc_sum=2 << 8, exc_sum_mask=0x1F00)
    tr(b, image.TR_MM_STAT, "M_STAT<ACV>", (0x29 << 4) | 2, 0xFFF)
    # UNALIGN (across a page, docs/cpu-fidelity.md): the 21464's EXC_SUM and
    # M_STAT (the plain opcode).
    begin(b, "UNALIGN: EXC_SUM, VA, M_STAT")
    b.emit(lda(9, R_SCR, 0x2000 - 4))
    b.trap_clear()
    at = b.va()
    word = ldq(2, 9, 0)
    b.emit(word)
    b.trap_check(vector=0x280, exc_addr=at, va=b.scratch + 0x2000 - 4)
    tr(b, image.TR8_EXC_SUM, "EXC_SUM", regs_of(word))
    tr(b, image.TR_MM_STAT, "M_STAT", 0x29 << 4, 0xFFF)
    # MT_FPCR traps to its entry outside PALmode, EXC_ADDR the next
    # instruction [pp. 581, 630].
    begin(b, "MT_FPCR: EXC_ADDR the next instruction")
    b.trap_clear()
    at = b.va()
    b.emit(A.fop(0x17, 0x024, 31, 31, 31))
    b.trap_check(vector=0x700, exc_addr=at)
    tr(b, image.TR8_EXC_ADDR, "EXC_ADDR", at + 4)
    # The PALcode forms outside PALmode raise OPCDEC [inference, 1.2].
    for nm, word in (("HW_MFPR", mf(13, I["pal_base"])),
                     ("HW_MTPR", mt(R31, I["vctl"])),
                     ("HW_LD", A.hw_ld8(13, R_TR, 0)),
                     ("HW_ST", A.hw_st8(R31, R_SCR, 0x100)),
                     ("IFETCHB", A.ifetchb())):
        begin(b, "%s in kernel mode raises OPCDEC" % nm)
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=0x400, exc_addr=at)
    begin(b, "IFETCHB in PALmode: nothing to wait for")
    call(b, Sn().e(lda(1, R31, 1), A.ifetchb(), lda(1, 1, 2)))
    stored(b, 1, "r1", 3)


# --- translation -----------------------------------------------------------------

def g_superpages(b):
    """Appendix C's superpages and sign check [IDS pp. 688-689, 695; N14]:
    SPE52 with 43-bit virtual addresses, SPE43 (the KSEG), SPE32; the loads
    and stores run compiled on a JIT lane's last pass."""
    begin(b, "superpages: SPE52, SPE43, SPE32 reach the same quadword")
    b.li(1, kseg(DATA_PA))
    b.li(2, image.SEG52 | DATA_PA)
    b.li(3, image.SEG32 | DATA_PA)
    b.load(4, 0x0123456789ABCDEF)
    b.load(5, 0xFEDCBA9876543210)
    b.emit(stq(4, 1, 0x300))
    b.emit(ldq(6, 2, 0x300))
    b.emit(ldq(7, 3, 0x300))
    b.emit(stq(5, 2, 0x308))
    b.emit(ldq(8, 1, 0x308))
    b.emit(ldq(9, 3, 0x308))
    stored(b, 6, "LDQ through SPE52", 0x0123456789ABCDEF)
    stored(b, 7, "LDQ through SPE32", 0x0123456789ABCDEF)
    stored(b, 8, "STQ through SPE52, LDQ through SPE43", 0xFEDCBA9876543210)
    stored(b, 9, "... through SPE32", 0xFEDCBA9876543210)
    begin(b, "superpages: SPE52 disabled, the address fails the sign check")
    b.li(2, image.SEG52 | DATA_PA)
    call(b, Sn().li(17, 3 << 24).e(mt(17, I["m_ctl_spe"]), A.ifetchb()))
    b.trap_clear()
    at = b.va()
    b.emit(ldq(6, 2, 0x300))
    b.trap_check(vector=0x380, exc_addr=at, va=image.SEG52 | DATA_PA | 0x300)
    tr(b, image.TR_MM_STAT, "M_STAT<ACV>", (0x29 << 4) | 2, 0xFFF)
    call(b, Sn().li(17, image.M_CTL8).e(mt(17, I["m_ctl_spe"]), A.ifetchb()))
    # The 21264's 48-bit superpage form (VA<47:46> = 2) is no superpage of
    # the 21464, and no 43-bit address: a load there faults, compiled too.
    begin(b, "the 21264's SPE<2> form (SEG48): bad sign on the 21464")
    b.li(2, image.SEG48 | DATA_PA)
    b.trap_clear()
    at = b.va()
    b.emit(ldq(6, 2, 0x300))
    b.trap_check(vector=0x380, exc_addr=at, va=image.SEG48 | DATA_PA | 0x300)
    begin(b, "a 52-bit address with 43-bit virtual addresses: bad sign")
    b.li(2, 0x0001000000000000)
    b.trap_clear()
    at = b.va()
    b.emit(ldq(6, 2, 0))
    b.trap_check(vector=0x380, exc_addr=at, va=0x0001000000000000)


def probe_d(b, va, hit, value, reg=6):
    """A kernel-mode LDQ: the value, or a DTB miss (the image resumes
    after it)."""
    b.load(reg, 0x5A5A)
    b.li(3, va)
    b.trap_clear()
    at = b.va()
    b.emit(ldq(reg, 3, 0))
    b.trap_check(vector=None if hit else 0x300, exc_addr=at)
    stored(b, reg, "LDQ %#x: %s" % (va, "the page's" if hit else "missed"),
           value if hit else 0x5A5A)


def probe_i(b, va, hit):
    """A kernel-mode JSR into the probe region: the stub sets R20 and
    returns; a miss returns to R17 from the ITB_MISS entry."""
    b.load(20, 0)
    b.li(2, va)
    b.trap_clear()
    b.emit(jmp(1, 17, 2))
    b.trap_check(vector=None if hit else 0x580, exc_addr=va)
    stored(b, 20, "fetch at %#x: %s" % (va, "ran" if hit else "missed"),
           1 if hit else 0)


def g_groups(b):
    """TB groups: an entry carries the writer's TPU group and matches only
    in it [IDS pp. 305-307]; ITB_IM and DTB_IM by mode, ITB_IS and DTB_IS
    [pp. 494-495, 502-503]; a PTE with VALID clear makes no entry (N10).
    The hits are compiled loads on a JIT lane's last pass."""
    val = 0x6060606060606060
    b.begin("ev8n: the probe pages")
    b.need(rows.NATIVE8)
    b.li(1, kseg(DATA_PA))
    b.load(2, val)
    b.emit(stq(2, 1, 0x400))
    b.li(1, kseg(PROBE_PA))
    for k, w in enumerate((lda(20, R31, 1), A.ret(17))):
        b.load(2, w)
        b.emit(mem(OP["STL"], 2, 1, 4 * k))
    b.emit(call_pal(0x8E))                  # IC_FLUSH after the stores

    G = 0x30000000                          # the group probe page
    pte = pte8(DATA_PA, PTE_K)
    begin(b, "TB groups: a fill in group 1 is not seen from group 0")
    b.li(1, UNMAPPED)
    call(b, dtb_fill(ctx(Sn().li(17, 0x10).e(mt(17, I["dtb_im"])), 1, 0),
                     G, pte))
    probe_d(b, G + 0x400, True, val)
    call(b, ctx(Sn(), 0, 0))
    probe_d(b, G + 0x400, False, val)
    call(b, ctx(Sn(), 1, 0))
    probe_d(b, G + 0x400, True, val)
    call(b, ctx(Sn().li(17, 0x01).e(mt(17, I["dtb_im"])), 1, 0))
    probe_d(b, G + 0x400, False, val)
    call(b, ctx(Sn(), 0, 0))

    # DTB_IM's modes over four entries: A (group 1, ASM), B (group 1, ASN 5),
    # C (group 1, ASN 6), D (group 0, ASN 5); the mode written in group 1,
    # ASN 5. The values the IDS gives are 00, 01, 03, 05, 0D, 10, 16.
    ents = (("A", 1, 5, 0x10, 0x30002000), ("B", 1, 5, 0, 0x30004000),
            ("C", 1, 6, 0, 0x30006000), ("D", 0, 5, 0, 0x30008000))
    stays = {0x00: "", 0x01: "D", 0x03: "BCD", 0x05: "AD", 0x0D: "ACD",
             0x10: "", 0x16: "ABCD"}
    for mode, keep in sorted(stays.items()):
        begin(b, "DTB_IM mode %02x" % mode)
        b.li(1, UNMAPPED)
        sn = Sn().li(17, 0x10).e(mt(17, I["dtb_im"]))
        for nm, grp, asn, asm, va in ents:
            dtb_fill(ctx(sn, grp, asn), va, pte8(DATA_PA, PTE_K | asm))
        ctx(sn, 1, 5).li(17, mode).e(mt(17, I["dtb_im"]), A.ifetchb())
        call(b, sn)
        for nm, grp, asn, asm, va in ents:
            call(b, ctx(Sn(), grp, asn))
            probe_d(b, va + 0x400, nm in keep, val)
        call(b, ctx(Sn().li(17, 0x10).e(mt(17, I["dtb_im"])), 0, 0))

    begin(b, "DTB_IS: one page of this group and ASN")
    b.li(1, UNMAPPED)
    sn = Sn().li(17, 0x10).e(mt(17, I["dtb_im"]))
    ctx(sn, 1, 5)
    dtb_fill(sn, 0x3000A000, pte)
    dtb_fill(sn, 0x3000C000, pte)
    sn.li(17, 0x3000A000).e(mt(17, I["dtb_is"]), A.ifetchb())
    call(b, sn)
    probe_d(b, 0x3000A400, False, val)
    probe_d(b, 0x3000C400, True, val)
    call(b, ctx(Sn().li(17, 0x10).e(mt(17, I["dtb_im"])), 0, 0))

    begin(b, "DTB_PTE with VALID clear, and from R31: no entry (N10)")
    b.li(1, UNMAPPED)
    sn = Sn()
    dtb_fill(sn, 0x3000E000, pte & ~1)
    sn.s0(0x30010000).e(mt(S0B, I["dtb_tag0"]), mt(S0B, I["dtb_tag1"]),
                        mt(R31, I["dtb_pte0"]), mt(R31, I["dtb_pte1"]))
    call(b, sn)
    probe_d(b, 0x3000E400, False, val)
    probe_d(b, 0x30010400, False, val)

    # ITB_IM's modes, the same four entries in the probe region, the I_PCTX
    # context; an IC_FLUSH after it, as PALcode must [p. 494].
    pv = image.PROBE_VA
    ients = (("A", 1, 5, 0x10, pv), ("B", 1, 5, 0, pv + 0x2000),
             ("C", 1, 6, 0, pv + 0x4000), ("D", 0, 5, 0, pv + 0x6000))
    for mode, keep in sorted(stays.items()):
        begin(b, "ITB_IM mode %02x" % mode)
        sn = Sn().li(17, 0x10).e(mt(17, I["itb_im"]))
        for nm, grp, asn, asm, va in ients:
            itb_fill(ctx(sn, grp, asn, itb=True), va,
                     pte8(PROBE_PA, 0x100 | asm))
        ctx(sn, 1, 5, itb=True).li(17, mode).e(mt(17, I["itb_im"]),
                                               mt(R31, I["ic_flush"]),
                                               A.ifetchb())
        call(b, sn)
        for nm, grp, asn, asm, va in ients:
            call(b, ctx(Sn(), grp, asn, itb=True))
            probe_i(b, va, nm in keep)
        call(b, ctx(Sn().li(17, 0x10).e(mt(17, I["itb_im"]),
                                        mt(R31, I["ic_flush"])), 0, 0,
                    itb=True))
    begin(b, "ITB_IS")
    sn = Sn().li(17, 0x10).e(mt(17, I["itb_im"]))
    itb_fill(sn, pv, pte8(PROBE_PA, 0x100))
    itb_fill(sn, pv + 0x2000, pte8(PROBE_PA, 0x100))
    sn.li(17, pv).e(mt(17, I["itb_is"]), mt(R31, I["ic_flush"]), A.ifetchb())
    call(b, sn)
    probe_i(b, pv, False)
    probe_i(b, pv + 0x2000, True)
    call(b, Sn().li(17, 0x10).e(mt(17, I["itb_im"]), mt(R31, I["ic_flush"]),
                                A.ifetchb()))


def g_pages(b):
    """64 KB pages (VA_SIZE and PAGE_SIZE set: 52-bit VA, the PFN PA<47:16>)
    and granularity hints, "increased by one" with 64 KB pages [IDS pp.
    306-307, 504, 690]; VA_FORM's formats [pp. 512-513, 692-693]. The loads
    run compiled on a JIT lane's last pass."""
    m52 = image.M_CTL8 | CTL_SIZES
    vals = {}
    begin(b, "64 KB pages: the data")
    for k, off in enumerate((0, 0x8008, 0xFFF8, 0x40000, 0x7FFF8, 0x100000 +
                             0xFFF8)):
        v = 0x6400000000000000 | (off << 8) | k
        vals[off] = v
        b.li(1, kseg(PA64 + off))
        b.load(2, v)
        b.emit(stq(2, 1, 0))
    X, X2 = 0x40000000, 0x40080000
    begin(b, "a 64 KB page, GH 0, in 52-bit mode")
    b.li(1, UNMAPPED)
    sn = Sn().li(17, m52).e(mt(17, I["m_ctl_spe"]), A.ifetchb())
    sn.li(17, 0x10).e(mt(17, I["dtb_im"]))
    dtb_fill(sn, X, pte64(PA64, PTE_K))
    call(b, sn)
    for off in (0, 0x8008, 0xFFF8):
        probe_d(b, X + off, True, vals[off])
    probe_d(b, X + 0x10000, False, 0)
    begin(b, "a 64 KB page with GH 1: 512 KB")
    b.li(1, UNMAPPED)
    sn = Sn().li(17, 0x10).e(mt(17, I["dtb_im"]))
    dtb_fill(sn, X2 & ~0x7FFFF, pte64(PA64, PTE_K | 0x20))
    call(b, sn)
    for off in (0x40000, 0x7FFF8):
        probe_d(b, (X2 & ~0x7FFFF) + off, True, vals[off])
    # VA_FORM in 52-bit mode: VA<51:16> in <38:3>, the sign in <41:39>;
    # with REDUCED_PT, quadrant 1 (VA<51:50> = 01): 01 in <38:37>, VA<49:29>
    # in <23:3> [C.4.3, C.4.4].
    z = 0x0000123456780000
    begin(b, "VA_FORM, 52-bit VA and 64 KB pages")
    b.li(1, UNMAPPED)
    b.li(2, z)
    b.trap_clear()
    at = b.va()
    b.emit(ldq(3, 2, 0))
    b.trap_check(vector=0x300, exc_addr=at, va=z)
    tr(b, image.TR8_VA_FORM, "VA_FORM", (z >> 13) & 0x7FFFFFFFF8)
    r = 0x0004000020000000
    begin(b, "VA_FORM, reduced page tables, quadrant 1")
    call(b, Sn().li(17, m52 | CTL_RED).e(mt(17, I["m_ctl"]), A.ifetchb()))
    b.li(2, r)
    b.trap_clear()
    at = b.va()
    b.emit(ldq(3, 2, 0))
    b.trap_check(vector=0x300, exc_addr=at, va=r)
    tr(b, image.TR8_VA_FORM, "VA_FORM", (1 << 37) | ((r >> 26) & 0xFFFFF8))
    call(b, Sn().li(17, image.M_CTL8).e(mt(17, I["m_ctl"]),
                                        mt(17, I["m_ctl_spe"]),
                                        A.ifetchb()))
    begin(b, "an 8 KB page with GH 1: 64 KB")
    b.li(1, UNMAPPED)
    Y = 0x50000000
    sn = Sn().li(17, 0x10).e(mt(17, I["dtb_im"]))
    dtb_fill(sn, Y, pte8(PA64, PTE_K | 0x20))
    call(b, sn)
    probe_d(b, Y + 0xFFF8, True, vals[0xFFF8])
    probe_d(b, Y + 0x10000, False, 0)
    call(b, Sn().li(17, 0x10).e(mt(17, I["dtb_im"]), A.ifetchb()))


GROUPS = [("ev8n", "the native EV8 (ev8n): reset, IPRs, HW_LD/HW_ST, modes, "
                   "RET, shadow registers, the entries, superpages, TB "
                   "groups, 64 KB pages (ev8-native.md, section 6)")]


def g_ev8n(b):
    for g in (g_reset, g_iprs, g_hwldst, g_modes, g_shadow, g_entries,
              g_superpages, g_groups, g_pages):
        g(b)


EMIT = {"ev8n": g_ev8n}
