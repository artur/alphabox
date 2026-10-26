"""IEEE S_floating (single) arithmetic as the Alpha architecture defines it
for a scalar instruction with any of its trap and rounding qualifiers, for
the paired-single cases (paired_cases.py): docs/platforms/ev8-isa.md 3.4
computes each half of a paired instruction "as the scalar S-format
instruction with the same qualifiers and the same FPCR" (D9).

Written from the ARM (4th edition, 4.7: the formats, rounding, the
exceptions and their qualifiers, 4.7.5 underflow decided at the denormal
position, 4.7.7.7 a true zero for an underflow, 4.7.7.11 DNZ for /S only)
and the 21264 HRM (A.8: a denormal operand takes an unmaskable invalid
operation trap; Table A-11: a NaN operand is returned quieted, B's first,
an invalid operation on other operands gives the canonical quiet NaN,
CVTfi's overflow with /S is also an invalid operation; Table 2-14: with
/U, UNFD set and UNDZ clear the underflow still traps). Exact arithmetic
with fractions.Fraction, then one rounding: nothing here is the emulator's
code.

Values are S memory-format words (32 bits): a half of a paired register
holds exactly that (ev8-isa.md 3.4, "Register format").

A half's outcome is a Half: the 32-bit result (None when it is
UNPREDICTABLE: after a denormal operand's trap), the exceptions detected
(the FPCR status bits it sets), and whether a denormal operand forced the
trap. Which exceptions trap is decided for the whole instruction from the
qualifiers and the FPCR (traps()).
"""
import math
from fractions import Fraction

INV, DZE, OVF, UNF, INE, IOV = 0x02, 0x04, 0x08, 0x10, 0x20, 0x40
ALL = INV | DZE | OVF | UNF | INE | IOV
SWC = 0x01
CHOPPED, MINUS, NORMAL, PLUS = 0, 1, 2, 3
SIGN = 0x80000000
QBIT = 0x00400000
CQNAN = 0xFFC00000           # the canonical quiet NaN, as STS stores T's
MAXS = 0x7F7FFFFF
INF = 0x7F800000
ONE = 0x3F800000
MIN = Fraction(1, 2 ** 126)
M32 = 0xFFFFFFFF

# FPCR fields (ARM 4.7.8).
DNZ = 1 << 48
INVD, DZED, OVFD = 1 << 49, 1 << 50, 1 << 51
UNDZ, UNFD, INED = 1 << 60, 1 << 61, 1 << 62
DIS = {INV: INVD, DZE: DZED, OVF: OVFD, UNF: UNFD, INE: INED, IOV: 0}


def status_bit(e):
    return e << 51


class Q:
    """An instruction's qualifiers and FPCR: trp <15:13>, rnd <12:11>."""

    def __init__(self, trp, rnd, fpcr):
        self.trp, self.fpcr = trp, fpcr
        self.S = bool(trp & 4)
        self.U = bool(trp & 1)            # /U, or /V for an integer result
        self.I = trp == 7                 # /SUI, /SVI
        self.rnd = (fpcr >> 58) & 3 if rnd == 3 else rnd
        self.dnz = self.S and bool(fpcr & DNZ)


class Half:
    def __init__(self, res, det=0, forced=False):
        self.res, self.det, self.forced = res, det, forced


def kind(s):
    e, f = (s >> 23) & 0xFF, s & 0x7FFFFF
    if e == 0xFF:
        if f == 0:
            return "inf"
        return "qnan" if f & QBIT else "snan"
    if e == 0:
        return "zero" if f == 0 else "denorm"
    return "fin"


def is_nan(s):
    return kind(s) in ("qnan", "snan")


def value(s):
    """The exact value of a finite S word (a denormal included)."""
    e, f = (s >> 23) & 0xFF, s & 0x7FFFFF
    x = Fraction(f, 1 << 23) * Fraction(2) ** -126 if e == 0 else \
        Fraction((1 << 23) | f, 1 << 23) * Fraction(2) ** (e - 127)
    return -x if s & SIGN else x


def quiet(s):
    return s | QBIT


def _round_int(m, sign, mode):
    """m (a non-negative Fraction) rounded to an integer by mode, for a
    value of the given sign."""
    q = m.numerator // m.denominator
    r = m - q
    if r:
        if mode == NORMAL:
            q += 1 if (r > Fraction(1, 2) or (r == Fraction(1, 2) and q & 1)) else 0
        elif mode == PLUS and not sign:
            q += 1
        elif mode == MINUS and sign:
            q += 1
    return q, bool(r)


def _log2_floor(a):
    e = a.numerator.bit_length() - a.denominator.bit_length()
    while Fraction(2) ** e > a:
        e -= 1
    while Fraction(2) ** (e + 1) <= a:
        e += 1
    return e


def round_s(x, mode):
    """A non-zero exact value rounded to S by mode: (word, exceptions).
    Below MIN it is rounded where a denormal has its last bit, and an
    underflow is a result still below MIN after that: a true zero, UNF and
    INE (ARM 4.7.5, 4.7.7.7); one that rounds up to MIN is MIN, inexact."""
    sign = 1 if x < 0 else 0
    a = -x if sign else x
    if a < MIN:
        q, _ = _round_int(a * 2 ** 149, sign, mode)
        if q < (1 << 23):
            return 0, UNF | INE
        return (SIGN if sign else 0) | (1 << 23), INE
    e = _log2_floor(a)
    q, inexact = _round_int(a / Fraction(2) ** (e - 23), sign, mode)
    if q == 1 << 24:
        q >>= 1
        e += 1
    flags = INE if inexact else 0
    if e > 127:
        to_inf = (mode == NORMAL or (mode == PLUS and not sign) or
                  (mode == MINUS and sign))
        return (SIGN if sign else 0) | (INF if to_inf else MAXS), OVF | INE
    return (SIGN if sign else 0) | ((e + 127) << 23) | (q - (1 << 23)), flags


def chop15(x):
    """A positive or negative non-zero value chopped to 15 significant bits
    (an estimate's: fraction <8:0> of the single zero), and whether it
    underflows (below MIN once chopped; chopping never reaches MIN from
    below)."""
    sign = 1 if x < 0 else 0
    a = -x if sign else x
    if a < MIN:
        return 0, True
    e = _log2_floor(a)
    q = (a / Fraction(2) ** (e - 14)).numerator // (a / Fraction(2) ** (e - 14)).denominator
    assert (1 << 14) <= q < (1 << 15) and e <= 127
    return (SIGN if sign else 0) | ((e + 127) << 23) | ((q - (1 << 14)) << 9), False


def _operands(q, *ops):
    """Check the operands: an SNaN is an invalid operation; a denormal is a
    signed zero with DNZ under /S, else it forces an invalid operation trap
    and the result is UNPREDICTABLE (HRM A.8). Returns (operands as used,
    detected, forced)."""
    out, det, forced = [], 0, False
    for s in ops:
        k = kind(s)
        if k == "denorm":
            if q.dnz:
                s &= SIGN
            else:
                det |= INV
                forced = True
        elif k == "snan":
            det |= INV
        out.append(s)
    return out, det, forced


def _nan_of(a, b):
    """The scalar rule for NaN operands: B's quieted first, then A's."""
    if b is not None and is_nan(b):
        return quiet(b)
    if a is not None and is_nan(a):
        return quiet(a)
    return None


def _zero(sign):
    return SIGN if sign else 0


def add(q, a, b, sub=False):
    (a, b), det, forced = _operands(q, a, b)
    if forced:
        return Half(None, det, True)
    n = _nan_of(a, b)
    if n is not None:
        return Half(n, det)
    if sub:
        b ^= SIGN
    ka, kb = kind(a), kind(b)
    if kb == "inf":
        if ka == "inf" and (a ^ b) & SIGN:
            return Half(CQNAN, det | INV)
        return Half(b, det)
    if ka == "inf":
        return Half(a, det)
    if ka == "zero" and kb == "zero":
        if (a ^ b) & SIGN:
            return Half(_zero(q.rnd == MINUS), det)
        return Half(a, det)
    if ka == "zero":
        return Half(b, det)
    if kb == "zero":
        return Half(a, det)
    x = value(a) + value(b)
    if x == 0:
        return Half(_zero(q.rnd == MINUS), det)
    r, f = round_s(x, q.rnd)
    return Half(r, det | f)


def mul(q, a, b):
    (a, b), det, forced = _operands(q, a, b)
    if forced:
        return Half(None, det, True)
    n = _nan_of(a, b)
    if n is not None:
        return Half(n, det)
    ka, kb = kind(a), kind(b)
    s = (a ^ b) & SIGN
    if "inf" in (ka, kb):
        if "zero" in (ka, kb):
            return Half(CQNAN, det | INV)
        return Half(s | INF, det)
    if "zero" in (ka, kb):
        return Half(s, det)
    r, f = round_s(value(a) * value(b), q.rnd)
    return Half(r, det | f)


def neg(h):
    """Negated after rounding (ev8-isa.md 3.4, PMULLN and PMULHN)."""
    if h.res is not None:
        h.res ^= SIGN
    return h


def rcp(q, b):
    """PARCPLx's half: the exact reciprocal chopped to 15 significant bits;
    the exceptions DIVS of 1.0 by the operand raises, but INE."""
    (b,), det, forced = _operands(q, b)
    if forced:
        return Half(None, det, True)
    k = kind(b)
    if k in ("qnan", "snan"):
        return Half(quiet(b), det)
    if k == "zero":
        return Half((b & SIGN) | INF, det | DZE)
    if k == "inf":
        return Half(b & SIGN, det)
    r, unf = chop15(1 / value(b))
    return Half(r, det | (UNF if unf else 0))


def rsqrt(q, b):
    """PARSQRTx's half: the exact reciprocal square root chopped to 15
    significant bits; the exceptions SQRTS raises, but INE; a zero's
    estimate is the infinity of its sign and raises division by zero, as
    IEEE 754's rSqrt and PARCPL of a zero do [decision]; a negative operand
    is an invalid operation."""
    (b,), det, forced = _operands(q, b)
    if forced:
        return Half(None, det, True)
    k = kind(b)
    if k in ("qnan", "snan"):
        return Half(quiet(b), det)
    if k == "zero":
        return Half((b & SIGN) | INF, det | DZE)
    if b & SIGN:
        return Half(CQNAN, det | INV)
    if k == "inf":
        return Half(0, det)
    v = value(b)
    # floor(2^G / sqrt(v)) by integers: 1/sqrt(v) = sqrt(1/v)
    num, den = v.denominator, v.numerator     # 1/v
    sh = 200
    r = math.isqrt((num << (2 * sh)) // den)  # floor(2^sh / sqrt(v))
    x = Fraction(r, 1 << sh)
    # r is exact to 2^-sh, far below the 15th significant bit (1/sqrt(v) >=
    # 2^-64): chopping x and the true value agree unless the true value
    # sits on a 15-bit boundary, where x = the boundary exactly anyway.
    out, unf = chop15(x)
    assert not unf
    return Half(out, det)


def compare(q, a, b, op):
    """The compares as CMPTxx: EQ, NE and UN raise INV for an SNaN only, LT
    and LE for any NaN (HRM Table A-11; ev8-isa.md 3.4: NEQ and UN as EQ).
    The half is 1.0 or a true zero."""
    (a, b), det, forced = _operands(q, a, b)
    if forced:
        return Half(None, det, True)
    nan = is_nan(a) or is_nan(b)
    if nan and op in ("lt", "le"):
        det |= INV
    if nan:
        t = op in ("ne", "un")
    else:
        x = value(a) if kind(a) in ("fin", "denorm") else (
            Fraction(0) if kind(a) == "zero" else
            Fraction(-1 if a & SIGN else 1) * Fraction(2) ** 500)
        y = value(b) if kind(b) in ("fin", "denorm") else (
            Fraction(0) if kind(b) == "zero" else
            Fraction(-1 if b & SIGN else 1) * Fraction(2) ** 500)
        t = {"eq": x == y, "ne": x != y, "lt": x < y, "le": x <= y,
             "un": False}[op]
    return Half(ONE if t else 0, det)


def maxmin(q, a, b, is_max):
    """PFMAX, PFMIN [decision, ev8-isa.md 3.4]: (faX < fbX) ? fbX : faX and
    the reverse, comparing as PCMPLT; an unordered pair is an invalid
    operation and the half gets what the scalar rules give one, the NaN
    operand quieted, Fb's first [inference]."""
    ra, rb = a, b
    (a, b), det, forced = _operands(q, a, b)
    if forced:
        return Half(None, det, True)
    if is_nan(a) or is_nan(b):
        return Half(_nan_of(a, b), det | INV)
    lt = compare(q, a, b, "lt").res == ONE
    if is_max:
        return Half(rb if lt else ra, det)
    return Half(ra if lt else rb, det)


def cvtfi(q, b):
    """PCVTFI's half: the integer rounded by Rnd, as a 32-bit two's
    complement value; out of range its low 32 bits, IOV, and INV with /S
    (as CVTTQ at 64 bits, HRM Table A-11); a NaN or infinity 0 and INV;
    INE when inexact."""
    (b,), det, forced = _operands(q, b)
    if forced:
        return Half(None, det, True)
    k = kind(b)
    if k in ("qnan", "snan", "inf"):
        return Half(0, det | INV)
    if k == "zero":
        return Half(0, det)
    x = value(b)
    sign = x < 0
    n, inexact = _round_int(-x if sign else x, sign, q.rnd)
    if inexact:
        det |= INE
    i = -n if sign else n
    if not (-(1 << 31) <= i < (1 << 31)):
        det |= IOV | (INV if q.S else 0)
    return Half(i & M32, det)


def traps(q, det, forced):
    """The exceptions of an instruction (both halves ORed) that trap:
    INV, DZE and OVF unless /S and their disable; UNF with /U, unless /S
    with UNFD and UNDZ; INE with /SUI unless INED; IOV with /V; a denormal
    operand's INV always."""
    t = 0
    for e in (INV, DZE, OVF):
        if det & e and not (q.S and q.fpcr & DIS[e]):
            t |= e
    if det & UNF and q.U and not (q.S and q.fpcr & UNFD and q.fpcr & UNDZ):
        t |= UNF
    if det & INE and q.I and not (q.fpcr & INED):
        t |= INE
    if det & IOV and q.U:
        t |= IOV
    if forced:
        t |= INV
    return t


def fpcr_read(v):
    """MF_FPCR: <47:0> read as zero, the summary <63> the OR of <57:52>."""
    v &= ~((1 << 48) - 1) & ((1 << 64) - 1)
    return (v & ~(1 << 63)) | ((1 << 63) if v & (0x3F << 52) else 0)
