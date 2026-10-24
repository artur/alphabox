"""A reference model of the integer instructions, from the Alpha
Architecture Reference Manual, 4th edition (ARM), chapter 4.

Each function is the manual's "Operation" pseudocode, written with its own
helpers (SEXT, BYTE_ZAP, LEFT_SHIFT, RIGHT_SHIFT) so that a reader can hold
the two side by side. Section numbers are given for each. Nothing here is
derived from the emulator's code: where the emulator and this file
disagree, the manual decides.

Values are Python integers holding 64-bit register contents (0 .. 2**64-1).
"""

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1


def u64(x):
    return x & M64


def s64(x):
    x &= M64
    return x - (1 << 64) if x >> 63 else x


def SEXT(x, bits):
    """Sign-extend the low `bits` bits of x to 64 bits."""
    x &= (1 << bits) - 1
    if x >> (bits - 1):
        x -= 1 << bits
    return u64(x)


def LEFT_SHIFT(x, n):
    return u64(x << n)


def RIGHT_SHIFT(x, n):
    return u64(x) >> n


def BYTE_ZAP(x, mask):
    """ARM 4.6: clear each byte i of x for which mask<i> is set."""
    out = 0
    for i in range(8):
        if not (mask >> i) & 1:
            out |= x & (0xFF << (8 * i))
    return out


def field(x, hi, lo):
    return (x >> lo) & ((1 << (hi - lo + 1)) - 1)


# --- 4.4 Integer arithmetic -------------------------------------------------
# Each returns (result, overflow). Overflow is the true result not fitting
# the destination precision; only the /V forms trap on it (4.4, 14.3.2.8),
# and "the result register is written with the truncated true result".

def ADDL(a, b):  # 4.4.1: overflow from Rav<31:0> + Rbv<31:0>
    true = s64(SEXT(a, 32)) + s64(SEXT(b, 32))
    return SEXT(a + b, 32), not (-(1 << 31) <= true < (1 << 31))


def S4ADDL(a, b):  # 4.4.2
    return SEXT(LEFT_SHIFT(a, 2) + b, 32), False


def S8ADDL(a, b):
    return SEXT(LEFT_SHIFT(a, 3) + b, 32), False


def ADDQ(a, b):  # 4.4.3
    true = s64(a) + s64(b)
    return u64(a + b), not (-(1 << 63) <= true < (1 << 63))


def S4ADDQ(a, b):  # 4.4.4
    return u64(LEFT_SHIFT(a, 2) + b), False


def S8ADDQ(a, b):
    return u64(LEFT_SHIFT(a, 3) + b), False


def SUBL(a, b):  # 4.4.13
    true = s64(SEXT(a, 32)) - s64(SEXT(b, 32))
    return SEXT(a - b, 32), not (-(1 << 31) <= true < (1 << 31))


def S4SUBL(a, b):  # 4.4.14
    return SEXT(LEFT_SHIFT(a, 2) - b, 32), False


def S8SUBL(a, b):
    return SEXT(LEFT_SHIFT(a, 3) - b, 32), False


def SUBQ(a, b):  # 4.4.15
    true = s64(a) - s64(b)
    return u64(a - b), not (-(1 << 63) <= true < (1 << 63))


def S4SUBQ(a, b):  # 4.4.16
    return u64(LEFT_SHIFT(a, 2) - b), False


def S8SUBQ(a, b):
    return u64(LEFT_SHIFT(a, 3) - b), False


def MULL(a, b):  # 4.4.10: overflow from the longword product
    true = s64(SEXT(a, 32)) * s64(SEXT(b, 32))
    return SEXT(true, 32), not (-(1 << 31) <= true < (1 << 31))


def MULQ(a, b):  # 4.4.11
    true = s64(a) * s64(b)
    return u64(true), not (-(1 << 63) <= true < (1 << 63))


def UMULH(a, b):  # 4.4.12: the high 64 bits of the unsigned 128-bit product
    return (u64(a) * u64(b)) >> 64, False


# 4.4.5 Integer signed compare, 4.4.6 unsigned compare
def CMPEQ(a, b):
    return int(a == b), False


def CMPLT(a, b):
    return int(s64(a) < s64(b)), False


def CMPLE(a, b):
    return int(s64(a) <= s64(b)), False


def CMPULT(a, b):
    return int(u64(a) < u64(b)), False


def CMPULE(a, b):
    return int(u64(a) <= u64(b)), False


def CMPBGE(a, b):  # 4.6.1, as written: 0||a_i + 0||NOT b_i + 1, keep bit 8
    rc = 0
    for i in range(8):
        ai, bi = field(a, 8 * i + 7, 8 * i), field(b, 8 * i + 7, 8 * i)
        temp = ai + ((~bi) & 0xFF) + 1
        rc |= ((temp >> 8) & 1) << i
    return rc, False


# 4.4.7-9 Count (CIX)
def CTLZ(_a, b):
    temp = 0
    for i in range(63, -1, -1):
        if (b >> i) & 1:
            break
        temp += 1
    return temp, False


def CTPOP(_a, b):
    return bin(u64(b)).count("1"), False


def CTTZ(_a, b):
    temp = 0
    for i in range(64):
        if (b >> i) & 1:
            break
        temp += 1
    return temp, False


# --- 4.5 Logical and shift ---------------------------------------------------

def AND(a, b):
    return a & b, False


def BIC(a, b):
    return a & u64(~b), False


def BIS(a, b):
    return a | b, False


def EQV(a, b):
    return a ^ u64(~b), False


def ORNOT(a, b):
    return a | u64(~b), False


def XOR(a, b):
    return a ^ b, False


def SLL(a, b):  # 4.5.3: shift amount Rbv<5:0>
    return LEFT_SHIFT(a, field(b, 5, 0)), False


def SRL(a, b):
    return RIGHT_SHIFT(a, field(b, 5, 0)), False


def SRA(a, b):  # 4.5.4: arithmetic: the sign is copied into the vacated bits
    return u64(s64(a) >> field(b, 5, 0)), False


# 4.5.2 Conditional move: IF test(Rav) THEN Rc <- Rbv (else Rc unchanged)
CMOV_TEST = {
    "CMOVEQ": lambda a: a == 0,
    "CMOVNE": lambda a: a != 0,
    "CMOVLT": lambda a: s64(a) < 0,
    "CMOVLE": lambda a: s64(a) <= 0,
    "CMOVGT": lambda a: s64(a) > 0,
    "CMOVGE": lambda a: s64(a) >= 0,
    "CMOVLBS": lambda a: a & 1 == 1,
    "CMOVLBC": lambda a: a & 1 == 0,
}

# --- 4.6 Byte manipulation (little-endian) ----------------------------------

_BYTE_MASK = {"B": 0x01, "W": 0x03, "L": 0x0F, "Q": 0xFF}


def EXT(size, high, a, b):  # 4.6.2
    byte_mask = _BYTE_MASK[size]
    if not high:
        byte_loc = field(b, 2, 0) * 8
        temp = RIGHT_SHIFT(a, byte_loc & 63)
    else:
        byte_loc = 64 - field(b, 2, 0) * 8
        temp = LEFT_SHIFT(a, byte_loc & 63)
    return BYTE_ZAP(temp, (~byte_mask) & 0xFF)


def INS(size, high, a, b):  # 4.6.3
    byte_mask = _BYTE_MASK[size] << field(b, 2, 0)
    if not high:
        byte_loc = field(b, 2, 0) * 8
        temp = LEFT_SHIFT(a, byte_loc & 63)
        return BYTE_ZAP(temp, (~byte_mask) & 0xFF)
    byte_loc = 64 - field(b, 2, 0) * 8
    temp = RIGHT_SHIFT(a, byte_loc & 63)
    return BYTE_ZAP(temp, (~(byte_mask >> 8)) & 0xFF)


def MSK(size, high, a, b):  # 4.6.4
    byte_mask = _BYTE_MASK[size] << field(b, 2, 0)
    if not high:
        return BYTE_ZAP(a, byte_mask & 0xFF)
    return BYTE_ZAP(a, (byte_mask >> 8) & 0xFF)


def SEXTB(_a, b):  # 4.6.5
    return SEXT(b, 8), False


def SEXTW(_a, b):
    return SEXT(b, 16), False


def ZAP(a, b):  # 4.6.6
    return BYTE_ZAP(a, field(b, 7, 0)), False


def ZAPNOT(a, b):
    return BYTE_ZAP(a, (~b) & 0xFF), False


for _sz in "BWLQ":
    for _hi in (False, True):
        if _sz == "B" and _hi:
            continue
        for _kind, _fn in (("EXT", EXT), ("INS", INS), ("MSK", MSK)):
            _name = "%s%s%s" % (_kind, _sz, "H" if _hi else "L")
            globals()[_name] = (lambda f, s, h: lambda a, b: (f(s, h, a, b), False))(_fn, _sz, _hi)

# --- 4.13 Multimedia (MVI) ---------------------------------------------------


def _lanes(x, width):
    return [field(x, i + width - 1, i) for i in range(0, 64, width)]


def _join(vals, width):
    out = 0
    for k, v in enumerate(vals):
        out |= (v & ((1 << width) - 1)) << (k * width)
    return out


def _sgn(v, width):
    return v - (1 << width) if v >> (width - 1) else v


def _minmax(a, b, width, signed, pick):
    out = []
    for x, y in zip(_lanes(a, width), _lanes(b, width)):
        if signed:
            xs, ys = _sgn(x, width), _sgn(y, width)
            out.append(x if pick(xs, ys) else y)
        else:
            out.append(x if pick(x, y) else y)
    return _join(out, width)


def _lt(x, y):
    return x < y


def _gt(x, y):
    return x > y


def MINUB8(a, b):
    return _minmax(a, b, 8, False, _lt), False


def MINSB8(a, b):
    return _minmax(a, b, 8, True, _lt), False


def MINUW4(a, b):
    return _minmax(a, b, 16, False, _lt), False


def MINSW4(a, b):
    return _minmax(a, b, 16, True, _lt), False


def MAXUB8(a, b):
    return _minmax(a, b, 8, False, _gt), False


def MAXSB8(a, b):
    return _minmax(a, b, 8, True, _gt), False


def MAXUW4(a, b):
    return _minmax(a, b, 16, False, _gt), False


def MAXSW4(a, b):
    return _minmax(a, b, 16, True, _gt), False


def PERR(a, b):  # 4.13.2
    temp = 0
    for i in range(8):
        x, y = field(a, 8 * i + 7, 8 * i), field(b, 8 * i + 7, 8 * i)
        temp += (x - y) if x >= y else (y - x)
    return temp, False


def PKLB(_a, b):  # 4.13.3
    return field(b, 7, 0) | (field(b, 39, 32) << 8), False


def PKWB(_a, b):
    return (field(b, 7, 0) | (field(b, 23, 16) << 8) | (field(b, 39, 32) << 16)
            | (field(b, 55, 48) << 24)), False


def UNPKBL(_a, b):  # 4.13.4
    return field(b, 7, 0) | (field(b, 15, 8) << 32), False


def UNPKBW(_a, b):
    return (field(b, 7, 0) | (field(b, 15, 8) << 16) | (field(b, 23, 16) << 32)
            | (field(b, 31, 24) << 48)), False


# --- the 21464's additions (not in the ARM) --------------------------------
# 21464 Internal Design Specification Rev 1.1k, PDF pp. 73-74, as
# docs/platforms/ev8-isa.md 3.2 reads it: the second half of a split CMOV
# copies Rbv when its first operand's predicate bit is set and that
# operand's value otherwise; executed from memory the bit is clear, so
# Rc <- Rav (FCMOV2: Fc <- Fav).

def CMOV2(a, _b):
    return u64(a), False


# --- 4.11 Miscellaneous --------------------------------------------------------


# The CPU feature mask and the IMPLVER value are the processor's: rows.py
# has them for each row the oracle runs.
def AMASK(_a, b, cpu_feature_mask):  # 4.11.1: Rc <- Rbv AND NOT CPU_feature_mask
    return b & u64(~cpu_feature_mask), False


# --- 2.2 / 4.10.18-19: floating register images and the FIX moves ----------

def MAP_S(e):  # ARM Table 2-2
    if e == 0xFF:
        return 0x7FF
    if e == 0:
        return 0
    return ((e >> 7) << 10) | ((0 if e >> 7 else 7) << 7) | (e & 0x7F)


def MAP_F(e):  # ARM Table 2-1
    if e == 0:
        return 0
    return ((e >> 7) << 10) | ((0 if e >> 7 else 7) << 7) | (e & 0x7F)


def ITOFS(a):  # 4.10.19
    return ((field(a, 31, 31) << 63) | (MAP_S(field(a, 30, 23)) << 52)
            | (field(a, 22, 0) << 29))


def ITOFF(a):  # 4.10.19 (no word swap)
    return ((field(a, 31, 31) << 63) | (MAP_F(field(a, 30, 23)) << 52)
            | (field(a, 22, 0) << 29))


def ITOFT(a):
    return u64(a)


def FTOIS(f):  # 4.10.18
    low = (field(f, 63, 62) << 30) | field(f, 58, 29)
    return (SEXT(field(f, 63, 63), 1) & ~M32 & M64) | low


def FTOIT(f):
    return u64(f)


# --- 4.3 Branches: the tests (4.3.1 integer, 4.9.1 floating) ----------------

BRANCH_TEST = {
    "BEQ": lambda a: a == 0, "BNE": lambda a: a != 0,
    "BLT": lambda a: s64(a) < 0, "BLE": lambda a: s64(a) <= 0,
    "BGT": lambda a: s64(a) > 0, "BGE": lambda a: s64(a) >= 0,
    "BLBC": lambda a: a & 1 == 0, "BLBS": lambda a: a & 1 == 1,
}


def _fzero(f):  # 4.9.1: a register is zero if all bits but the sign are 0
    return (f & ~(1 << 63)) == 0


FBRANCH_TEST = {
    "FBEQ": lambda f: _fzero(f),
    "FBNE": lambda f: not _fzero(f),
    "FBLT": lambda f: (f >> 63) == 1 and not _fzero(f),
    "FBLE": lambda f: (f >> 63) == 1 or _fzero(f),
    "FBGT": lambda f: (f >> 63) == 0 and not _fzero(f),
    "FBGE": lambda f: (f >> 63) == 0 or _fzero(f),
}


def operate(name, a, b):
    """(result, overflow) of an integer operate instruction."""
    return globals()[name.replace("/V", "")](a, b)
