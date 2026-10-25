"""Tarantula's cases (group tarantula): the vector extension proposed for the
EV8, as docs/platforms/ev8-isa.md section 4 fixes it (the contract with the
nada toolchain), on the rows that have it (`ev8`, `ev8n`): the vector enable
VCTL<VEN> (section 2), VC (4.7), every VV and VS function (4.4) in register
and literal forms, masks and vector lengths (4.3), $vr31, the field rules,
FP rounding, the FPCR and the trap (4.3, section 5). Stage S7 of
docs/platforms/ev8.md; the expected values are computed here from the sheet
(and, for an FP element, from tfloat.py, the scalar /SUI instruction), not
from the emulator.

How a case gets values in and out. S7 has no vector loads or stores (SM and
RM are S8), and the sheet moves no element between vector and scalar
registers (point 13): a scalar enters through VS, a vector leaves through
memory only. So the cases use a test hook, ALPHABOX_TARANTULA_PEEK=1
(docs/headless.md), which the oracle sets for these rows: the processor's
vector registers in the last 32 KB of its EV7 register window, element i of
$vrN at offset 0x3F8000 + 0x400 x N + 8 x i, read and written with
quadword loads and stores. The image fills a register from a table of 64
quadwords (element i <- T[(start + i x step) mod 64], the subroutine t_fill)
and copies elements into the result buffer (t_read). The boot-image test
(tarantula_vv.py) puts values in through VS instead.

VEN: on the `ev8` row VCTL is the CSR at offset 0x3F0000 of the
processor's window, which the ES40 answers under the
ALPHABOX_CPU_ANY_BOARD test hook as a Marvel board does [decision, ev8.md
S7]; on the `ev8n` row it is the IPR 0xAC (N8), written in PALmode
(CALL_PAL 91), and the CSR is not decoded. VEN is 0 outside this group's
window, so the other groups' OPCDEC cases of opcodes 01-07 hold on these
rows too, and the group starts with such words on every row.

Registers: R14 the table area's address and R15 the window's, for the
whole group; R16-R22 and R23 (the link) are the subroutines'.
"""
import alpha_asm as A
from alpha_asm import OP, R31, call_pal, fop, jmp, mem, opl, opr
import image
import rows
import tfloat as T
from image import R_RES, kseg

M64 = (1 << 64) - 1
R_TAB, R_WIN, R_LINK = 14, 15, 23
I8 = A.IPR8

# PID 0's EV7 register window (cpu/ev7/Ev7.hpp, kEv7Map.csr_base): VCTL
# and the hook's register window in it (src/cpu/Tarantula.hpp).
WIN_PA = 0xFFFFFC00000
VCTL_PA = WIN_PA + 0x3F0000
PEEK_PA = WIN_PA + 0x3F8000
# The native EV8's: PID 0's block in the 21464 mode's map (N18), which the
# processor's window follows on that row (AlphaCPU_vector.cpp); the ES40
# folds both maps' addresses into one.
WIN8_PA = 0xFFFFFFC00000
VCTL8_PA = WIN8_PA + 0x3F0000
PEEK8_PA = WIN8_PA + 0x3F8000

GROUPS = [("tarantula", "Tarantula (ev8-isa.md 4): OPCDEC for 01-06 with VEN "
                        "0 and on every other row, VCTL, VC, every VV and VS "
                        "function, masks and vl, $vr31, the field rules, FP "
                        "rounding, the FPCR and the trap")]


def lda(ra, rb, d):
    return mem(OP["LDA"], ra, rb, d)


def ldq(ra, rb, d):
    return mem(OP["LDQ"], ra, rb, d)


def stq(ra, rb, d):
    return mem(OP["STQ"], ra, rb, d)


def itoft(ra, fc):
    return fop(0x14, 0x024, ra, R31, fc)


def ftoit(fa, rc):
    return fop(0x1C, 0x070, fa, R31, rc)


def mt_fpcr(fa):
    return fop(0x17, 0x024, fa, fa, fa)


def mf_fpcr(fa):
    return fop(0x17, 0x025, fa, fa, fa)


def u(x):
    return x & M64


def s64(x):
    x &= M64
    return x - (1 << 64) if x >> 63 else x


# --- the tables (64 quadwords each) -------------------------------------------


def _lcg(seed, n):
    out, x = [], seed
    for _ in range(n):
        x = (x * 6364136223846793005 + 1442695040888963407) & M64
        out.append(x)
    return out


E = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
     0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
     0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE, 0x0123456789ABCDEF,
     0xFEDCBA9876543210, 0x00FF00FF00FF00FF, 0x8000000000000001,
     0x00000000FFFF8000, 0xFFFFFFFF80000000, 0x3F, 0x40, 0x41, 0x1F, 0x20,
     0x21, 0x3E, 0x7E, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA,
     0x8000000000000000 | 63, 0xC000000000000001]
TABLES = {
    "int": E + _lcg(1, 64 - len(E)),
    "mix": _lcg(7, 64),
    # Low bits for SETVM in an irregular pattern, random high bits.
    "mask": [(v & ~1) | (1 if (i * 37 + i // 5) % 3 else 0)
             for i, v in enumerate(_lcg(11, 64))],
    "fill": [0xD000000000000000 | (0x1111 * i) for i in range(64)],
    "zero": [0] * 64,
}
QNAN, SNAN = 0x7FF8000000000123, 0x7FF4000000000456
INF, NINF = 0x7FF0000000000000, 0xFFF0000000000000
b_ = T.bits
# FP operand pairs (element i: FA[i], FB[i]): normal values whose results
# are exact or not, zeros, infinities, one NaN at a time, x/0, overflow; no
# result underflows (tfloat raises if one would).
FA = [b_(x) for x in (1.0, -2.0, 3.5, 0.1, 1e300, -1e300, 1e-5, 7.0, 0.0,
                      -0.0, float("inf"), float("-inf"), 2.0, 1 / 3, -0.75,
                      1e10)] + [QNAN, SNAN] + \
     [b_(x) for x in (5.0, -1.5, 100.0, 0.3, 2.5, -2.5, 1.0, 4.0, 9.0, 0.0,
                      float("inf"), 6.0, -7.25, 1e-3, 123456.789, -0.1,
                      3.0, 1e200, 2.0 ** 52, -(2.0 ** 60), 0.7, 11.0,
                      -13.5, 1e5, 0.25, 1.0, -1.0, 8.0, 0.2, 1e-2, 77.0,
                      -3.0, 1.0, 5.5, 2.0, -64.0, 1.0 / 7, 1e150, -1e-4,
                      0.5, 3.75, 10.0, -6.5, 1.25, 1e100, 2.0)]
FB = [b_(x) for x in (3.0, 2.0, -1.5, 0.2, 1e300, 1e300, 3.0, 0.0, 0.0,
                      0.0, float("inf"), float("inf"), float("-inf"), 3.0,
                      0.75, -1e10, 1.0, 2.0, -0.0, 1.5, 3.0, 0.7, -2.5,
                      2.5)] + [QNAN] + \
     [b_(x) for x in (4.0, 3.0, 0.0, float("-inf"), 6.0, 7.25, 1e3,
                      0.001, 10.0, 3.0, 1e200, 1.0, 2.0 ** 60, 0.3, 11.0,
                      -13.5, 1e-5, 0.25, -1.0, 1.0, 8.0, 0.1, 1e-2, 77.0,
                      3.0, 3.0, 5.5, -2.0, 64.0, 1.0 / 7, 1e150, 1e-4,
                      0.5, -3.75, 10.0, 6.5, 1.25, 1e-100, 2.0)]
# Unary operands (SQRTT, CVTTQ).
FU = [b_(x) for x in (4.0, 2.0, 0.0, -0.0, float("inf"), float("-inf"),
                      -1.0, 1e300, 1e-300, 2.5, -2.5, 3.5, 0.5, -0.5, 1e19,
                      -1e19, 9.2e18, 1.5, 123.456, 2.0 ** 63, -(2.0 ** 63),
                      1e-5, 0.1, 7.0, 1.0 / 3, 6.25, 81.0, -1.5)] + \
     [QNAN, SNAN] + [b_(1.0 + i / 8.0) for i in range(34)]
FS = [b_(2.0), b_(-0.1), b_(3.0), b_(0.0)]   # VS scalars, Fb
TABLES["fa"] = FA
TABLES["fb"] = FB
TABLES["fu"] = FU
# The rounding cases: inexact results in every element.
TABLES["ra"] = [b_(1.0 + i / 3.0) if i % 2 else b_(-(1.0 + i / 7.0))
                for i in range(64)]
TABLES["rb"] = [b_(3.0 + i / 9.0) if i % 3 else b_(0.1 * (i + 1))
                for i in range(64)]
for _k in ("fa", "fb", "fu"):
    assert len(TABLES[_k]) == 64, (_k, len(TABLES[_k]))
TAB = {}            # name -> index in the image's table area


class State:
    """The vector state as the sheet defines it, tracked through the group.
    None: not known (set by an earlier pass or case), never compared."""

    def __init__(self):
        self.vr = [[None] * 128 for _ in range(31)] + [[0] * 128]
        self.vl = None
        self.vs = None
        self.vm = None
        self.fpcr = None

    def active(self, i, m):
        return i < self.vl and (not m or self.vm[i])


# --- what the sheet says an element is (4.4) -----------------------------------


def int_op(f, a, b, vm_i):
    if f == 0x00:
        return u(a + b)
    if f == 0x01:
        return u(a - b)
    if f == 0x02:
        return u(a * b)
    if f == 0x03:
        return (a * b) >> 64
    if f == 0x04:
        return a & b
    if f == 0x05:
        return a & ~b & M64
    if f == 0x06:
        return a | b
    if f == 0x07:
        return (a | ~b) & M64
    if f == 0x08:
        return a ^ b
    if f == 0x09:
        return (a ^ ~b) & M64
    if f == 0x0A:
        return u(a << (b & 63))
    if f == 0x0B:
        return a >> (b & 63)
    if f == 0x0C:
        return u(s64(a) >> (b & 63))
    if f == 0x0D:
        return b if vm_i else a
    cmp = {0x10: a == b, 0x11: a != b, 0x12: s64(a) < s64(b),
           0x13: s64(a) <= s64(b), 0x14: s64(a) > s64(b),
           0x15: s64(a) >= s64(b), 0x16: a < b, 0x17: a <= b, 0x18: a > b,
           0x19: a >= b}
    return 1 if cmp[f] else 0


CMP_T = {0x25: "eq", 0x26: "lt", 0x27: "le", 0x28: "un", 0x29: "ne",
         0x2A: "gt", 0x2B: "ge"}


def fp_op(f, a, b, mode):
    """(result, exceptions) of one element: the scalar /SUID instruction."""
    if f == 0x20:
        return T.add(a, b, mode)
    if f == 0x21:
        return T.add(a, b, mode, sub=True)
    if f == 0x22:
        return T.mul(a, b, mode)
    if f == 0x23:
        return T.div(a, b, mode)
    if f == 0x24:
        return T.sqrt(b, mode)
    if f in CMP_T:
        r, e = T.compare(a, b, CMP_T[f])
        return (1 if r else 0), e
    if f == 0x2C:
        return T.cvtqt(b, mode)
    if f == 0x2D:
        return T.cvttq(b, mode)
    if f == 0x2E:
        return T.cvttq(b, T.CHOPPED)
    return T.cpys(a, b, {0x30: "cpys", 0x31: "cpysn", 0x32: "cpyse"}[f]), 0


# --- the image's side ----------------------------------------------------------


class G:
    """The group's emitter: the builder, the tracked state, the row."""

    def __init__(self, b):
        self.b = b
        self.s = State()
        self.ev8n = b.ev8n
        self.seg = image.SEG52 if b.ev8n else image.SEG48
        self.k = 0

    def e(self, *words):
        for w in words:
            self.b.emit(w)

    # VEN
    def set_ven(self, on):
        b = self.b
        if self.ev8n:
            b.pal8_call([lda(1, R31, 1 if on else 0), A.hw_mtpr8(1, I8["vctl"]),
                         A.ifetchb()])
        else:
            b.li(1, self.seg | VCTL_PA)
            b.emit(lda(2, R31, 1 if on else 0))
            b.emit(stq(2, 1, 0))

    def read_ven(self, field, expect):
        b = self.b
        if self.ev8n:
            b.pal8_call([A.hw_mfpr8(3, I8["vctl"])])
        else:
            b.li(1, self.seg | VCTL_PA)
            b.emit(ldq(3, 1, 0))
        b.store(3, field, expect)

    # registers through the window
    def fill(self, n, table, start=0, step=1):
        b = self.b
        b.emit(lda(16, R31, n))
        b.emit(lda(17, R31, TAB[table]))
        b.emit(lda(18, R31, start))
        b.emit(lda(19, R31, step))
        b.asm.branch(OP["BSR"], R_LINK, "t_fill")
        if n != 31:
            t = TABLES[table]
            self.s.vr[n] = [t[(start + i * step) & 63] for i in range(128)]

    def read(self, n, first=0, count=128, name=None):
        b = self.b
        want = self.s.vr[n][first:first + count]
        assert None not in want, (b.case, n, first, count)
        base = 8 * len(b.results)
        for k in range(count):
            b.slot("%s[%d]" % (name or "$vr%d" % n, first + k), want[k])
        d = base - b.res_base
        b.asm._add32(17, R_RES, d)
        b.emit(lda(16, R31, n * 0x400 + 8 * first))
        b.emit(lda(18, R31, count))
        b.asm.branch(OP["BSR"], R_LINK, "t_read")

    def setvl(self, n, rc=R31):
        if n < 256:
            self.e(A.vc("setvl", lit=n, rc=rc))
        else:
            self.b.load(1, n)
            self.e(A.vc("setvl", rb=1, rc=rc))
        self.s.vl = min(n, 128)

    def setvm(self, n):
        """vm from $vrN at vl = 128, then vl is left at 128."""
        self.setvl(128)
        self.e(A.vc("setvm", rb=n))
        self.s.vm = [self.s.vr[n][i] & 1 for i in range(128)]

    def mask_from(self, table="mask", start=0, step=1, reg=30):
        self.fill(reg, table, start, step)
        self.setvm(reg)

    # the FPCR
    def set_fpcr(self, v):
        b = self.b
        b.load(1, v)
        b.emit(itoft(1, 1))
        if self.ev8n:          # the 21464's MT_FPCR trap (its entry 0x700)
            b.trap_clear()
            at = b.va()
            b.emit(mt_fpcr(1))
            b.trap_check(vector=0x700, exc_addr=at)
        else:
            b.emit(mt_fpcr(1))
        self.s.fpcr = T.fpcr_write(v)

    def store_fpcr(self, field="FPCR"):
        b = self.b
        b.emit(mf_fpcr(2))
        b.emit(ftoit(2, 2))
        b.store(2, field, T.fpcr_read(self.s.fpcr))

    def regs(self):
        """Three distinct vector registers out of 1-29 (30 is the mask's)."""
        k = self.k
        self.k += 1
        va = 1 + (k * 5) % 29
        vb = 1 + (k * 11 + 7) % 29
        if vb == va:
            vb = 1 + vb % 29
        vc = 1 + (k * 13 + 3) % 29
        while vc in (va, vb):
            vc = 1 + vc % 29
        return va, vb, vc


def opcdec(b, name, word, need=None):
    """A word that raises OPCDEC, with EXC_ADDR its own PC."""
    b.begin(name)
    if need:
        b.need(need)
    b.trap_clear()
    at = b.va()
    b.emit(word)
    b.trap_check(vector=0x400, exc_addr=at)


# The sheet's example words (4.4 to 4.7), and spare opcode 06.
EXAMPLES = [
    ("vvaddq $vr1, $vr2, $vr3", 0x04220003),
    ("vvaddq $vr1, $vr2, $vr3, $vm", 0x04220803),
    ("vsmulq $vr1, $2, $vr3", 0x08220043),
    ("vsmult $vr1, $f2, $vr3", 0x08220443),
    ("vscmpne $vr0, 0, $vr6", 0x08001226),
    ("vscmpgt $vr1, 2, $vr7", 0x08205287),
    ("vvand $vr6, $vr7, $vr8", 0x04C70088),
    ("vloadq $vr0, ($16)", 0x0C100000),
    ("vstoreq $vr0, ($16), $vm", 0x0C100820),
    ("vgathq $vr3, $vr1($2)", 0x10620001),
    ("vscatq $vr3, $vr1($2)", 0x10620021),
    ("setvm $vr8", 0x17E8005F),
    ("setvs 8", 0x17E1103F),
    ("getvl $1", 0x17FF0081),
    ("opcode 06, all zero", 0x06 << 26),
    ("opcode 06, all ones", (0x06 << 26) | 0x3FFFFFF),
]


def check_words():
    """alpha_asm's encoder against the sheet's words."""
    pairs = [
        (A.vv("vvaddq", 1, 2, 3), 0x04220003),
        (A.vv("vvaddq", 1, 2, 3, m=True), 0x04220803),
        (A.vs("vsmulq", 1, 2, 3), 0x08220043),
        (A.vs("vsmult", 1, 2, 3), 0x08220443),
        (A.vs("vscmpne", 0, 0, 6, lit=0), 0x08001226),
        (A.vs("vscmpgt", 1, 0, 7, lit=2), 0x08205287),
        (A.vv("vvand", 6, 7, 8), 0x04C70088),
        (A.vc("setvm", rb=8), 0x17E8005F),
        (A.vc("setvl", rb=16, rc=1), 0x17F00001),
        (A.vc("setvs", lit=8), 0x17E1103F),
        (A.vc("getvl", rc=1), 0x17FF0081),
    ]
    for got, want in pairs:
        assert got == want, "%08x is not the sheet's %08x" % (got, want)


# --- the group -----------------------------------------------------------------


def g_tarantula(b):
    check_words()
    TAB.clear()
    for name, vals in TABLES.items():
        TAB[name] = b.table(vals)
    # On every row: with VEN = 0 (a Tarantula row outside this group's
    # window) or no Tarantula, opcodes 01-06 raise OPCDEC (section 5; the
    # ARM's reserved opcodes on the other rows).
    for name, word in EXAMPLES:
        opcdec(b, "tarantula: %s raises OPCDEC with VEN = 0 or no Tarantula"
               % name, word)
    b.begin("tarantula: setvl $16, $1 with VEN = 0 writes nothing")
    b.load(1, 0x5A5A5A5A5A5A5A5A)
    b.load(16, 5)
    b.trap_clear()
    at = b.va()
    b.emit(0x17F00001)
    b.trap_check(vector=0x400, exc_addr=at)
    b.store(1, "r1", 0x5A5A5A5A5A5A5A5A)

    g = G(b)
    harness(g)
    g_vc(g)
    g_int(g)
    g_mech(g)
    g_vr31(g)
    g_fields(g)
    g_fp(g)
    ending(g)


def harness(g):
    b = g.b
    b.begin("tarantula: the harness, VCTL")
    b.need(rows.TARANTULA)
    b.li(R_TAB, kseg(image.TAB_PA))
    b.li(R_WIN, g.seg | (PEEK8_PA if g.ev8n else PEEK_PA))
    a = b.asm
    a.branch(OP["BR"], R31, "t_over")
    # t_fill: $vr(r16)[i] <- tab[r17 + ((r18 + i x r19) mod 64)], i < 128.
    a.label("t_fill")
    a.emit(opl(0x12, 0x39, 16, 10, 20))          # SLL r16,10,r20
    a.emit(opr(0x10, 0x20, R_WIN, 20, 20))       # ADDQ: element 0
    a.emit(opr(0x10, 0x32, 17, R_TAB, 17))       # S8ADDQ: the table
    a.emit(lda(21, R31, 128))
    a.label("t_fill_loop")
    a.emit(opl(0x11, 0x00, 18, 63, 22))          # AND r18,63,r22
    a.emit(opr(0x10, 0x32, 22, 17, 22))          # S8ADDQ
    a.emit(ldq(22, 22, 0))
    a.emit(stq(22, 20, 0))
    a.emit(opr(0x10, 0x20, 18, 19, 18))          # ADDQ r18,r19,r18
    a.emit(lda(20, 20, 8))
    a.emit(opl(0x10, 0x29, 21, 1, 21))           # SUBQ r21,1,r21
    a.branch(OP["BGT"], 21, "t_fill_loop")
    a.emit(jmp(2, R31, R_LINK))
    # t_read: r18 quadwords from the window at offset r16 to r17.
    a.label("t_read")
    a.emit(opr(0x10, 0x20, R_WIN, 16, 16))
    a.label("t_read_loop")
    a.emit(ldq(20, 16, 0))
    a.emit(stq(20, 17, 0))
    a.emit(lda(16, 16, 8))
    a.emit(lda(17, 17, 8))
    a.emit(opl(0x10, 0x29, 18, 1, 18))
    a.branch(OP["BGT"], 18, "t_read_loop")
    a.emit(jmp(2, R31, R_LINK))
    a.label("t_over")
    g.read_ven("VCTL before the group", 0)
    g.set_ven(True)
    g.read_ven("VCTL after a store of 1", 1)
    if not g.ev8n:
        # <63:1> RAZ/IGN: a store of all ones reads back 1; VEN clear in a
        # value with other bits set is VEN 0.
        b.begin("tarantula: VCTL<63:1> RAZ/IGN")
        b.need(rows.TARANTULA)
        b.li(1, g.seg | VCTL_PA)
        b.emit(lda(2, R31, -1))
        b.emit(stq(2, 1, 0))
        b.emit(ldq(3, 1, 0))
        b.store(3, "VCTL after a store of all ones", 1)
        b.emit(lda(2, R31, -2))
        b.emit(stq(2, 1, 0))
        b.emit(ldq(3, 1, 0))
        b.store(3, "VCTL after a store of ...FFFE", 0)
        opcdec(b, "tarantula: vvaddq with VEN = 0 after ...FFFE",
               A.vv("vvaddq", 1, 2, 3), rows.TARANTULA)
        b.begin("tarantula: VEN = 1 again")
        b.need(rows.TARANTULA)
        g.set_ven(True)
    else:
        # The native EV8: VCTL is the IPR; the CSR offset is not decoded
        # (N8), so a store of 0 there leaves VEN 1.
        b.begin("tarantula: ev8n: a store to the CSR's address is not VCTL")
        b.need(rows.TARANTULA)
        b.li(1, g.seg | VCTL8_PA)
        b.emit(stq(R31, 1, 0))
        g.read_ven("VCTL (the IPR) after the store", 1)
        g.setvl(5)
        b.trap_clear()
        b.emit(A.vc("getvl", rc=4))
        b.trap_check()
        b.store(4, "getvl (executes: VEN is 1)", 5)
    # With VEN = 0 each group raises OPCDEC and changes nothing; VEN = 1
    # brings everything back as it was.
    b.begin("tarantula: VEN = 0: each group OPCDEC, nothing changed")
    b.need(rows.TARANTULA)
    g.fill(3, "int", 0, 1)
    g.fill(4, "mix", 0, 1)
    g.setvl(7)
    g.set_ven(False)
    for word in (A.vv("vvaddq", 3, 3, 4), A.vs("vsaddq", 3, 0, 4, lit=9),
                 A.vs("vsaddt", 3, 2, 4), A.vc("setvl", lit=99),
                 A.vc("getvl", rc=5), A.vc("setvm", rb=3),
                 A.vv("vvcmptlt", 3, 3, 4)):
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=0x400, exc_addr=at)
    g.set_ven(True)
    b.emit(A.vc("getvl", rc=6))
    b.store(6, "vl, unchanged", 7)
    g.read(4, 0, 8)


# --- VC (4.7) ------------------------------------------------------------------


def g_vc(g):
    b = g.b
    for v in (0, 1, 64, 127, 128, 129, 255, 256, 1 << 63, M64, 0x100000080):
        b.begin("tarantula: setvl r1 = %x" % v)
        b.need(rows.TARANTULA)
        b.load(1, v)
        b.load(2, 0x77)
        b.emit(A.vc("setvl", rb=1, rc=2))
        b.emit(A.vc("getvl", rc=3))
        want = min(v, 128)
        b.store(2, "Rc", want)
        b.store(3, "getvl", want)
    for v in (0, 1, 64, 128, 129, 200, 255):
        b.begin("tarantula: setvl %d (literal)" % v)
        b.need(rows.TARANTULA)
        b.load(2, 0x77)
        b.emit(A.vc("setvl", lit=v, rc=2))
        b.emit(A.vc("getvl", rc=3))
        b.store(2, "Rc", min(v, 128))
        b.store(3, "getvl", min(v, 128))
    b.begin("tarantula: setvl into r31; Ra not 31 is ignored")
    b.need(rows.TARANTULA)
    b.emit(A.vc("setvl", lit=33))
    b.emit(A.vc("getvl", rc=3))
    b.store(3, "getvl", 33)
    b.load(1, 21)
    b.load(5, 0x123)
    b.emit(A.vc("setvl", rb=1, rc=4, ra=5))
    b.emit(A.vc("getvl", rc=3, rb=9, ra=6))
    b.store(4, "Rc (Ra = 5)", 21)
    b.store(3, "getvl (Ra = 6, Rb = 9)", 21)
    b.store(5, "r5", 0x123)
    for v in (0, 8, u(-8), 1 << 63, 0x123456789, M64):
        b.begin("tarantula: setvs r1 = %x" % v)
        b.need(rows.TARANTULA)
        b.load(1, v)
        b.load(4, 0x44)
        b.emit(A.vc("setvs", rb=1, rc=4))     # Rc is not written
        b.emit(A.vc("getvs", rc=3))
        b.store(3, "getvs", v)
        b.store(4, "r4 (setvs's Rc field)", 0x44)
    for v in (8, 255):
        b.begin("tarantula: setvs %d (literal, zero-extended)" % v)
        b.need(rows.TARANTULA)
        b.emit(A.vc("setvs", lit=v))
        b.emit(A.vc("getvs", rc=3))
        b.store(3, "getvs", v)
    # SETVM below vl, zero above; GETVM below vl, the rest unchanged.
    for vl in (0, 1, 64, 100, 128):
        b.begin("tarantula: setvm at vl %d, getvm at vl 128" % vl)
        b.need(rows.TARANTULA)
        g.fill(9, "mask", 3, 5)
        g.fill(10, "fill")
        g.setvl(vl)
        b.emit(A.vc("setvm", rb=9))
        g.s.vm = [(g.s.vr[9][i] & 1) if i < vl else 0 for i in range(128)]
        g.setvl(128)
        b.emit(A.vc("getvm", rc=10))
        g.s.vr[10] = list(g.s.vm)
        g.read(10)
    for vl in (0, 1, 70):
        b.begin("tarantula: getvm at vl %d leaves the rest" % vl)
        b.need(rows.TARANTULA)
        g.fill(9, "mask", 1, 3)
        g.setvm(9)
        g.fill(11, "fill", 2, 1)
        g.setvl(vl)
        b.emit(A.vc("getvm", rc=11))
        for i in range(vl):
            g.s.vr[11][i] = g.s.vm[i]
        g.read(11)
    b.begin("tarantula: setvm $vr31, getvm $vr31")
    b.need(rows.TARANTULA)
    g.fill(12, "fill")
    g.setvl(128)
    b.emit(A.vc("setvm", rb=31))
    b.emit(A.vc("getvm", rc=12))
    g.s.vm = [0] * 128
    g.s.vr[12] = [0] * 128
    g.read(12, 0, 16)
    g.read(12, 112, 16)
    g.mask_from("mask", 0, 1, reg=13)
    b.emit(A.vc("getvm", rc=31))
    g.read(31, 0, 8, name="$vr31 after getvm")


# --- VV and VS, the integer half (4.4) -------------------------------------------

INT_FNC = [f for f in sorted(A.VV_FNC.values()) if f < 0x20]
NAME = {v: k for k, v in A.VV_FNC.items()}
VS_SCALARS = [0x8000000000000001, M64, 7, 0x3F, 0x0123456789ABCDEF, 0x41]
VS_LITS = [0, 0xFF, 1, 0x80, 63, 0x41]
# (vl, mask) by turns over the sweep; every function meets several.
CONFIGS = [(64, False), (64, True), (128, True), (1, False), (128, False),
           (0, True), (1, True), (0, False)]


def read_after(g, vc, vl):
    """Elements 0..vl and one past it (all 128 at vl = 128)."""
    g.read(vc, 0, min(vl + 1, 128))


def int_case(g, form, f, cfg, scalar=None, full=False):
    b = g.b
    vl, m = cfg
    if f == 0x0D:
        m = False                       # MRG takes no mask (4.4)
    va, vb, vc = g.regs()
    vv = form == "vv"
    mn = NAME[f] if vv else "vs" + NAME[f][2:]
    what = ("%s $vr%d, $vr%d, $vr%d" % (mn, va, vb, vc) if vv else
            "%s $vr%d, %s, $vr%d" % (mn, va, scalar[1], vc))
    b.begin("tarantula: %s%s at vl %d" % (what, ", $vm" if m else "", vl))
    b.need(rows.TARANTULA)
    if m or f == 0x0D:
        g.mask_from("mask", g.k % 64, 1 + 2 * (g.k % 7))
    g.fill(vc, "fill", g.k % 64)
    g.fill(va, "int", g.k % 64, 1)
    if vv:
        g.fill(vb, "int", (3 + g.k) % 64, 7)
    elif scalar[0] == "reg":
        b.load(5, scalar[2])
    g.setvl(vl)
    s = g.s
    if vv:
        b.emit(A.vv(mn, va, vb, vc, m=m))
    elif scalar[0] == "reg":
        b.emit(A.vs(mn, va, 5, vc, m=m))
    else:
        b.emit(A.vs(mn, va, 0, vc, m=m, lit=scalar[2]))
    out = list(s.vr[vc])
    for i in range(128):
        if f == 0x0D:
            act = i < s.vl
        else:
            act = s.active(i, m)
        if act:
            bv = s.vr[vb][i] if vv else scalar[2]
            out[i] = int_op(f, s.vr[va][i], bv, s.vm[i] if s.vm else 0)
    s.vr[vc] = out
    if full:
        g.read(vc)
    else:
        read_after(g, vc, vl)


def g_int(g):
    for k, f in enumerate(INT_FNC):
        g.k = 3 * k
        int_case(g, "vv", f, CONFIGS[k % len(CONFIGS)])
        int_case(g, "vv", f, (64, False) if k % 2 else (128, True))
        for j in range(2):
            sv = VS_SCALARS[(k + 3 * j) % len(VS_SCALARS)]
            int_case(g, "vs", f, CONFIGS[(k + j + 1) % len(CONFIGS)],
                     ("reg", "$5", sv))
            lit = VS_LITS[(k + 3 * j) % len(VS_LITS)]
            int_case(g, "vs", f, CONFIGS[(k + j + 4) % len(CONFIGS)],
                     ("lit", "%d" % lit, lit))


def g_mech(g):
    """vl 0, 1, 64, 128, a SETVL above 128, with and without the mask, on a
    few functions; all 128 elements read back each time."""
    for f, form in ((0x00, "vv"), (0x12, "vv"), (0x01, "vs")):
        for vl in (0, 1, 64, 128):
            for m in (False, True):
                sc = None if form == "vv" else ("lit", "200", 200)
                int_case(g, form, f, (vl, m), sc, full=True)
    b = g.b
    for vl in (129, 300, 255):
        b.begin("tarantula: setvl %d, then vvaddq: 128 elements" % vl)
        b.need(rows.TARANTULA)
        g.fill(20, "fill")
        g.fill(21, "int")
        g.fill(22, "mix")
        g.setvl(vl)
        b.emit(A.vv("vvaddq", 21, 22, 20))
        g.s.vr[20] = [u(g.s.vr[21][i] + g.s.vr[22][i]) for i in range(128)]
        g.read(20)
    for vl, m in ((1, False), (64, True), (0, True)):
        b.begin("tarantula: vsmrg at vl %d, every element" % vl)
        b.need(rows.TARANTULA)
        g.mask_from("mask", 5, 3)
        g.fill(24, "fill", 9)
        g.fill(25, "mix")
        b.load(6, 0xABCDEF0123456789)
        g.setvl(vl)
        b.emit(A.vs("vsmrg", 25, 6, 24))
        for i in range(vl):
            g.s.vr[24][i] = 0xABCDEF0123456789 if g.s.vm[i] else g.s.vr[25][i]
        g.read(24)


# --- $vr31 (4.3) ---------------------------------------------------------------


def g_vr31(g):
    b = g.b
    b.begin("tarantula: $vr31 as sources and destination")
    b.need(rows.TARANTULA)
    g.fill(31, "mix")                    # through the window: discarded
    g.read(31, 0, 4, name="$vr31 after a store")
    g.fill(7, "fill")
    g.fill(8, "int")
    g.setvl(16)
    b.emit(A.vv("vvaddq", 31, 8, 7))     # $vr31 + B = B
    g.s.vr[7][:16] = g.s.vr[8][:16]
    g.read(7, 0, 17)
    g.fill(7, "fill")
    b.emit(A.vv("vvsubq", 8, 31, 7))     # A - 0 = A
    g.s.vr[7][:16] = g.s.vr[8][:16]
    g.read(7, 0, 17)
    b.load(3, 0x1234567890ABCDEF)
    g.fill(7, "fill")
    g.setvl(24)
    b.emit(A.vs("vsbis", 31, 3, 7))      # the broadcast of 4.4
    g.s.vr[7][:24] = [0x1234567890ABCDEF] * 24
    g.read(7, 0, 25)
    g.setvl(128)
    b.emit(A.vv("vvaddq", 8, 8, 31))     # discarded
    b.emit(A.vs("vsbis", 31, 3, 31))
    g.read(31, 0, 8, name="$vr31 as a destination")
    g.read(31, 120, 8, name="$vr31 as a destination")
    b.emit(A.vv("vvcmpeq", 31, 31, 9))   # 0 = 0
    g.s.vr[9] = [1] * 128
    g.read(9, 0, 4)
    g.read(9, 124, 4)
    g.fill(10, "fill")
    g.setvl(8)
    b.emit(A.vv("vvaddt", 31, 31, 10))   # 0.0 + 0.0
    g.s.vr[10][:8] = [0] * 8
    g.read(10, 0, 9)


# --- the field rules (4.3, 4.4, 4.7, 4.9) ----------------------------------------


def g_fields(g):
    b = g.b
    T_ = rows.TARANTULA
    bad = [
        ("VV <15:13> = 1", A.vop(0x01, 0x00, 1, 2, 3, sbz=1)),
        ("VV <15:13> = 4", A.vop(0x01, 0x04, 1, 2, 3, sbz=4)),
        ("VV <12> = 1", A.vop(0x01, 0x00, 1, 0, 3, lit=5)),
        ("VS <15:13> = 2", A.vop(0x02, 0x00, 1, 2, 3, sbz=2)),
        ("VS literal with 20 (vsaddt)", A.vop(0x02, 0x20, 1, 0, 3, lit=1)),
        ("VS literal with 30 (vscpys)", A.vop(0x02, 0x30, 1, 0, 3, lit=1)),
        ("VS literal with 25 (vscmpteq)", A.vop(0x02, 0x25, 1, 0, 3, lit=0)),
        ("vvmrg with M", A.vop(0x01, 0x0D, 1, 2, 3, m=True)),
        ("vsmrg with M", A.vop(0x02, 0x0D, 1, 2, 3, m=True)),
        ("vsmrg literal with M", A.vop(0x02, 0x0D, 1, 0, 3, m=True, lit=4)),
        ("setvl with M", A.vop(0x05, 0x00, 31, 1, 2, m=True)),
        ("getvl with <15:13> = 7", A.vop(0x05, 0x04, 31, 31, 2, sbz=7)),
        ("setvl with <15:13> = 1", A.vop(0x05, 0x00, 31, 1, 2, sbz=1)),
        ("setvm with L", A.vop(0x05, 0x02, 31, 0, 31, lit=3)),
        ("getvl with L", A.vop(0x05, 0x04, 31, 0, 2, lit=3)),
        ("getvs with L", A.vop(0x05, 0x05, 31, 0, 2, lit=3)),
        ("getvm with L", A.vop(0x05, 0x06, 31, 0, 2, lit=3)),
        ("vloadq (SM, S8's)", 0x0C100000),
        ("vstoreq (SM, S8's)", 0x0C100020),
        ("vgathq (RM, S8's)", 0x10620001),
        ("vscatq (RM, S8's)", 0x10620021),
        ("opcode 06", 0x18221003),
        ("opcode 06, 3F", 0x18000FE0),
    ]
    for f in (0x0E, 0x0F, 0x1A, 0x1F, 0x2F, 0x33, 0x3F):
        bad.append(("VV %02x" % f, A.vop(0x01, f, 1, 2, 3)))
        bad.append(("VS %02x" % f, A.vop(0x02, f, 1, 2, 3)))
    for f in (0x24, 0x2C, 0x2D, 0x2E):
        bad.append(("VS %02x (VV only)" % f, A.vop(0x02, f, 31, 2, 3)))
    for f in (0x03, 0x07, 0x08, 0x3F):
        bad.append(("VC %02x" % f, A.vop(0x05, f, 31, 1, 2)))
    for name, word in bad:
        opcdec(b, "tarantula: %s raises OPCDEC with VEN = 1" % name, word, T_)
    # Neither Rc of a failed SETVL nor vl changes: one check after them.
    b.begin("tarantula: the field rules changed nothing")
    b.need(T_)
    g.setvl(3)
    b.load(2, 0x2222)
    b.trap_clear()
    at = b.va()
    b.emit(A.vop(0x05, 0x00, 31, 31, 2, m=True, lit=100))
    b.trap_check(vector=0x400, exc_addr=at)
    b.emit(A.vc("getvl", rc=3))
    b.store(2, "Rc", 0x2222)
    b.store(3, "vl", 3)
    # Unused register fields are ignored (4.3): the unary VV with Va not 31.
    b.begin("tarantula: vvsqrtt and vvcvtqt with Va not 31")
    b.need(T_)
    g.set_fpcr(T.NORMAL << 58 | DIS_ALL | T.status_bit(T.INE))
    g.fill(16, "fu")
    g.fill(17, "fill")
    g.fill(18, "fill", 5)
    g.fill(19, "int")
    g.setvl(4)
    b.trap_clear()
    b.emit(A.vv("vvsqrtt", 19, 16, 17))
    b.emit(A.vv("vvcvtqt", 16, 19, 18))
    b.trap_check()
    for i in range(4):
        g.s.vr[17][i] = T.sqrt(g.s.vr[16][i], T.NORMAL)[0]
        g.s.vr[18][i] = T.cvtqt(g.s.vr[19][i], T.NORMAL)[0]
    g.read(17, 0, 5)
    g.read(18, 0, 5)


# --- VV and VS, the floating-point half (4.3, 4.4, section 5) --------------------

FP_FNC = [f for f in sorted(A.VV_FNC.values()) if f >= 0x20]
UNARY = (0x24, 0x2C, 0x2D, 0x2E)
DIS_ALL = (1 << 49) | (1 << 50) | (1 << 51) | (1 << 61) | (1 << 62) | (1 << 60)


def fp_case(g, form, f, vl, m, fpcr, ta="fa", tb=None, scalar=None,
            start=0, label=""):
    """One FP instruction: the elements, the trap, the FPCR after."""
    b = g.b
    va, vb, vc = g.regs()
    vv = form == "vv"
    mn = NAME[f] if vv else "vs" + NAME[f][2:]
    what = ("%s $vr%d, $vr%d, $vr%d" % (mn, va, vb, vc) if vv else
            "%s $vr%d, $f%d, $vr%d" % (mn, va, 4, vc))
    b.begin("tarantula: %s%s at vl %d, FPCR %x%s" % (
        what, ", $vm" if m else "", vl, fpcr, label))
    b.need(rows.TARANTULA)
    if m:
        g.mask_from("mask", g.k % 64, 1 + 2 * (g.k % 5))
    g.fill(vc, "fill", g.k % 64)
    if tb is None:
        tb = ("int" if f == 0x2C else "fu") if f in UNARY else "fb"
    if f in UNARY:
        g.fill(vb, tb, start, 1)
        g.fill(va, "mix")             # Va is not used (31 by the syntax)
    else:
        g.fill(va, ta, start, 1)
        if vv:
            g.fill(vb, tb, start, 1)
        else:
            b.load(1, scalar)
            b.emit(itoft(1, 4))
    g.set_fpcr(fpcr)
    g.setvl(vl)
    s = g.s
    b.trap_clear()
    at = b.va()
    if vv:
        b.emit(A.vv(mn, 31 if f in UNARY else va, vb, vc, m=m))
    else:
        b.emit(A.vs(mn, va, 4, vc, m=m))
    mode = (fpcr >> 58) & 3
    out = list(s.vr[vc])
    trap = sets = 0
    fp = s.fpcr
    for i in range(128):
        if not s.active(i, m):
            continue
        bv = s.vr[vb][i] if vv else scalar
        r, exc = fp_op(f, s.vr[va][i], bv, mode)
        fp, t_, st = T.raise_exc(fp, exc)
        trap |= t_
        sets |= st
        out[i] = r
    s.vr[vc] = out
    s.fpcr = fp
    if trap or sets:
        want = trap | T.SWC | (31 << 8)
        mask = 0x1FFF
        if not g.ev8n:      # EXC_SUM<SET_x> (the native EV8's read zero)
            want |= sets << 41
            mask |= 0x7F << 41
        b.trap_check(vector=0x600, exc_addr=at, exc_sum=want,
                     exc_sum_mask=mask)
    else:
        b.trap_check()
    read_after(g, vc, vl)
    g.store_fpcr()


def g_fp(g):
    b = g.b
    base = T.NORMAL << 58
    # The sweep: every function, the trap disables set (an exception then
    # traps only to set its FPCR status bit), the status clear.
    for k, f in enumerate(FP_FNC):
        g.k = 5 * k
        vl, m = CONFIGS[k % len(CONFIGS)]
        if vl == 0 and k % 3:
            vl = 64
        fp_case(g, "vv", f, vl, m, base | DIS_ALL)
        fp_case(g, "vv", f, 64, False, base | DIS_ALL,
                start=32 if f not in UNARY else 28)
        if f not in UNARY:
            for j, sc in enumerate(FS[:2] if f < 0x30 else FS[1:3]):
                vl, m = CONFIGS[(k + j + 2) % len(CONFIGS)]
                fp_case(g, "vs", f, max(vl, 1), m, base | DIS_ALL, scalar=sc)
    # The rounding modes, from FPCR<DYN>; VVCVTTQC chops whatever DYN is.
    for mode in (T.CHOPPED, T.MINUS, T.PLUS, T.NORMAL):
        for f in (0x20, 0x21, 0x22, 0x23, 0x24, 0x2C, 0x2D, 0x2E):
            g.k += 1
            fp_case(g, "vv", f, 64, False, mode << 58 | DIS_ALL,
                    ta="ra", tb="rb" if f not in UNARY else "ra",
                    label=", rounding %d" % mode)
        fp_case(g, "vs", 0x23, 64, False, mode << 58 | DIS_ALL,
                ta="ra", scalar=T.bits(3.0), label=", rounding %d" % mode)
    # The FPCR across elements: status clear, disables set. The first
    # instruction finds INE, DZE, INV and OVF and traps to set them (SET);
    # the same again finds them set and does not trap.
    for rep in range(2):
        b.begin("tarantula: the FPCR across elements, %s" %
                ("first" if rep == 0 else "again, with the status set"))
        b.need(rows.TARANTULA)
        g.k += 1
        if rep == 0:
            g.set_fpcr(base | DIS_ALL)
        fp_tail(g, 0x23, 64, False)
    # A trapping element: DZE enabled (DZED clear) in element 7 only;
    # every element is written, element 7 with the IEEE default; the
    # trap is after the instruction (EXC_ADDR as for a scalar ARITH).
    TABLES_DZ = "dz"
    for case, (vl, m, mstart, dis) in enumerate((
            (64, False, 0, DIS_ALL & ~(1 << 50)),       # DZE traps
            (64, False, 0, DIS_ALL),                    # disabled: SET only
            (7, False, 0, DIS_ALL & ~(1 << 50)),        # element 7 past vl
            (64, True, 0, DIS_ALL & ~(1 << 50)),        # under the mask
            (64, False, 0, 0),                          # every trap enabled
            (64, False, 0, (DIS_ALL & ~(1 << 50)) | T.status_bit(T.DZE)))):
        # the last: DZE's status already set, the trap without SET
        g.k += 1
        fp_case(g, "vv", 0x23, vl, m, base | dis, ta="one", tb=TABLES_DZ,
                label=", element 7 divides by zero (%d)" % case)
    # FEN: with the FPU disabled the FP half takes FEN, the integer half and
    # VC do not (an inference: the scalar instruction's rule).
    b.begin("tarantula: FEN for the FP functions only")
    b.need(rows.TARANTULA)
    g.fill(2, "fill")
    g.fill(3, "int")
    g.setvl(4)
    b.emit(lda(16, R31, 0))
    b.emit(call_pal(0x01))               # the image's service: FPE <- 0
    for word, vec in ((A.vv("vvaddt", 3, 3, 2), 0x200),
                      (A.vs("vsmult", 3, 1, 2), 0x200),
                      (A.vv("vvcpys", 3, 3, 2), 0x200),
                      (A.vv("vvaddq", 3, 3, 2), None),
                      (A.vc("setvl", lit=4), None)):
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=vec, exc_addr=at)
    b.emit(lda(16, R31, 1))
    b.emit(call_pal(0x01))
    g.s.vr[2][:4] = [u(g.s.vr[3][i] * 2) for i in range(4)]
    g.read(2, 0, 5)


def fp_tail(g, f, vl, m):
    """fp_case without a new FPCR (the FPCR the last one left)."""
    b = g.b
    va, vb, vc = g.regs()
    g.fill(vc, "fill")
    g.fill(va, "fa")
    g.fill(vb, "fb")
    g.setvl(vl)
    s = g.s
    b.trap_clear()
    at = b.va()
    b.emit(A.vv(NAME[f], va, vb, vc, m=m))
    mode = (s.fpcr >> 58) & 3
    trap = sets = 0
    fp = s.fpcr
    for i in range(vl):
        r, exc = fp_op(f, s.vr[va][i], s.vr[vb][i], mode)
        fp, t_, st = T.raise_exc(fp, exc)
        trap |= t_
        sets |= st
        s.vr[vc][i] = r
    s.fpcr = fp
    if trap or sets:
        want = trap | T.SWC | (31 << 8)
        mask = 0x1FFF
        if not g.ev8n:
            want |= sets << 41
            mask |= 0x7F << 41
        b.trap_check(vector=0x600, exc_addr=at, exc_sum=want,
                     exc_sum_mask=mask)
    else:
        b.trap_check()
    g.read(vc, 0, vl + 1)
    g.store_fpcr()


TABLES["one"] = [T.bits(1.0 + i) for i in range(64)]
TABLES["dz"] = [T.bits(0.0) if i == 7 else T.bits(2.0) for i in range(64)]


def ending(g):
    b = g.b
    b.begin("tarantula: VEN = 0 at the end; the FPCR back to 0")
    b.need(rows.TARANTULA)
    g.set_ven(False)
    g.read_ven("VCTL at the end", 0)
    b.trap_clear()
    at = b.va()
    b.emit(A.vv("vvaddq", 1, 2, 3))
    b.trap_check(vector=0x400, exc_addr=at)
    g.set_fpcr(0)


EMIT = {"tarantula": g_tarantula}
