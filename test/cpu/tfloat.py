"""IEEE T_floating (double) arithmetic as the Alpha architecture defines it
for an instruction with /SUI and a given rounding mode, for the Tarantula
cases (tarantula_cases.py, tarantula_vv.py): each vector FP element is
"computed as the scalar T instruction with /SUI and dynamic rounding"
(docs/platforms/ev8-isa.md 4.3, D13).

Written from the ARM (4th edition, 4.7: the formats, rounding modes and
exceptions; 4.10.1, 4.10.3: CVTQT and CVTTQ) and the 21264 HRM's Table A-11
(the results and exceptions of an IEEE instruction with /S: an invalid
operation without NaN operands gives the canonical quiet NaN
0xFFF8000000000000, a NaN operand is returned quieted, an exponent overflow
gives the infinity or the largest finite number the rounding mode calls for,
and so on). Exact arithmetic with fractions.Fraction, then one rounding:
nothing here is the emulator's code.

Not modelled, because the cases avoid them: denormal operands and results
(an underflow), and two NaN operands at once. A result that would underflow
raises ValueError, so that a case built on one is noticed.

Exception bits are those of EXC_SUM and the FPCR's status field: INV 0x02,
DZE 0x04, OVF 0x08, UNF 0x10, INE 0x20, IOV 0x40.
"""
import math
from fractions import Fraction

INV, DZE, OVF, UNF, INE, IOV = 0x02, 0x04, 0x08, 0x10, 0x20, 0x40
SWC = 0x01
# Rounding modes as FPCR<DYN> <59:58> encodes them (ARM 4.7.8).
CHOPPED, MINUS, NORMAL, PLUS = 0, 1, 2, 3
CQNAN = 0xFFF8000000000000
SIGN = 1 << 63
MAXF = 0x7FEFFFFFFFFFFFFF
M64 = (1 << 64) - 1


def bits(x):
    """A Python float's T_floating image."""
    import struct
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def kind(v):
    e = (v >> 52) & 0x7FF
    f = v & ((1 << 52) - 1)
    if e == 0x7FF:
        if f == 0:
            return "inf"
        return "qnan" if f >> 51 else "snan"
    if e == 0:
        return "zero" if f == 0 else "denorm"
    return "fin"


def value(v):
    """The exact value of a finite, non-zero, normal T_floating."""
    e = (v >> 52) & 0x7FF
    f = (v & ((1 << 52) - 1)) | (1 << 52)
    x = Fraction(f) * Fraction(2) ** (e - 1075)
    return -x if v >> 63 else x


def neg(v):
    return v ^ SIGN


def quiet(v):
    return v | (1 << 51)


def round_t(x, mode):
    """The T_floating nearest x (a non-zero Fraction) by `mode`, and the
    exceptions: INE when not exact, OVF and INE on an exponent overflow."""
    sign = 1 if x < 0 else 0
    a = -x if sign else x
    e = math.floor(math.log2(a.numerator) - math.log2(a.denominator))
    # make 2^e <= a < 2^(e+1) exact
    while Fraction(2) ** e > a:
        e -= 1
    while Fraction(2) ** (e + 1) <= a:
        e += 1
    if e < -1022:
        raise ValueError("an underflow: not modelled")
    m = a / Fraction(2) ** (e - 52)      # in [2^52, 2^53)
    q = m.numerator // m.denominator
    r = m - q
    up = False
    if r:
        if mode == NORMAL:
            up = r > Fraction(1, 2) or (r == Fraction(1, 2) and q & 1)
        elif mode == PLUS:
            up = not sign
        elif mode == MINUS:
            up = bool(sign)
    if up:
        q += 1
        if q == 1 << 53:
            q >>= 1
            e += 1
    flags = INE if r else 0
    if e > 1023:
        flags |= OVF | INE
        to_inf = (mode == NORMAL or (mode == PLUS and not sign) or
                  (mode == MINUS and sign))
        return ((SIGN if sign else 0) | (0x7FF << 52 if to_inf else MAXF),
                flags)
    return ((SIGN if sign else 0) | ((e + 1023) << 52) |
            (q & ((1 << 52) - 1))), flags


def _zero(sign):
    return SIGN if sign else 0


def _nan_in(a, b):
    """The result for NaN operands, or None: the NaN quieted (SNaN: INV)."""
    for v in (a, b):
        if v is not None and kind(v) in ("qnan", "snan"):
            return quiet(v), (INV if kind(v) == "snan" else 0)
    return None


def add(a, b, mode, sub=False):
    if sub:
        b = neg(b) if kind(b) not in ("qnan", "snan") else b
    n = _nan_in(a, b)
    if n:
        return n
    ka, kb = kind(a), kind(b)
    assert "denorm" not in (ka, kb)
    if ka == "inf" and kb == "inf":
        if (a ^ b) & SIGN:
            return CQNAN, INV
        return a, 0
    if ka == "inf":
        return a, 0
    if kb == "inf":
        return b, 0
    if ka == "zero" and kb == "zero":
        if (a ^ b) & SIGN:
            return _zero(mode == MINUS), 0
        return a, 0
    if ka == "zero":
        return b, 0
    if kb == "zero":
        return a, 0
    x = value(a) + value(b)
    if x == 0:
        return _zero(mode == MINUS), 0
    return round_t(x, mode)


def mul(a, b, mode):
    n = _nan_in(a, b)
    if n:
        return n
    ka, kb = kind(a), kind(b)
    s = (a ^ b) & SIGN
    if "inf" in (ka, kb):
        if "zero" in (ka, kb):
            return CQNAN, INV
        return s | (0x7FF << 52), 0
    if "zero" in (ka, kb):
        return s, 0
    return round_t(value(a) * value(b), mode)


def div(a, b, mode):
    n = _nan_in(a, b)
    if n:
        return n
    ka, kb = kind(a), kind(b)
    s = (a ^ b) & SIGN
    if ka == "inf":
        if kb == "inf":
            return CQNAN, INV
        return s | (0x7FF << 52), 0
    if kb == "inf":
        return s, 0
    if kb == "zero":
        if ka == "zero":
            return CQNAN, INV
        return s | (0x7FF << 52), DZE
    if ka == "zero":
        return s, 0
    return round_t(value(a) / value(b), mode)


def sqrt(b, mode):
    n = _nan_in(b, None)
    if n:
        return n
    kb = kind(b)
    if kb == "zero":
        return b, 0
    if b & SIGN:
        return CQNAN, INV
    if kb == "inf":
        return b, 0
    # x = f x 2^E. The root of x x 2^sh, sh even and large enough that it is
    # an integer of at least 60 bits, then a sticky bit below it: no
    # rounding decision depends on the bits past those (a root is never
    # halfway).
    f = (b & ((1 << 52) - 1)) | (1 << 52)
    E = ((b >> 52) & 0x7FF) - 1075
    sh = max(120 - (E + 52), -E)
    sh += sh & 1
    N = f << (E + sh)
    r = math.isqrt(N)
    exact = r * r == N
    approx = Fraction(2 * r + (0 if exact else 1), 2) / Fraction(2) ** (sh // 2)
    return round_t(approx, mode)


def compare(a, b, op):
    """CMPTxx/SU's result as a boolean and the exceptions: an SNaN is an
    invalid operation; LT, LE (and GT, GE, which the sheet raises as LT and
    LE) are for any NaN [HRM Table A-11; ev8-isa.md 4.4]."""
    ka, kb = kind(a), kind(b)
    nan = "qnan" in (ka, kb) or "snan" in (ka, kb)
    exc = INV if "snan" in (ka, kb) else 0
    if nan and op in ("lt", "le", "gt", "ge"):
        exc = INV
    if nan:
        return {"eq": False, "un": True, "ne": True}.get(op, False), exc

    def val(v, k):
        if k == "zero":
            return Fraction(0)
        if k == "inf":
            return Fraction(-1 if v >> 63 else 1) * Fraction(2) ** 2000
        return value(v)
    x, y = val(a, ka), val(b, kb)
    return {"eq": x == y, "ne": x != y, "lt": x < y, "le": x <= y,
            "gt": x > y, "ge": x >= y, "un": False}[op], exc


def cvtqt(q, mode):
    x = q - (1 << 64) if q >> 63 else q
    if x == 0:
        return 0, 0
    return round_t(Fraction(x), mode)


def cvttq(b, mode):
    """CVTTQ/SVI: the integer, rounded by `mode`; out of range the low 64
    bits of the integer and IOV with INV (with /S, HRM Table A-11), a NaN or
    infinity 0 and INV; INE when inexact."""
    kb = kind(b)
    if kb in ("qnan", "snan", "inf"):
        return 0, INV
    if kb == "zero":
        return 0, 0
    x = value(b)
    sign = x < 0
    a = -x if sign else x
    q = a.numerator // a.denominator
    r = a - q
    if r:
        if mode == NORMAL:
            q += 1 if (r > Fraction(1, 2) or (r == Fraction(1, 2) and q & 1)) else 0
        elif mode == PLUS and not sign:
            q += 1
        elif mode == MINUS and sign:
            q += 1
    flags = INE if r else 0
    i = -q if sign else q
    if not (-(1 << 63) <= i < (1 << 63)):
        flags |= IOV | INV
    return i & M64, flags


def cpys(a, b, kind_):
    if kind_ == "cpys":
        return (a & SIGN) | (b & ~SIGN & M64)
    if kind_ == "cpysn":
        return ((a & SIGN) ^ SIGN) | (b & ~SIGN & M64)
    exp = SIGN | (0x7FF << 52)
    return (a & exp) | (b & ~exp & M64)


# --- the FPCR and the trap, as a /SUI (/SVI) instruction raises them -------
#
# On this core (the 21264's, which the ev8 rows are underneath), an FP
# exception sets the FPCR's status bit; an instruction traps when an
# exception is enabled (INV, DZE, OVF always; UNF with /U; INE with /SUI;
# IOV with /V) and its FPCR disable bit is clear, and also when it finds a
# status bit clear that it has to set: EXC_SUM<SET_x>, "PALcode should set
# FPCR[x]" [21264 HRM 5.2.13, Table 5-9].

FPCR_DIS = {INV: 1 << 49, DZE: 1 << 50, OVF: 1 << 51, UNF: 1 << 61,
            INE: 1 << 62, IOV: 0}
UNDZ = 1 << 60


def status_bit(e):
    return e << 51


def raise_exc(fpcr, flags):
    """One instruction's (or element's) exceptions against the FPCR: the new
    FPCR, the EXC_SUM trap bits and the SET bits (EXC_SUM<47:41> >> 41)."""
    trap = 0
    sets = 0
    for e in (INV, DZE, OVF, UNF, INE, IOV):
        if not flags & e:
            continue
        if not fpcr & status_bit(e):
            fpcr |= status_bit(e)
            sets |= e
        dis = FPCR_DIS[e]
        if e == UNF and not fpcr & UNDZ:
            dis = 0
        if not (dis and fpcr & dis):
            trap |= e
    return fpcr, trap, sets


def fpcr_write(v):
    """MT_FPCR: <47:0> read as zero; the summary bit <63> is the OR of the
    status bits <57:52> (ARM 4.7.8)."""
    v &= M64 & ~((1 << 48) - 1)
    return (v & ~SIGN) | (SIGN if v & (0x3F << 52) else 0)


def fpcr_read(fpcr):
    return fpcr_write(fpcr)
