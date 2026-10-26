"""The 21464's paired single precision under opcode 07 (group paired): every
row of docs/platforms/ev8-isa.md 3.4 (the contract with the nada
toolchain) on the rows that have it (`ev8`, `ev8n`, CPU_EXT_EV8), and
opcode 07 raising OPCDEC on every other row. Stage S6 of
docs/platforms/ev8.md.

Each half is computed as the scalar S-format instruction with the same
qualifiers and the same FPCR (D9), its exceptions ORed into the FPCR and
the one trap report; the expected values are sfloat.py's, the scalar
rules written from the ARM and the 21264 HRM, and the instruction table is
the sheet's (alpha_asm.PAIRED: Fnc, qualifier class; the 0x080 | Fnc base,
an inference). Nothing is taken from the emulator.

The cases:
  - a sweep of every instruction under every qualifier its class accepts,
    and for /D every FPCR rounding mode, over operand pairs that do not
    trap (the FPCR's status bits set and its disables set beforehand, so
    that nothing traps to set a status bit either): the results;
  - one-instruction cases with the trap record and the FPCR afterwards:
    each exception in one half, the other, both; NaNs, infinities,
    denormals (the unmaskable trap; DNZ with /S), underflow with and
    without /U and the FPCR's disables, the estimates' DZE and their
    unused halves, PCVTFI's overflow, the compares' NaN rules;
  - FEN with the FPU disabled; OPCDEC for the codes the sheet leaves free
    and the qualifiers a class does not accept, counted in batches.
"""
import alpha_asm as A
from alpha_asm import OP, R31, call_pal, fop, mem
import image
import rows
import sfloat as F
from image import R_TR, TR_COUNT, TR_VEC

M32 = 0xFFFFFFFF
M64 = (1 << 64) - 1

GROUPS = [("paired", "the 21464's opcode 07 paired single precision "
                     "(ev8-isa.md 3.4) where the row has it, OPCDEC "
                     "elsewhere and for the codes and qualifiers the sheet "
                     "leaves free")]


def lda(ra, rb, d):
    return mem(OP["LDA"], ra, rb, d)


def ldq(ra, rb, d):
    return mem(OP["LDQ"], ra, rb, d)


def itoft(ra, fc):
    return fop(0x14, 0x024, ra, R31, fc)


def ftoit(fa, rc):
    return fop(0x1C, 0x070, fa, R31, rc)


def mt_fpcr(fa):
    return fop(0x17, 0x024, fa, fa, fa)


def mf_fpcr(fa):
    return fop(0x17, 0x025, fa, fa, fa)


def pq(hi, lo):
    return ((hi & M32) << 32) | (lo & M32)


def lds(s):
    """LDS's mapping of a single into the register format (MAP_S)."""
    e = (s >> 23) & 0xFF
    re = 0 if e == 0 else (0x7FF if e == 0xFF else e + 896)
    return ((s >> 31) << 63) | (re << 52) | ((s & 0x7FFFFF) << 29)


def sts(r):
    return (((r >> 62) & 3) << 30) | ((r >> 29) & 0x3FFFFFFF)


# --- the sheet's operations (3.4) ---------------------------------------------


EST = {"PARCPL": (F.rcp, True, True), "PARCPLH": (F.rcp, True, False),
       "PARCPLL": (F.rcp, False, True), "PARSQRT": (F.rsqrt, True, True),
       "PARSQRTH": (F.rsqrt, True, False), "PARSQRTL": (F.rsqrt, False, True)}


def halves(name, q, fa, fb):
    """(high Half, low Half) of `name` on Fav, Fbv, each as the scalar
    instruction of its half (sfloat.py)."""
    aH, aL, bH, bL = fa >> 32, fa & M32, fb >> 32, fb & M32
    H = F.Half
    if name in ("PMUL", "PMULL", "PMULH", "PMULLN", "PMULHN"):
        src = {"PMUL": ((aH, bH), (aL, bL)), "PMULL": ((aH, bL), (aL, bL)),
               "PMULH": ((aH, bH), (aL, bH)), "PMULLN": ((aH, bL), (aL, bL)),
               "PMULHN": ((aH, bH), (aL, bH))}[name]
        hi, lo = F.mul(q, *src[0]), F.mul(q, *src[1])
        if name == "PMULLN":
            lo = F.neg(lo)
        if name == "PMULHN":
            hi = F.neg(hi)
        return hi, lo
    adds = {"PADD": ((aH, bH), (aL, bL), False),
            "PCADD": ((aH, bL), (aL, bH), False),
            "PHADD": ((aH, aL), (bH, bL), False),
            "PSUB": ((aH, bH), (aL, bL), True),
            "PSUBC": ((aH, bL), (aL, bH), True),
            "PHSUB": ((aH, aL), (bH, bL), True),
            "PHSUBR": ((aL, aH), (bL, bH), True)}
    if name in adds:
        h, l, sub = adds[name]
        return F.add(q, *h, sub=sub), F.add(q, *l, sub=sub)
    if name in EST:
        # The H and L forms leave the other half 0 and neither check nor
        # raise anything for it (note 1).
        f, use_hi, use_lo = EST[name]
        return (f(q, bH) if use_hi else H(0)), (f(q, bL) if use_lo else H(0))
    cmps = {"PCMPEQ": "eq", "PCMPNEQ": "ne", "PCMPUN": "un", "PCMPLT": "lt",
            "PCMPLE": "le"}
    if name in cmps:
        return F.compare(q, aH, bH, cmps[name]), F.compare(q, aL, bL, cmps[name])
    if name in ("PFMAX", "PFMIN"):
        m = name == "PFMAX"
        return F.maxmin(q, aH, bH, m), F.maxmin(q, aL, bL, m)
    if name == "PCVTFI":
        return F.cvtfi(q, bH), F.cvtfi(q, bL)
    # P-BIT: no checking, no exception.
    if name == "PMOVLL":
        return H(aL), H(bL)
    if name == "PMOVLH":
        return H(aL), H(bH)
    if name == "PMOVHL":
        return H(aH), H(bL)
    if name == "PMOVHH":
        return H(aH), H(bH)
    if name in ("PCPYS", "PCPYSN", "PCPYSE"):
        def cp(x, y):
            if name == "PCPYS":
                return (x & F.SIGN) | (y & 0x7FFFFFFF)
            if name == "PCPYSN":
                return ((x ^ F.SIGN) & F.SIGN) | (y & 0x7FFFFFFF)
            return (x & 0xFF800000) | (y & 0x007FFFFF)
        return H(cp(aH, bH)), H(cp(aL, bL))
    if name == "PCVTSP":
        return H(sts(fa)), H(sts(fb))
    raise KeyError(name)


class Outcome:
    """What one paired instruction does, from the model."""

    def __init__(self, name, trp, rnd, fpcr, fa, fb, regs=(1, 2, 3),
                 ev8n=False):
        q = F.Q(trp, rnd, fpcr)
        if name in ("PEXTL", "PEXTH"):
            # Fc <- the half in register format, as LDS maps it (note 6).
            self.res = lds((fb if name == "PEXTL" else fb >> 32) & M32)
            self.mask = M64
            hi = lo = F.Half(0)
        else:
            hi, lo = halves(name, q, fa, fb)
            self.res = pq(hi.res or 0, lo.res or 0)
            self.mask = pq(0 if hi.res is None else M32,
                           0 if lo.res is None else M32)
        det = hi.det | lo.det
        forced = hi.forced or lo.forced
        known = (0 if hi.forced else hi.det) | (0 if lo.forced else lo.det)
        self.dc = (F.ALL & ~F.INV & ~known) if forced else 0
        self.fpcr = F.fpcr_read(fpcr | F.status_bit(det))
        self.fpcr_mask = M64 & ~F.status_bit(self.dc)
        trap = F.traps(q, det, forced)
        sets = det & ~((fpcr >> 51) & F.ALL)
        self.trapped = bool(trap or sets or forced)
        fa_, fb_, fc_ = regs
        if ev8n:      # the 21464's EXC_SUM, which the image's PALcode records
            # in the 21264's form (image.py): the causes, REG = Fc; no SET_x
            self.exc_sum = trap | (F.SWC if q.S else 0) | (fc_ << 8)
            self.exc_mask = 0x1FFF & ~self.dc
        else:         # the 21264's: the causes, REG = Fc, SET_x
            self.exc_sum = (trap | (F.SWC if q.S else 0) | (fc_ << 8) |
                            (sets << 41))
            self.exc_mask = (0x1FFF | (0x7F << 41)) & ~(self.dc |
                                                        (self.dc << 41))


# --- operands ----------------------------------------------------------------

ONE, TWO, HALF, THREE = 0x3F800000, 0x40000000, 0x3F000000, 0x40400000
PI, THIRD, MTWOTHIRD = 0x40490FDB, 0x3EAAAAAB, 0xBF2AAAAB
BIG, TEN3, TENTH, MTENTH = 0x4B800001, 0x447A0000, 0x3DCCCCCD, 0xBDCCCCCD
MIN, MINP, MMIN, MAX = 0x00800000, 0x00800001, 0x80800000, 0x7F7FFFFF
INF, NINF, ZERO, MZERO = 0x7F800000, 0xFF800000, 0, 0x80000000
QNAN, SNAN, DEN, MDEN = 0x7FC00123, 0x7F800456, 0x00000005, 0x80400000
ONEP, SMALL, HUGE = 0x3F800001, 0x2F000000, 0x5F000000
# Finite operand pairs for the sweep (high half, low half).
PV = [pq(PI, THIRD), pq(MTWOTHIRD, TENTH), pq(TEN3, MTENTH), pq(BIG, ONEP),
      pq(THIRD, PI), pq(MIN, TWO), pq(TENTH, MINP), pq(MTENTH, BIG),
      pq(ONEP, MTWOTHIRD), pq(HUGE, SMALL), pq(0x42F6E979, 0xC1C80000),
      pq(0x3F7FFFFF, 0x3F800003), pq(MMIN, HALF), pq(0x4EFFFFFF, 0xCF000000)]
# PCVTFI's: in and out of a longword's range, halfway cases.
CV = [pq(0x3FC00000, 0x40200000), pq(0xBFC00000, 0xC0200000),
      pq(0x4EFFFFFF, 0xCF000000), pq(0x3F000000, 0xBF000000),
      pq(0x40490FDB, 0xC0490FDB), pq(0x3F7FFFFF, 0x3E800000)]

DIS_ALL = F.INVD | F.DZED | F.OVFD | F.UNFD | F.UNDZ | F.INED
STATUS_ALL = F.status_bit(F.ALL)
SENT = 0x5EA1ED0DDEADBEEF


class G:
    def __init__(self, b):
        self.b = b
        self.ev8n = b.ev8n
        self.k = 0

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

    def fload(self, f, v):
        if f != R31:
            self.b.load(1, v)
            self.b.emit(itoft(1, f))

    def fstore(self, f, field, want, mask=M64):
        self.b.emit(ftoit(f, 2))
        self.b.store(2, field, want, mask=mask)

    def store_fpcr(self, want, mask=M64):
        b = self.b
        b.emit(mf_fpcr(2))
        b.emit(ftoit(2, 2))
        b.store(2, "FPCR", want, mask=mask)

    def regs(self, unary=False):
        """Fa, Fb, Fc out of f4-f29: distinct, now and then Fc = Fa or Fb."""
        k = self.k
        self.k += 1
        fa = 4 + (k * 5) % 26
        fb = 4 + (k * 11 + 7) % 26
        if fb == fa:
            fb = 4 + (fb - 3) % 26
        fc = 4 + (k * 13 + 3) % 26
        while fc in (fa, fb):
            fc = 4 + (fc - 3) % 26
        if k % 7 == 3 and not unary:
            fc = fa
        elif k % 7 == 5:
            fc = fb
        if unary:
            fa = R31
        return fa, fb, fc


def qual_name(trp, rnd, cls):
    t = {0: "", 1: "/V" if cls == "CVT" else "/U",
         5: "/SV" if cls == "CVT" else "/SU",
         7: "/SVI" if cls == "CVT" else "/SUI"}[trp]
    r = {0: "C", 1: "M", 2: "", 3: "D"}[rnd]
    if r and not t:
        return "/" + r
    return t + r


def ex_case(g, name, fa, fb, fpcr, trp=0, rnd=2, label="", regs=None):
    """One instruction with the trap record, Fc and the FPCR after it."""
    b = g.b
    cls = A.PAIRED[name][1]
    ra, rb, rc = regs or (R31 if name in A.PAIRED_UNARY else 1, 2, 3)
    w = A.paired(name, ra, rb, rc, trp, rnd)
    assert rows.ROWS["ev8"].own_instruction(w) == name
    if ra == R31:
        fa = 0
    o = Outcome(name, trp, rnd, fpcr, fa, fb, (ra, rb, rc), g.ev8n)
    b.begin("paired: %s%s f%d,f%d,f%d (a=%x b=%x, FPCR %x)%s" % (
        name.lower(), qual_name(trp, rnd, cls).lower(), ra, rb, rc, fa, fb,
        fpcr, label))
    b.need(rows.EV8)
    g.set_fpcr(fpcr)
    g.fload(ra, fa)
    if rb != ra:
        g.fload(rb, fb)
    if rc not in (ra, rb):
        g.fload(rc, SENT)
    b.trap_clear()
    at = b.va()
    b.emit(w)
    if o.trapped:
        b.trap_check(vector=0x600, exc_addr=at, exc_sum=o.exc_sum,
                     exc_sum_mask=o.exc_mask)
    else:
        b.trap_check()
    if rc == R31:                    # nothing written; F31 reads +0.0
        g.fstore(rc, "Fc", 0)
    else:
        g.fstore(rc, "Fc", o.res, o.mask)
    g.store_fpcr(o.fpcr, o.fpcr_mask)
    return o


def sweep(g, name, trp, rnd, fpcr, pairs, label):
    """Instructions that do not trap: their results only."""
    b = g.b
    unary = name in A.PAIRED_UNARY
    b.begin("paired: %s%s, %s" % (name.lower(),
                                   qual_name(trp, rnd, A.PAIRED[name][1]).lower(),
                                   label))
    b.need(rows.EV8)
    g.set_fpcr(fpcr)
    n = 0
    for fa, fb in pairs:
        ra, rb, rc = g.regs(unary)
        if ra == R31:
            fa = 0
        if rb == ra:
            fb = fa
        o = Outcome(name, trp, rnd, fpcr, fa, fb, (ra, rb, rc), g.ev8n)
        if o.trapped or o.mask != M64:
            continue
        w = A.paired(name, ra, rb, rc, trp, rnd)
        assert rows.ROWS["ev8"].own_instruction(w) == name
        g.fload(ra, fa)
        g.fload(rb, fb)
        b.emit(w)
        g.fstore(rc, "%s f%d,f%d,f%d (a=%x b=%x)" % (name, ra, rb, rc, fa, fb),
                 o.res)
        n += 1
    assert n, (name, trp, rnd, label)


def opcdec_batch(g, label, words, need=None):
    """Words that must each raise OPCDEC, in a row: the trap count is
    theirs. Fb is f2, which holds 1.0, and Fa f1, also 1.0: no paired
    instruction traps on them, so a word that executed would show in the
    count."""
    b = g.b
    for k in range(0, len(words), 32):
        part = words[k:k + 32]
        b.begin("paired: %s (%d)" % (label, k // 32))
        if need is not None:
            b.need(need)
        b.load(1, lds(ONE))
        b.emit(itoft(1, 1))
        b.emit(itoft(1, 2))
        b.trap_clear()
        for w in part:
            b.emit(w)
        b.emit(ldq(0, R_TR, TR_COUNT))
        b.store(0, image.TRAP_COUNT, len(part))
        b.expected_traps += len(part)
        b.emit(ldq(0, R_TR, TR_VEC))
        b.store(0, "trap vector", 0x400)


def check_words():
    """The sheet's example words (3.4), on the 0x080 base."""
    for w, want in ((A.paired("PADD", 1, 2, 3), 0x1C221203),
                    (A.paired("PADD", 1, 2, 3, trp=5), 0x1C22B203),
                    (A.paired("PCMPEQ", 1, 2, 3), 0x1C221503),
                    (A.paired("PMOVHL", 1, 2, 3), 0x1C221343),
                    (A.paired("PARSQRT", R31, 2, 3), 0x1FE21183)):
        assert w == want, "%08x, ev8-isa.md 3.4 gives %08x" % (w, want)


def g_paired(b):
    check_words()
    g = G(b)
    codes = A.paired_codes()
    names = sorted(A.PAIRED, key=lambda n: A.PAIRED[n][0])

    # Opcode 07 raises OPCDEC on every row without the EV8's additions: all
    # of the sheet's codes, in batches.
    words = [fop(0x07, f, 1, 2, 30) for f in sorted(codes)]
    opcdec_batch(g, "every code of the sheet raises OPCDEC where the row "
                 "lacks it", words, rows.NOT_EV8)
    # Everywhere: the Fnc codes the sheet leaves free (at the 0x080 base and
    # with /SUI), and every qualifier a class does not accept.
    assigned = {f for f, _ in A.PAIRED.values()}
    free = [f for f in range(0x40) if f not in assigned]
    words = [fop(0x07, 0x080 | f, 1, 2, 30) for f in free]
    words += [fop(0x07, 0x7C0 | f, 1, 2, 30) for f in free]
    opcdec_batch(g, "the Fnc codes the sheet leaves free raise OPCDEC", words)
    bad = []
    for name in names:
        fnc, cls = A.PAIRED[name]
        trps, rnds = A.PAIRED_QUALS[cls]
        for t in range(8):
            for r in range(4):
                f = (t << 8) | (r << 6) | fnc
                if f not in codes:
                    bad.append(fop(0x07, f, 1, 2, 30))
    opcdec_batch(g, "the qualifiers a class does not accept raise OPCDEC",
                 bad)

    # The sweep: every instruction, every qualifier its class accepts, and
    # /D under each FPCR rounding mode; the FPCR's status and disables set.
    base = STATUS_ALL | DIS_ALL
    for i, name in enumerate(names):
        fnc, cls = A.PAIRED[name]
        trps, rnds = A.PAIRED_QUALS[cls]
        src = CV if name == "PCVTFI" else PV
        configs = [(t, r, F.NORMAL) for t in trps for r in rnds]
        configs += [(trps[-1], 3, m) for m in (F.CHOPPED, F.MINUS, F.PLUS)
                    if 3 in rnds]
        for j, (t, r, mode) in enumerate(configs):
            pairs = [(src[(i + j + 3 * k) % len(src)],
                      src[(2 * i + j + 5 * k + 1) % len(src)])
                     for k in range(5 if cls in ("ARITH", "CVT") else 3)]
            sweep(g, name, t, r, base | (mode << 58), pairs,
                  "rounding %d" % mode if r == 3 else "results")

    # The exceptions, per half: INV, DZE, OVF, UNF, INE, both halves and
    # each alone; the FPCR status clear (each exception then also asks
    # PALcode to set its bit, EXC_SUM<SET_x>) or set, the disables clear
    # or set.
    N = F.NORMAL << 58
    for trp in (0, 1, 5, 7):
        for fpcr in (N, N | DIS_ALL, N | STATUS_ALL | DIS_ALL):
            ex_case(g, "PADD", pq(INF, ONE), pq(NINF, ONE), fpcr, trp,
                    label=", INV high")
            ex_case(g, "PADD", pq(ONE, INF), pq(TWO, NINF), fpcr, trp,
                    label=", INV low")
            ex_case(g, "PSUB", pq(INF, INF), pq(INF, INF), fpcr, trp,
                    label=", INV both")
            ex_case(g, "PMUL", pq(MAX, ONE), pq(TWO, THIRD), fpcr, trp,
                    label=", OVF high, INE low")
            ex_case(g, "PMUL", pq(THREE, MIN), pq(TWO, TENTH), fpcr, trp,
                    label=", UNF low")
            ex_case(g, "PMUL", pq(MIN, MAX), pq(TENTH, TWO), fpcr, trp,
                    label=", UNF high, OVF low")
            ex_case(g, "PADD", pq(THIRD, ONE), pq(ONE, ONE), fpcr, trp,
                    label=", INE high")
    # UNFD without UNDZ still traps with /U (the 21264's rule).
    for fpcr in (N | F.UNFD, N | F.UNFD | F.UNDZ, N | F.UNDZ):
        for trp in (1, 5, 7):
            ex_case(g, "PMUL", pq(MIN, ONE), pq(HALF, ONE), fpcr, trp,
                    label=", UNF high, disables %x" % (fpcr >> 60))
    # The rounding modes on an inexact pair, the FPCR's DYN and Rnd.
    for rnd in (0, 1, 2, 3):
        for mode in (F.CHOPPED, F.MINUS, F.NORMAL, F.PLUS):
            if rnd != 3 and mode != F.NORMAL:
                continue
            for name in ("PADD", "PMULLN", "PHSUBR"):
                ex_case(g, name, pq(THIRD, MTWOTHIRD), pq(PI, MTENTH),
                        mode << 58, 5, rnd, label=", rounding")
    # NaNs and infinities per half: B's NaN first, quieted; an SNaN is an
    # invalid operation; the negated product of a NaN keeps its sign flip.
    for name, fa, fb in (("PADD", pq(QNAN, ONE), pq(ONE, SNAN)),
                         ("PADD", pq(SNAN, QNAN), pq(QNAN | F.SIGN, SNAN)),
                         ("PSUB", pq(INF, ONE), pq(ONE, INF)),
                         ("PMUL", pq(INF, ZERO), pq(MZERO, NINF)),
                         ("PMULLN", pq(TWO, QNAN), pq(ONE, THREE)),
                         ("PMULHN", pq(ZERO, ONE), pq(THREE, TWO)),
                         ("PMULLN", pq(TWO, MIN), pq(MZERO, TENTH)),
                         ("PHADD", pq(INF, NINF), pq(ONE, MZERO)),
                         ("PCADD", pq(MZERO, ZERO), pq(ZERO, MZERO)),
                         ("PSUBC", pq(ONE, TWO), pq(TWO, ONE))):
        for trp, fpcr in ((0, N), (5, N | DIS_ALL)):
            ex_case(g, name, fa, fb, fpcr, trp, label=", NaN and infinity")
        ex_case(g, name, fa, fb, F.MINUS << 58, 0, 3,
                label=", rounding minus")
    # Denormal operands: the unmaskable trap (the half's result
    # UNPREDICTABLE, the other half's correct), or a signed zero with DNZ
    # under /S.
    for name, fa, fb in (("PADD", pq(DEN, ONE), pq(ONE, TWO)),
                         ("PMUL", pq(ONE, THREE), pq(TWO, MDEN)),
                         ("PCMPEQ", pq(DEN, ONE), pq(ZERO, ONE)),
                         ("PCVTFI", 0, pq(ONE, MDEN)),
                         ("PARCPL", 0, pq(DEN, TWO))):
        for trp, fpcr in ((0, N), (5, N | F.DNZ), (5, N)):
            if A.PAIRED[name][1] == "CMP" and trp == 0 and fpcr & F.DNZ:
                continue
            ex_case(g, name, fa, fb, fpcr, trp, label=", denormal")
    # The compares: 1.0 or a true zero per half; NaN rules (EQ, NE, UN
    # quiet; LT and LE any NaN); +0 = -0; PFMAX and PFMIN.
    cmp_pairs = [(pq(ONE, TWO), pq(ONE, ONE)), (pq(MZERO, NINF), pq(ZERO, INF)),
                 (pq(QNAN, ONE), pq(ONE, QNAN)), (pq(SNAN, THREE), pq(ONE, TWO)),
                 (pq(TWO, MTENTH), pq(ONE, TENTH))]
    for name in ("PCMPEQ", "PCMPNEQ", "PCMPUN", "PCMPLT", "PCMPLE", "PFMAX",
                 "PFMIN"):
        for fa, fb in cmp_pairs:
            for trp, fpcr in ((0, N), (5, N | F.INVD),
                              (5, N | F.INVD | STATUS_ALL)):
                if fpcr & STATUS_ALL and name not in ("PCMPLT", "PFMAX"):
                    continue
                ex_case(g, name, fa, fb, fpcr, trp, label=", compare")
    # The estimates: chopped to 14 fraction bits whatever Rnd is; DZE for a
    # zero; the H and L forms' other half 0 and unchecked; UNF for the
    # reciprocal of a large value; a negative root an invalid operation.
    est_pairs = [pq(THREE, PI), pq(ZERO, TWO), pq(MZERO, ZERO),
                 pq(MAX, HUGE), pq(NINF, INF), pq(SNAN, 0xC0800000),
                 pq(QNAN, 0x3E800000), pq(0x4C000001, MIN)]
    for name in ("PARCPL", "PARCPLH", "PARCPLL", "PARSQRT", "PARSQRTH",
                 "PARSQRTL"):
        configs = ((0, 2, N), (1, 0, N), (5, 3, N | DIS_ALL),
                   (1, 1, N | STATUS_ALL))
        if name not in ("PARCPL", "PARSQRT"):
            configs = configs[1:3]
        for fb in est_pairs:
            for trp, rnd, fpcr in configs:
                ex_case(g, name, 0, fb, fpcr, trp, rnd, label=", estimate")
    # PCVTFI: in range, inexact, out of a longword's range, NaN, under the
    # four rounding modes.
    for fb in (pq(0x4F000000, 0xCF000000), pq(0x4F000000, 0x3FC00000),
               pq(QNAN, NINF), pq(0x40200000, 0xC0200000),
               pq(0x5F800000, 0x4EFFFFFF), pq(0xCF000001, ZERO)):
        for trp in (0, 1, 5, 7):
            for rnd in (0, 3):
                ex_case(g, "PCVTFI", 0, fb, (F.PLUS << 58) | F.INED, trp,
                        rnd, label=", convert")
    # P-BIT: no checking and no exception, whatever the halves hold, with
    # every exception enabled and the status clear.
    bit_pairs = [(pq(SNAN, DEN), pq(MDEN, QNAN)), (pq(NINF, MZERO),
                                                   pq(PI, MAX)),
                 (pq(0xFFFFFFFF, 0x00000001), pq(0x80000000, 0x7FFFFFFF))]
    for name in ("PMOVLL", "PMOVLH", "PMOVHL", "PMOVHH", "PCPYS", "PCPYSN",
                 "PCPYSE", "PEXTL", "PEXTH"):
        for fa, fb in bit_pairs:
            ex_case(g, name, fa, fb, N, label=", no checking")
    # PCVTSP: two register-format singles packed as STS packs them.
    for fa, fb in ((lds(PI), lds(MTENTH)), (lds(QNAN), lds(DEN)),
                   (0x3FF0000000000000, 0xC00921FB54442D18),
                   (0x7FF4000000000001, 0x0000000000000001)):
        ex_case(g, "PCVTSP", fa, fb, N, label=", no checking")
    # Fb = $f31 or Fa = $f31 (note 2): a PMOV clears the half; Fc = Fa, Fb.
    for regs in ((1, 31, 3), (31, 2, 3), (3, 2, 3), (1, 3, 3), (2, 2, 3)):
        fa, fb = pq(PI, THIRD), pq(TEN3, MTENTH)
        if regs[0] == 31:
            fa = 0
        if regs[1] == 31:
            fb = 0
        if regs[0] == regs[1]:
            fb = fa
        for name in ("PMOVHL", "PADD"):
            ex_case(g, name, fa, fb, N, regs=regs, label=", registers")
    # Fc = $f31: nothing is written, the exceptions still happen.
    ex_case(g, "PADD", pq(INF, ONE), pq(NINF, ONE), N, regs=(1, 2, 31),
            label=", Fc = f31")

    # FEN with the FPU disabled, for one instruction of each class; Fc
    # unchanged.
    b.begin("paired: FEN with the FPU disabled")
    b.need(rows.EV8)
    g.set_fpcr(0)
    g.fload(3, SENT)
    b.emit(lda(16, R31, 0))
    b.emit(call_pal(0x01))                     # the image's service: FPE <- 0
    for name in ("PADD", "PARCPL", "PCMPLT", "PCVTFI", "PMOVHL", "PEXTL"):
        b.trap_clear()
        at = b.va()
        b.emit(A.paired(name, 31 if name in A.PAIRED_UNARY else 1, 2, 3))
        b.trap_check(vector=0x200, exc_addr=at)
    b.emit(lda(16, R31, 1))
    b.emit(call_pal(0x01))
    g.fstore(3, "f3 unchanged", SENT)

    b.begin("paired: the FPCR back to 0")
    b.need(rows.EV8)
    g.set_fpcr(0)


EMIT = {"paired": g_paired}
