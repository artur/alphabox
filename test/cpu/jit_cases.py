"""The JIT's cases: instruction sequences shaped for what the translator
does (src/jit, src/cpu/AlphaCPU_jit.cpp), each with its expected results
from aarm.py and the manuals, so they check the interpreter as well.

What the shapes are for, briefly. The JIT compiles a block of at most 64
instructions once it has been dispatched 16 times (every case starts a
block of its own, image.Builder.block), keeps guest registers in host
registers for a whole chain of blocks (16 on AArch64, 4 on x86-64), runs
on through a conditional branch the recording pass did not take (AArch64:
an exit in the middle of the block), and bails to the interpreter at a
memory, FP or CALL_PAL instruction that would fault, after a probe that
has no side effects. So the cases put many registers live across chained
exits, make branches go one way in the passes that compile a block and
the other way in the pass that runs it compiled, and put faults in the
middle of compiled blocks.

The edge register. The first pass is interpreted and the last runs
compiled code; a case that must behave one way in those two and another
way while its blocks are being recorded computes `edge` = 1 in the first
and the last pass and 0 in the others (edge()), and selects with it. Only
the first and the last pass are checked, so the expected values are those
of edge = 1.

References: ARM = Alpha Architecture Reference Manual, 4th edition; HRM =
21264/EV68CB-DC Hardware Reference Manual.
"""
import random

import aarm
import alpha_asm as A
from alpha_asm import OP, R31, bra, call_pal, fop, jmp, mem, mfc, opl, opr
import image
import rows
from image import R_PASS, R_SCR, R_TR

M64 = (1 << 64) - 1
UNALIGNED_REF = "cpu-fidelity: unaligned data accesses do not trap"

# Scratch for these cases: page 2 from 0x1000 (KSEG, kernel read/write),
# away from the other groups' bytes; R_SCR + JSCR is its address.
JSCR = 0x5000
UNMAPPED = 0x10000000           # no translation: a DTB miss in kernel mode
SIRR_SI1 = 1 << 14              # SIRR<SIR1> (HRM Table 5-6)
IER_SIEN1 = 1 << 14             # IER<SIEN1> (HRM Table 5-5)
IER_ASTEN = 1 << 13
AST_K = (1 << 5) | (1 << 9)     # PCTX ASTER<0>, ASTRR<0>: a kernel AST
I_CTL_SDE = 0x100096            # the image's I_CTL (image.build_pal)
I_CTL_NOSDE = I_CTL_SDE & ~0x80  # SDE<1> clear (HRM 5.2.15 Table 5-11)

# Registers a case may use: R0-R23 and R30 (R24-R29 are the harness's).
REGS = list(range(0, 24)) + [30]


def _h(v):
    return "%x" % v


def edge(b, rc, rt):
    """rc = 1 in the first and the last pass, 0 in the others."""
    b.emit(opl(0x10, 0x2D, R_PASS, 0, rc))                      # CMPEQ
    b.emit(opl(0x10, 0x2D, R_PASS, b.passes - 1, rt))           # CMPEQ
    b.emit(opr(0x11, 0x20, rc, rt, rc))                          # BIS


def select(b, c, rd, rt, normal, on_edge):
    """rd = on_edge in the first and last pass, normal in the others."""
    b.load(rd, normal)
    b.load(rt, on_edge)
    b.emit(opr(0x11, 0x26, c, rt, rd))                           # CMOVNE


def link_r17(b, words_after):
    """R17 = the address just past the `words_after` instructions that
    follow these two (a return address for a CALL_PAL that returns to
    R17)."""
    b.emit(bra(OP["BR"], 17, 0))
    b.emit(mem(OP["LDA"], 17, 17, 4 * (words_after + 1)))


# --- a small Alpha machine for generated sequences ----------------------------
#
# Generated code is a list of words with, for each, what it does to the
# model (aarm.py's operations). The model starts from the registers the case
# loaded and runs the words in order, following branches, as the manual
# says; its registers afterwards are the expected results.

SAFE_OPS = ["ADDQ", "SUBQ", "ADDL", "SUBL", "S4ADDQ", "S8SUBQ", "S4ADDL",
            "S8SUBL", "MULQ", "MULL", "UMULH", "CMPEQ", "CMPLT", "CMPULE",
            "CMPBGE", "AND", "BIC", "BIS", "ORNOT", "XOR", "EQV", "SLL",
            "SRL", "SRA", "EXTBL", "EXTWH", "EXTQH", "INSWL", "INSLH",
            "MSKBL", "MSKQH", "ZAP", "ZAPNOT"]
UNARY_RB = ["SEXTB", "SEXTW", "CTLZ", "CTPOP", "CTTZ"]  # Ra must be R31
CMOVS = list(aarm.CMOV_TEST)
BRANCHES = list(aarm.BRANCH_TEST)


class Seq:
    def __init__(self):
        self.words = []
        self.sems = []   # fn(state) -> None, or the index to go to

    def add(self, word, sem):
        self.words.append(word & 0xFFFFFFFF)
        self.sems.append(sem)

    def run(self, regs, memory, limit=100000):
        st = {"r": dict(regs), "m": memory, "lock": None}
        i = 0
        while i < len(self.words):
            limit -= 1
            assert limit > 0, "model did not finish"
            nxt = self.sems[i](st)
            i = i + 1 if nxt is None else nxt
        return st

    def emit_to(self, b):
        for w in self.words:
            b.emit(w)


def rget(st, r):
    return 0 if r == R31 else st["r"][r]


def rset(st, r, v):
    if r != R31:
        st["r"][r] = v & M64


def add_op(seq, name, ra, rb, rc, lit=None):
    word = A.ins_op(name, ra, lit if lit is not None else rb, rc,
                    lit is not None)

    def sem(st):
        a = rget(st, ra)
        bv = lit if lit is not None else rget(st, rb)
        if name in aarm.CMOV_TEST:
            if aarm.CMOV_TEST[name](a):
                rset(st, rc, bv)
        else:
            rset(st, rc, aarm.operate(name, a, bv)[0])
    seq.add(word, sem)


def add_lda(seq, high, ra, rb, disp):
    def sem(st):
        rset(st, ra, rget(st, rb) + (disp << 16 if high else disp))
    seq.add(mem(OP["LDAH" if high else "LDA"], ra, rb, disp), sem)


def add_mem(seq, name, ra, rb, disp):
    """Integer loads and stores, aligned, on the byte model st['m']."""
    size = {"LDQ": 8, "LDL": 4, "LDWU": 2, "LDBU": 1,
            "STQ": 8, "STL": 4, "STW": 2, "STB": 1}[name]

    def sem(st):
        va = (rget(st, rb) + disp) & M64
        assert va % size == 0, "generated an unaligned access"
        m = st["m"]
        if name.startswith("LD"):
            v = sum(m[va + j] << (8 * j) for j in range(size))
            if name == "LDL":
                v = aarm.SEXT(v, 32)
            rset(st, ra, v)
        else:
            v = rget(st, ra)
            for j in range(size):
                m[va + j] = (v >> (8 * j)) & 0xFF
    seq.add(mem(OP[name], ra, rb, disp), sem)


def add_branch(seq, name, ra, disp):
    here = len(seq.words)

    def sem(st):
        if aarm.BRANCH_TEST[name](rget(st, ra)):
            return here + 1 + disp
        return None
    seq.add(bra(OP[name], ra, disp), sem)


def add_block(seq):
    """BR over one word (image.Builder.block), inside a sequence."""
    here = len(seq.words)
    seq.add(bra(OP["BR"], R31, 1), lambda st: here + 2)
    seq.add(0x01 << 26, lambda st: None)


def add_fix_move(seq, ra, fr, rc):
    """ITOFT Ra,Fr; FTOIT Fr,Rc: helper calls in compiled code that move
    the bits unchanged (ARM 4.10.18-19)."""
    def sem(st):
        st.setdefault("f", {})[fr] = rget(st, ra)
    seq.add(fop(*A.ITOF["ITOFT"], ra, R31, fr), sem)
    seq.add(fop(*A.FTOI["FTOIT"], fr, R31, rc),
            lambda st: rset(st, rc, st["f"][fr]))


def add_llsc(seq, rb, disp, rx, ry, k):
    """LDQ_L Rx; ADDQ Rx,#k,Ry; STQ_C Ry: nothing between, so it succeeds
    (ARM 4.2.4-5): Ry = 1 and the quadword is incremented."""
    def ld(st):
        va = (rget(st, rb) + disp) & M64
        m = st["m"]
        rset(st, rx, sum(m[va + j] << (8 * j) for j in range(8)))
        st["lock"] = va

    def sc(st):
        va = (rget(st, rb) + disp) & M64
        ok = st["lock"] == va
        st["lock"] = None
        if ok:
            v = rget(st, ry)
            for j in range(8):
                st["m"][va + j] = (v >> (8 * j)) & 0xFF
        rset(st, ry, 1 if ok else 0)
    seq.add(mem(OP["LDQ_L"], rx, rb, disp), ld)
    add_op(seq, "ADDQ", rx, None, ry, lit=k)
    seq.add(mem(OP["STQ_C"], ry, rb, disp), sc)


def random_op(seq, rng, regs, dests=None):
    dests = dests or regs
    r = rng.random()
    rc = rng.choice(dests)
    ra = rng.choice(regs + [R31] if rng.random() < 0.05 else regs)
    if r < 0.08:
        add_op(seq, rng.choice(UNARY_RB), R31, rng.choice(regs), rc)
    elif r < 0.18:
        add_op(seq, rng.choice(CMOVS), ra, rng.choice(regs), rc,
               lit=rng.randrange(256) if rng.random() < 0.3 else None)
    elif r < 0.24:
        add_lda(seq, rng.random() < 0.5, rc, ra, rng.randrange(-0x8000, 0x8000))
    else:
        lit = rng.randrange(256) if rng.random() < 0.3 else None
        add_op(seq, rng.choice(SAFE_OPS), ra, rng.choice(regs), rc, lit=lit)


def random_value(rng):
    pick = rng.random()
    if pick < 0.3:
        return rng.choice([0, 1, 2, 7, 0x7F, 0x80, 0xFF, 0x7FFFFFFF,
                           0x80000000, 0xFFFFFFFF, M64, M64 - 1,
                           0x8000000000000000, 0x7FFFFFFFFFFFFFFF])
    return rng.getrandbits(64)


def store_regs(b, regs, st, what):
    for r in regs:
        b.store(r, "r%d (%s)" % (r, what), rget(st, r))


# --- jitregs: the register allocators, values across chained exits ------------

def g_jitregs(b):
    """Every case register live across chained block exits, through
    blocks that use more registers than either back end pins, with helper
    calls (FIX moves, LDQ_L/STQ_C) between, and probe-reuse runs of loads
    and stores on one base."""
    rng = random.Random(0x5EED)
    for seed in range(8):
        regs0 = {r: random_value(rng) for r in REGS}
        b.begin("jitregs %d: all 25 registers live across two chained exits"
                % seed)
        for r in REGS:
            b.load(r, regs0[r])
        seq = Seq()
        add_block(seq)
        for _ in range(40):
            random_op(seq, rng, REGS)
        add_block(seq)
        for _ in range(20):
            random_op(seq, rng, REGS)
        st = seq.run(regs0, {})
        seq.emit_to(b)
        b.block()
        store_regs(b, REGS, st, "after two exits")

    # Helper calls with the registers live: the AArch64 call thunk stores
    # the caller-saved pins and loads them back; nothing may be lost.
    for seed in range(6):
        regs0 = {r: random_value(rng) for r in REGS}
        base = 13                        # page 2 + 0x1000, unpinned on x86
        regs0[base] = 0
        b.begin("jitregs helpers %d: FIX moves and LDQ_L/STQ_C among live "
                "registers" % seed)
        for r in REGS:
            if r != base:
                b.load(r, regs0[r])
        b.emit(mem(OP["LDA"], base, R_SCR, JSCR))
        regs0[base] = b.scratch + JSCR
        memory = {}
        for k in range(4):              # four quadwords the LL/SC count up
            v = 0x1111111111111111 * (k + 1)
            b.load(0, v)
            regs0[0] = v
            b.emit(mem(OP["STQ"], 0, base, 0x100 + 8 * k))
            for j in range(8):
                memory[regs0[base] + 0x100 + 8 * k + j] = (v >> (8 * j)) & 0xFF
        seq = Seq()
        add_block(seq)
        live = [r for r in REGS if r != base]
        for step in range(5):
            for _ in range(5):
                random_op(seq, rng, live)
            if step % 2 == 0:
                add_fix_move(seq, rng.choice(live), 10 + step, rng.choice(live))
            else:
                rx, ry = rng.sample(live, 2)
                add_llsc(seq, base, 0x100 + 8 * (step // 2), rx, ry,
                         rng.randrange(1, 256))
        st = seq.run(regs0, memory)
        seq.emit_to(b)
        b.block()
        store_regs(b, live, st, "after helper calls")
        for k in range(2):
            b.emit(mem(OP["LDQ"], 0, base, 0x100 + 8 * k))
            va = regs0[base] + 0x100 + 8 * k
            b.store(0, "quadword %d after LL/SC" % k,
                    sum(st["m"][va + j] << (8 * j) for j in range(8)))

    # Probe reuse (AArch64): consecutive loads, then stores, on one base,
    # displacements within 8 KB, the base unchanged; then the same base
    # written in between, which must end the reuse.
    for seed in range(4):
        b.begin("jitregs reuse %d: loads and stores on one base" % seed)
        base = 30 if seed % 2 else 12    # R30 is pinned on both back ends
        b.emit(mem(OP["LDA"], base, R_SCR, JSCR + 0x200))
        memory = {}
        bva = b.scratch + JSCR + 0x200
        for k in range(16):
            v = random_value(rng)
            b.load(1, v)
            b.emit(mem(OP["STQ"], 1, base, 8 * k))
            for j in range(8):
                memory[bva + 8 * k + j] = (v >> (8 * j)) & 0xFF
        regs0 = {r: 0 for r in REGS}
        for r in (1, 2, 3, 8, 9, 10, 11, 14):
            regs0[r] = random_value(rng)
            b.load(r, regs0[r])
        regs0[base] = bva
        regs0[R_SCR] = b.scratch
        seq = Seq()
        add_block(seq)
        sizes = [("LDQ", 8), ("LDL", 4), ("LDWU", 2), ("LDBU", 1)]
        for _ in range(10):
            name, size = rng.choice(sizes)
            add_mem(seq, name, rng.choice([1, 2, 3, 8, 9]), base,
                    size * rng.randrange(0, 128 // size))
        for _ in range(8):
            name = rng.choice(["STQ", "STL", "STW", "STB"])
            size = {"STQ": 8, "STL": 4, "STW": 2, "STB": 1}[name]
            add_mem(seq, name, rng.choice([1, 2, 3, 8, 9]), base,
                    size * rng.randrange(0, 128 // size))
        add_lda(seq, False, base, base, 8)        # the base moves
        add_mem(seq, "LDQ", 10, base, 0)
        add_mem(seq, "LDQ", 11, base, 8)
        add_mem(seq, "LDQ", base, base, 16)       # a load into its own base
        add_mem(seq, "LDQ", 14, R_SCR, JSCR + 0x200)
        st = seq.run(regs0, memory)
        seq.emit_to(b)
        b.block()
        store_regs(b, [1, 2, 3, 8, 9, 10, 11, 14, base], st, "probe reuse")
        for k in range(16):
            b.emit(mem(OP["LDQ"], 0, R_SCR, JSCR + 0x200 + 8 * k))
            va = bva + 8 * k
            b.store(0, "quadword %d" % k,
                    sum(st["m"][va + j] << (8 * j) for j in range(8)))
    # A misaligned load the helper performs in the page, then one the
    # previous probe's alignment would vouch for (the kA64NoPage rule of
    # jitemit_a64.hpp). The manual's answer is two UNALIGN faults; Alphabox
    # performs both (the documented divergence), and both values must be
    # the bytes at those addresses.
    b.begin("jitregs reuse: misaligned LDWU, then LDWU 2 bytes on")
    b.emit(mem(OP["LDA"], 12, R_SCR, JSCR + 0x400))
    pat = 0x0807060504030201
    b.load(1, pat)
    b.emit(mem(OP["STQ"], 1, 12, 0))
    b.load(2, 0x5151)
    b.load(3, 0x5252)
    b.trap_clear()
    b.block()
    b.emit(mem(OP["LDWU"], 2, 12, 1))
    b.emit(mem(OP["LDWU"], 3, 12, 3))
    b.block()
    b.emit(mem(OP["LDQ"], 0, R_TR, image.TR_COUNT))
    b.store(0, image.TRAP_COUNT, 2, known=(0, UNALIGNED_REF))
    b.expected_traps += 2
    b.store(2, "r2 (not written: the fault skips it)", 0x5151,
            known=(0x0302, UNALIGNED_REF))
    b.store(3, "r3 (not written: the fault skips it)", 0x5252,
            known=(0x0504, UNALIGNED_REF))


# --- jitbranch: branches, CMOVs and loops ---------------------------------------

def g_jitbranch(b):
    """Conditional branches that go one way while a block is recorded and
    the other way when it runs compiled (on AArch64 the not-taken ones are
    exits in the middle of the block), CMOVs on the values just computed,
    and loops: a self-loop, one with an exit in the middle, one of two
    blocks, nested ones."""
    rng = random.Random(0xB4A7C4)
    data = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 30]
    E, T = 13, 0
    for seed in range(12):
        regs0 = {r: random_value(rng) for r in REGS}
        b.begin("jitbranch %d: branches and CMOVs on edge-dependent values"
                % seed)
        for r in data:
            b.load(r, regs0[r])
        edge(b, E, T)
        regs0[E] = 1
        seq = Seq()
        # Values that differ between the edge passes and the others.
        for r in rng.sample(data, 5):
            add_op(seq, "XOR", r, E, r)
        for r in rng.sample(data, 3):
            add_op(seq, "CMOVNE", E, rng.choice(data), r)
        add_block(seq)
        n = 0
        while n < 55:
            k = rng.random()
            if k < 0.22:
                skip = rng.randrange(1, 4)
                reg = rng.choice(data + [E])
                add_branch(seq, rng.choice(BRANCHES), reg, skip)
                for _ in range(skip):
                    random_op(seq, rng, data)
                n += 1 + skip
            elif k < 0.27:
                add_block(seq)
                n += 2
            else:
                random_op(seq, rng, data)
                n += 1
        st = seq.run(regs0, {})
        seq.emit_to(b)
        b.block()
        store_regs(b, data, st, "after the branches")

    # A self-loop: 9 turns, 12 on the edge passes; four accumulators.
    b.begin("jitloop: self-loop, four accumulators, edge-dependent count")
    edge(b, E, T)
    vals = {1: 0x0123456789ABCDEF, 2: 0x1111, 3: 0xF0F0, 4: 7, 5: 3}
    for r, v in vals.items():
        b.load(r, v)
    b.emit(mem(OP["LDA"], 6, R31, 9))
    b.emit(opr(0x10, 0x20, E, E, 7))
    b.emit(opr(0x10, 0x20, 7, E, 7))               # r7 = 3 * edge
    b.emit(opr(0x10, 0x20, 6, 7, 6))
    b.block()
    loop = b.asm.here
    b.emit(opr(0x10, 0x20, 1, 2, 1))               # ADDQ r1,r2,r1
    b.emit(opr(0x11, 0x40, 3, 1, 3))               # XOR r3,r1,r3
    b.emit(opr(0x10, 0x22, 4, 3, 4))               # S4ADDQ r4,r3,r4
    b.emit(opl(0x13, 0x20, 5, 3, 5))               # MULQ r5,#3,r5
    b.emit(opl(0x10, 0x29, 6, 1, 6))               # SUBQ r6,#1,r6
    b.emit(bra(OP["BNE"], 6, (loop - (b.asm.here + 4)) // 4))
    r = dict(vals)
    for _ in range(12):
        r[1] = (r[1] + r[2]) & M64
        r[3] ^= r[1]
        r[4] = (4 * r[4] + r[3]) & M64
        r[5] = (r[5] * 3) & M64
    for reg in (1, 3, 4, 5):
        b.store(reg, "r%d after 12 turns" % reg, r[reg])
    b.store(6, "r6 (count)", 0)

    # The same, with an exit in the middle of the loop's block that only the
    # edge passes take (r6 == r8 at the fourth turn).
    b.begin("jitloop: an exit in the middle of a self-loop, taken on the "
            "edge passes")
    edge(b, E, T)
    b.load(1, 5)
    b.load(2, 0x9E3779B97F4A7C15)
    b.emit(mem(OP["LDA"], 6, R31, 10))
    select(b, E, 8, T, 1000, 6)
    b.emit(opr(0x11, 0x20, R31, R31, 3))
    b.block()
    loop = b.asm.here
    b.emit(opl(0x10, 0x20, 1, 5, 1))               # ADDQ r1,#5,r1
    b.emit(opl(0x10, 0x29, 6, 1, 6))               # SUBQ r6,#1,r6
    b.emit(opr(0x10, 0x2D, 6, 8, 9))               # CMPEQ r6,r8,r9
    b.emit(bra(OP["BNE"], 9, 3))                   # out, past the +1
    b.emit(opr(0x13, 0x20, 1, 2, 3))               # MULQ r1,r2,r3
    b.emit(bra(OP["BNE"], 6, (loop - (b.asm.here + 4)) // 4))
    b.emit(mem(OP["LDA"], 3, 3, 1))                # fell out the bottom: +1
    r1, r3, r6 = 5, 0, 10
    while True:                                    # the edge passes' run
        r1 += 5
        r6 -= 1
        if r6 == 6:
            break
        r3 = (r1 * 0x9E3779B97F4A7C15) & M64
        if r6 == 0:
            r3 += 1
            break
    b.store(1, "r1", r1)
    b.store(3, "r3", r3)
    b.store(6, "r6", r6)
    b.store(9, "r9 (the exit's condition)", 1)

    # A loop of two blocks: the back edge is a chained exit into the head.
    b.begin("jitloop: a loop of two blocks")
    edge(b, E, T)
    b.load(1, 1)
    b.load(2, 0xFFFFFFFF)
    b.load(3, 0)
    select(b, E, 6, T, 6, 11)
    b.block()
    loop = b.asm.here
    b.emit(opr(0x10, 0x00, 1, 2, 4))               # ADDL r1,r2,r4
    b.emit(opl(0x12, 0x39, 1, 1, 1))               # SLL r1,#1,r1
    b.emit(bra(OP["BR"], R31, 1))                  # into the second block
    b.emit(0x01 << 26)
    b.emit(opr(0x10, 0x20, 3, 4, 3))               # ADDQ r3,r4,r3
    b.emit(opl(0x10, 0x29, 6, 1, 6))
    b.emit(bra(OP["BGT"], 6, (loop - (b.asm.here + 4)) // 4))
    r1, r3 = 1, 0
    for _ in range(11):
        r4 = aarm.ADDL(r1, 0xFFFFFFFF)[0]
        r1 = (r1 << 1) & M64
        r3 = (r3 + r4) & M64
    b.store(1, "r1", r1)
    b.store(3, "r3", r3)
    b.store(4, "r4", r4)

    # Nested loops: an inner self-loop entered from an outer loop.
    b.begin("jitloop: nested loops")
    edge(b, E, T)
    b.load(1, 0)
    b.load(5, 0x10)
    select(b, E, 6, T, 3, 5)                       # outer turns
    b.block()
    outer = b.asm.here
    b.emit(mem(OP["LDA"], 7, R31, 4))              # inner turns
    b.block()
    inner = b.asm.here
    b.emit(opr(0x10, 0x20, 1, 5, 1))               # ADDQ r1,r5,r1
    b.emit(opl(0x10, 0x29, 7, 1, 7))
    b.emit(bra(OP["BNE"], 7, (inner - (b.asm.here + 4)) // 4))
    b.emit(opl(0x12, 0x39, 5, 1, 5))               # SLL r5,#1,r5
    b.emit(opl(0x10, 0x29, 6, 1, 6))
    b.emit(bra(OP["BNE"], 6, (outer - (b.asm.here + 4)) // 4))
    r1, r5 = 0, 0x10
    for _ in range(5):
        r1 += 4 * r5
        r5 <<= 1
    b.store(1, "r1", r1)
    b.store(5, "r5", r5)


# --- jitfault: a fault in the middle of a compiled block -------------------------

# What faults: a DTB miss on a load and on a store, a load that crosses a
# page (UNALIGN), a misaligned LDQ_L, FEN on four kinds of FP instruction
# (helper and inline paths), an ADDQ/V overflow and an OPCDEC.
FAULTS = ["dtbm-load", "dtbm-store", "unalign-load", "unalign-ldq_l",
          "fen-itoft", "fen-ftoit", "fen-ldt", "fen-addt", "ovf", "opcdec"]
POSITIONS = ["first", "middle", "last", "after-store", "after-branch"]


def g_jitfault(b):
    """A fault in the middle of a block that compiled while it did not
    fault: the compiled code must leave every register written before the
    faulting instruction in place, perform each earlier store once, and let
    the interpreter take the fault (the bail protocol, docs/cpu-fidelity.md
    "The JIT"). The fault happens in the edge passes only. The PALcode
    resumes after the faulting instruction (image.py), so its destination is
    not written (HRM 5.2.7: EXC_ADDR is the faulting instruction for a
    fault and the trigger for a synchronous trap)."""
    rng = random.Random(0xFA017)
    E, T = 13, 14
    pre = [1, 2, 3, 8, 9, 16, 17, 30]          # pinned and not, on both
    for kind in FAULTS:
        for pos in POSITIONS:
            fault_case(b, rng, kind, pos, E, T, pre)


def fault_case(b, rng, kind, pos, E, T, pre):
    scr = b.scratch
    b.begin("jitfault %s at %s" % (kind, pos))
    edge(b, E, T)
    regs0 = {r: random_value(rng) for r in pre}
    for r in pre:
        b.load(r, regs0[r])
    sentinel = 0x5E5E5E5E5E5E5E5E
    b.load(10, sentinel)                        # the faulting load's target
    regs0[10] = sentinel
    b.load(11, 0x0BADC0DE0BADC0DE)              # a store's value
    regs0[11] = 0x0BADC0DE0BADC0DE
    memory = {}
    good = scr + JSCR + 0x600
    for k in range(4):                          # the bytes the cases read
        v = 0x0101010101010101 * (k + 3)
        b.load(0, v)
        b.emit(mem(OP["STQ"], 0, R_SCR, JSCR + 0x600 + 8 * k))
        for j in range(8):
            memory[good + 8 * k + j] = (v >> (8 * j)) & 0xFF
    regs0[0] = v
    counter = scr + JSCR + 0x700                # incremented once a pass
    b.load(0, 41)
    b.emit(mem(OP["STQ"], 0, R_SCR, JSCR + 0x700))
    regs0[0] = 41
    for j in range(8):
        memory[counter + j] = (41 >> (8 * j)) & 0xFF
    b.emit(mem(OP["STQ"], R31, R_SCR, JSCR + 0x708))
    for j in range(8):
        memory[counter + 8 + j] = 0
    regs0[12] = scr + JSCR + 0x700
    b.emit(mem(OP["LDA"], 12, R_SCR, JSCR + 0x700))
    # The base, or the operands, that fault on the edge passes.
    vec, va, reg, exc_sum, fpe_off = None, None, None, None, False
    if kind in ("dtbm-load", "dtbm-store"):
        select(b, E, 15, T, good, UNMAPPED)
        vec, va = 0x300, UNMAPPED + (8 if kind == "dtbm-store" else 0)
        reg = 10 if kind == "dtbm-load" else None
    elif kind == "unalign-load":
        select(b, E, 15, T, good, scr + 0x2000 - 4)   # crosses a page
        vec, va, reg = 0x280, scr + 0x2000 - 4, 10
    elif kind == "unalign-ldq_l":
        select(b, E, 15, T, good, good + 4)
        vec, va, reg = 0x280, good + 4, 10
    elif kind.startswith("fen"):
        b.load(15, good)
        fpe_off = True
        vec = 0x200
        b.load(0, sentinel)
        b.emit(fop(*A.ITOF["ITOFT"], 0, R31, 20))   # f20, FPE still on
        b.load(0, 0x4000000000000000)               # 2.0
        b.emit(fop(*A.ITOF["ITOFT"], 0, R31, 21))
        regs0[0] = 0x4000000000000000
    elif kind == "ovf":
        select(b, E, 15, T, 5, 0x7FFFFFFFFFFFFFFF)
        b.load(10, 1)
        regs0[10] = 1
        vec, exc_sum = 0x600, 0xC0 | (11 << 8)
    elif kind == "opcdec":
        vec = 0x400
    regs0[15] = None                            # set by the selection
    b.trap_clear()
    if fpe_off:                                 # FPE <- NOT edge
        b.emit(opl(0x11, 0x40, E, 1, 16))      # XOR edge,#1,r16 (r16 is pre)
        b.emit(call_pal(0x01))
        b.load(16, regs0[16])
    # The block under test.
    b.block()
    seq = Seq()
    if pos == "middle":
        for _ in range(10):
            random_op(seq, rng, pre)
    if pos == "after-store":
        add_op(seq, "ADDQ", 1, 2, 3)
        add_mem(seq, "STQ", 3, 12, 8)
        add_mem(seq, "LDQ", 9, 12, 0)
        add_op(seq, "ADDQ", 9, None, 9, lit=1)
        add_mem(seq, "STQ", 9, 12, 0)           # the counter, once
    if pos == "after-branch":
        for _ in range(4):
            random_op(seq, rng, pre)
        add_branch(seq, "BEQ", 12, 1)           # r12 is never zero
        random_op(seq, rng, pre)
        for _ in range(3):
            random_op(seq, rng, pre)
    at_index = len(seq.words)
    if kind == "dtbm-load" or kind == "unalign-load":
        word = mem(OP["LDQ"], 10, 15, 0)
    elif kind == "dtbm-store":
        word = mem(OP["STQ"], 11, 15, 8)
    elif kind == "unalign-ldq_l":
        word = mem(OP["LDQ_L"], 10, 15, 0)
    elif kind == "fen-itoft":
        word = fop(*A.ITOF["ITOFT"], 11, R31, 20)
    elif kind == "fen-ftoit":
        word = fop(*A.FTOI["FTOIT"], 21, R31, 10)
    elif kind == "fen-ldt":
        word = mem(OP["LDT"], 20, 15, 0)
    elif kind == "fen-addt":
        word = fop(0x16, 0x0A0, 21, 21, 20)     # ADDT f21,f21,f20
    elif kind == "ovf":
        word = A.ins_op("ADDQ/V", 10, 15, 11)
    else:
        word = 0x01 << 26
        b.need(rows.Unused(word))
    # On the edge passes the instruction faults and does nothing (the
    # PALcode skips it); ADDQ/V is a trap, which writes its result first.
    if kind == "ovf":
        seq.add(word, lambda st: rset(st, 11, rget(st, 10) +
                                      0x7FFFFFFFFFFFFFFF))
    else:
        seq.add(word, lambda st: None)
    if pos != "last":
        for _ in range(4):
            random_op(seq, rng, pre)
        add_op(seq, "ADDQ", 10, 1, 2)            # reads the faulting dest
    # The model: the selected base on the edge passes.
    regs_m = dict(regs0)
    regs_m[15] = {"dtbm-load": UNMAPPED, "dtbm-store": UNMAPPED,
                  "unalign-load": scr + 0x2000 - 4,
                  "unalign-ldq_l": good + 4, "ovf": 0x7FFFFFFFFFFFFFFF,
                  }.get(kind, good)
    regs_m[E] = regs_m[T] = 1
    st = seq.run({r: v for r, v in regs_m.items() if v is not None}, memory)
    at = b.va() + 4 * at_index
    seq.emit_to(b)
    b.block()
    b.trap_check(vector=vec, exc_addr=at, va=va,
                 exc_sum=(reg << 8) if reg is not None else exc_sum,
                 exc_sum_mask=0x1F00 if reg is not None else 0x1FFF)
    store_regs(b, pre + [10, 11], st, "after the fault")
    if fpe_off:
        b.emit(mem(OP["LDA"], 16, R31, 1))
        b.emit(call_pal(0x01))
        b.emit(fop(*A.FTOI["FTOIT"], 20, R31, 0))
        b.store(0, "f20 (not written)", sentinel)
    b.emit(mem(OP["LDQ"], 0, R_SCR, JSCR + 0x700))
    b.store(0, "counter (once a pass)", sum(
        st["m"][counter + j] << (8 * j) for j in range(8)))
    b.emit(mem(OP["LDQ"], 0, R_SCR, JSCR + 0x708))
    b.store(0, "stored before the fault", sum(
        st["m"][counter + 8 + j] << (8 * j) for j in range(8)))


# --- jitint: an interrupt at a block boundary ----------------------------------------

def g_jitint(b):
    """A software interrupt and a kernel AST requested by PALcode, taken on
    the return to native mode: HW_RET/STALL after an Ibox IPR update makes
    the update "affect all instructions following the return path" (HRM
    D.8), so EXC_ADDR is the first native instruction, the one "that would
    have executed if the interrupt had not occurred" (HRM 5.2.7). The
    interrupt is taken once (the image's PALcode clears the request) and
    every instruction after it runs once. Compiled, the PAL service and the
    native blocks around it are chained: the HW_RET's exit must return to
    the dispatcher, which takes the interrupt."""
    def work(b, regs):
        for i, r in enumerate(regs):
            b.emit(opl(0x10, 0x20, r, i + 1, r))         # ADDQ r,#i+1,r

    vals = {1: 100, 2: 200, 3: 300, 8: 0x800, 9: 0x900, 30: 0x3000}
    for name, svc, r16, r17 in (
            ("software interrupt (SIRR, IER)", 0x85, SIRR_SI1, IER_SIEN1),
            ("kernel AST (ASTER, ASTRR, ASTEN)", 0x86, AST_K, IER_ASTEN)):
        b.begin("jitint: %s raised in PALcode" % name)
        if svc == 0x86:     # the AST registers are the 21264's
            b.need(rows.PAL21264)
        for r, v in vals.items():
            b.load(r, v)
        b.load(16, r16)
        b.load(17, r17)
        b.trap_clear()
        b.emit(call_pal(svc))
        at = b.va()
        work(b, [1, 2, 3])
        b.block()
        work(b, [8, 9, 30])
        b.trap_check(vector=0x680, exc_addr=at)
        for i, r in enumerate([1, 2, 3, 8, 9, 30]):
            b.store(r, "r%d (once)" % r, vals[r] + (i % 3) + 1)

    # Requested while disabled, then enabled by a write of IER alone.
    b.begin("jitint: SIRR set with IER clear, then IER enabled")
    for r, v in vals.items():
        b.load(r, v)
    b.load(16, SIRR_SI1)
    b.emit(opr(0x11, 0x20, R31, R31, 17))
    b.trap_clear()
    b.emit(call_pal(0x85))
    work(b, [1, 2, 3])
    b.block()
    b.emit(mem(OP["LDQ"], 0, R_TR, image.TR_COUNT))
    b.store(0, "trap count while disabled", 0)
    b.load(17, IER_SIEN1)
    b.emit(call_pal(0x8C))
    at = b.va()
    work(b, [8, 9, 30])
    b.trap_check(vector=0x680, exc_addr=at)
    for i, r in enumerate([1, 2, 3, 8, 9, 30]):
        b.store(r, "r%d (once)" % r, vals[r] + (i % 3) + 1)


# --- jitllsc: LDx_L / STx_C and blocks -----------------------------------------------

def g_jitllsc(b):
    """HRM 4.6: the 21264's lock is the LDx_L's Dcache line, and a STx_C
    "is allowed to succeed if its associated cache line is still present";
    branches between the pair are anticipated (4.6.2), memory operations
    between them make it fail (4.6.2, D.19 Restriction 23). Here the pair
    is split across blocks of compiled code."""
    v0 = 0x2000000000000005

    def setq(off, v):
        b.emit(mem(OP["LDA"], 9, R_SCR, 0x2000 + 0x200))   # page 1 + 0x200
        b.load(1, v)
        b.emit(mem(OP["STQ"], 1, 9, off))

    def tail(off, ok, newv):
        b.store(3, "STQ_C result", 1 if ok else 0)
        b.emit(mem(OP["LDQ"], 4, 9, off))
        b.store(4, "memory", newv if ok else v0)

    b.begin("jitllsc: LDQ_L, then STQ_C in the next block")
    setq(0, v0)
    b.emit(mem(OP["LDQ_L"], 2, 9, 0))
    b.emit(opl(0x10, 0x20, 2, 1, 3))
    b.block()
    b.emit(mem(OP["STQ_C"], 3, 9, 0))
    tail(0, True, v0 + 1)

    b.begin("jitllsc: LDQ_L, a branch not taken, STQ_C")
    setq(8, v0)
    b.emit(mem(OP["LDQ_L"], 2, 9, 8))
    b.emit(bra(OP["BEQ"], 9, 1))                         # r9 is never zero
    b.emit(opl(0x10, 0x20, 2, 2, 3))
    b.emit(opl(0x10, 0x20, 2, 2, 3))
    b.emit(mem(OP["STQ_C"], 3, 9, 8))
    tail(8, True, v0 + 2)

    b.begin("jitllsc: the retry loop")
    setq(16, v0)
    b.block()
    retry = b.asm.here
    b.emit(mem(OP["LDQ_L"], 2, 9, 16))
    b.emit(opl(0x10, 0x20, 2, 3, 3))
    b.emit(mem(OP["STQ_C"], 3, 9, 16))
    b.emit(bra(OP["BEQ"], 3, (retry - (b.asm.here + 4)) // 4))
    tail(16, True, v0 + 3)

    b.begin("jitllsc: LDQ_L, two block exits, LDQ, STQ_C")
    setq(24, v0)
    b.load(6, 0)
    b.emit(mem(OP["LDQ_L"], 2, 9, 24))
    b.block()
    b.emit(opl(0x10, 0x20, 2, 1, 3))
    b.block()
    b.emit(mem(OP["LDQ"], 6, 9, 0x80))
    b.emit(mem(OP["STQ_C"], 3, 9, 24))
    tail(24, False, 0)

    b.begin("jitllsc: three pairs in one block, a load inside the second")
    setq(32, v0)
    b.emit(mem(OP["LDQ_L"], 2, 9, 32))
    b.emit(opl(0x10, 0x20, 2, 1, 3))
    b.emit(mem(OP["STQ_C"], 3, 9, 32))
    b.emit(opr(0x11, 0x20, 3, 3, 5))                     # r5 = first result
    b.emit(mem(OP["LDQ_L"], 2, 9, 32))
    b.emit(mem(OP["LDQ"], 6, 9, 0x80))
    b.emit(opl(0x10, 0x20, 2, 1, 3))
    b.emit(mem(OP["STQ_C"], 3, 9, 32))
    b.emit(opr(0x11, 0x20, 3, 3, 7))                     # r7 = second result
    b.emit(mem(OP["LDQ_L"], 2, 9, 32))
    b.emit(opl(0x10, 0x20, 2, 1, 3))
    b.emit(mem(OP["STQ_C"], 3, 9, 32))
    b.store(5, "first STQ_C", 1)
    b.store(7, "second STQ_C (a load between)", 0)
    tail(32, True, v0 + 2)


# --- jitpal: PALmode compiled, the shadow registers ------------------------------------

def g_jitpal(b):
    """PALmode code compiles too, with R4-R7 and R20-R23 naming the
    PALshadow registers when I_CTL<SDE<1>> is set (HRM 5.2.15, 6.6) and
    the main ones when it is clear; PAL_BASE+0x3280 (CALL_PAL 8A) computes
    with both banks, a physical store and load, a branch and a CMOV."""
    shadowed = [4, 5, 6, 7, 20, 21, 22, 23]
    for a16, a17 in ((5, 9), (0xFFFFFFFFFFFFFFF0, 3), (0x123456789, 0x123456789),
                     (0x7FFFFFFFFFFFFFFF, 0x8000000000000000)):
        b.begin("jitpal: CALL_PAL 8A (R16=%s R17=%s)" % (_h(a16), _h(a17)))
        b.need(rows.PAL21264)
        for r in shadowed:
            b.load(r, 0xA0A0A0A000000000 | r)
        b.load(16, a16)
        b.load(17, a17)
        b.emit(call_pal(0x8A))
        p4 = (a16 + 1) & M64
        p5 = (p4 * 3) & M64
        p6 = p5 ^ a17
        lt = a16 < a17
        r0 = (p6 + p4 + (0 if lt else 7)) & M64
        r1 = ((4 * p4 + p5) & M64) if lt else p6
        b.store(0, "R0", r0)
        b.store(1, "R1", r1)
        for r in shadowed:
            b.store(r, "native R%d (unchanged)" % r, 0xA0A0A0A000000000 | r)

    # With SDE<1> clear (the edge passes only), PALmode's R4 is the main
    # R4; the block of CALL_PAL 88 compiled with it set must not run then.
    E, T = 13, 14
    b.begin("jitpal: PALmode R4 with SDE<1> clear on the edge passes")
    b.need(rows.PAL21264)
    edge(b, E, T)
    b.load(4, 0x4444444444444444)
    b.load(23, 0x2323232323232323)
    b.load(16, 0x5A5)
    b.emit(call_pal(0x89))                         # PALshadow R4 <- 5A5
    select(b, E, 16, T, I_CTL_SDE, I_CTL_NOSDE)
    link_r17(b, 1)
    b.emit(call_pal(0x87))                         # I_CTL <- r16
    link_r17(b, 1)
    b.emit(call_pal(0x88))                         # R0 <- R4 (PALmode's)
    b.load(16, I_CTL_SDE)
    link_r17(b, 1)
    b.emit(call_pal(0x87))
    b.store(0, "PALmode R4 with SDE<1> clear (the main R4)",
            0x4444444444444444)
    b.store(4, "native R4", 0x4444444444444444)
    b.store(23, "native R23 (the linkage is the shadow R23)",
            0x2323232323232323)


# --- jitmode: one virtual address, two modes --------------------------------------------

def g_jitmode(b):
    """A block at a page every mode may execute that branches to a page
    only kernel mode may (ITB KRE only), run in kernel mode until it
    compiles, then in user mode: the branch's target takes an IACV in user
    mode (HRM 6.8.2 Table 6-8: an Istream access violation; EXC_ADDR the
    target, a fault). Compiled blocks are keyed by the mode they were translated in,
    because that is when the mode's right to execute was settled."""
    exit_ = A.call_pal(0x84)
    b.begin("jitmode: a block compiled in kernel mode, entered in user mode")
    x = b.kcode_snippet([exit_])                   # kernel-only: back at R17
    y = b.user.here
    y = b.user_snippet([bra(OP["BR"], R31, (x - (y + 4)) // 4)])
    noop = b.user_snippet([exit_])
    # the ITB entries (CALL_PAL 02 installs them; user mode, straight back)
    b.li(16, noop)
    link_r17(b, 1)
    b.emit(call_pal(0x02))
    # kernel mode: Y -> X -> CALL_PAL 84 -> back here
    b.li(1, y)
    link_r17(b, 1)
    b.emit(jmp(0, R31, 1))
    b.trap_clear()
    # user mode: Y -> X takes an IACV; the PALcode returns at R17
    b.li(16, y)
    link_r17(b, 1)
    b.emit(call_pal(0x02))
    b.trap_check(vector=0x480, exc_addr=x)


# --- jitsmc: code rewritten under a compiled block -------------------------------------

def g_jitsmc(b):
    """An instruction that compiled, rewritten on the edge passes by a
    store and by PALcode's HW_ST, then IC_FLUSH and HW_RET/STALL: "Writing
    to this register invalidates all Icache blocks. The cache is flushed
    when the next HW_RET/STALL instruction is retired" (HRM 5.2.17, D.20),
    so the next fetch sees the new instruction. Compiled code skips an
    IC_FLUSH when the code-page map says nothing was written since the
    last one (docs/headless.md, ALPHABOX_JIT_NOPFLUSH), and checks a block's
    source words again only after a flush it did not skip; so the routine
    is called once just before the rewrite (its block is then known good)
    and once after it, and every path that stores to memory must report to
    the map."""
    E, T = 13, 14
    orig = mem(OP["LDA"], 1, R31, 1)
    new = mem(OP["LDA"], 1, R31, 2)
    for how in ("HW_ST (PALmode, physical)", "STL (a store to a code page)"):
        hw = how.startswith("HW_ST")
        b.begin("jitsmc: an instruction rewritten by %s, then IC_FLUSH" % how)
        b.emit(bra(OP["BR"], R31, 2))              # over the routine
        p = b.va()
        b.emit(orig)                               # P: LDA r1,#1; RET (r18)
        b.emit(jmp(2, R31, 18))
        edge(b, E, T)
        b.emit(bra(OP["BR"], 16, 0))               # r16 = the next word's VA
        b.emit(mem(OP["LDA"], 16, 16, p - b.va()))
        if hw:
            b.emit(opl(0x12, 0x31, 16, 0x1F, 16))  # ZAPNOT: KSEG -> physical
        b.emit(bra(OP["BSR"], 18, (p - (b.va() + 4)) // 4))
        b.store(1, "r1, the routine before the rewrite", 1)
        select(b, E, 17, T, orig, new)
        if hw:
            b.emit(call_pal(0x8D))                 # HW_STL r17 -> [r16]; IC_FLUSH
        else:
            b.emit(mem(OP["STL"], 17, 16, 0))
            b.emit(call_pal(0x8E))                 # IC_FLUSH
        b.emit(bra(OP["BSR"], 18, (p - (b.va() + 4)) // 4))
        b.store(1, "r1, the routine after the rewrite", 2)
        b.load(17, orig)                           # put it back for the next pass
        if hw:
            b.emit(call_pal(0x8D))
        else:
            b.emit(mem(OP["STL"], 17, 16, 0))
            b.emit(call_pal(0x8E))


# --- jitmisc --------------------------------------------------------------------------

def g_jitmisc(b):
    """RPCC in compiled code (AArch64: an inline stub) against the counter's
    rule: the low 32 bits count up (HRM 5.1.1), so a later read is not
    smaller, within a block and across a chained exit."""
    b.begin("jitmisc: RPCC, RPCC in one block, and in the next")
    b.emit(mfc(A.MISC["RPCC"], 1, R31))
    b.emit(mfc(A.MISC["RPCC"], 2, R31))
    b.block()
    b.emit(mfc(A.MISC["RPCC"], 3, R31))
    b.emit(opr(0x10, 0x09, 2, 1, 4))               # SUBL r2,r1,r4
    b.emit(opr(0x10, 0x09, 3, 2, 5))
    b.emit(opr(0x10, 0x6D, R31, 4, 6))             # CMPLE r31,r4,r6
    b.emit(opr(0x10, 0x6D, R31, 5, 7))
    b.store(6, "second read >= first", 1)
    b.store(7, "third read >= second", 1)


GROUPS = [
    ("jitregs", "JIT: all registers live across chained exits, helper calls "
                "among them, probe-reuse runs"),
    ("jitbranch", "JIT: branches and CMOVs that change direction once "
                  "compiled, loops (self, exit in the middle, two blocks, "
                  "nested)"),
    ("jitfault", "JIT: DTB miss, UNALIGN, FEN, /V and OPCDEC in the middle "
                 "of a compiled block, at five positions"),
    ("jitint", "JIT: a software interrupt and an AST at a block boundary"),
    ("jitllsc", "JIT: LDx_L/STx_C split across compiled blocks"),
    ("jitpal", "JIT: PALmode blocks, the shadow registers, SDE<1> clear"),
    ("jitmode", "JIT: one block entered in kernel and in user mode"),
    ("jitsmc", "JIT: an instruction rewritten by a store and by HW_ST, then "
               "IC_FLUSH"),
    ("jitmisc", "JIT: RPCC in compiled code"),
]
EMIT = {"jitregs": g_jitregs, "jitbranch": g_jitbranch,
        "jitfault": g_jitfault, "jitint": g_jitint, "jitllsc": g_jitllsc,
        "jitpal": g_jitpal, "jitmode": g_jitmode, "jitsmc": g_jitsmc,
        "jitmisc": g_jitmisc}
