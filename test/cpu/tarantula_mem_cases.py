"""Tarantula's memory instructions (group tarmem, the oracle's second image):
SM (VLOADQ, VSTOREQ), RM (VGATHQ, VSCATQ) and DRAINM, as
docs/platforms/ev8-isa.md fixes them (4.3, 4.5, 4.6, 4.8 and section 5),
on the rows that have Tarantula (`ev8`, `ev8n`), and what every other row
does with the same words. Stage S8 of docs/platforms/ev8.md. The expected
values are computed here from the sheet's rules, not from the emulator.

What the sheet says and these cases check:
  - an element is active when i < vl and, under the mask, vm[i] = 1; only
    active elements touch memory; an inactive element of a load's
    destination is UNPREDICTABLE, and Alphabox leaves it unchanged, which
    is checked as the sheet states it (4.3);
  - SM's element address is Rbv + i x vs, vs a signed byte stride; RM's
    Vx[i] + Rbv (4.5, 4.6); each element an aligned quadword: an address
    that is not a multiple of 8 raises the alignment fault;
  - two active elements of a store or scatter to one address: Alphabox
    writes in increasing i, so the highest active element's value remains
    (4.3, the sheet's Alphabox choice);
  - a load or gather into $vr31 is a prefetch: no fault, no effect; a store
    or scatter of $vr31 stores zeros (4.3);
  - memory faults are faults (section 5): EXC_ADDR the instruction's PC, VA
    the lowest-numbered active faulting element's address, MM_STAT its
    opcode (03 or 04) and WR for a store or scatter, and nothing changed,
    neither memory nor a register;
  - opcodes 03 and 04 raise OPCDEC with VEN = 0, an SBZ field or an
    unassigned function code (4.3, section 5); DRAINM executes whatever
    VEN is (4.8), and is an unused 18.xxxx code on the rows without
    Tarantula;
  - a scalar store, DRAINM, then a vector load of the same address reads
    the store (4.8).

How values get in and out. Memory is the sheet's route. The cases first
check VSTOREQ of registers built without a load (a broadcast by VSBIS and
the iota built by VC and VS, as tarantula_vv.py builds it); from then on a
register is read out by `dump` (vl 128, vs 8, VSTOREQ to a scratch page,
then quadword loads into the results) and filled by `fill` (VLOADQ from
memory whose contents the cases know), both restoring vl and vs.

Memory: the load region L (2 MB at 0x200000, above the image) holds
pat(va) at every quadword, written once in the first pass; the store
region S (4 MB at 0xC00000, above the result buffers) is set to sent(va)
around each element a store case may touch before the store, and the
checker reads every element's quadword and sums the quadwords before and
after it too, so a store to a neighbour shows.

Registers: R14 the load region's middle, R15 the store region's; R16-R22
and R23 (the link) are the subroutines'.
"""
import alpha_asm as A
from alpha_asm import OP, R31, bra, call_pal, jmp, mem, mfc, opl, opr
import image
import rows
from image import R_RES, R_SCR, kseg

M64 = (1 << 64) - 1
R_L, R_S, R_LINK = 14, 15, 23
I8 = A.IPR8

L_PA, L_SIZE = 0x200000, 0x200000
S_PA, S_SIZE = 0xC00000, 0x400000
L_MID = kseg(L_PA + L_SIZE // 2)
S_MID = kseg(S_PA + S_SIZE // 2)
LIST_OFF = 0x0000              # scratch page 0: the checker's address list
DUMP_OFF = 0x0400              # and the dump buffer
K1, C1, X1 = 0x9E3779B97F4A7C15, 0x0123456789ABCDEF, 0xA5A5A5A5A5A5A5A5
UNMAPPED = 0x10000000          # no translation: a DTB miss in kernel mode
UD, UK, UF = image.USER_DATA_VA, image.USER_KONLY_VA, image.USER_FAULT_VA
PAGE2 = image.SCR_PA + 0x4000  # where the three user data pages map

# VCTL, as tarantula_cases.py reaches it.
VCTL_PA = 0xFFFFFC00000 + 0x3F0000

GROUPS = [("tarmem", "Tarantula's memory instructions (ev8-isa.md 4.5, 4.6, "
                     "4.8): strides, gathers and scatters, masks, $vr31, "
                     "the faults and what they leave, OPCDEC, DRAINM")]


def pat(va):
    return (va * K1 + C1) & M64


def sent(va):
    return pat(va) ^ X1


def lda(ra, rb, d):
    return mem(OP["LDA"], ra, rb, d)


def ldq(ra, rb, d):
    return mem(OP["LDQ"], ra, rb, d)


def stq(ra, rb, d):
    return mem(OP["STQ"], ra, rb, d)


def addq(ra, rb, rc):
    return opr(0x10, 0x20, ra, rb, rc)


def xor(ra, rb, rc):
    return opr(0x11, 0x40, ra, rb, rc)


def mulq(ra, rb, rc):
    return opr(0x13, 0x20, ra, rb, rc)


def subq_l(ra, lit, rc):
    return opl(0x10, 0x29, ra, lit, rc)


def bic_l(ra, lit, rc):
    return opl(0x11, 0x08, ra, lit, rc)


DRAINM = mfc(A.DRAINM_FN)

# The sheet's words (4.5, 4.6, 4.8).
SHEET = [
    ("vloadq $vr0, ($16)", 0x0C100000),
    ("vstoreq $vr0, ($16), $vm", 0x0C100820),
    ("vgathq $vr3, $vr1($2)", 0x10620001),
    ("vscatq $vr3, $vr1($2)", 0x10620021),
]


def check_words():
    pairs = [(A.vmem("vloadq", 0, 16), 0x0C100000),
             (A.vmem("vstoreq", 0, 16, m=True), 0x0C100820),
             (A.vmem("vgathq", 3, 2, 1), 0x10620001),
             (A.vmem("vscatq", 3, 2, 1), 0x10620021),
             (DRAINM, 0x63FF5000)]
    for got, want in pairs:
        assert got == want, "%08x is not the sheet's %08x" % (got, want)


class Model:
    """The vector state and the memory the cases know, as the sheet says
    they change."""

    def __init__(self):
        self.vr = [[None] * 128 for _ in range(31)] + [[0] * 128]
        self.vl = None
        self.vs = None
        self.vm = None
        self.mem = {}

    def get(self, va):
        va &= M64
        if va in self.mem:
            return self.mem[va]
        if kseg(L_PA) <= va < kseg(L_PA + L_SIZE):
            return pat(va)
        raise KeyError("no known value at %x" % va)

    def active(self, i, m):
        return i < self.vl and (not m or self.vm[i])


def tables(b):
    """The image's tables: index vectors and masks, 128 quadwords each."""
    t = {}
    # Irregular low bits for a mixed mask.
    t["mix"] = [1 if (i * 37 + i // 5) % 3 else 0 for i in range(128)]
    t["iota8"] = [8 * i for i in range(128)]
    # Offsets of a gather or scatter: a permutation of 128 quadwords spread
    # over 45 KB (six pages) around the base, about half of them below it.
    t["perm"] = [8 * ((i * 53 + 17) % 128) * 45 - 0x6000 for i in range(128)]
    # Duplicates: element i at offset 8 x (i mod 5), so five addresses
    # each written by many elements.
    t["dup"] = [8 * (i % 5) for i in range(128)]
    # Element 9 misaligned (+4), the rest aligned.
    t["mis9"] = [8 * i + (4 if i == 9 else 0) for i in range(128)]
    # Element 5 outside every translation (UNMAPPED - Rbv, from L_MID).
    t["unm5"] = [((UNMAPPED - L_MID) if i == 5 else 8 * i) & M64
                 for i in range(128)]
    # User mode from USER_DATA_VA: element 3 to the FOR/FOW page (a
    # gather), element 6 to it (a scatter); the rest in the data page.
    t["for3"] = [(UF - UD) if i == 3 else 8 * i for i in range(128)]
    t["fow6"] = [(UF - UD + 8) if i == 6 else 8 * i for i in range(128)]
    idx = {}
    for name, vals in t.items():
        idx[name] = b.table(vals)
    return t, idx


class G:
    def __init__(self, b, t, idx):
        self.b = b
        self.t = t
        self.idx = idx
        self.s = Model()
        self.ev8n = b.ev8n
        self.seg = image.SEG52 if b.ev8n else image.SEG48
        self.k = 0
        for name, vals in t.items():
            for i, v in enumerate(vals):
                self.s.mem[self.tab_va(name) + 8 * i] = v & M64

    def e(self, *words):
        for w in words:
            self.b.emit(w)

    def tab_va(self, name):
        return kseg(image.TAB_PA) + 8 * self.idx[name]

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

    # vl, vs, vm
    def setvl(self, n):
        if n < 256:
            self.e(A.vc("setvl", lit=n))
        else:
            self.b.load(1, n)
            self.e(A.vc("setvl", rb=1))
        self.s.vl = min(n, 128)

    def setvs(self, v):
        v &= M64
        if v < 256:
            self.e(A.vc("setvs", lit=v))
        else:
            self.b.load(1, v)
            self.e(A.vc("setvs", rb=1))
        self.s.vs = v

    def mask(self, kind):
        """vm <- all on, all off, the mixed table, or every eighth element;
        leaves vl at 128. Uses $vr29."""
        self.setvl(128)
        if kind in ("on", "off"):
            self.e(A.vs("vsbis", 31, 0, 29, lit=1 if kind == "on" else 0))
            self.s.vr[29] = [1 if kind == "on" else 0] * 128
        elif kind == "mix":
            self.fill(29, self.tab_va("mix"), 8)
        elif kind == "every8":
            # $vr29 <- (iota8 AND 0x38) == 0
            self.fill(29, self.tab_va("iota8"), 8)
            self.e(A.vs("vsand", 29, 0, 29, lit=0x38),
                   A.vs("vscmpeq", 29, 0, 29, lit=0))
            self.s.vr[29] = [1 if (8 * i) & 0x38 == 0 else 0
                             for i in range(128)]
        else:
            raise ValueError(kind)
        self.e(A.vc("setvm", rb=29))
        self.s.vm = [v & 1 for v in self.s.vr[29]]

    # Registers in and out, restoring vl and vs.
    def _save(self):
        self.e(A.vc("getvl", rc=20), A.vc("getvs", rc=21),
               A.vc("setvl", lit=128))

    def _restore(self):
        self.e(A.vc("setvs", rb=21), A.vc("setvl", rb=20))

    def fill(self, n, va, stride):
        """$vrN <- 128 quadwords from va, stride bytes apart (VLOADQ)."""
        b = self.b
        self._save()
        b.li(22, va)
        b.load(16, stride)
        self.e(A.vc("setvs", rb=16), A.vmem("vloadq", n, 22))
        self._restore()
        if n != 31:
            self.s.vr[n] = [self.s.get(va + i * stride) for i in range(128)]

    def dump(self, n, name=None, want=None):
        """All 128 elements of $vrN into the results (VSTOREQ at vl 128 and
        vs 8 to the dump buffer, then copied)."""
        b = self.b
        self._save()
        self.e(A.vc("setvs", lit=8), lda(22, R_SCR, DUMP_OFF),
               A.vmem("vstoreq", n, 22))
        self._restore()
        want = self.s.vr[n] if want is None else want
        assert None not in want, (b.case, n)
        base = 8 * len(b.results)
        for i in range(128):
            b.slot("%s[%d]" % (name or "$vr%d" % n, i), want[i])
        d = base - b.res_base
        b.asm._add32(17, R_RES, d)
        self.e(lda(16, R_SCR, DUMP_OFF), lda(18, R31, 128))
        b.asm.branch(OP["BSR"], R_LINK, "tm_copy")

    def regs(self):
        """Two distinct vector registers out of 1-28."""
        k = self.k
        self.k += 1
        a = 1 + (k * 5) % 28
        c = 1 + (k * 11 + 7) % 28
        if c == a:
            c = 1 + c % 28
        return a, c

    # The store checker.
    def addresses(self, eas):
        """The checker's list: each element's quadword."""
        return [(ea & M64) & ~7 for ea in eas]


def strided(base, stride):
    return [(base + i * stride) & M64 for i in range(128)]


def indexed(base, offs):
    return [(base + o) & M64 for o in offs]


class Checker:
    """How a case names its elements' addresses to the image: the list is
    built in the image by tm_list_s (base, stride) or tm_list_x (base, the
    table of offsets)."""

    def __init__(self, g, kind, base, arg):
        self.g, self.kind, self.base, self.arg = g, kind, base, arg
        if kind == "s":
            self.eas = strided(base, arg)
        else:
            self.eas = indexed(base, g.t[arg])

    def build(self):
        b = self.g.b
        b.li(16, self.base)
        if self.kind == "s":
            b.load(17, self.arg)
            b.asm.branch(OP["BSR"], R_LINK, "tm_list_s")
        else:
            b.li(17, self.g.tab_va(self.arg))
            b.asm.branch(OP["BSR"], R_LINK, "tm_list_x")

    def reset(self):
        g, b = self.g, self.g.b
        self.build()
        b.load(17, K1)
        b.load(18, C1)
        b.load(19, X1)
        b.asm.branch(OP["BSR"], R_LINK, "tm_reset")
        for a in g.addresses(self.eas):
            for d in (-8, 0, 8):
                g.s.mem[(a + d) & M64] = sent((a + d) & M64)

    def check(self, name):
        g, b = self.g, self.g.b
        self.build()
        addrs = g.addresses(self.eas)
        base = 8 * len(b.results)
        for i, a in enumerate(addrs):
            b.slot("%s: memory of element %d" % (name, i), g.s.get(a))
        tot, x = 0, 0
        for a in addrs:
            for d in (-8, 0, 8):
                v = g.s.get((a + d) & M64)
                tot = (tot + v) & M64
                x ^= v
        b.slot("%s: sum around the elements" % name, tot)
        b.slot("%s: XOR around the elements" % name, x)
        d = base - b.res_base
        b.asm._add32(17, R_RES, d)
        b.asm.branch(OP["BSR"], R_LINK, "tm_check")


def subroutines(b):
    """The image's helpers, jumped over: tm_copy (r18 quadwords from r16
    to r17), tm_list_s (the list: (r16 + i x r17) & ~7), tm_list_x
    ((r16 + [r17 + 8 i]) & ~7), tm_reset (sent() around every listed
    address; K1, C1, X1 in r17, r18, r19), tm_check (each listed quadword
    to r17 on, then the sum and XOR of the 384 around them)."""
    a = b.asm
    a.branch(OP["BR"], R31, "tm_over")
    a.label("tm_copy")
    a.emit(ldq(20, 16, 0))
    a.emit(stq(20, 17, 0))
    a.emit(lda(16, 16, 8))
    a.emit(lda(17, 17, 8))
    a.emit(subq_l(18, 1, 18))
    a.branch(OP["BGT"], 18, "tm_copy")
    a.emit(jmp(2, R31, R_LINK))
    a.label("tm_list_s")
    a.emit(lda(20, R_SCR, LIST_OFF))
    a.emit(lda(21, R31, 128))
    a.label("tm_ls_loop")
    a.emit(bic_l(16, 7, 22))
    a.emit(stq(22, 20, 0))
    a.emit(addq(16, 17, 16))
    a.emit(lda(20, 20, 8))
    a.emit(subq_l(21, 1, 21))
    a.branch(OP["BGT"], 21, "tm_ls_loop")
    a.emit(jmp(2, R31, R_LINK))
    a.label("tm_list_x")
    a.emit(lda(20, R_SCR, LIST_OFF))
    a.emit(lda(21, R31, 128))
    a.label("tm_lx_loop")
    a.emit(ldq(22, 17, 0))
    a.emit(addq(22, 16, 22))
    a.emit(bic_l(22, 7, 22))
    a.emit(stq(22, 20, 0))
    a.emit(lda(17, 17, 8))
    a.emit(lda(20, 20, 8))
    a.emit(subq_l(21, 1, 21))
    a.branch(OP["BGT"], 21, "tm_lx_loop")
    a.emit(jmp(2, R31, R_LINK))
    a.label("tm_reset")
    a.emit(lda(20, R_SCR, LIST_OFF))
    a.emit(lda(21, R31, 128))
    a.label("tm_rs_loop")
    a.emit(ldq(22, 20, 0))
    a.emit(lda(22, 22, -8))
    for _ in range(3):
        a.emit(mulq(22, 17, 16))
        a.emit(addq(16, 18, 16))
        a.emit(xor(16, 19, 16))
        a.emit(stq(16, 22, 0))
        a.emit(lda(22, 22, 8))
    a.emit(lda(20, 20, 8))
    a.emit(subq_l(21, 1, 21))
    a.branch(OP["BGT"], 21, "tm_rs_loop")
    a.emit(jmp(2, R31, R_LINK))
    a.label("tm_check")
    a.emit(lda(20, R_SCR, LIST_OFF))
    a.emit(lda(21, R31, 128))
    a.emit(opr(0x11, 0x20, R31, R31, 18))
    a.emit(opr(0x11, 0x20, R31, R31, 19))
    a.label("tm_ck_loop")
    a.emit(ldq(22, 20, 0))
    a.emit(ldq(16, 22, 0))
    a.emit(stq(16, 17, 0))
    a.emit(lda(17, 17, 8))
    for d in (0, -8, 8):
        if d:
            a.emit(ldq(16, 22, d))
        a.emit(addq(18, 16, 18))
        a.emit(xor(19, 16, 19))
    a.emit(lda(20, 20, 8))
    a.emit(subq_l(21, 1, 21))
    a.branch(OP["BGT"], 21, "tm_ck_loop")
    a.emit(stq(18, 17, 0))
    a.emit(stq(19, 17, 8))
    a.emit(jmp(2, R31, R_LINK))
    a.label("tm_over")


def opcdec(b, name, word, need=None):
    b.begin(name)
    if need:
        b.need(need)
    b.trap_clear()
    at = b.va()
    b.emit(word)
    b.trap_check(vector=0x400, exc_addr=at)


def enter_user(b, snippet_va):
    """CALL_PAL 02: user mode at the snippet, which ends in CALL_PAL 84
    back here (cases.py, group mode)."""
    b.li(16, snippet_va)
    b.emit(bra(OP["BR"], 17, 0))
    b.emit(mem(OP["LDA"], 17, 17, 8))
    b.emit(call_pal(0x02))


# --- the group -----------------------------------------------------------------


def g_tarmem(b):
    check_words()
    t, idx = tables(b)

    # On every row: VEN is 0 at reset (and this image never set it), so on
    # a Tarantula row the sheet's words raise OPCDEC as on a row without
    # Tarantula, where opcodes 03 and 04 are reserved.
    words = SHEET + [
        ("vloadq $vr5, ($9), $vm", A.vmem("vloadq", 5, 9, m=True)),
        ("vstoreq $vr5, ($9)", A.vmem("vstoreq", 5, 9)),
        ("vgathq $vr5, $vr6($9), $vm", A.vmem("vgathq", 5, 9, 6, m=True)),
        ("vscatq $vr5, $vr6($9), $vm", A.vmem("vscatq", 5, 9, 6, m=True)),
    ]
    for name, word in words:
        opcdec(b, "tarmem: %s raises OPCDEC with VEN = 0 or no Tarantula"
               % name, word)
    # DRAINM: on a Tarantula row an instruction, executed whatever VEN is
    # (4.8); elsewhere an unused 18.xxxx code, a no-op where the row
    # completes those (rows.UNUSED_MISC_NOP).
    for nm, word in (("drainm", DRAINM), ("18.5000 with Ra 21, Rb 22",
                                           mfc(A.DRAINM_FN, 21, 22))):
        b.begin("tarmem: %s completes, VEN = 0 or no Tarantula" % nm)
        with b.part("an instruction of the row"):
            b.need(rows.Own(word))
            _drainm_case(b, word, nm)
        with b.part("an unused code of the row"):
            b.need(rows.Unused(word))
            b.need(rows.UNUSED_MISC_NOP)
            _drainm_case(b, word, nm)

    # The sheet's test for Tarantula: `amask 8, $t0`, zero where it is there
    # (section 2, D14).
    b.begin("tarmem: AMASK #8 (bit 3, Tarantula)")
    b.emit(A.ins_op("AMASK", R31, 8, 1, lit=True))
    b.store(1, "AMASK #8", 0 if "TARANTULA" in b.row.ext else 8)

    g = G(b, t, idx)
    harness(g)
    g_store_first(g)
    g_loads(g)
    g_stores(g)
    g_gather_scatter(g)
    g_vr31(g)
    g_nothing_active(g)
    g_unaligned(g)
    g_tb_miss(g)
    g_dtb_pages(g)
    g_user(g)
    g_drainm(g)
    g_fields(g)
    g_ven0(g)
    b.begin("tarmem: VEN = 0 at the end")
    b.need(rows.TARANTULA)
    g.set_ven(False)


def _drainm_case(b, word, nm):
    b.load(21, 0x0123456789ABCDEF)
    b.load(22, 0x5A5A5A5A12345678)
    b.trap_clear()
    b.emit(word)
    b.trap_check()
    b.emit(mem(OP["STQ"], 22, R_SCR, 0x1F00))      # the next one runs
    b.emit(mem(OP["LDQ"], 23, R_SCR, 0x1F00))
    b.store(23, "%s: the next instruction ran" % nm, 0x5A5A5A5A12345678)
    b.store(21, "%s: Ra unchanged" % nm, 0x0123456789ABCDEF)
    b.store(22, "%s: Rb unchanged" % nm, 0x5A5A5A5A12345678)


def harness(g):
    b = g.b
    b.begin("tarmem: the harness, VEN = 1, the load region")
    b.need(rows.TARANTULA)
    subroutines(b)
    a = b.asm
    # The load region's pattern, pat(va) at every quadword, written in the
    # first pass only: a case that stores there puts the pattern back.
    a.branch(OP["BNE"], image.R_PASS, "tm_have_pattern")
    b.li(16, kseg(L_PA))
    b.li(17, L_SIZE // 8)
    b.load(18, K1)
    b.load(19, C1)
    a.label("tm_pat_loop")
    a.emit(mulq(16, 18, 20))
    a.emit(addq(20, 19, 20))
    a.emit(stq(20, 16, 0))
    a.emit(lda(16, 16, 8))
    a.emit(subq_l(17, 1, 17))
    a.branch(OP["BGT"], 17, "tm_pat_loop")
    a.label("tm_have_pattern")
    b.li(R_L, L_MID)
    b.li(R_S, S_MID)
    g.set_ven(True)
    # The region's ends and middle, by scalar loads.
    for off in (-0x100000, 0, 0xFFFF8):
        b.li(2, L_MID + off)
        b.emit(ldq(1, 2, 0))
        b.store(1, "the load region at %+x" % off, pat(L_MID + off))


def g_store_first(g):
    """VSTOREQ of registers no load wrote: a broadcast, then the iota built
    by VC and VS (tarantula_vv.py's way); this is the route `dump` takes."""
    b = g.b
    b.begin("tarmem: VSTOREQ of a broadcast, vl 128, vs 8")
    b.need(rows.TARANTULA)
    g.setvl(128)
    g.setvs(8)
    g.e(A.vs("vsbis", 31, 0, 3, lit=0x5A))
    g.s.vr[3] = [0x5A] * 128
    c = Checker(g, "s", S_MID, 8)
    c.reset()
    b.emit(A.vmem("vstoreq", 3, R_S))
    for i in range(128):
        g.s.mem[c.eas[i]] = 0x5A
    c.check("vstoreq of 0x5A")
    b.begin("tarmem: VSTOREQ of the iota built by VC and VS")
    b.need(rows.TARANTULA)
    g.setvl(128)
    g.e(A.vs("vsbis", 31, 0, 30, lit=0))
    b.emit(lda(9, R31, 1))
    a = b.asm
    a.label("tm_iota")
    g.e(A.vc("setvl", rb=9), A.vs("vsbis", 31, 0, 27, lit=1),
        A.vc("setvm", rb=27), A.vc("setvl", lit=128),
        A.vs("vsaddq", 30, 0, 28, lit=1), A.vv("vvmrg", 28, 30, 30))
    b.emit(opl(0x10, 0x20, 9, 1, 9))
    b.emit(opl(0x10, 0x4D, 9, 128, 10))
    a.branch(OP["BNE"], 10, "tm_iota")
    g.s.vr[30] = list(range(128))
    g.s.vr[27] = [None] * 128
    g.s.vr[28] = [None] * 128
    g.s.vl = 128
    g.s.vm = [1] * 127 + [0]
    c = Checker(g, "s", S_MID + 0x2000, 8)
    c.reset()
    b.li(9, S_MID + 0x2000)
    b.emit(A.vmem("vstoreq", 30, 9))
    for i in range(128):
        g.s.mem[c.eas[i]] = i
    c.check("vstoreq of the iota")
    b.begin("tarmem: dump of the iota")
    b.need(rows.TARANTULA)
    g.dump(30, "iota")


# The strided cases: (stride, [(vl, mask)]) where mask is None (no M),
# "on", "off", "mix" or "every8".
STRIDES = [
    (8, [(128, None), (64, "mix"), (1, "on"), (0, None), (128, "off"),
         (128, "mix")]),
    (-8, [(128, None), (100, "mix"), (64, "on")]),
    (0, [(128, None), (64, "mix")]),
    (24, [(128, None), (77, "mix")]),
    (-40, [(128, None), (128, "mix")]),
    (136, [(128, None), (33, "mix")]),
    (8200, [(128, None), (128, "mix")]),
    (-8200, [(128, None), (65, "mix")]),
    (1, [(128, "every8"), (64, "every8")]),
    (-1, [(128, "every8")]),
    (8 * 1023, [(128, None)]),
]


def _setup(g, vl, mask, stride):
    if mask is not None:
        g.mask(mask)
    g.setvl(vl)
    g.setvs(stride)


def g_loads(g):
    b = g.b
    for stride, combos in STRIDES:
        for vl, mask in combos:
            b.begin("tarmem: vloadq, vs %d, vl %d, %s" % (
                stride, vl, "no mask" if mask is None else "mask " + mask))
            b.need(rows.TARANTULA)
            vt, other = g.regs()
            # vt starts with values of its own, so that inactive elements
            # show; the load reads around the region's middle.
            g.fill(vt, L_MID - 0x80000 + 8 * g.k, 8)
            _setup(g, vl, mask, stride)
            m = mask is not None
            b.trap_clear()
            b.emit(A.vmem("vloadq", vt, R_L, m=m))
            b.trap_check()
            new = list(g.s.vr[vt])
            for i in range(128):
                if g.s.active(i, m):
                    new[i] = g.s.get(L_MID + i * stride)
            g.s.vr[vt] = new
            g.dump(vt)


def g_stores(g):
    b = g.b
    for stride, combos in STRIDES:
        for vl, mask in combos[:2]:
            b.begin("tarmem: vstoreq, vs %d, vl %d, %s" % (
                stride, vl, "no mask" if mask is None else "mask " + mask))
            b.need(rows.TARANTULA)
            vt, other = g.regs()
            g.fill(vt, L_MID + 0x40000 + 8 * g.k, 8)
            _setup(g, vl, mask, stride)
            m = mask is not None
            c = Checker(g, "s", S_MID, stride)
            c.reset()
            b.trap_clear()
            b.emit(A.vmem("vstoreq", vt, R_S, m=m))
            b.trap_check()
            for i in range(128):
                if g.s.active(i, m):
                    g.s.mem[c.eas[i]] = g.s.vr[vt][i]
            c.check("vstoreq vs %d" % stride)


def g_gather_scatter(g):
    b = g.b
    for tab, combos in (("perm", [(128, None), (100, "mix"), (1, "on")]),
                        ("dup", [(128, None), (128, "mix"), (64, "mix")]),
                        ("iota8", [(128, "mix")])):
        for vl, mask in combos:
            m = mask is not None
            b.begin("tarmem: vgathq through %s, vl %d, %s" % (
                tab, vl, "no mask" if mask is None else "mask " + mask))
            b.need(rows.TARANTULA)
            vt, vx = g.regs()
            g.fill(vx, g.tab_va(tab), 8)
            g.fill(vt, L_MID - 0x20000 + 8 * g.k, 8)
            if m:
                g.mask(mask)
            g.setvl(vl)
            b.trap_clear()
            b.emit(A.vmem("vgathq", vt, R_L, vx, m=m))
            b.trap_check()
            new = list(g.s.vr[vt])
            for i in range(128):
                if g.s.active(i, m):
                    new[i] = g.s.get(L_MID + g.t[tab][i])
            g.s.vr[vt] = new
            g.dump(vt)
            g.dump(vx, "the index register")

            b.begin("tarmem: vscatq through %s, vl %d, %s" % (
                tab, vl, "no mask" if mask is None else "mask " + mask))
            b.need(rows.TARANTULA)
            vt, vx = g.regs()
            g.fill(vx, g.tab_va(tab), 8)
            g.fill(vt, L_MID + 0x60000 + 8 * g.k, 8)
            if m:
                g.mask(mask)
            g.setvl(vl)
            c = Checker(g, "x", S_MID, tab)
            c.reset()
            b.trap_clear()
            b.emit(A.vmem("vscatq", vt, R_S, vx, m=m))
            b.trap_check()
            for i in range(128):          # increasing i: the last one stays
                if g.s.active(i, m):
                    g.s.mem[c.eas[i]] = g.s.vr[vt][i]
            c.check("vscatq through %s" % tab)
    # The index register as the destination: every address is computed
    # before any element is written.
    b.begin("tarmem: vgathq $vr7, $vr7(r14): the index register loaded")
    b.need(rows.TARANTULA)
    g.fill(7, g.tab_va("perm"), 8)
    g.setvl(128)
    b.emit(A.vmem("vgathq", 7, R_L, 7))
    g.s.vr[7] = [g.s.get(L_MID + g.t["perm"][i]) for i in range(128)]
    g.dump(7)


def g_vr31(g):
    b = g.b
    b.begin("tarmem: vloadq $vr31 is a prefetch: no fault, no effect")
    b.need(rows.TARANTULA)
    g.setvl(128)
    g.setvs(8)
    for nm, va in (("unmapped", UNMAPPED), ("misaligned", L_MID + 4)):
        b.li(9, va)
        b.trap_clear()
        b.emit(A.vmem("vloadq", 31, 9))
        b.trap_check()
    b.begin("tarmem: vgathq $vr31 is a prefetch: no fault (elements 5 and 9 "
            "unmapped or misaligned)")
    b.need(rows.TARANTULA)
    for tab in ("unm5", "mis9"):
        g.fill(6, g.tab_va(tab), 8)
        g.setvl(128)
        b.trap_clear()
        b.emit(A.vmem("vgathq", 31, R_L, 6))
        b.trap_check()
    g.dump(31, "$vr31")
    for nm, word, chk in (
            ("vstoreq $vr31", A.vmem("vstoreq", 31, R_S),
             lambda: Checker(g, "s", S_MID, 8)),
            ("vscatq $vr31", A.vmem("vscatq", 31, R_S, 6),
             lambda: Checker(g, "x", S_MID, "perm"))):
        b.begin("tarmem: %s stores zeros" % nm)
        b.need(rows.TARANTULA)
        g.fill(6, g.tab_va("perm"), 8)
        g.setvl(100)
        g.setvs(8)
        c = chk()
        c.reset()
        b.trap_clear()
        b.emit(word)
        b.trap_check()
        for i in range(100):
            g.s.mem[c.eas[i]] = 0
        c.check(nm)


def g_nothing_active(g):
    """vl 0, or every element masked off: no access, so no fault, whatever
    the addresses."""
    b = g.b
    for nm, vl, mask in (("vl 0", 0, None), ("all masked off", 128, "off")):
        for ins, word in (("vloadq", A.vmem("vloadq", 4, 9, m=mask is not None)),
                          ("vstoreq", A.vmem("vstoreq", 4, 9,
                                             m=mask is not None)),
                          ("vgathq", A.vmem("vgathq", 4, 9, 6,
                                            m=mask is not None)),
                          ("vscatq", A.vmem("vscatq", 4, 9, 6,
                                            m=mask is not None))):
            b.begin("tarmem: %s with %s and unmapped, misaligned addresses"
                    % (ins, nm))
            b.need(rows.TARANTULA)
            g.fill(4, L_MID + 0x1000, 8)
            g.fill(6, g.tab_va("mis9"), 8)
            if mask:
                g.mask(mask)
            g.setvl(vl)
            g.setvs(12)
            b.li(9, UNMAPPED + 4)
            b.trap_clear()
            b.emit(word)
            b.trap_check()
            g.dump(4)


def _fault_check(g, at, vec, va, mmstat):
    b = g.b
    b.trap_check(vector=vec, exc_addr=at, va=va)
    b.emit(ldq(0, image.R_TR, image.TR_MM_STAT))
    b.store(0, "MM_STAT<OPCODE, WR, FOW, FOR, ACV>", mmstat, mask=0x3FF)


def g_unaligned(g):
    b = g.b
    # VLOADQ from a misaligned base: element 0 faults.
    b.begin("tarmem: vloadq from base + 4: UNALIGN at element 0, $vr4 "
            "unchanged")
    b.need(rows.TARANTULA)
    g.fill(4, L_MID + 0x2000, 8)
    g.setvl(5)
    g.setvs(8)
    b.li(9, L_MID + 4)
    b.trap_clear()
    at = b.va()
    b.emit(A.vmem("vloadq", 4, 9))
    _fault_check(g, at, 0x280, L_MID + 4, 0x030)
    g.dump(4)
    # VSTOREQ, stride 12 under a mask of 0, 2, 4, 6, 7: element 7 is the
    # lowest active misaligned one; 0, 2, 4, 6 are not written.
    # The mask from the iota (still in $vr30): the even elements, and with
    # a fault element 7 too; the store runs at vl 8.
    sel = [0, 2, 4, 6, 7]
    for faulting in (True, False):
        b.begin("tarmem: vstoreq, vs 12, mask %s: %s" % (
            "0 2 4 6 7" if faulting else "0 2 4 6",
            "UNALIGN at element 7, nothing written" if faulting else
            "the active elements aligned, no fault"))
        b.need(rows.TARANTULA)
        g.fill(4, L_MID + 0x3000, 8)
        g.setvl(128)
        g.e(A.vs("vsand", 30, 0, 28, lit=1),
            A.vs("vscmpeq", 28, 0, 29, lit=0))
        even = [1 if i % 2 == 0 else 0 for i in range(128)]
        g.s.vr[28] = [i & 1 for i in range(128)]
        g.s.vr[29] = even
        if faulting:
            g.e(A.vs("vscmpeq", 30, 0, 28, lit=7),
                A.vv("vvbis", 29, 28, 29))
            g.s.vr[28] = [1 if i == 7 else 0 for i in range(128)]
            g.s.vr[29] = [1 if (e or i == 7) else 0
                          for i, e in enumerate(even)]
        g.e(A.vc("setvm", rb=29))
        g.s.vm = list(g.s.vr[29])
        g.setvl(8)
        g.setvs(12)
        c = Checker(g, "s", S_MID, 12)
        c.reset()
        b.trap_clear()
        at = b.va()
        b.emit(A.vmem("vstoreq", 4, R_S, m=True))
        if faulting:
            _fault_check(g, at, 0x280, S_MID + 84, 0x031)
        else:
            b.trap_check()
            for i in sel[:-1]:
                g.s.mem[c.eas[i]] = g.s.vr[4][i]
        c.check("vstoreq vs 12")
    # A gather and a scatter with element 9 misaligned.
    for ins in ("vgathq", "vscatq"):
        b.begin("tarmem: %s with element 9 misaligned: UNALIGN at element 9, "
                "nothing changed" % ins)
        b.need(rows.TARANTULA)
        g.fill(6, g.tab_va("mis9"), 8)
        g.fill(4, L_MID + 0x4000, 8)
        g.setvl(16)
        base = R_L if ins == "vgathq" else R_S
        bva = L_MID if ins == "vgathq" else S_MID
        if ins == "vscatq":
            c = Checker(g, "x", S_MID, "mis9")
            c.reset()
        b.trap_clear()
        at = b.va()
        b.emit(A.vmem(ins, 4, base, 6))
        _fault_check(g, at, 0x280, bva + 8 * 9 + 4,
                     0x040 if ins == "vgathq" else 0x041)
        if ins == "vgathq":
            g.dump(4)
        else:
            c.check("vscatq, element 9 misaligned")


def g_tb_miss(g):
    """Kernel mode, element 5 at an address no translation covers: the DTB
    miss for element 5, nothing changed; masked off, the instruction
    completes."""
    b = g.b
    for ins in ("vgathq", "vscatq"):
        for off5 in (False, True):
            b.begin("tarmem: %s, element 5 unmapped%s" % (
                ins, ", masked off: completes" if off5 else
                ": DTB miss at element 5, nothing changed"))
            b.need(rows.TARANTULA)
            g.fill(6, g.tab_va("unm5"), 8)
            g.fill(4, L_MID + 0x5000, 8)
            if off5:
                g.setvl(128)
                g.e(A.vs("vscmpne", 30, 0, 29, lit=5))
                g.s.vr[29] = [1 if i != 5 else 0 for i in range(128)]
                g.e(A.vc("setvm", rb=29))
                g.s.vm = list(g.s.vr[29])
            g.setvl(16)
            # The scatter's base is the load region's middle too, so that
            # element 5's offset leads to UNMAPPED; the other elements land
            # in L at L_MID + 8 i, which the checker watches (element 5's
            # quadword there included: not written) and puts back.
            if ins == "vscatq":
                c = Checker(g, "s", L_MID, 8)
                c.reset()
            b.trap_clear()
            at = b.va()
            b.emit(A.vmem(ins, 4, R_L, 6, m=off5))
            if not off5:
                _fault_check(g, at, 0x300, UNMAPPED,
                             0x040 if ins == "vgathq" else 0x041)
            else:
                b.trap_check()
                for i in range(16):
                    if i == 5:
                        continue
                    if ins == "vgathq":
                        g.s.vr[4][i] = g.s.get(L_MID + 8 * i)
                    else:
                        g.s.mem[c.eas[i]] = g.s.vr[4][i]
            if ins == "vgathq":
                g.dump(4)
            else:
                c.check("vscatq, element 5 unmapped")
                _restore_l(g, c)


def _restore_l(g, c):
    """Put the load region's pattern back where a checker reset it."""
    b = g.b
    c.build()
    b.load(17, K1)
    b.load(18, C1)
    b.load(19, 0)
    b.asm.branch(OP["BSR"], R_LINK, "tm_reset")
    for a in g.addresses(c.eas):
        for d in (-8, 0, 8):
            g.s.mem.pop((a + d) & M64, None)


def g_dtb_pages(g):
    """Kernel mode across two pages the DTB maps to one physical page
    (CALL_PAL 02 installs them: USER_DATA_VA and, after it, USER_KONLY_VA,
    both to scratch page 2): elements 0-3 at the end of the page, 4-7 at
    its start."""
    b = g.b
    exit_ = call_pal(0x84)
    b.begin("tarmem: the DTB translations (CALL_PAL 02 and back)")
    b.need(rows.TARANTULA)
    snip = b.user_snippet([exit_])
    enter_user(b, snip)
    page = kseg(PAGE2)
    vas = [UD + 0x1FE0 + 8 * i for i in range(8)]
    pas = [page + 0x1FE0 + 8 * i if i < 4 else page + 8 * (i - 4)
           for i in range(8)]
    b.begin("tarmem: vloadq across two pages, kernel mode, through the DTB")
    b.need(rows.TARANTULA)
    for i, pa in enumerate(pas):
        b.load(1, 0x1111111111111111 * (i + 1))
        b.li(2, pa)
        b.emit(stq(1, 2, 0))
        g.s.mem[pa] = 0x1111111111111111 * (i + 1)
    g.fill(4, L_MID + 0x6000, 8)
    g.setvl(8)
    g.setvs(8)
    b.li(9, vas[0])
    b.trap_clear()
    b.emit(A.vmem("vloadq", 4, 9))
    b.trap_check()
    for i in range(8):
        g.s.vr[4][i] = g.s.mem[pas[i]]
    g.dump(4)
    b.begin("tarmem: vstoreq across two pages, kernel mode, through the DTB")
    b.need(rows.TARANTULA)
    g.fill(5, L_MID + 0x7000, 8)
    g.setvl(8)
    g.setvs(8)
    b.li(9, vas[0])
    b.trap_clear()
    b.emit(A.vmem("vstoreq", 5, 9))
    b.trap_check()
    for i, pa in enumerate(pas):
        b.li(2, pa)
        b.emit(ldq(1, 2, 0))
        b.store(1, "page 2 at %x (element %d)" % (pa & 0x1FFF, i),
                g.s.vr[5][i])


def g_user(g):
    """User mode: an element in a kernel-only page (ACV), in a page with
    FOR or FOW (DFAULT), or in no page at all (DTB miss): the fault for that
    element, the earlier ones not done."""
    b = g.b
    exit_ = call_pal(0x84)
    page = kseg(PAGE2)
    # (what, instruction, Rbv, the index table or the stride, the vector,
    # VA, MM_STAT). The page below USER_DATA_VA is the user code's, which
    # the DTB does not map.
    cases = [
        ("vstoreq into the kernel-only page at element 4", "vstoreq",
         UD + 0x1FE0, 8, 0x380, UK, 0x031 | 2),
        ("vloadq from the kernel-only page at element 4", "vloadq",
         UD + 0x1FE0, 8, 0x380, UK, 0x030 | 2),
        ("vgathq from the FOR page at element 3", "vgathq", UD, "for3",
         0x380, UF, 0x040 | 4),
        ("vscatq to the FOW page at element 6", "vscatq", UD, "fow6",
         0x380, UF + 8, 0x041 | 8),
        ("vstoreq, vs -8, to no page at element 2", "vstoreq", UD + 8, -8,
         0x300, UD - 8, 0x031),
    ]
    for nm, ins, base, tab, vec, va, mmstat in cases:
        b.begin("tarmem: user mode: %s" % nm)
        b.need(rows.TARANTULA)
        # The data page through KSEG: its first 0x40 bytes and last 0x20.
        offs = [0x1FE0 + 8 * i for i in range(4)] + [8 * i for i in range(8)]
        for o in offs:
            b.load(1, sent(page + o))
            b.li(2, page + o)
            b.emit(stq(1, 2, 0))
            g.s.mem[page + o] = sent(page + o)
        g.fill(4, L_MID + 0x8000, 8)
        rm = isinstance(tab, str)
        if rm:
            g.fill(6, g.tab_va(tab), 8)
        g.setvl(8)
        g.setvs(8 if rm else tab)
        word = A.vmem(ins, 4, 9, 6 if rm else 0)
        snip = b.user_snippet([word, exit_])
        b.li(9, base)
        b.trap_clear()
        enter_user(b, snip)
        b.trap_check(vector=vec, exc_addr=snip, va=va)
        b.emit(ldq(0, image.R_TR, image.TR_MM_STAT))
        b.store(0, "MM_STAT<OPCODE, WR, FOW, FOR, ACV>", mmstat, mask=0x3FF)
        g.dump(4)
        for o in offs:
            b.li(2, page + o)
            b.emit(ldq(1, 2, 0))
            b.store(1, "page 2 at %x (not written)" % o, sent(page + o))


def g_drainm(g):
    b = g.b
    b.begin("tarmem: a scalar store, DRAINM, a vector load of it")
    b.need(rows.TARANTULA)
    for i in range(4):
        b.load(1, 0x0D0D0D0D00000000 | i)
        b.emit(stq(1, R_S, 8 * i))
        g.s.mem[S_MID + 8 * i] = 0x0D0D0D0D00000000 | i
    g.fill(4, L_MID + 0x9000, 8)
    g.setvl(4)
    g.setvs(8)
    b.trap_clear()
    b.emit(DRAINM)
    b.emit(A.vmem("vloadq", 4, R_S))
    b.trap_check()
    for i in range(4):
        g.s.vr[4][i] = 0x0D0D0D0D00000000 | i
    g.dump(4)
    b.begin("tarmem: a vector store, a scalar load of it")
    b.need(rows.TARANTULA)
    g.fill(5, L_MID + 0xA000, 8)
    g.setvl(4)
    g.setvs(-8)
    b.emit(A.vmem("vstoreq", 5, R_S))
    for i in range(4):
        b.emit(ldq(1, R_S, -8 * i))
        b.store(1, "element %d, by LDQ" % i, g.s.vr[5][i])
        g.s.mem[(S_MID - 8 * i) & M64] = g.s.vr[5][i]
    b.begin("tarmem: FPU disabled: the memory instructions take no FEN")
    b.need(rows.TARANTULA)
    b.emit(lda(16, R31, 0))
    b.emit(call_pal(0x01))
    g.fill(4, L_MID + 0xB000, 8)
    g.setvl(16)
    g.setvs(16)
    b.trap_clear()
    b.emit(A.vmem("vloadq", 4, R_L))
    b.trap_check()
    for i in range(16):
        g.s.vr[4][i] = g.s.get(L_MID + 16 * i)
    b.emit(lda(16, R31, 1))
    b.emit(call_pal(0x01))
    g.dump(4)


def g_fields(g):
    """SBZ fields and unassigned functions raise OPCDEC with VEN = 1, and
    change nothing."""
    b = g.b
    bad = []
    for sbz in (1, 2, 4, 8):
        bad.append(("vloadq <15:12> = %x" % sbz, A.vmem("vloadq", 4, 9, sbz=sbz)))
        bad.append(("vscatq <15:12> = %x" % sbz,
                    A.vmem("vscatq", 4, 9, 6, sbz=sbz)))
    for low in (1, 16):
        bad.append(("vstoreq <4:0> = %d" % low, A.vmem("vstoreq", 4, 9, low)))
    for f in (0x02, 0x03, 0x10, 0x21, 0x3F):
        bad.append(("SM function %02X" % f, A.vmem("vloadq", 4, 9, fnc=f)))
        bad.append(("RM function %02X" % f, A.vmem("vgathq", 4, 9, 6, fnc=f)))
    for nm, word in bad:
        b.begin("tarmem: %s raises OPCDEC with VEN = 1" % nm)
        b.need(rows.TARANTULA)
        g.fill(4, L_MID + 0xC000, 8)
        g.fill(6, g.tab_va("iota8"), 8)
        g.setvl(128)
        g.setvs(8)
        b.li(9, S_MID)
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=0x400, exc_addr=at)
    b.begin("tarmem: after the field cases: $vr4 unchanged")
    b.need(rows.TARANTULA)
    g.dump(4)


def g_ven0(g):
    """VEN = 0: each instruction raises OPCDEC and changes nothing."""
    b = g.b
    b.begin("tarmem: VEN = 0: OPCDEC, nothing changed")
    b.need(rows.TARANTULA)
    g.fill(4, L_MID + 0xD000, 8)
    g.fill(6, g.tab_va("iota8"), 8)
    g.setvl(128)
    g.setvs(8)
    c = Checker(g, "s", S_MID, 8)
    c.reset()
    g.set_ven(False)
    for ins in ("vloadq", "vstoreq", "vgathq", "vscatq"):
        word = A.vmem(ins, 4, R_S, 6 if ins in ("vgathq", "vscatq") else 0)
        b.trap_clear()
        at = b.va()
        b.emit(word)
        b.trap_check(vector=0x400, exc_addr=at)
    g.set_ven(True)
    g.dump(4)
    c.check("memory after the VEN = 0 stores")


EMIT = {"tarmem": g_tarmem}
