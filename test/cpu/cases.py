"""The conformance cases. Each group emits instructions through the image
builder (image.Builder) and states, from aarm.py and the manuals, what
every stored result must be.

References: ARM = Alpha Architecture Reference Manual, 4th edition; HRM =
21264/EV68CB-DC Hardware Reference Manual. A `known` value is a documented
divergence: docs/cpu-fidelity.md, or a finding of lab/legacy-audit/cpu/
AUDIT.md (CPU-nn) or lab/legacy-audit/jit/AUDIT.md (JIT-nn) until it is
fixed. The JIT groups are in jit_cases.py, the native EV8's in
ev8n_cases.py, Tarantula's in tarantula_cases.py, the EV8's 1C SIMD set in
simd_cases.py and its paired single in paired_cases.py.

What a case expects of the processor (IMPLVER, AMASK) is the row's
(b.row, rows.py), and a case that needs what the row lacks is skipped
(image.Builder): the extensions are found from the words a case emits, the
rest it asks for with b.need().
"""
import aarm
import alpha_asm as A
import ev8n_cases
from alpha_asm import OP, R31, bra, jmp, mem, mfc, opl, opr, fop, call_pal
import image
import jit_cases
import paired_cases
import rows
import simd_cases
import tarantula_cases
from image import R_SCR, R_TR, TR_COUNT

M64 = (1 << 64) - 1

GROUPS = [
    ("intop", "integer operate: arithmetic, compare, logical, shift, byte "
              "manipulation, multiply, CIX, SEXT, MVI; register and literal "
              "forms, aliased registers, R31"),
    ("cmov", "conditional moves, taken and not, aliased registers"),
    ("trapv", "the /V forms: overflow traps, EXC_ADDR, EXC_SUM, the "
              "truncated result"),
    ("mem", "loads and stores, LDA/LDAH, LDQ_U/STQ_U, BWX, prefetches"),
    ("unaligned", "misaligned data accesses (UNALIGN; docs/cpu-fidelity.md)"),
    ("llsc", "LDx_L/STx_C success and failure"),
    ("branch", "conditional and unconditional branches, link registers, "
               "jumps and their hints, FP branches"),
    ("fix", "ITOFS/ITOFF/ITOFT, FTOIS/FTOIT, FEN, FNOP"),
    ("misc", "AMASK, IMPLVER, RPCC, barriers, hints, RC/RS, OPCDEC, CALL_PAL"),
    ("mode", "user mode: privileged CALL_PAL and PALRES OPCDEC, superpage "
             "ACV, ITB/DTB fills, DTB protection, FOR/FOW, MM_STAT"),
    ("ev8", "the 21464's stateless additions: CCB, CMOV2, FCMOV2 where the "
            "row has them, MAP_ALIGN and WH64EN on every row"),
    ("quiesce", "the 21464's LDL_ARM, LDQ_ARM and QUIESCE where the row has "
                "them (one processor: the timeout), AMASK bit 10"),
] + jit_cases.GROUPS + ev8n_cases.GROUPS + tarantula_cases.GROUPS + \
    simd_cases.GROUPS + paired_cases.GROUPS

# --- operand sets ------------------------------------------------------------

E20 = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
       0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
       0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE, 0x0123456789ABCDEF,
       0xFEDCBA9876543210, 0x00FF00FF00FF00FF, 0x8000000000000001,
       0x00000000FFFF8000, 0xFFFFFFFF80000000]
E10 = [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFFFFFFFFFF,
       0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0x0123456789ABCDEF,
       0xFEDCBA9876543210]
E8 = [0, 0xFFFFFFFFFFFFFFFF, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA,
      0x0123456789ABCDEF, 0x8000000000000000, 1, 0x00FF00FF00FF00FF]
LANES = [0, 0xFFFFFFFFFFFFFFFF, 0x7F7F7F7F7F7F7F7F, 0x8080808080808080,
         0x7FFF8000FFFF0001, 0x017F80FE00FF7F81, 0x0123456789ABCDEF,
         0xFEDCBA9876543210, 0x8000800080008000, 0x7FFF7FFF7FFF7FFF,
         0x00FF00FF00FF00FF, 0x5A5AA5A55AA5A55A]
SHIFTS = [0, 1, 4, 7, 8, 31, 32, 33, 63, 64, 65, 127, 0xFFFFFFFFFFFFFFFF,
          0x8000000000000020, 0xFFFFFFFFFFFFFFC1]
BYTEPOS = [0, 1, 2, 3, 4, 5, 6, 7, 11, 0xFFFFFFFFFFFFFFF9, 0x106,
           0x8000000000000005]
ZAPMASK = [0, 1, 0x80, 0xFF, 0x55, 0xAA, 0x0F, 0xF0, 0x1FE,
           0xFFFFFFFFFFFFFF00]
UNARY = E20 + [0x40, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x0000000100000001,
               0x4000000000000000, 0xF0, 0x0F00000000000000,
               0xF000000000000000, 0x00000000FF7F8001]
ARITH_LITS = [0, 1, 0x7F, 0x80, 0xFF]
SHIFT_LITS = [0, 1, 7, 8, 31, 32, 63, 64, 127, 128, 255]

ARITH = ["ADDL", "S4ADDL", "S8ADDL", "ADDQ", "S4ADDQ", "S8ADDQ", "SUBL",
         "S4SUBL", "S8SUBL", "SUBQ", "S4SUBQ", "S8SUBQ", "CMPEQ", "CMPLT",
         "CMPLE", "CMPULT", "CMPULE", "MULL", "MULQ", "UMULH", "CMPBGE"]
LOGIC = ["AND", "BIC", "BIS", "EQV", "ORNOT", "XOR"]
SHIFT = ["SLL", "SRL", "SRA"]
BYTEM = ["EXTBL", "EXTWL", "EXTLL", "EXTQL", "EXTWH", "EXTLH", "EXTQH",
         "INSBL", "INSWL", "INSLL", "INSQL", "INSWH", "INSLH", "INSQH",
         "MSKBL", "MSKWL", "MSKLL", "MSKQL", "MSKWH", "MSKLH", "MSKQH"]
ZAPS = ["ZAP", "ZAPNOT"]
# Ra must be R31 (ARM 4.4.7-9, 4.6.5, 4.13.3-4).
UNARY_OPS = ["CTLZ", "CTPOP", "CTTZ", "SEXTB", "SEXTW", "PKLB", "PKWB",
             "UNPKBL", "UNPKBW"]
UNARY_LIT = ["SEXTB", "SEXTW"]          # ARM 4.6.5 lists a literal form
MVI = ["MINUB8", "MINSB8", "MINUW4", "MINSW4", "MAXUB8", "MAXSB8", "MAXUW4",
       "MAXSW4"]

_k = [0]


def _regs(distinct_only=False):
    """Rotate the operand and destination registers (R1-R23): distinct,
    Rc = Ra, Rc = Rb, and now and then R31 as the destination."""
    k = _k[0]
    _k[0] += 1
    ra = 1 + (k * 5) % 23
    rb = 1 + (k * 7 + 3) % 23
    if rb == ra:
        rb = 1 + rb % 23
    rc = 1 + (k * 11 + 7) % 23
    while rc in (ra, rb):
        rc = 1 + rc % 23
    mode = k % 6
    if not distinct_only:
        if mode == 1:
            rc = ra
        elif mode == 2:
            rc = rb
        elif k % 41 == 5:
            rc = R31
    return ra, rb, rc


def _h(v):
    return "%x" % v


def op_case(b, name, a, bv, ra, rb, rc, lit=False, rc_init=None):
    """One integer operate instruction; the destination is stored."""
    b.start()                   # the operand loads are the case's
    regs = {}
    if ra != R31:
        b.load(ra, a)
        regs[ra] = a
    else:
        a = 0
    if not lit:
        if rb == R31:
            bv = 0
        elif rb == ra:
            bv = a
        else:
            b.load(rb, bv)
            regs[rb] = bv
    if rc_init is not None and rc not in (ra, rb, R31) and rc not in regs:
        b.load(rc, rc_init)
        regs[rc] = rc_init
    if lit:
        src = "#%d" % bv
    else:
        src = "r%d" % rb
    b.begin("%s r%d,%s,r%d (a=%s b=%s)" % (name, ra, src, rc, _h(a), _h(bv)))
    b.emit(A.ins_op(name, ra, bv if lit else rb, rc, lit))
    if name in aarm.CMOV_TEST:
        if aarm.CMOV_TEST[name](a):
            regs[rc] = bv
    else:
        regs[rc] = aarm.operate(name, a, bv)[0]
    want = 0 if rc == R31 else regs.get(rc, 0)
    b.store(rc, "r%d" % rc, want)


def g_intop(b):
    for name in ARITH:
        for a in E20:
            for bv in E20:
                ra, rb, rc = _regs()
                op_case(b, name, a, bv, ra, rb, rc)
        for a in E10:
            for lit in ARITH_LITS:
                ra, rb, rc = _regs()
                op_case(b, name, a, lit, ra, rb, rc, lit=True)
        # Ra and Rb the same register.
        for a in E10:
            r = 1 + _k[0] % 23
            _k[0] += 1
            op_case(b, name, a, a, r, r, r)
    for name in LOGIC:
        for a in E8:
            for bv in E8:
                ra, rb, rc = _regs()
                op_case(b, name, a, bv, ra, rb, rc)
        for a in E8:
            for lit in (0, 1, 0x80, 0xFF):
                ra, rb, rc = _regs()
                op_case(b, name, a, lit, ra, rb, rc, lit=True)
    for name in SHIFT:
        for a in E8 + [0x8000000000000001, 0x7FFFFFFFFFFFFFFF]:
            for bv in SHIFTS:
                ra, rb, rc = _regs()
                op_case(b, name, a, bv, ra, rb, rc)
            for lit in SHIFT_LITS:
                ra, rb, rc = _regs()
                op_case(b, name, a, lit, ra, rb, rc, lit=True)
    for name in BYTEM:
        for a in (0x0123456789ABCDEF, 0xFFFFFFFFFFFFFFFF, 0x8040201008040201,
                  0xFEDCBA9876543210):
            for bv in BYTEPOS:
                ra, rb, rc = _regs()
                op_case(b, name, a, bv, ra, rb, rc)
            for lit in range(8):
                ra, rb, rc = _regs()
                op_case(b, name, a, lit, ra, rb, rc, lit=True)
    for name in ZAPS:
        for a in (0x0123456789ABCDEF, 0xFFFFFFFFFFFFFFFF, 0x8040201008040201):
            for bv in ZAPMASK:
                ra, rb, rc = _regs()
                op_case(b, name, a, bv, ra, rb, rc)
            for lit in (0, 1, 0x80, 0xFF, 0x5A):
                ra, rb, rc = _regs()
                op_case(b, name, a, lit, ra, rb, rc, lit=True)
    for name in UNARY_OPS:
        for bv in UNARY:
            ra, rb, rc = _regs()
            op_case(b, name, 0, bv, R31, rb, rc)
        if name in UNARY_LIT:
            for lit in (0, 1, 0x7F, 0x80, 0xFF):
                ra, rb, rc = _regs()
                op_case(b, name, 0, lit, R31, rb, rc, lit=True)
    for name in MVI:
        for a in LANES:
            for bv in LANES:
                ra, rb, rc = _regs()
                op_case(b, name, a, bv, ra, rb, rc)
        for a in LANES[:6]:
            for lit in (0, 0x7F, 0x80, 0xFF):
                ra, rb, rc = _regs()
                op_case(b, name, a, lit, ra, rb, rc, lit=True)
    for a in LANES:
        for bv in LANES:
            ra, rb, rc = _regs()
            op_case(b, "PERR", a, bv, ra, rb, rc)
    # R31 as a source: reads as zero.
    for name in ("ADDQ", "SUBQ", "BIS", "ORNOT", "CMPULT", "SLL"):
        for bv in (0, 5, 0xFFFFFFFFFFFFFFFF):
            ra, rb, rc = _regs(distinct_only=True)
            op_case(b, name, 0, bv, R31, rb, rc)
            ra, rb, rc = _regs(distinct_only=True)
            op_case(b, name, bv, 0, ra, R31, rc)


def g_cmov(b):
    conds = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x8000000000000000,
             0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE, 0x100000000]
    for name in aarm.CMOV_TEST:
        for a in conds:
            ra, rb, rc = _regs()
            op_case(b, name, a, 0x1122334455667788, ra, rb, rc,
                    rc_init=0xCAFEBABEDEADBEEF)
            ra, rb, rc = _regs(distinct_only=True)
            op_case(b, name, a, 0x99, ra, rb, rc, lit=True,
                    rc_init=0x0F0F0F0F0F0F0F0F)


VOPS = ["ADDL/V", "SUBL/V", "ADDQ/V", "SUBQ/V", "MULL/V", "MULQ/V"]
VVALS = [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000, 0x40000000,
         0xB504F334, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
         0xFFFFFFFFFFFFFFFF, 0x0123456789ABCDEF, 0xFFFFFFFF80000000]


OVERFLOWS = {"ADDL/V": (0x7FFFFFFF, 1), "SUBL/V": (0x80000000, 1),
             "ADDQ/V": (0x7FFFFFFFFFFFFFFF, 1),
             "SUBQ/V": (0x8000000000000000, 1),
             "MULL/V": (0x10000, 0x10000), "MULQ/V": (1 << 32, 1 << 32)}


def g_trapv(b):
    """ARM 4.4 and 14.3.2.8: the truncated true result is written and, with
    /V, an integer overflow arithmetic trap. HRM 5.2.7: EXC_ADDR is the
    trigger's PC; HRM 5.2.13: EXC_SUM<INT> and <IOV> and REG = Rc."""
    for name in VOPS:
        for a in VVALS:
            for bv in VVALS:
                ra, rb, rc = _regs(distinct_only=(_k[0] % 3 == 0))
                if rc == R31:     # ARM 14.3.2: UNPREDICTABLE whether it traps
                    rc = 1 + (rc + 3) % 23
                regs = {ra: a}
                b.start()
                b.load(ra, a)
                if rb != ra:
                    b.load(rb, bv)
                    regs[rb] = bv
                else:
                    bv = a
                res, ovf = aarm.operate(name, a, bv)
                b.begin("%s r%d,r%d,r%d (a=%s b=%s)" % (name, ra, rb, rc,
                                                        _h(a), _h(bv)))
                b.trap_clear()
                at = b.va()
                b.emit(A.ins_op(name, ra, rb, rc))
                b.trap_check(vector=0x600 if ovf else None, exc_addr=at,
                             exc_sum=0xC0 | (rc << 8))
                b.store(rc, "r%d (truncated result)" % rc, res)
        # HRM 2.5, Table 2-6: an operate with R31 as destination is
        # retired at decode, so the 21264 does not trap (ARM 14.3.2 leaves
        # it UNPREDICTABLE).
        ra = 1 + _k[0] % 20
        _k[0] += 1
        va_, vb_ = OVERFLOWS[name]
        assert aarm.operate(name, va_, vb_)[1]
        b.begin("%s r%d,r%d,r31 (overflows)" % (name, ra, ra + 1))
        b.load(ra, va_)
        b.load(ra + 1, vb_)
        b.trap_clear()
        b.emit(A.ins_op(name, ra, ra + 1, R31))
        b.trap_check()
        b.emit(opr(0x11, 0x20, R31, R31, 2))
        b.emit(opr(0x10, 0x20, R31, R31, 2))
        b.store(2, "r31 read back", 0)
        for a in (0x7FFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x80000000):
            for lit in (1, 0xFF):
                ra, rb, rc = _regs(distinct_only=True)
                b.start()
                b.load(ra, a)
                res, ovf = aarm.operate(name, a, lit)
                b.begin("%s r%d,#%d,r%d (a=%s)" % (name, ra, lit, rc, _h(a)))
                b.trap_clear()
                at = b.va()
                b.emit(A.ins_op(name, ra, lit, rc, lit=True))
                b.trap_check(vector=0x600 if ovf else None, exc_addr=at,
                             exc_sum=0xC0 | (rc << 8))
                b.store(rc, "r%d (truncated result)" % rc, res)


# --- memory --------------------------------------------------------------------

def pattern_q(i):
    """Scratch page 0: byte n holds n (n < 256)."""
    return sum(((8 * i + j) & 0xFF) << (8 * j) for j in range(8))


def init_scratch(b, mem_model, base_reg=R_SCR, quads=40, at=0):
    for i in range(quads):
        v = pattern_q(i)
        b.load(1, v)
        b.emit(mem(OP["STQ"], 1, base_reg, at + 8 * i))
        for j in range(8):
            mem_model[at + 8 * i + j] = (v >> (8 * j)) & 0xFF


def mread(m, off, n):
    return sum(m[off + j] << (8 * j) for j in range(n))


def mwrite(m, off, n, v):
    for j in range(n):
        m[off + j] = (v >> (8 * j)) & 0xFF


def g_mem(b):
    m = {}
    b.begin("scratch init")
    init_scratch(b, m)
    # Loads, positive and negative displacements.
    loads = [("LDQ", 8, False), ("LDL", 4, True), ("LDWU", 2, False),
             ("LDBU", 1, False)]
    b.emit(mem(OP["LDA"], 9, R_SCR, 256))            # r9 = scratch + 256
    for name, size, sext in loads:
        offs = [0, size, 8 * size, 128, 256 - size]
        for off in offs:
            for base, disp in ((R_SCR, off), (9, off - 256)):
                rd = 1 + _k[0] % 8
                _k[0] += 1
                b.begin("%s r%d,%d(r%d)" % (name, rd, disp, base))
                b.emit(mem(OP[name], rd, base, disp))
                v = mread(m, off, size)
                b.store(rd, "r%d" % rd, aarm.SEXT(v, 8 * size) if sext else v)
    # LDQ_U: the low three address bits are ignored (ARM 4.2.3).
    for off in range(1, 16):
        b.begin("LDQ_U r3,%d(r26)" % off)
        b.emit(mem(OP["LDQ_U"], 3, R_SCR, off))
        b.store(3, "r3", mread(m, off & ~7, 8))
    b.begin("LDQ_U r3,-1(r9)")
    b.emit(mem(OP["LDQ_U"], 3, 9, -1))
    b.store(3, "r3", mread(m, 248, 8))
    # LDA/LDAH: the displacement is sign-extended (ARM 4.2.1, 4.2.2).
    for basev in (0, 0x123456789, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFF0000):
        for disp in (0, 1, -1, 0x7FFF, -0x8000, 0x1234):
            b.begin("LDA r4,%d(r5) (r5=%s)" % (disp, _h(basev)))
            b.load(5, basev)
            b.emit(mem(OP["LDA"], 4, 5, disp))
            b.store(4, "r4", (basev + disp) & M64)
            b.begin("LDAH r4,%d(r5) (r5=%s)" % (disp, _h(basev)))
            b.emit(mem(OP["LDAH"], 4, 5, disp))
            b.store(4, "r4", (basev + disp * 65536) & M64)
    for disp in (-0x8000, 0x7FFF):
        b.begin("LDAH r4,%d(r31)" % disp)
        b.emit(mem(OP["LDAH"], 4, R31, disp))
        b.store(4, "r4", (disp * 65536) & M64)
    # Stores: only the addressed bytes change.
    stores = [("STQ", 8), ("STL", 4), ("STW", 2), ("STB", 1)]
    val = 0xF1E2D3C4B5A69788
    for name, size in stores:
        for off in (0x100 + 0, 0x108 + size, 0x118 - size):
            b.begin("%s r6,%d(r26)" % (name, off))
            b.load(6, val)
            b.emit(mem(OP[name], 6, R_SCR, off))
            mwrite(m, off, size, val)
            q = off & ~7
            b.emit(mem(OP["LDQ"], 7, R_SCR, q))
            b.store(7, "quadword at %d" % q, mread(m, q, 8))
            b.emit(mem(OP["LDQ"], 7, R_SCR, q + 8))
            b.store(7, "quadword at %d" % (q + 8), mread(m, q + 8, 8))
    # STQ_U: stores the aligned quadword (ARM 4.2.7).
    for off in (0x125, 0x12F):
        b.begin("STQ_U r6,%d(r26)" % off)
        b.load(6, 0x0A0B0C0D0E0F1011)
        b.emit(mem(OP["STQ_U"], 6, R_SCR, off))
        mwrite(m, off & ~7, 8, 0x0A0B0C0D0E0F1011)
        b.emit(mem(OP["LDQ"], 7, R_SCR, off & ~7))
        b.store(7, "quadword at %d" % (off & ~7), mread(m, off & ~7, 8))
    # Store then load forwarding, the same register.
    b.begin("STQ r8 / LDQ r8")
    b.load(8, 0x5EED5EED5EED5EED)
    b.emit(mem(OP["STQ"], 8, R_SCR, 0x140))
    b.emit(mem(OP["LDQ"], 8, R_SCR, 0x140))
    b.store(8, "r8", 0x5EED5EED5EED5EED)
    # A load into its own base register.
    b.begin("LDQ r10,0(r10)")
    b.emit(mem(OP["LDA"], 10, R_SCR, 0x40))
    b.emit(mem(OP["LDQ"], 10, 10, 0))
    b.store(10, "r10", mread(m, 0x40, 8))
    # A load to R31 on a mapped page takes no trap (HRM 2.5, 2.6); compiled
    # code answers it from its page cache.
    for op in ("LDL", "LDQ", "LDBU", "LDWU", "LDQ_U"):
        b.begin("%s r31,8(r26) on a mapped address" % op)
        b.trap_clear()
        b.emit(mem(OP[op], R31, R_SCR, 8))
        b.trap_check()
    # Prefetches and hints take no exception, even unmapped (ARM 4.2.5 notes,
    # 4.11.3, 4.11.5, 4.11.8, 4.11.11): VA 0x10000000 has no translation.
    b.load(11, 0x10000000)
    # HRM 2.6: a load to R31 is a prefetch, and "load operations to R31 and
    # F31 may generate exceptions. These exceptions must be dismissed by
    # PALcode": the 21264 takes the DTB miss, and the PALcode hides it.
    for nm, op in (("LDL r31 (PREFETCH)", "LDL"),
                   ("LDQ r31 (PREFETCH_EN)", "LDQ"),
                   ("LDBU r31 (prefetch)", "LDBU"),
                   ("LDWU r31 (prefetch)", "LDWU")):
        b.begin("%s on an unmapped address" % nm)
        b.trap_clear()
        at = b.va()
        b.emit(mem(OP[op], R31, 11, 0))
        b.trap_check(vector=0x300, exc_addr=at, exc_sum=31 << 8,
                     exc_sum_mask=0x1F00, va=0x10000000)
    # HRM 2.5, Table 2-6: LDQ_U to R31 (UNOP) is retired without execution.
    b.begin("LDQ_U r31 (UNOP) on an unmapped address")
    b.trap_clear()
    b.emit(mem(OP["LDQ_U"], R31, 11, 0))
    b.trap_check()
    for nm, word in (("FETCH", mfc(A.MISC["FETCH"], R31, 11)),
                     ("FETCH_M", mfc(A.MISC["FETCH_M"], R31, 11)),
                     ("ECB", mfc(A.MISC["ECB"], R31, 11)),
                     ("WH64", mfc(A.MISC["WH64"], R31, 11)),
                     ("WH64EN", mfc(A.MISC["WH64EN"], R31, 11))):
        b.begin("%s on an unmapped address" % nm)
        b.trap_clear()
        b.emit(word)
        b.trap_check()


MT_FPCR_REF = "cpu-fidelity: MT_FPCR does not take its synchronous trap"
UNALIGNED_REF = "cpu-fidelity: unaligned data accesses do not trap"


def g_unaligned(b):
    """A misaligned LDx/STx takes the UNALIGN fault (ARM 4.2, HRM Table 6-8:
    PAL_BASE+0x280; EXC_ADDR the instruction, VA the address, EXC_SUM REG =
    Ra). Alphabox performs the access instead unless it crosses a page:
    docs/cpu-fidelity.md, the first known divergence. The value it then
    loads or stores is checked as that divergence's value."""
    m = {}
    b.begin("scratch init")
    init_scratch(b, m)
    scr = b.scratch
    sentinel = 0x5151515151515151
    for name, size, sext, off in (("LDQ", 8, False, 3), ("LDQ", 8, False, 4),
                                  ("LDL", 4, True, 0x81), ("LDL", 4, True, 2),
                                  ("LDWU", 2, False, 0x7F), ("LDWU", 2, False, 1)):
        b.begin("%s r2,%d(r26) (misaligned)" % (name, off))
        b.load(2, sentinel)
        b.trap_clear()
        at = b.va()
        b.emit(mem(OP[name], 2, R_SCR, off))
        b.trap_check(vector=0x280, exc_addr=at, exc_sum=2 << 8,
                     exc_sum_mask=0x1F00, va=scr + off,
                     known=(0, UNALIGNED_REF))
        v = mread(m, off, size)
        b.store(2, "r2 (not written: the fault skips it)", sentinel,
                known=(aarm.SEXT(v, 8 * size) if sext else v, UNALIGNED_REF))
    for name, size, off in (("STQ", 8, 0x101), ("STL", 4, 0x10A),
                            ("STW", 2, 0x113)):
        b.begin("%s r3,%d(r26) (misaligned)" % (name, off))
        b.load(3, 0xA1A2A3A4A5A6A7A8)
        b.trap_clear()
        at = b.va()
        b.emit(mem(OP[name], 3, R_SCR, off))
        b.trap_check(vector=0x280, exc_addr=at, exc_sum=3 << 8,
                     exc_sum_mask=0x1F00, va=scr + off,
                     known=(0, UNALIGNED_REF))
        q = off & ~7
        before = [mread(m, q, 8), mread(m, q + 8, 8)]
        mwrite(m, off, size, 0xA1A2A3A4A5A6A7A8)
        after = [mread(m, q, 8), mread(m, q + 8, 8)]
        for i in range(2):
            b.emit(mem(OP["LDQ"], 4, R_SCR, q + 8 * i))
            b.store(4, "quadword at %d (unchanged)" % (q + 8 * i), before[i],
                    known=(after[i], UNALIGNED_REF))
        # put the bytes back for the next pass's first case
        for i in range(2):
            b.load(4, before[i])
            b.emit(mem(OP["STQ"], 4, R_SCR, q + 8 * i))
        mwrite(m, q, 8, before[0])
        mwrite(m, q + 8, 8, before[1])
    # Crossing into the next page: Alphabox traps here, as the 21264 does.
    b.start()
    b.emit(mem(OP["LDA"], 9, R_SCR, 0x2000))         # r9 = page 1
    b.begin("LDQ r2,-4(r9) (misaligned, crosses a page)")
    b.load(2, sentinel)
    b.trap_clear()
    at = b.va()
    b.emit(mem(OP["LDQ"], 2, 9, -4))
    b.trap_check(vector=0x280, exc_addr=at, exc_sum=2 << 8,
                 exc_sum_mask=0x1F00, va=scr + 0x2000 - 4)
    b.store(2, "r2 (not written)", sentinel)


def g_llsc(b):
    """ARM 4.2.4-5: STx_C stores and returns 1 if lock_flag is still set
    and the address is in the LDx_L's 16-byte block; it always clears
    lock_flag, so a second STx_C fails. HRM D.19 (Restriction 23): on the
    21264 a STx_C fails if an LDL/Q/F/G/S/T, STL/Q/F/G/S/T, LDQ_U (not to
    R31) or STQ_U ran between it and its LDx_L. A misaligned LDx_L/STx_C
    takes UNALIGN."""
    b.emit(mem(OP["LDA"], 9, R_SCR, 0x2000))         # r9 = page 1
    scr1 = b.scratch + 0x2000
    v0 = 0x1000000000000007

    def setq(off, v):
        b.load(1, v)
        b.emit(mem(OP["STQ"], 1, 9, off))

    b.begin("LDQ_L/STQ_C, nothing between")
    setq(0, v0)
    b.emit(mem(OP["LDQ_L"], 2, 9, 0))
    b.emit(opl(0x10, 0x20, 2, 1, 3))                 # r3 = r2 + 1
    b.emit(mem(OP["STQ_C"], 3, 9, 0))
    b.store(2, "LDQ_L value", v0)
    b.store(3, "STQ_C result", 1)
    b.emit(mem(OP["LDQ"], 4, 9, 0))
    b.store(4, "memory", v0 + 1)
    b.begin("STQ_C again, no LDx_L")
    b.load(5, 0x77)
    b.emit(mem(OP["STQ_C"], 5, 9, 0))
    b.store(5, "STQ_C result", 0)
    b.emit(mem(OP["LDQ"], 4, 9, 0))
    b.store(4, "memory (unchanged)", v0 + 1)

    # The operands are loaded before the LDx_L: b.load is an LDQ from the
    # literal pool, which between LDx_L and STx_C fails the STx_C (HRM D.19).
    b.begin("LDL_L/STL_C sign extension and longword store")
    setq(8, 0x1122334480000001)
    b.load(3, 0xDEADBEEF0BADF00D)
    b.emit(mem(OP["LDL_L"], 2, 9, 8))
    b.emit(mem(OP["STL_C"], 3, 9, 8))
    b.store(2, "LDL_L value", 0xFFFFFFFF80000001)
    b.store(3, "STL_C result", 1)
    b.emit(mem(OP["LDQ"], 4, 9, 8))
    b.store(4, "memory", 0x112233440BADF00D)

    b.begin("STQ_C to the other quadword of the LDQ_L's 16-byte block")
    setq(16, 0xAA)
    setq(24, 0xBB)
    b.load(3, 0xCC)
    b.emit(mem(OP["LDQ_L"], 2, 9, 16))
    b.emit(mem(OP["STQ_C"], 3, 9, 24))
    b.store(3, "STQ_C result", 1)
    b.emit(mem(OP["LDQ"], 4, 9, 24))
    b.store(4, "memory", 0xCC)

    for what, word in (("STQ", mem(OP["STQ"], 6, 9, 128)),
                       ("LDQ", mem(OP["LDQ"], 6, 9, 128)),
                       ("LDQ_U", mem(OP["LDQ_U"], 6, 9, 131)),
                       ("STQ_U", mem(OP["STQ_U"], 6, 9, 131))):
        b.begin("LDQ_L, then %s elsewhere, then STQ_C" % what)
        setq(32, 0x10)
        b.load(6, 0x66)
        b.emit(mem(OP["LDQ_L"], 2, 9, 32))
        b.emit(word)
        b.emit(opl(0x10, 0x20, 2, 1, 3))
        b.emit(mem(OP["STQ_C"], 3, 9, 32))
        b.store(3, "STQ_C result", 0)
        b.emit(mem(OP["LDQ"], 4, 9, 32))
        b.store(4, "memory", 0x10)
    # The list includes the FP loads and stores, and LDL/LDQ whatever their
    # destination (a load to R31 is a prefetch, HRM 2.6); LDQ_U to R31 is
    # retired without execution (HRM 2.5) and is excluded by name.
    for what, word, fails in (
            ("LDT", mem(OP["LDT"], 6, 9, 128), True),
            ("STT", mem(OP["STT"], 6, 9, 136), True),
            ("LDL r31 (prefetch)", mem(OP["LDL"], R31, 9, 128), True),
            ("LDQ_U r31 (UNOP)", mem(OP["LDQ_U"], R31, 9, 128), False)):
        b.begin("LDQ_L, then %s, then STQ_C" % what)
        setq(32, 0x10)
        b.emit(mem(OP["LDQ_L"], 2, 9, 32))
        b.emit(word)
        b.emit(opl(0x10, 0x20, 2, 1, 3))
        b.emit(mem(OP["STQ_C"], 3, 9, 32))
        b.store(3, "STQ_C result", 0 if fails else 1)
        b.emit(mem(OP["LDQ"], 4, 9, 32))
        b.store(4, "memory", 0x10 if fails else 0x11)
    # Across a branch: the operation after the LDx_L is in another block of
    # compiled code (a taken branch alone "may" fail it too, ARM 4.2.4). An
    # operation before the LDx_L does not count.
    b.begin("LDQ_L, BR, LDQ, STQ_C")
    setq(32, 0x10)
    b.emit(mem(OP["LDQ_L"], 2, 9, 32))
    b.emit(bra(OP["BR"], R31, 0))
    b.emit(mem(OP["LDQ"], 6, 9, 128))
    b.emit(opl(0x10, 0x20, 2, 1, 3))
    b.emit(mem(OP["STQ_C"], 3, 9, 32))
    b.store(3, "STQ_C result", 0)
    b.begin("LDQ, LDQ_L, STQ_C")
    setq(32, 0x10)
    b.emit(mem(OP["LDQ"], 6, 9, 128))
    b.emit(mem(OP["LDQ_L"], 2, 9, 32))
    b.emit(opl(0x10, 0x20, 2, 1, 3))
    b.emit(mem(OP["STQ_C"], 3, 9, 32))
    b.store(3, "STQ_C result", 1)
    b.emit(mem(OP["LDQ"], 4, 9, 32))
    b.store(4, "memory", 0x11)

    for name, off in (("LDQ_L", 4), ("LDL_L", 2), ("STQ_C", 4), ("STL_C", 1)):
        b.begin("%s r2,%d(r9) (misaligned)" % (name, off))
        b.load(2, 0x42)
        b.trap_clear()
        at = b.va()
        b.emit(mem(OP[name], 2, 9, off))
        b.trap_check(vector=0x280, exc_addr=at, exc_sum=2 << 8,
                     exc_sum_mask=0x1F00, va=scr1 + off)


# --- control -------------------------------------------------------------------

def g_branch(b):
    a = b.asm
    vals = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x8000000000000000,
            0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE, 0x100000000,
            0x0000000100000000 | 1]
    for name, test in aarm.BRANCH_TEST.items():
        for v in vals:
            ra = 1 + _k[0] % 20
            _k[0] += 1
            r2 = 21 if ra != 21 else 22
            b.begin("%s r%d forward (a=%s)" % (name, ra, _h(v)))
            b.load(ra, v)
            b.emit(opr(0x11, 0x20, R31, R31, r2))
            b.emit(bra(OP[name], ra, 1))
            b.emit(mem(OP["LDA"], r2, R31, 1))
            b.store(r2, "1 = fell through", 0 if test(v) else 1)
            b.begin("%s r%d backward (a=%s)" % (name, ra, _h(v)))
            b.emit(opr(0x11, 0x20, R31, R31, r2))
            b.emit(bra(OP["BR"], R31, 2))             # to the test
            b.emit(mem(OP["LDA"], r2, R31, 5))        # back: taken
            b.emit(bra(OP["BR"], R31, 2))             # to out
            b.emit(bra(OP[name], ra, -3))             # test
            b.emit(mem(OP["LDA"], r2, R31, 9))        # fell through
            b.store(r2, "5 = taken, 9 = fell through", 5 if test(v) else 9)
    # BR/BSR: Ra <- the updated PC (ARM 4.3.2).
    for name in ("BR", "BSR"):
        for ra in (3, 23, R31):
            b.begin("%s r%d,+1" % (name, ra))
            b.emit(opr(0x11, 0x20, R31, R31, 4))
            at = b.va()
            b.emit(bra(OP[name], ra, 1))
            b.emit(mem(OP["LDA"], 4, R31, 1))        # skipped
            b.store(ra, "r%d (link)" % ra, 0 if ra == R31 else at + 4)
            b.store(4, "skipped instruction", 0)
    # Jumps: Ra <- the updated PC, PC <- Rbv AND NOT 3; the hint is only a
    # hint; Ra = Rb uses the old value (ARM 4.3.3).
    for kind in range(4):
        for hint in (0, 0x3FFF, 0x1234):
            for low in (0, 3):
                for ra, rb in ((6, 5), (5, 5), (R31, 5), (23, 7)):
                    b.begin("JMP-type %d r%d,(r%d),%x, target+%d" % (kind, ra, rb, hint, low))
                    b.emit(opr(0x11, 0x20, R31, R31, 8))
                    b.emit(bra(OP["BR"], rb, 0))         # rb = address of next
                    base = b.va()
                    b.emit(mem(OP["LDA"], rb, rb, 16 + low))
                    at = b.va()
                    b.emit(jmp(kind, ra, rb, hint))
                    b.emit(mem(OP["LDA"], 8, R31, 1))
                    b.emit(mem(OP["LDA"], 8, R31, 2))
                    assert b.va() == base + 16
                    b.store(8, "skipped instructions", 0)
                    if ra != R31:
                        b.store(ra, "r%d (link)" % ra, at + 4)
                    if rb != ra:
                        b.store(rb, "r%d (target)" % rb, base + 16 + low)
    # Floating branches: sign and the rest of the register (ARM 4.9).
    fvals = [0, 0x8000000000000000, 1, 0x8000000000000001,
             0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
             0xFFF8000000000000, 0x0000000000000400]
    for name, test in aarm.FBRANCH_TEST.items():
        for v in fvals:
            b.begin("%s f3 (f3=%s)" % (name, _h(v)))
            b.load(1, v)
            b.emit(fop(*A.ITOF["ITOFT"], 1, R31, 3))
            b.emit(opr(0x11, 0x20, R31, R31, 2))
            b.emit(bra(OP[name], 3, 1))
            b.emit(mem(OP["LDA"], 2, R31, 1))
            b.store(2, "1 = fell through", 0 if test(v) else 1)


def g_fix(b):
    """ARM 4.10.18-19. The FIX moves do not interpret the bits."""
    vals32 = [0, 1, 0x7F800000, 0xFF800000, 0x7FC00001, 0x00000001,
              0x80000000, 0x3F800000, 0x00800000, 0x807FFFFF, 0x12345678,
              0xFFFFFFFF, 0x40490FDB, 0x7F7FFFFF]
    hi = 0xABCD000000000000
    for name, model in (("ITOFS", aarm.ITOFS), ("ITOFF", aarm.ITOFF)):
        for v in vals32:
            b.begin("%s r1,f4; FTOIT f4,r2 (r1=%s)" % (name, _h(v | hi)))
            b.load(1, v | hi)                       # high half ignored
            b.emit(fop(*A.ITOF[name], 1, R31, 4))
            b.emit(fop(*A.FTOI["FTOIT"], 4, R31, 2))
            b.store(2, "f4 image", model(v | hi))
            if name == "ITOFS":
                b.begin("ITOFS then FTOIS (r1=%s)" % _h(v))
                b.emit(fop(*A.FTOI["FTOIS"], 4, R31, 3))
                b.store(3, "r3", aarm.SEXT(v, 32))
    for v in E20:
        b.begin("ITOFT/FTOIT/FTOIS (r1=%s)" % _h(v))
        b.load(1, v)
        b.emit(fop(*A.ITOF["ITOFT"], 1, R31, 7))
        b.emit(fop(*A.FTOI["FTOIT"], 7, R31, 2))
        b.emit(fop(*A.FTOI["FTOIS"], 7, R31, 3))
        b.store(2, "FTOIT", aarm.FTOIT(aarm.ITOFT(v)))
        b.store(3, "FTOIS", aarm.FTOIS(aarm.ITOFT(v)))
    # Floating point disabled: FEN for the moves (HRM Table 6-8), none for
    # FNOP, "these generate no exceptions" (cpu-fidelity: FNOP).
    b.begin("FPE off (CALL_PAL 01 service)")
    b.emit(opr(0x11, 0x20, R31, R31, 16))
    b.emit(call_pal(0x01))
    for nm, word in (("ITOFT r1,f5", fop(*A.ITOF["ITOFT"], 1, R31, 5)),
                     ("FTOIT f5,r2", fop(*A.FTOI["FTOIT"], 5, R31, 2))):
        b.begin("%s with FPE clear" % nm)
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=0x200, exc_addr=at)
    b.begin("FNOP with FPE clear")
    b.trap_clear()
    b.emit(fop(*A.CPYS, R31, R31, R31))
    b.trap_check()
    b.emit(mem(OP["LDA"], 16, R31, 1))
    b.emit(call_pal(0x01))
    # HRM 6.7.3: MT_FPCR writes the first latch and then "invokes a
    # synchronous trap to the MT_FPCR PALcode entry point" (0x700).
    b.begin("MT_FPCR f31")
    b.trap_clear()
    at = b.va()
    b.emit(fop(0x17, 0x024, R31, R31, R31))
    b.trap_check(vector=0x700, exc_addr=at, known=(0, MT_FPCR_REF))


def g_misc(b):
    b.begin("AMASK #-1 and #0xff")
    b.load(1, M64)
    b.emit(A.ins_op("AMASK", R31, 1, 2))
    b.store(2, "AMASK -1", aarm.AMASK(0, M64, b.row.amask)[0])
    b.emit(A.ins_op("AMASK", R31, 0xFF, 3, lit=True))
    b.store(3, "AMASK #0xff", aarm.AMASK(0, 0xFF, b.row.amask)[0])
    # Each bit on its own: cleared where the row has the feature [ARM
    # 4.11.1, D-4]. The EV8's bits 3 (Tarantula), 10 (SMT) and 11 (its SIMD
    # and paired-single sets) stay set until the stage that implements them
    # (docs/platforms/ev8-isa.md, section 2), on every row today.
    b.begin("AMASK of each feature bit")
    for bit in (0, 1, 2, 3, 8, 9, 10, 11, 12):
        b.load(5, 1 << bit)
        b.emit(A.ins_op("AMASK", R31, 5, 6))
        b.store(6, "AMASK bit %d" % bit, aarm.AMASK(0, 1 << bit, b.row.amask)[0])
    b.begin("IMPLVER")
    b.emit(A.ins_op("IMPLVER", R31, 1, 4, lit=True))
    b.store(4, "IMPLVER", b.row.implver)
    # RPCC returns all of CC: the offset written to CC<63:32> (HRM 5.1.1).
    b.begin("RPCC")
    b.emit(mfc(A.MISC["RPCC"], 5, R31))
    b.emit(opl(0x12, 0x34, 5, 32, 6))               # SRL r5,#32,r6
    b.store(6, "RPCC<63:32>", 0x12345678)
    # Barriers and hints execute, and none traps (ARM 4.11).
    for nm in ("TRAPB", "EXCB", "MB", "WMB"):
        b.begin(nm)
        b.trap_clear()
        b.emit(mfc(A.MISC[nm]))
        b.trap_check()
    # ARM C.13: the reserved 18.4800 and 18.4C00 must act as MB.
    for code in (0x4800, 0x4C00):
        b.begin("18.%04X (reserved barrier)" % code)
        b.trap_clear()
        b.emit(mfc(code))
        b.trap_check()
    # An opcode 18 function with no instruction is a NOP on the 21264
    # (21464 IDS p.676; docs/cpu-fidelity.md): no trap, Ra and the memory
    # at Rb untouched, the next instruction runs. A row that traps them, or
    # has an instruction at one of these codes, skips it (rows.py).
    # 18.B000, 18.B400 and 18.B800 are the 21464's LDL_ARM, LDQ_ARM and
    # QUIESCE and 18.EC00 its CCB (docs/platforms/ev8-isa.md 3.1): unused
    # codes on every other row, and on the EV8 row the first three are too
    # until plan S3 implements them, with AMASK bit 10 clear (an Alphabox
    # decision, docs/cpu-fidelity.md).
    b.begin("unused 18.xxxx functions are NOPs")
    b.load(21, 0x0123456789ABCDEF)
    b.load(22, 0x5A5A5A5A12345678)
    for code in (0x0001, 0x0200, 0x0800, 0x1000, 0x2000, 0x3C00, 0x5000,
                 0x6000, 0x7C00, 0x9000, 0xB000, 0xB400, 0xB800, 0xD000,
                 0xE400, 0xEC00, 0xF400, 0xFE00, 0xFFFF):
        with b.part("unused 18.%04X is a NOP" % code):
            b.need(rows.UNUSED_MISC_NOP)
            b.need(rows.Unused(mfc(code, 21, 22)))
            b.trap_clear()
            b.emit(mfc(code, 21, 22))
            b.trap_check()
            b.emit(mem(OP["STQ"], 22, R_SCR, 0x200))    # next instruction runs
            b.emit(mem(OP["LDQ"], 23, R_SCR, 0x200))
            b.store(23, "18.%04X: next instruction ran" % code,
                    0x5A5A5A5A12345678)
            b.store(21, "18.%04X: Ra unchanged" % code, 0x0123456789ABCDEF)
            b.store(22, "18.%04X: Rb unchanged" % code, 0x5A5A5A5A12345678)
    # RC/RS: Ra <- intr_flag, then clear/set it (ARM 4.12).
    b.begin("RS, RS, RC, RC")
    b.emit(mfc(A.MISC["RS"], 7, R31))
    b.emit(mfc(A.MISC["RS"], 8, R31))
    b.emit(mfc(A.MISC["RC"], 9, R31))
    b.emit(mfc(A.MISC["RC"], 10, R31))
    b.store(8, "second RS", 1)
    b.store(9, "first RC", 1)
    b.store(10, "second RC", 0)
    # OPCDEC (HRM Table 6-8): opcodes 01-07; the PALRES opcodes outside
    # PALmode with I_CTL[HWE] clear; unimplemented functions of 11, 14, 17
    # and 1C (11.68 and 17.068 are the 21464's CMOV2 and FCMOV2, so a row
    # that has them skips those two: group ev8); CALL_PAL 40-7F, above BF.
    b.emit(bra(OP["BR"], 12, 0))                    # r12 = next address
    illegal = [("opcode %02x" % op, op << 26) for op in range(1, 8)]
    illegal += [("HW_MFPR in kernel mode", A.hw_mfpr(13, A.IPR["EXC_ADDR"])),
                ("HW_MTPR in kernel mode", A.hw_mtpr(R31, 0x2C)),
                # virtual forms on mapped addresses: harmless if executed
                ("HW_LD in kernel mode", A.hw_ld(13, R_TR, 0, typ=4)),
                ("HW_ST in kernel mode", A.hw_st(R31, R_SCR, 0x100, typ=2)),
                ("1C.002", opr(0x1C, 0x02, R31, 1, 13)),
                ("1C.7F", opr(0x1C, 0x7F, R31, 1, 13)),
                ("11.68", opr(0x11, 0x68, 1, 2, 13)),
                ("17.068", fop(0x17, 0x068, 1, 2, 13)),
                ("14.001", fop(0x14, 0x001, 1, R31, 13)),
                ("14.7FF", fop(0x14, 0x7FF, 1, R31, 13)),
                ("CALL_PAL 40", call_pal(0x40)),
                ("CALL_PAL 7F", call_pal(0x7F)),
                ("CALL_PAL C0", call_pal(0xC0)),
                ("CALL_PAL 100", call_pal(0x100))]
    for nm, word in illegal:
        b.begin("%s raises OPCDEC" % nm)
        b.need(rows.Unused(word))
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=0x400, exc_addr=at)
    # HW_RET in kernel mode: the target is the next instruction, so the run
    # carries on even if it is (wrongly) executed.
    b.begin("HW_RET in kernel mode raises OPCDEC")
    b.trap_clear()
    b.emit(bra(OP["BR"], 13, 0))
    at = b.va()
    b.emit(mem(OP["LDA"], 13, 13, 8))
    at = b.va()
    b.emit(A.hw_ret(13))
    b.trap_check(vector=0x400, exc_addr=at)
    # CALL_PAL (HRM 6.8.1): the linkage register gets the PC of the next
    # instruction, bit 0 clear from native mode; CALL_PAL is not an
    # exception, so EXC_ADDR keeps the last exception's PC (HRM 5.2.7).
    b.begin("CALL_PAL 83: linkage and EXC_ADDR")
    b.need(rows.Unused(0x01 << 26))
    b.trap_clear()
    last = b.va()
    b.emit(0x01 << 26)                              # OPCDEC: EXC_ADDR = here
    at = b.va()
    b.emit(call_pal(0x83))
    b.store(0, "linkage (R23)", at + 4)
    b.store(1, "EXC_ADDR inside CALL_PAL", last)
    b.emit(mem(OP["LDQ"], 0, R_TR, TR_COUNT))
    b.store(0, image.TRAP_COUNT, 1)
    b.expected_traps += 1
    # Every other CALL_PAL entry counts itself; one privileged, one not.
    for func in (0x09, 0xAA, 0xBF):
        b.begin("CALL_PAL %02x enters PAL_BASE+%x" % (
            func, 0x2000 + ((func & 0x80) << 5) + ((func & 0x3F) << 6)))
        b.trap_clear()
        at = b.va()
        b.emit(call_pal(func))
        b.trap_check(vector=0x2000 + ((func & 0x80) << 5) + ((func & 0x3F) << 6),
                     exc_addr=at + 4)


def g_mode(b):
    """User mode, entered through CALL_PAL 02 (which installs one ITB and
    three DTB translations) and left through CALL_PAL 84. HRM 6.8.1:
    a privileged CALL_PAL outside kernel mode is OPCDEC; Table 6-8: so is
    a PALRES opcode; 5.3.9: "Non-kernel mode references to superpages
    result in access violations"; 5.3.8 MM_STAT: OPCODE<9:4>, FOW<3>,
    FOR<2>, ACV<1>, WR<0>; DFAULT at PAL_BASE+0x380."""
    exit_ = A.call_pal(0x84)
    page2 = b.scratch + 0x4000
    ud, uk, uf = image.USER_DATA_VA, image.USER_KONLY_VA, image.USER_FAULT_VA

    def enter(snippet_va):
        b.li(16, snippet_va)
        b.emit(bra(OP["BR"], 17, 0))            # r17 = here
        b.emit(mem(OP["LDA"], 17, 17, 8))       # r17 = after the CALL_PAL
        b.emit(call_pal(0x02))

    # A store and a load through the DTB, in user mode. (A case places its
    # user-mode code after begin(), so that the code is the case's.)
    b.begin("user mode: STQ/LDQ/STL/LDBU through the DTB")
    snip = b.user_snippet([
        mem(OP["STQ"], 1, 9, 0x10), mem(OP["LDQ"], 2, 9, 0x10),
        mem(OP["STL"], 1, 9, 0x18), mem(OP["LDBU"], 3, 9, 0x19), exit_])
    b.li(9, ud)
    b.load(1, 0x0102030405060708)
    b.trap_clear()
    enter(snip)
    b.trap_check()
    b.store(2, "r2 (LDQ)", 0x0102030405060708)
    b.store(3, "r3 (LDBU byte 1 of the STL)", 0x07)
    b.emit(mem(OP["LDQ"], 4, R_SCR, 0x4010))
    b.store(4, "memory, through KSEG", 0x0102030405060708)
    # RPCC, AMASK and the unprivileged CALL_PAL linkage in user mode.
    b.begin("user mode: RPCC, AMASK, CALL_PAL 83")
    snip = b.user_snippet([mfc(A.MISC["RPCC"], 5, R31),
                           A.ins_op("AMASK", R31, 0xFF, 6, lit=True),
                           call_pal(0x83), exit_])
    enter(snip)
    b.emit(opl(0x12, 0x34, 5, 32, 5))
    b.store(5, "RPCC<63:32>", 0x12345678)
    b.store(6, "AMASK #0xff", aarm.AMASK(0, 0xFF, b.row.amask)[0])
    b.store(0, "linkage", snip + 12)
    # Traps from user mode: one instruction, then back.
    cases = [
        ("privileged CALL_PAL 01", call_pal(0x01), 0x400, None, None, None),
        ("HW_MFPR", A.hw_mfpr(13, A.IPR["EXC_ADDR"]), 0x400, None, None, None),
        ("HW_MTPR", A.hw_mtpr(R31, 0x2C), 0x400, None, None, None),
        ("LDQ from a superpage", mem(OP["LDQ"], 2, 10, 0), 0x380,
         page2, (0x29 << 4) | 2, 2),
        ("STQ to a superpage", mem(OP["STQ"], 2, 10, 8), 0x380,
         page2 + 8, (0x2D << 4) | 2 | 1, 2),
        ("LDQ from a kernel-only page", mem(OP["LDQ"], 3, 11, 0), 0x380,
         uk, (0x29 << 4) | 2, 3),
        ("STL to a kernel-only page", mem(OP["STL"], 3, 11, 4), 0x380,
         uk + 4, (0x2C << 4) | 2 | 1, 3),
        ("LDL from a FOR page", mem(OP["LDL"], 4, 12, 0), 0x380,
         uf, (0x28 << 4) | 4, 4),
        ("STQ to a FOW page", mem(OP["STQ"], 4, 12, 8), 0x380,
         uf + 8, (0x2D << 4) | 8 | 1, 4),
        ("LDBU from a FOR page", mem(OP["LDBU"], 5, 12, 3), 0x380,
         uf + 3, (0x0A << 4) | 4, 5),
    ]
    for nm, word, vec, va, mmstat, reg in cases:
        b.begin("user mode: %s" % nm)
        snip = b.user_snippet([word, exit_])
        b.load(10, page2)
        b.li(11, uk)
        b.li(12, uf)
        b.trap_clear()
        enter(snip)
        b.trap_check(vector=vec, exc_addr=snip,
                     exc_sum=None if reg is None else reg << 8,
                     exc_sum_mask=0x1F00, va=va)
        if mmstat is not None:
            b.emit(mem(OP["LDQ"], 0, R_TR, image.TR_MM_STAT))
            b.store(0, "MM_STAT", mmstat, mask=0x7FF)
    # Kernel mode may use the kernel-only page.
    b.begin("kernel mode: LDQ from the kernel-only page")
    b.li(11, uk)
    b.trap_clear()
    b.emit(mem(OP["LDQ"], 3, 11, 0x10))
    b.trap_check()
    b.store(3, "r3", 0x0102030405060708)


# The example words of docs/platforms/ev8-isa.md (3.1, 3.2), which the nada
# toolchain assembles: the encodings here must be the same.
EV8_WORDS = [
    (mfc(A.MISC["LDQ_ARM"], 1, 2), 0x6022B400),
    (mfc(A.MISC["LDL_ARM"], 1, 2), 0x6022B000),
    (mfc(A.MISC["QUIESCE"]), 0x63FFB800),
    (mfc(A.MISC["CCB"], R31, 3), 0x63E3EC00),
    (mfc(A.MISC["WH64EN"], R31, 3), 0x63E3FC00),
    (A.ins_op("CMOV2", 1, 2, 1), 0x44220D01),
    (fop(*A.FCMOV2, 1, 2, 1), 0x5C220D01),
    (A.MAP_ALIGN, 0x47FF081F),
]
EV8_VALS = [0, 0xFFFFFFFFFFFFFFFF, 0x0123456789ABCDEF, 0x8000000000000000]


def _next_runs(b, reg=23):
    """After the instruction under test: the next one runs."""
    b.emit(mem(OP["LDA"], reg, R31, 0x55))
    b.store(reg, "next instruction ran", 0x55)


def _ev8_cmov2(b, ra, rb, rc, a, bv, rc_init, lit=False):
    """CMOV2 Ra, Rb|#lit, Rc: Rc <- Rav, every other register unchanged."""
    word = A.ins_op("CMOV2", ra, bv if lit else rb, rc, lit)
    b.start()
    vals = {}
    for r, v in ((ra, a), (None if lit else rb, bv), (rc, rc_init)):
        if r is not None and r != R31 and r not in vals:
            b.load(r, v)
            vals[r] = v
    b.begin("CMOV2 r%d,%s,r%d (a=%s b=%s)" % (
        ra, ("#%d" % bv) if lit else "r%d" % rb, rc, _h(a), _h(bv)))
    b.need(rows.Own(word))
    b.trap_clear()
    b.emit(word)
    b.trap_check()
    _next_runs(b)
    want = dict(vals)
    if rc != R31:
        want[rc] = aarm.CMOV2(vals.get(ra, 0), None)[0]
    for r in sorted(want):
        b.store(r, "r%d" % r, want[r])


def _ev8_fcmov2(b, fa, fb, fc, a, bv, fc_init):
    """FCMOV2 Fa, Fb, Fc: Fc <- Fav, every other register unchanged."""
    word = fop(*A.FCMOV2, fa, fb, fc)
    b.start()
    vals = {}
    for f, v in ((fa, a), (fb, bv), (fc, fc_init)):
        if f != R31 and f not in vals:
            b.load(1, v)
            b.emit(fop(*A.ITOF["ITOFT"], 1, R31, f))
            vals[f] = v
    b.begin("FCMOV2 f%d,f%d,f%d (a=%s b=%s)" % (fa, fb, fc, _h(a), _h(bv)))
    b.need(rows.Own(word))
    b.trap_clear()
    b.emit(word)
    b.trap_check()
    _next_runs(b)
    want = dict(vals)
    if fc != R31:
        want[fc] = vals.get(fa, 0)       # F31 reads as +0.0
    for f in sorted(want):
        b.emit(fop(*A.FTOI["FTOIT"], f, R31, 2))
        b.store(2, "f%d" % f, want[f])


def g_ev8(b):
    """The 21464's (EV8's) instructions that keep no state of their own
    (docs/platforms/ev8-isa.md 3.1 and 3.2; 21464 IDS Rev 1.1k, PDF pp.
    72-75, 411, 661). CCB, CMOV2 and FCMOV2 run where the row has them
    (rows.Own): elsewhere 18.EC00 is an unused code and 11.68 and 17.068
    raise OPCDEC (group misc). MAP_ALIGN and WH64EN are the core's and run
    on every row. Each case checks the result, that nothing traps, that the
    other registers are unchanged and that the next instruction runs."""
    for word, want in EV8_WORDS:
        assert word == want, "%08x, ev8-isa.md gives %08x" % (word, want)

    def ccb(ra, rb):
        return mfc(A.MISC["CCB"], ra, rb)

    # CCB: "a hint with no architectural effect on data" [IDS pp. 411, 661].
    b.begin("CCB (r22): memory, Rb, the next instruction")
    b.need(rows.Own(ccb(R31, 22)))
    b.load(20, 0x0123456789ABCDEF)
    b.emit(mem(OP["LDA"], 22, R_SCR, 0x280))
    b.emit(mem(OP["STQ"], 20, 22, 0))
    b.trap_clear()
    b.emit(ccb(R31, 22))
    b.trap_check()
    _next_runs(b)
    b.emit(mem(OP["LDQ"], 21, 22, 0))
    b.store(21, "memory at Rb", 0x0123456789ABCDEF)
    b.store(22, "Rb", b.scratch + 0x280)
    b.begin("CCB with Ra = r21 leaves Ra")
    b.need(rows.Own(ccb(21, 22)))
    b.load(21, 0xFEDCBA9876543210)
    b.emit(mem(OP["LDA"], 22, R_SCR, 0x280))
    b.trap_clear()
    b.emit(ccb(21, 22))
    b.trap_check()
    b.store(21, "Ra", 0xFEDCBA9876543210)
    # Unmapped, as the hints of group mem: CCB shares ECB's predecode (IIX)
    # [IDS p. 661; ARM 4.11.3] [inference].
    b.begin("CCB on an unmapped address")
    b.need(rows.Own(ccb(R31, 11)))
    b.load(11, 0x10000000)
    b.trap_clear()
    b.emit(ccb(R31, 11))
    b.trap_check()
    _next_runs(b)

    # WH64EN, the core's on every row [ARM 4.11.11; IDS p. 661]: the block's
    # contents become UNPREDICTABLE, so only Rb and the flow are checked.
    b.begin("WH64EN (r22): Rb, the next instruction")
    b.emit(mem(OP["LDA"], 22, R_SCR, 0x2C0))
    b.trap_clear()
    b.emit(mfc(A.MISC["WH64EN"], R31, 22))
    b.trap_check()
    _next_runs(b)
    b.store(22, "Rb", b.scratch + 0x2C0)

    # MAP_ALIGN, XOR R31,R31,R31, in the last slot of an aligned block of
    # eight instructions, where it starts a map chunk [IDS p. 75]: a NOP.
    b.begin("MAP_ALIGN in the last slot of an aligned half-block")
    b.load(20, 0x5A5A5A5AA5A5A5A5)
    b.trap_clear()
    while (b.asm.here & 0x1F) != 0x1C:
        b.emit(opr(0x11, 0x20, R31, R31, R31))      # NOP
    b.emit(A.MAP_ALIGN)
    b.trap_check()
    _next_runs(b)
    b.store(20, "r20", 0x5A5A5A5AA5A5A5A5)

    # CMOV2 [IDS pp. 73-74]: the IDS's own form copies Rc to Rc; every
    # other form copies Ra; Rb and the literal are never read.
    for a in EV8_VALS:
        _ev8_cmov2(b, 1, 2, 1, a, 0x1122334455667788, None)   # Rc = Ra
        _ev8_cmov2(b, 3, 4, 5, a, 0x1122334455667788, 0xCAFEBABEDEADBEEF)
        _ev8_cmov2(b, 6, 7, 7, a, 0x1122334455667788, None)   # Rc = Rb
        _ev8_cmov2(b, 8, 8, 8, a, a, None)                   # Ra = Rb = Rc
        _ev8_cmov2(b, 10, 0x99, 11, a, 0x99, 0x0F0F0F0F0F0F0F0F, lit=True)
        _ev8_cmov2(b, R31, 12, 13, a, a, 0xCAFEBABEDEADBEEF)  # Rc <- 0
        _ev8_cmov2(b, 14, 15, R31, a, 0x1122334455667788, None)
    # The JIT forwards the value an operate leaves in its result register
    # (rax, x0) to the next instruction's Ra: CMOV2 reading it, and the
    # next one reading CMOV2's.
    b.start()
    b.load(1, 0x0000000100000002)
    b.load(2, 0x0000001000000020)
    b.load(4, 0x7777777777777777)
    b.load(5, 0x5555555555555555)
    word = A.ins_op("CMOV2", 3, 4, 5)
    b.begin("ADDQ r1,r2,r3; CMOV2 r3,r4,r5; ADDQ r5,#1,r6")
    b.need(rows.Own(word))
    b.emit(opr(0x10, 0x20, 1, 2, 3))
    b.emit(word)
    b.emit(opl(0x10, 0x20, 5, 1, 6))
    b.store(5, "r5", 0x0000001100000022)
    b.store(6, "r6", 0x0000001100000023)
    b.store(4, "r4", 0x7777777777777777)

    # FCMOV2, the same in the FP registers.
    fv = [0, 0x8000000000000000, 0x7FF8000000000001, 0x400921FB54442D18]
    for a in fv:
        _ev8_fcmov2(b, 1, 2, 1, a, 0x3FF0000000000000, None)   # Fc = Fa
        _ev8_fcmov2(b, 3, 4, 5, a, 0x3FF0000000000000, 0xC000000000000000)
        _ev8_fcmov2(b, 6, 7, 7, a, 0x3FF0000000000000, None)   # Fc = Fb
        _ev8_fcmov2(b, R31, 8, 9, a, a, 0xC000000000000000)    # Fc <- +0
        _ev8_fcmov2(b, 10, 11, R31, a, 0x3FF0000000000000, None)
    # An FP operate instruction, so FEN with the FPU disabled, as FCMOVxx
    # (HRM Table 6-8) [inference: the IDS says nothing of it].
    word = fop(*A.FCMOV2, 3, 4, 5)
    b.start()
    for f, v in ((3, 0x4000000000000000), (5, 0xC008000000000000)):
        b.load(1, v)
        b.emit(fop(*A.ITOF["ITOFT"], 1, R31, f))
    b.begin("FCMOV2 with FPE clear takes FEN")
    b.need(rows.Own(word))
    b.emit(opr(0x11, 0x20, R31, R31, 16))
    b.emit(call_pal(0x01))                          # FPE off
    b.trap_clear()
    at = b.va()
    b.emit(word)
    b.trap_check(vector=0x200, exc_addr=at)
    b.emit(mem(OP["LDA"], 16, R31, 1))
    b.emit(call_pal(0x01))                          # FPE on
    b.emit(fop(*A.FTOI["FTOIT"], 5, R31, 2))
    b.store(2, "f5 unchanged", 0xC008000000000000)


# QUIESCE's timeout for the oracle's run, in cycles (ALPHABOX_QUIESCE_TIMEOUT,
# which cpu_oracle.py sets on a row that has QUIESCE): 2**24, 21 ms at the
# oracle's 800 MHz. A QUIESCE that slept took at least this long by RPCC; one
# that did not is a few instructions, so the cases tell the two apart with a
# margin no host stall reaches.
QUIESCE_TIMEOUT = 1 << 24


def _ldx_arm(name, ra, rb):
    return mfc(A.MISC[name], ra, rb)


QUIESCE = mfc(A.MISC["QUIESCE"])


def _unwatch(b):
    """CALL_PAL 01 with R16 = 1 (the image's FPE service, which leaves the
    FPU on): a CALL_PAL clears watch_flag [21464 IDS p. 670; on the row every
    CALL_PAL does, an Alphabox decision], so the next case starts clear."""
    b.emit(mem(OP["LDA"], 16, R31, 1))
    b.emit(call_pal(0x01))


def _quiesce_timed(b, field, slept):
    """QUIESCE between two RPCCs; store whether it slept: whether the cycles
    between them reach QUIESCE_TIMEOUT."""
    b.emit(mfc(A.MISC["RPCC"], 10))
    b.emit(QUIESCE)
    b.emit(mfc(A.MISC["RPCC"], 11))
    b.emit(opr(0x10, 0x29, 11, 10, 12))            # SUBQ r11,r10,r12
    b.emit(opl(0x12, 0x31, 12, 0x0F, 12))           # ZAPNOT: the 32-bit count
    # The native EV8's timeout is its IPR's, which the EV8 PALcode sets.
    timeout = image.QUIESCE8_TIMEOUT if b.ev8n else QUIESCE_TIMEOUT
    b.emit(mem(OP["LDAH"], 13, R31, timeout >> 16))
    b.emit(opr(0x10, 0x3D, 13, 12, 14))            # CMPULE r13,r12,r14
    b.store(14, field, 1 if slept else 0)


def g_quiesce(b):
    """The 21464's LDL_ARM, LDQ_ARM and QUIESCE (docs/platforms/ev8-isa.md
    3.1; 21464 IDS Rev 1.1k, PDF pp. 668-678) where the row has them, and
    AMASK bit 10 that software tests first on every row. LDx_ARM loads as
    LDL/LDQ from Rbv, with LDQ's faults [p. 669], and arms only when it did
    not fault [p. 670]. QUIESCE sleeps while watch_flag is set: here, one
    processor and nothing else writing, until the timeout [p. 673]; with the
    flag clear it runs on at once, and the flag is clear after a QUIESCE
    [p. 671], after a CALL_PAL [p. 670] and after an exception (both
    Alphabox decisions where the IDS leaves it open)."""
    m = {}
    b.begin("scratch init")
    init_scratch(b, m, at=0x300, quads=4)
    scr = b.scratch

    # AMASK bit 10, the IDS's own test: LDA R2,0x400(R31); AMASK R2,R2 [IDS
    # p. 674] (its BEQ is a BNE: ev8-isa.md section 2).
    b.begin("AMASK of bit 10 (SMT)")
    b.emit(mem(OP["LDA"], 2, R31, 0x400))
    b.emit(opr(0x11, 0x61, R31, 2, 2))
    b.store(2, "r2", 0x400 & ~b.row.amask)

    # The loads: Ra <- MEM[Rbv], sign-extended for LDL_ARM; Rb unchanged.
    for name, size, off in (("LDQ_ARM", 8, 0x300), ("LDQ_ARM", 8, 0x318),
                            ("LDL_ARM", 4, 0x300), ("LDL_ARM", 4, 0x304),
                            ("LDL_ARM", 4, 0x30C)):
        word = _ldx_arm(name, 1, 22)
        b.begin("%s r1,(r22), r22 = scratch+%#x" % (name, off))
        b.need(rows.Own(word))
        b.load(1, 0x5151515151515151)
        b.emit(mem(OP["LDA"], 22, R_SCR, off))
        b.trap_clear()
        b.emit(word)
        b.trap_check()
        _next_runs(b)
        v = mread(m, off, size)
        b.store(1, "r1", aarm.SEXT(v, 32) if size == 4 else v)
        b.store(22, "r22", scr + off)
        _unwatch(b)
    # Ra = Rb; and R31, which LDx_ARM loads and arms with as LDx_L does.
    word = _ldx_arm("LDQ_ARM", 22, 22)
    b.begin("LDQ_ARM r22,(r22)")
    b.need(rows.Own(word))
    b.emit(mem(OP["LDA"], 22, R_SCR, 0x308))
    b.emit(word)
    b.store(22, "r22", mread(m, 0x308, 8))
    _unwatch(b)
    word = _ldx_arm("LDQ_ARM", R31, 22)
    b.begin("LDQ_ARM r31,(r22) arms")
    b.need(rows.Own(word))
    b.emit(mem(OP["LDA"], 22, R_SCR, 0x300))
    b.trap_clear()
    b.emit(word)
    b.trap_check()
    _quiesce_timed(b, "QUIESCE slept (to the timeout)", True)

    # QUIESCE with watch_flag clear: no sleep, nothing changed, the next
    # instruction runs.
    b.begin("QUIESCE with watch_flag clear")
    b.need(rows.Own(QUIESCE))
    b.load(20, 0x0123456789ABCDEF)
    b.trap_clear()
    _quiesce_timed(b, "QUIESCE slept", False)
    b.trap_check()
    _next_runs(b)
    b.store(20, "r20", 0x0123456789ABCDEF)

    # An arm, and nothing writes the line: QUIESCE returns at the timeout,
    # with the flag clear, so a second one does not sleep [IDS p. 671].
    for name in ("LDQ_ARM", "LDL_ARM"):
        word = _ldx_arm(name, 1, 22)
        b.begin("%s, QUIESCE, QUIESCE" % name)
        b.need(rows.Own(word))
        b.need(rows.Own(QUIESCE))
        b.emit(mem(OP["LDA"], 22, R_SCR, 0x310))
        b.emit(word)
        b.trap_clear()
        _quiesce_timed(b, "first QUIESCE slept (to the timeout)", True)
        _quiesce_timed(b, "second QUIESCE slept", False)
        b.trap_check()
        _next_runs(b)
        b.store(1, "r1", aarm.SEXT(mread(m, 0x310, 4), 32)
                if name == "LDL_ARM" else mread(m, 0x310, 8))

    # A CALL_PAL between them clears the flag (REI, rti, rfe, retsys and urti
    # do [IDS p. 670]; any other may, and on the row every one does).
    word = _ldx_arm("LDQ_ARM", 1, 22)
    b.begin("LDQ_ARM, CALL_PAL, QUIESCE")
    b.need(rows.Own(word))
    b.need(rows.Own(QUIESCE))
    b.emit(mem(OP["LDA"], 22, R_SCR, 0x300))
    b.emit(word)
    # The interpreter runs on from LDQ_ARM to the next jump; a branch here
    # makes the CALL_PAL start a block, compiled on a JIT lane's last pass.
    b.emit(bra(OP["BR"], R31, 1))
    b.emit(opr(0x11, 0x20, R31, R31, R31))          # NOP, jumped over
    _unwatch(b)
    _quiesce_timed(b, "QUIESCE slept", False)
    # An exception between them, taken and returned from by the image's own
    # PALcode (HW_RET, no CALL_PAL), clears it too [decision].
    b.begin("LDQ_ARM, OPCDEC, QUIESCE")
    b.need(rows.Own(word))
    b.need(rows.Own(QUIESCE))
    b.emit(mem(OP["LDA"], 22, R_SCR, 0x300))
    b.emit(word)
    b.trap_clear()
    at = b.va()
    b.emit(1 << 26)                                 # opcode 01: OPCDEC
    b.trap_check(vector=0x400, exc_addr=at)
    _quiesce_timed(b, "QUIESCE slept", False)

    # The faults are LDQ's [IDS p. 669], and a load that faults does not arm
    # [p. 670]: a DTB miss on an unmapped address (the image resumes after
    # it), then QUIESCE does not sleep.
    for name in ("LDQ_ARM", "LDL_ARM"):
        word = _ldx_arm(name, 2, 11)
        b.begin("%s on an unmapped address, then QUIESCE" % name)
        b.need(rows.Own(word))
        b.need(rows.Own(QUIESCE))
        b.load(11, 0x10000000)
        b.load(2, 0x5151515151515151)
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=0x300, exc_addr=at, exc_sum=2 << 8,
                     exc_sum_mask=0x1F00, va=0x10000000)
        b.store(2, "r2 (not written)", 0x5151515151515151)
        _quiesce_timed(b, "QUIESCE slept", False)
    # Misaligned: LDQ's UNALIGN fault, which Alphabox, as for LDQ, takes only
    # across a page (docs/cpu-fidelity.md, the first known divergence).
    for name, size, off in (("LDQ_ARM", 8, 0x304), ("LDL_ARM", 4, 0x302)):
        word = _ldx_arm(name, 2, 22)
        b.begin("%s r2,(r22), r22 = scratch+%#x (misaligned)" % (name, off))
        b.need(rows.Own(word))
        b.load(2, 0x5151515151515151)
        b.emit(mem(OP["LDA"], 22, R_SCR, off))
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=0x280, exc_addr=at, exc_sum=2 << 8,
                     exc_sum_mask=0x1F00, va=scr + off,
                     known=(0, UNALIGNED_REF))
        v = mread(m, off, size)
        b.store(2, "r2 (not written: the fault skips it)", 0x5151515151515151,
                known=(aarm.SEXT(v, 32) if size == 4 else v, UNALIGNED_REF))
        _unwatch(b)
    word = _ldx_arm("LDQ_ARM", 2, 9)
    b.start()
    b.emit(mem(OP["LDA"], 9, R_SCR, 0x2000 - 4))    # across into page 1
    b.begin("LDQ_ARM r2,(r9) (misaligned, crosses a page), then QUIESCE")
    b.need(rows.Own(word))
    b.need(rows.Own(QUIESCE))
    b.load(2, 0x5151515151515151)
    b.trap_clear()
    at = b.va()
    b.emit(word)
    b.trap_check(vector=0x280, exc_addr=at, exc_sum=2 << 8,
                 exc_sum_mask=0x1F00, va=scr + 0x2000 - 4)
    b.store(2, "r2 (not written)", 0x5151515151515151)
    _quiesce_timed(b, "QUIESCE slept", False)


EMIT = {"intop": g_intop, "cmov": g_cmov, "trapv": g_trapv, "mem": g_mem,
        "unaligned": g_unaligned, "llsc": g_llsc, "branch": g_branch,
        "fix": g_fix, "misc": g_misc, "mode": g_mode, "ev8": g_ev8,
        "quiesce": g_quiesce}
EMIT.update(jit_cases.EMIT)
EMIT.update(ev8n_cases.EMIT)
EMIT.update(tarantula_cases.EMIT)
EMIT.update(simd_cases.EMIT)
EMIT.update(paired_cases.EMIT)


def emit(b, groups):
    _k[0] = 0
    for g in groups:
        EMIT[g](b)
        # The harness's own registers must come through every group.
        b.begin("harness: reserved registers after group %s" % g)
        for reg, value in ((image.R_TR, b.trap_record),
                           (image.R_SCR, b.scratch),
                           (image.R_POOL, image.kseg(image.POOL_PA) + 32768),
                           (image.R_UART, b.uart_va)):
            b.li(0, value)
            b.emit(opr(0x11, 0x40, reg, 0, 0))     # XOR
            b.store(0, "r%d XOR its value" % reg, 0)
