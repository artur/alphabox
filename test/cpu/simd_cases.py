"""The 21464's short-vector SIMD set under opcode 1C (group simd): every
row of docs/platforms/ev8-isa.md 3.3 (the contract with the nada
toolchain) on the rows that have it (`ev8`, `ev8n`, which carry
CPU_EXT_EV8), in the register and literal forms, and the same words
raising OPCDEC on every other row. Stage S5 of docs/platforms/ev8.md.

The expected values are computed here from the sheet's definitions (the
zz size rule, SAT, PERMB8's selectors, the tree sums, VMINMAX's running
pair, the shift counts), not from the emulator. A 1C code the sheet does
not assign raises OPCDEC on every row, and the existing codes (00, 01, 30
to 3F, 70, 78) are group intop's and fix's, unchanged.

Notation as the sheet's: x.b[i], x.w[i], x.l[i] are byte, word and
longword i of a quadword, i = 0 the least significant; SAT clamps to the
element's range (unsigned 0 to 2^n - 1, signed -2^(n-1) to 2^(n-1) - 1).
"""
import alpha_asm as A
from alpha_asm import OP, R31, mem, opl, opr
import rows

M64 = (1 << 64) - 1

GROUPS = [("simd", "the 21464's opcode 1C short-vector SIMD set (ev8-isa.md "
                   "3.3) where the row has it, OPCDEC elsewhere and for the "
                   "codes the sheet leaves free")]

# --- the sheet's table (ev8-isa.md 3.3): function code -> mnemonic ----------

# zz = 00 UB8, 01 UW4, 10 SB8, 11 SW4: bit 0 the wider element, bit 1 the
# signed form [inference, agreed].
ZZ = (("UB8", 8, False), ("UW4", 16, False), ("SB8", 8, True),
      ("SW4", 16, True))
L2 = (("UL2", 32, False), (None, None, None), ("SL2", 32, True))

CODES = {f: n for n, f in A.SIMD1C.items()}
FNC = {v: k for k, v in CODES.items()}
# Ra = $31 by the syntax: the operand is Rb's, as MVI's UNPKBW [decision].
UNARY = {"UPKUBW4", "UPKUWL2", "UPKSBW4", "UPKSWL2"}
# The core's own 1C codes [IDS pp. 662-664; ARM p. 783].
EXISTING = {0x00, 0x01, 0x70, 0x78} | set(range(0x30, 0x40))
FREE = [f for f in range(0x80) if f not in CODES and f not in EXISTING]


def word(name, ra, b, rc, lit=False):
    """The instruction word: Rb, or the 8-bit literal b with lit."""
    fn = FNC[name]
    return opl(0x1C, fn, ra, b, rc) if lit else opr(0x1C, fn, ra, b, rc)


# --- the sheet's definitions -------------------------------------------------


def el(x, w, i, signed=False):
    v = (x >> (w * i)) & ((1 << w) - 1)
    if signed and v >> (w - 1):
        v -= 1 << w
    return v


def elems(x, w, signed=False):
    return [el(x, w, i, signed) for i in range(64 // w)]


def join(vals, w):
    out = 0
    for i, v in enumerate(vals):
        out |= (v & ((1 << w) - 1)) << (w * i)
    return out


def sat(v, w, signed):
    lo, hi = (-(1 << (w - 1)), (1 << (w - 1)) - 1) if signed else (0, (1 << w) - 1)
    return min(max(v, lo), hi)


def ext32(v, w, signed):
    """An element-size value as a longword: sign- or zero-extended."""
    v &= (1 << w) - 1
    if signed and v >> (w - 1):
        v -= 1 << w
    return v & 0xFFFFFFFF


def form(name):
    """(width, signed) of a zz or L2 mnemonic's elements."""
    for sfx, w, s in ZZ + (L2[0], L2[2]):
        if name.endswith(sfx):
            return w, s
    raise KeyError(name)


def simd(name, a, b):
    """Rc of `name` with Rav = a and Rbv = b (the literal, zero-extended,
    in the literal form), as ev8-isa.md 3.3 defines it."""
    a &= M64
    b &= M64
    if name == "CMPWGE":
        # Rc<2i+1:2i> <- 11 if Rav.w[i] >= Rbv.w[i] unsigned, else 00;
        # Rc<63:8> <- 0.
        return sum(3 << (2 * i) for i in range(4)
                   if el(a, 16, i) >= el(b, 16, i))
    if name == "CMPLGE":
        return sum(0xF << (4 * i) for i in range(2)
                   if el(a, 32, i) >= el(b, 32, i))
    if name == "PERMB8":
        out = []
        for i in range(8):
            s = (b >> (4 * i)) & 0xF
            if s <= 7:
                v = el(a, 8, s)
            elif s <= 11:
                v = el(b, 8, s - 4)          # Rb's high longword
            elif s == 12:                    # sign fill from Rc.b[i-1]
                v = 0xFF if i and out[i - 1] & 0x80 else 0
            elif s == 13:                    # reserved: Alphabox 0
                v = 0
            elif s == 14:
                v = 0xFF
            else:
                v = 0
            out.append(v)
        return join(out, 8)
    if name == "GPKBLB4":
        src = [el(a, 32, 0, True), el(a, 32, 1, True), el(b, 32, 0, True),
               el(b, 32, 1, True)]
        return join([min(max(v, 0), 255) for v in src], 8)
    if name in ("PKUWB8", "PKSWB8", "PKULW4", "PKSLW4"):
        w = 16 if name.endswith("B8") else 32
        signed = name[2] == "S"
        out = [sat(v, w // 2, signed)
               for v in elems(a, w, signed) + elems(b, w, signed)]
        return join(out, w // 2)
    if name in UNARY:
        w = 8 if name.endswith("BW4") else 16
        signed = name[3] == "S"
        return join([el(b, w, i, signed) for i in range(64 // (2 * w))], 2 * w)
    stem = next(s for s in ("VMINMAX", "VADD", "VSUB", "TADD", "TSUB",
                            "TABSERR", "TSQERR", "TMULUS", "TMUL",
                            "VMULLUW4", "VMULHUW4", "VSRA", "VSR", "VSL")
                if name.startswith(s))
    if stem in ("VMULLUW4", "VMULHUW4"):
        sh = 0 if stem == "VMULLUW4" else 16
        return join([((x * y) >> sh) & 0xFFFF
                     for x, y in zip(elems(a, 16), elems(b, 16))], 16)
    if stem in ("VSR", "VSRA", "VSL"):
        w = {"B8": 8, "W4": 16, "L2": 32}[name[-2:]]
        n = b & (w - 1)                      # one count, modulo the width
        if stem == "VSL":
            return join([x << n for x in elems(a, w)], w)
        return join([x >> n for x in elems(a, w, stem == "VSRA")], w)
    if stem == "TMULUS":
        w = 8 if name.endswith("B8") else 16
        return sum(x * y for x, y in zip(elems(a, w), elems(b, w, True))) & M64
    w, signed = form(name)
    ea, eb = elems(a, w, signed), elems(b, w, signed)
    if stem == "VADD":
        return join([sat(x + y, w, signed) for x, y in zip(ea, eb)], w)
    if stem == "VSUB":
        return join([sat(x - y, w, signed) for x, y in zip(ea, eb)], w)
    if stem == "VMINMAX":
        # Rb<31:0> the running minimum, Rb<63:32> the running maximum, each
        # an element-size value in the low bits of its longword [decision].
        mn = el(b, w, 0, signed)
        mx = el(b >> 32, w, 0, signed)
        mn = min([mn] + ea)
        mx = max([mx] + ea)
        return ext32(mn, w, signed) | (ext32(mx, w, signed) << 32)
    f = {"TADD": lambda x, y: x + y, "TSUB": lambda x, y: x - y,
         "TABSERR": lambda x, y: abs(x - y),
         "TSQERR": lambda x, y: (x - y) ** 2,
         "TMUL": lambda x, y: x * y}[stem]
    return sum(f(x, y) for x, y in zip(ea, eb)) & M64


# --- operands ----------------------------------------------------------------

V = [0, M64, 0x7F7F7F7F7F7F7F7F, 0x8080808080808080, 0x7FFF8000FFFF0001,
     0x017F80FE00FF7F81, 0x0123456789ABCDEF, 0xFEDCBA9876543210,
     0x8000800080008000, 0x7FFF7FFF7FFF7FFF, 0x00FF00FF00FF00FF,
     0x5A5AA5A55AA5A55A, 0x7FFFFFFF80000000, 0x80000000FFFFFFFF,
     0x00000001FFFFFFFE, 0xFF00FF0001FF7F80]
# Fewer for the full cross of each instruction; the rest pair along.
VX = [M64, 0x7F7F7F7F7F7F7F7F, 0x8080808080808080, 0x7FFF8000FFFF0001,
      0x7FFFFFFF80000000]
LITS = [0, 1, 0x7F, 0x80, 0xFF]
SHIFT_B = [0, 1, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 0xFFFFFFFFFFFFFFF9,
           0x8000000000000005]


class Regs:
    """Ra, Rb, Rc out of R1-R23: distinct, Rc = Ra, Rc = Rb, Ra = Rb, now
    and then R31 as a source or as the destination."""

    def __init__(self):
        self.k = 0

    def next(self):
        k = self.k
        self.k += 1
        ra = 1 + (k * 5) % 23
        rb = 1 + (k * 7 + 3) % 23
        if rb == ra:
            rb = 1 + rb % 23
        rc = 1 + (k * 11 + 7) % 23
        while rc in (ra, rb):
            rc = 1 + rc % 23
        m = k % 9
        if m == 1:
            rc = ra
        elif m == 2:
            rc = rb
        elif m == 4:
            rb = ra
        elif k % 37 == 5:
            rc = R31
        elif k % 41 == 6:
            ra = R31
        elif k % 43 == 7:
            rb = R31
        return ra, rb, rc




SENT = 0xC0DEC0DEC0DEC0DE
BATCH = 12


def batch(b, regs, name, label, items):
    """Cases of one instruction on the row that has it, BATCH instructions
    to a case (one block): each loads its operands, runs and stores Rc; the
    first also stores Ra and Rb, unchanged. items: (a, b, literal form)."""
    for k in range(0, len(items), BATCH):
        part = items[k:k + BATCH]
        b.start()
        b.begin("simd: %s, %s (%d)" % (name, label, k // BATCH))
        for j, (a, bv, lit) in enumerate(part):
            ra, rb, rc = regs.next()
            if name in UNARY:
                ra = R31
            w = word(name, ra, bv if lit else rb, rc, lit)
            assert rows.ROWS["ev8"].own_instruction(w) == name
            b.need(rows.EV8)
            vals = {}
            if ra != R31:
                b.load(ra, a)
                vals[ra] = a
            else:
                a = 0
            if lit:
                bval = bv
            elif rb == R31:
                bval = 0
            elif rb in vals:
                bval = vals[rb]
            else:
                b.load(rb, bv)
                vals[rb] = bval = bv
            if j == 0 and rc != R31 and rc not in vals:
                b.load(rc, SENT)
            b.emit(w)
            r = simd(name, a, bval) if rc != R31 else 0
            what = "%s r%d,%s,r%d (a=%x b=%x)" % (
                name, ra, ("#%d" % bv) if lit else "r%d" % rb, rc, a, bval)
            b.store(rc, what, r)
            if j == 0:
                for reg in sorted(vals):
                    if reg != rc:
                        b.store(reg, "%s: r%d unchanged" % (what, reg),
                                vals[reg])


def opcdec(b, name, w, need=None):
    b.begin(name)
    if need is not None:
        b.need(need)
    b.trap_clear()
    at = b.va()
    b.emit(w)
    b.trap_check(vector=0x400, exc_addr=at)


def check_words():
    """The sheet's example words (3.3)."""
    for w, want in ((word("PKUWB8", 1, 2, 3), 0x70220503),
                    (word("UPKSBW4", R31, 2, 3), 0x73E205C3),
                    (word("VADDUB8", 1, 2, 3), 0x70220203)):
        assert w == want, "%08x, ev8-isa.md 3.3 gives %08x" % (w, want)


def g_simd(b):
    check_words()
    regs = Regs()
    # Every new word raises OPCDEC where the row does not have it (rows
    # without CPU_EXT_EV8), and every code the sheet leaves free raises it
    # on every row (ev8-isa.md section 1: unused 1C codes trap).
    for fn, name in sorted(CODES.items()):
        w = opr(0x1C, fn, 1, 2, 13)
        assert rows.ROWS["ev8"].own_instruction(w) == name
        opcdec(b, "simd: 1C.%02X (%s) raises OPCDEC where the row lacks it"
               % (fn, name), w, rows.NOT_EV8)
        wl = opl(0x1C, fn, 1, 0x5A, 13)
        opcdec(b, "simd: 1C.%02X (%s) literal form raises OPCDEC where the "
               "row lacks it" % (fn, name), wl, rows.NOT_EV8)
    for fn in FREE:
        opcdec(b, "simd: 1C.%02X, a code the sheet leaves free, raises OPCDEC"
               % fn, opr(0x1C, fn, 1, 2, 13))

    binary = [n for _, n in sorted(CODES.items()) if n not in UNARY]
    for name in binary:
        if name.startswith(("VSR", "VSL")):
            items = [(a, bv, False) for a in V[:6] for bv in SHIFT_B]
            items += [(a, lit, True) for a in V[:4]
                      for lit in (0, 1, 7, 8, 15, 16, 31, 32, 0xFF)]
            batch(b, regs, name, "counts", items)
            continue
        items = [(a, bv, False) for a in VX for bv in VX]
        items += [(a, V[(i * 5 + 3) % len(V)], False) for i, a in enumerate(V)]
        items += [(a, lit, True) for a in (V[6], V[3], V[11])
                  for lit in LITS]
        batch(b, regs, name, "operands", items)
    for name in sorted(UNARY):
        batch(b, regs, name, "operands", [(0, bv, False) for bv in V] +
              [(0, lit, True) for lit in LITS])

    # Saturation at both ends, each lane on its own.
    for stem in ("VADD", "VSUB"):
        for sfx, w, signed in ZZ + (L2[0], L2[2]):
            name = stem + sfx
            n = 64 // w
            top = (1 << (w - 1)) - 1 if signed else (1 << w) - 1
            bot = -(1 << (w - 1)) if signed else 0
            pairs = [(top, 1), (top, top), (bot, -1 if signed else 0),
                     (bot, bot), (top - 1, 1), (bot + 1, -1),
                     (top, -1 if signed else 0), (bot, 1), (1, top),
                     (0, top), (-1 if signed else 0, bot)]
            items = []
            for x, y in pairs:
                for lane in range(n):
                    a = join([x if i == lane else i + 1 for i in range(n)], w)
                    bv = join([y if i == lane else 2 for i in range(n)], w)
                    items.append((a, bv, False))
            batch(b, regs, name, "saturation", items)
    for name in ("PKUWB8", "PKSWB8", "PKULW4", "PKSLW4"):
        w = 16 if name.endswith("B8") else 32
        n = 64 // w
        edge = [0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF,
                0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFF80,
                0xFFFFFF7F, 0xFFFF8000, 0xFFFF7FFF]
        edge = [e for e in edge if e < (1 << w)]
        items = []
        for k in range(0, len(edge), n):
            chunk = edge[k:k + n]
            a = join(chunk + [0] * (n - len(chunk)), w)
            rchunk = list(reversed(edge))[k:k + n]
            bv = join(rchunk + [3] * (n - len(rchunk)), w)
            items.append((a, bv, False))
        batch(b, regs, name, "saturation", items)
    batch(b, regs, "GPKBLB4", "clamping",
          [(x | (0xFF << 32), (x << 32) | 0x1234, False)
           for x in (0, 0xFF, 0x100, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
                     0x7F, 0xFFFFFF01, 0x00000180)])

    # PERMB8: every selector in every byte.
    items = []
    for s in range(16):
        for lane in range(8):
            sel = join([s if i == lane else (i + 3) % 8 for i in range(8)], 4)
            hi = (0x80FF7F01, 0x0102037F)[(s + lane) & 1]
            items.append((0x8877665544332211, sel | (hi << 32), False))
    # Sign fill after a negative and a positive byte, and from byte 0.
    for sel in (0xCCCCCCC0, 0xCCCCCCC1, 0xC0C0C0C0, 0x0C0C0C0C, 0xCCCCCCCC,
                0xEDCBA987, 0x7654321F, 0xC7C6C5C4):
        for a in (0x80FF7F0102804001, 0x7F01FF8000000080):
            items.append((a, sel | (0x8001FF7F << 32), False))
    batch(b, regs, "PERMB8", "selectors", items)

    # Tree sums with carries out of every element width.
    for stem in ("TADD", "TSUB", "TABSERR", "TSQERR", "TMUL"):
        for sfx, w, signed in ZZ:
            batch(b, regs, stem + sfx, "carries", [
                (a, bv, False) for a, bv in (
                    (M64, M64), (0x8080808080808080, 0x7F7F7F7F7F7F7F7F),
                    (0x8000800080008000, 0x8000800080008000),
                    (0x7FFF7FFF7FFF7FFF, 0x8000800080008000), (0, M64),
                    (M64, 0), (0xFF00FF00FF00FF00, 0x00FF00FF00FF00FF))])
    for name in ("TMULUSB8", "TMULUSW4"):
        batch(b, regs, name, "carries", [
            (a, bv, False) for a, bv in (
                (M64, M64), (M64, 0x8080808080808080),
                (0x8080808080808080, M64),
                (0xFFFF0000FFFF0000, 0x8000800080008000))])
    # VMINMAX: the running pair, from the start values the sheet gives, and
    # carried through four quadwords; the pair's other bits are not read.
    for sfx, w, signed in ZZ + (L2[0], L2[2]):
        name = "VMINMAX" + sfx
        big = (1 << (w - 1)) - 1 if signed else (1 << w) - 1
        small = -(1 << (w - 1)) if signed else 0
        run = ext32(big, w, signed) | (ext32(small, w, signed) << 32)
        items = []
        for a in (0x0123456789ABCDEF, 0x8080808080808080, 0x7F7F7F7F7F7F7F7F,
                  0x00FF00FF00FF00FF):
            items.append((a, run, False))
            run = simd(name, a, run)
        items.append((0x1111111111111111,
                      0xABCDEF00 | (big & 0xFF) | (0x12345600 << 32), False))
        items.append((0x1111111111111111, 0x12340005 | (0xFFFF0001 << 32),
                      False))
        batch(b, regs, name, "running pair", items)
    # The JIT forwards the value an operate leaves in its result register to
    # the next instruction's Ra: a SIMD word reading it, and the next one
    # reading the SIMD word's.
    for name in ("VADDUB8", "TABSERRSW4", "PERMB8", "VMINMAXSL2"):
        w = word(name, 3, 4, 5)
        b.start()
        b.load(1, 0x0000000100000002)
        b.load(2, 0x0F0F0F0F70707070)
        b.load(4, 0x7766554433221100)
        b.load(5, SENT)
        b.begin("simd: ADDQ r1,r2,r3; %s r3,r4,r5; ADDQ r5,#1,r6" % name)
        b.need(rows.EV8)
        b.emit(opr(0x10, 0x20, 1, 2, 3))
        b.emit(w)
        b.emit(opl(0x10, 0x20, 5, 1, 6))
        r = simd(name, 0x0F0F0F1070707072, 0x7766554433221100)
        b.store(5, "r5", r)
        b.store(6, "r6", (r + 1) & M64)
        b.store(4, "r4", 0x7766554433221100)


EMIT = {"simd": g_simd}
