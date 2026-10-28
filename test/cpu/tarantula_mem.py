#!/usr/bin/env python3
"""Tarantula's memory instructions through translation and across
processors: a boot image of its own (docs/platforms/ev8.md, S8;
docs/development.md, "The oracle").

    tarantula_mem.py <alphabox-binary> [label] [--port N] [--keep]
                     [--iterations N]

Two `ev8` processors on the ES40 board, through the ALPHABOX_CPU_ANY_BOARD
test hook, as the CPU oracle runs one; the image replaces the console
(image.py's PALcode and loader, with a DTB-miss handler of its own).
What docs/platforms/ev8-isa.md says, and this checks:

  - a TB miss is reported for the lowest-numbered active element whose
    translation misses, with the instruction's PC; the PALcode fills the
    entry and returns to the instruction, which restarts from its first
    element (section 5, D15);
  - forward progress is the emulator's to guarantee: "a strided access can
    touch 128 pages, more than a small TB holds", and S8 must show that such
    an instruction completes (section 5);
  - a fault on a middle element leaves everything unchanged; after the
    handler the instruction completes (section 5);
  - MB and WMB order vector accesses with respect to other processors as
    they order scalar ones; DRAINM orders as MB does and makes earlier
    stores visible to later vector loads (4.8, D12).

The PALcode's DTB-miss handler (this script's) looks the page up in a table
of its own and writes the DTB, as an EV7 PALcode does from the page table;
and before it, as such a PALcode's virtual page-table fetch does, it
writes one more entry, for a page-table page of its own invention, so each
miss takes two TB entries. A page the table marks absent is a page fault:
the handler records what memory held at that moment, marks the page
present and returns to the instruction without filling, so it misses
again. After 5000 misses in one instruction it gives up (a livelock).

Processor 0, with an empty DTB before each:
  1 VLOADQ, 128 elements, vs 8 KB + 8: 128 pages, each mapped to a page of
    its own: 128 misses, the values the pages hold;
  2 VLOADQ, 128 elements, vs 32 MB: 128 pages whose TB shadow slots
    (src/cpu/AlphaCPU.hpp) are all one, so nothing but the instruction
    itself remembers the earlier elements' translations: 128 misses;
  3 VSCATQ of 128 elements to 128 pages, the 65th absent: when the page
    fault is taken element 0's quadword still holds its old value; then
    129 misses and every element written.
Then processor 1 is started, and the two pass N messages (default 2000):
processor 1 writes 128 quadwords (by VSTOREQ in even rounds, by STQ in odd
ones), WMB, a flag; processor 0 waits for the flag, MB (even) or DRAINM
(odd), VLOADQ, and counts the elements that are not what was written.

Exit status 0 when every count is as stated and no element is wrong.
"""
import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import alpha_asm as A  # noqa: E402
import cpu_oracle  # noqa: E402
import image  # noqa: E402
from alpha_asm import (IPR, OP, R31, Asm, bra, call_pal, hw_ld, hw_mfpr,  # noqa: E402
                       hw_mtpr, hw_ret, hw_st, mem, mfc, opl, opr)
from image import P4, P5, P6, P7, P21, P22, P23, kseg  # noqa: E402

M64 = (1 << 64) - 1
VCTL_PA = 0xFFFFFC00000 + 0x3F0000
DPR_START1 = 0x80110000000 + (0x3428 << 6)   # DPR byte 0x3428: start CPU 1

DATA_PA = 0x1E8000           # results and the shared lines
RES = 0x000                  # results: quadwords from here
NRES = 16
WHO, ALIVE, FLAG, ACK = 0x400, 0x440, 0x480, 0x4C0
IOTA_PA = 0x1EA000           # 8 x i, i < 128: the scatter's offsets
DUMP_PA = 0x1EA400           # vector registers out, to be checked
BUF_PA = 0x1EA800            # part 2's message
MAP_PA = 0x1EC000            # the handler's table: 32 bytes an entry
MAP_ENTRIES = 384
POOL_PA = 0x1000000          # 256 pages of 8 KB the regions map to
MAGIC = 0x5555AAAA3C3C9696
# Trap-record quadwords the handler keeps (image.TR_PA + ...).
T_MISS, T_PF, T_SNAP, T_SNAP_PA, T_DUMMY = 0x100, 0x108, 0x110, 0x118, 0x120
MISS_LIMIT = 5000

R1, S1 = 0x40000000, 8192 + 8        # scenario 1: region, stride
R2, S2 = 0x50000000, 32 << 20        # scenario 2
R3 = 0x200000000                     # scenario 3, stride 8192 + 8 by Vx
DUMMY_VA = 0x300000000             # the page-table pages of the handler
PF_ELEMENT = 64


def pte(pa):
    """A DTB_PTE value: the PFN <52:32>, read and write in every mode, ASM
    (image.py's user data page's)."""
    return ((pa >> 13) << 32) | 0xFF10


def pool_page(k):
    return POOL_PA + 8192 * k


def mapping():
    """(VA page, PTE, PTE once present) for every page the scenarios use."""
    out = []
    for i in range(128):
        out.append((R1 + 8192 * i, pte(pool_page(i)), 0))
    for i in range(128):
        out.append((R2 + S2 * i, pte(pool_page(128 + i)), 0))
    for i in range(128):
        p = pte(pool_page(i))
        out.append((R3 + 8192 * i, 0 if i == PF_ELEMENT else p, p))
    assert len(out) <= MAP_ENTRIES
    # The handler takes the first entry of a page: the regions, and the
    # handler's own page-table pages, must not share one.
    pages = [va for va, _, _ in out]
    assert len(set(pages)) == len(pages)
    assert not any(DUMMY_VA <= va < DUMMY_VA + 256 * 8192 for va in pages)
    return out


def lda(ra, rb, d):
    return mem(OP["LDA"], ra, rb, d)


def ldq(ra, rb, d):
    return mem(OP["LDQ"], ra, rb, d)


def stq(ra, rb, d):
    return mem(OP["STQ"], ra, rb, d)


def addq(ra, rb, rc):
    return opr(0x10, 0x20, ra, rb, rc)


def handler():
    """The DTB-miss handler, at PAL_BASE + 0x6000 (the vector branches
    here). p20 is the trap record."""
    h = Asm(image.PAL_PA + 0x6000)
    tr = 20
    h.emit(hw_mfpr(P4, IPR["VA"]))
    h.emit(hw_mfpr(P5, IPR["EXC_ADDR"]))
    # A livelock ends here: the common handler records it and skips.
    h.emit(hw_ld(P6, tr, T_MISS))
    h.emit(opl(0x10, 0x20, P6, 1, P6))
    h.emit(hw_st(P6, tr, T_MISS))
    h.li(P7, MISS_LIMIT)
    h.emit(opr(0x10, 0x4D, P7, P6, P7))          # CMPLT limit, count
    h.branch(OP["BNE"], P7, "give_up")
    # Find the page: p6 the entry, p7 entries left.
    h.emit(opl(0x12, 0x34, P4, 13, P21))         # SRL va, 13
    h.emit(opl(0x12, 0x39, P21, 13, P21))        # SLL: the page
    h.li(P6, MAP_PA)
    h.li(P7, MAP_ENTRIES)
    h.label("find")
    h.emit(hw_ld(P22, P6, 0))
    h.emit(opr(0x10, 0x2D, P22, P21, P22))       # CMPEQ
    h.branch(OP["BNE"], P22, "found")
    h.emit(lda(P6, P6, 32))
    h.emit(opl(0x10, 0x29, P7, 1, P7))
    h.branch(OP["BGT"], P7, "find")
    h.branch(OP["BR"], R31, "give_up")
    h.label("found")
    h.emit(hw_ld(P22, P6, 8))
    h.branch(OP["BNE"], P22, "fill")
    # Absent: a page fault. What memory holds at T_SNAP_PA now, then the
    # page present, and back to the instruction, which misses again.
    h.emit(hw_ld(P7, tr, T_PF))
    h.emit(opl(0x10, 0x20, P7, 1, P7))
    h.emit(hw_st(P7, tr, T_PF))
    h.emit(hw_ld(P7, tr, T_SNAP_PA))
    h.emit(hw_ld(P7, P7, 0))
    h.emit(hw_st(P7, tr, T_SNAP))
    h.emit(hw_ld(P22, P6, 16))
    h.emit(hw_st(P22, P6, 8))
    h.emit(hw_ret(P5))
    h.label("fill")
    # The page-table page's entry first, at a VA of its own each time.
    h.emit(hw_ld(P7, tr, T_DUMMY))
    h.emit(opl(0x10, 0x20, P7, 1, P7))
    h.emit(hw_st(P7, tr, T_DUMMY))
    h.emit(opl(0x11, 0x00, P7, 0xFF, P7))        # AND 255
    h.emit(opl(0x12, 0x39, P7, 13, P7))
    h.li(P23, DUMMY_VA)
    h.emit(addq(P7, P23, P7))
    h.li(P23, pte(pool_page(255)))
    for ipr in ("DTB_TAG0", "DTB_TAG1"):
        h.emit(hw_mtpr(P7, IPR[ipr]))
    for ipr in ("DTB_PTE0", "DTB_PTE1"):
        h.emit(hw_mtpr(P23, IPR[ipr]))
    # Then the page itself, and back to the instruction.
    for ipr in ("DTB_TAG0", "DTB_TAG1"):
        h.emit(hw_mtpr(P4, IPR[ipr]))
    for ipr in ("DTB_PTE0", "DTB_PTE1"):
        h.emit(hw_mtpr(P22, IPR[ipr]))
    h.emit(hw_ret(P5))
    h.label("give_up")
    h.emit(lda(P4, R31, 0x300))
    h.emit(bra(OP["BR"], R31, (image.PAL_PA + 0x4800 - (h.here + 4)) // 4))
    return h


def build(iterations):
    a = Asm(image.CODE_PA)
    D = 20
    seg = image.SEG48
    a.branch(OP["BR"], R31, "main")
    image.emit_print_hex(a)
    a.label("fatal")
    a.li(image.R_UART, image.UART_VA)
    image.emit_print_text(a, "\n@@FATAL\n")
    a.li(16, kseg(image.TR_PA))
    a.li(17, 7)
    a.branch(OP["BSR"], 18, "print_hex")
    a.emit(call_pal(0x00))

    def res(k, reg):
        a.emit(stq(reg, D, RES + 8 * k))

    def wait_eq(off, reg, tag):
        a.label(tag)
        a.emit(ldq(1, D, off))
        a.emit(opr(0x10, 0x29, 1, reg, 1))
        a.branch(OP["BNE"], 1, tag)

    def check(tag, reg_expect_base, step, rv):
        """r0 <- the elements of $vr(rv) (dumped) that differ from
        (r(reg_expect_base) + i x r(step)) ^ MAGIC; r10 <- 128 - the last
        such element (0: none)."""
        a.li(2, kseg(DUMP_PA))
        a.emit(A.vmem("vstoreq", rv, 2))
        a.emit(lda(0, R31, 0))
        a.emit(lda(10, R31, 0))
        a.emit(lda(3, R31, 128))
        a.emit(opr(0x11, 0x20, reg_expect_base, reg_expect_base, 4))
        a.label(tag)
        a.emit(ldq(5, 2, 0))
        a.emit(opr(0x11, 0x40, 4, 11, 6))        # XOR MAGIC (r11)
        a.emit(opr(0x10, 0x2D, 5, 6, 6))         # CMPEQ
        a.emit(opl(0x11, 0x40, 6, 1, 6))
        a.emit(addq(0, 6, 0))
        a.emit(opr(0x11, 0x26, 6, 3, 10))        # CMOVNE: where it differs
        a.emit(addq(4, step, 4))
        a.emit(lda(2, 2, 8))
        a.emit(opl(0x10, 0x29, 3, 1, 3))
        a.branch(OP["BGT"], 3, tag)

    def counters(k):
        """The handler's counts since the last zeroing: misses, page faults
        into results k, k + 1; then both zeroed."""
        a.li(5, kseg(image.TR_PA))
        a.emit(ldq(1, 5, T_MISS))
        res(k, 1)
        a.emit(ldq(1, 5, T_PF))
        res(k + 1, 1)
        a.emit(stq(R31, 5, T_MISS))
        a.emit(stq(R31, 5, T_PF))

    a.label("main")
    a.li(image.R_UART, image.UART_VA)
    a.li(D, kseg(DATA_PA))
    a.emit(ldq(1, D, WHO))
    a.branch(OP["BNE"], 1, "secondary")
    a.emit(lda(1, R31, 1))
    a.emit(stq(1, D, WHO))
    a.emit(mfc(A.MISC["MB"]))
    # VEN = 1 (each processor reaches its own VCTL on the ES40).
    a.li(1, seg | VCTL_PA)
    a.emit(lda(2, R31, 1))
    a.emit(stq(2, 1, 0))
    a.li(11, MAGIC)
    # The pool: PA ^ MAGIC in every quadword.
    a.li(1, kseg(POOL_PA))
    a.li(2, POOL_PA)
    a.li(3, 256 * 1024)
    a.label("pool")
    a.emit(opr(0x11, 0x40, 2, 11, 4))
    a.emit(stq(4, 1, 0))
    a.emit(lda(1, 1, 8))
    a.emit(lda(2, 2, 8))
    a.emit(opl(0x10, 0x29, 3, 1, 3))
    a.branch(OP["BGT"], 3, "pool")
    a.li(5, kseg(image.TR_PA))
    for off in (T_MISS, T_PF, T_SNAP, T_DUMMY):
        a.emit(stq(R31, 5, off))
    a.emit(A.vc("setvl", lit=128))

    # 1: VLOADQ, vs 8 KB + 8, 128 pages.
    a.emit(call_pal(0x90))
    a.li(9, R1)
    a.li(1, S1)
    a.emit(A.vc("setvs", rb=1))
    a.emit(A.vmem("vloadq", 1, 9))
    counters(0)
    a.emit(A.vc("setvs", lit=8))
    a.li(7, POOL_PA)
    a.li(8, S1)
    check("chk1", 7, 8, 1)
    res(2, 0)
    res(13, 10)
    # 2: VLOADQ, vs 32 MB, 128 pages in one shadow slot.
    a.emit(call_pal(0x90))
    a.li(9, R2)
    a.li(1, S2)
    a.emit(A.vc("setvs", rb=1))
    a.emit(A.vmem("vloadq", 2, 9))
    counters(3)
    a.emit(A.vc("setvs", lit=8))
    a.li(7, pool_page(128))
    a.li(8, 8192)
    check("chk2", 7, 8, 2)
    res(5, 0)
    res(14, 10)
    # 3: VSCATQ of $vr1 + 1 through Vx[i] = i x (8 KB + 8), page 64 absent;
    # the handler snapshots element 0's quadword at the page fault.
    a.emit(call_pal(0x90))
    a.li(1, kseg(IOTA_PA))
    a.emit(A.vmem("vloadq", 4, 1))                # 8 i
    a.li(1, 1025)
    a.emit(A.vs("vsmulq", 4, 1, 4))               # 8 i x 1025 = i (8 KB + 8)
    a.emit(A.vs("vsaddq", 1, 0, 3, lit=1))
    a.li(5, kseg(image.TR_PA))
    a.li(1, POOL_PA)
    a.emit(stq(1, 5, T_SNAP_PA))
    a.li(9, R3)
    a.emit(A.vmem("vscatq", 3, 9, 4))
    counters(6)
    a.li(5, kseg(image.TR_PA))
    a.emit(ldq(1, 5, T_SNAP))
    res(8, 1)
    # Memory after: (PA ^ MAGIC) + 1 for each element, read back by VLOADQ
    # through KSEG at the stride the pool has them.
    a.li(9, kseg(POOL_PA))
    a.li(1, S1)
    a.emit(A.vc("setvs", rb=1))
    a.emit(A.vmem("vloadq", 5, 9))
    a.emit(A.vc("setvs", lit=8))
    a.emit(A.vs("vssubq", 5, 0, 5, lit=1))
    a.li(7, POOL_PA)
    a.li(8, S1)
    check("chk3", 7, 8, 5)
    res(9, 0)
    res(15, 10)

    # Part 2: processor 1 starts and sends; processor 0 receives.
    a.li(5, seg | DPR_START1)
    a.emit(stq(R31, 5, 0))
    a.emit(lda(12, R31, 1))
    wait_eq(ALIVE, 12, "w_alive")
    a.li(13, iterations)
    a.emit(lda(14, R31, 0))                       # errors
    a.emit(lda(15, R31, 1))                       # the round
    a.li(16, kseg(BUF_PA))
    a.li(1, kseg(IOTA_PA))
    a.emit(A.vmem("vloadq", 6, 1))                # $vr6: 8 i
    a.label("recv")
    wait_eq(FLAG, 15, "w_flag")
    a.emit(opl(0x11, 0x00, 15, 1, 1))             # odd rounds: DRAINM
    a.branch(OP["BNE"], 1, "use_drainm")
    a.emit(mfc(A.MISC["MB"]))
    a.branch(OP["BR"], R31, "barrier_done")
    a.label("use_drainm")
    a.emit(mfc(A.DRAINM_FN))
    a.label("barrier_done")
    a.emit(A.vmem("vloadq", 7, 16))
    # Element i must be round x 1024 + 8 i: $vr7 - $vr6 - round x 1024.
    a.emit(A.vv("vvsubq", 7, 6, 7))
    a.emit(opl(0x12, 0x39, 15, 10, 1))
    a.emit(A.vs("vscmpne", 7, 1, 8))
    a.li(2, kseg(DUMP_PA))
    a.emit(A.vmem("vstoreq", 8, 2))
    a.emit(lda(3, R31, 128))
    a.label("sum")
    a.emit(ldq(4, 2, 0))
    a.emit(addq(14, 4, 14))
    a.emit(lda(2, 2, 8))
    a.emit(opl(0x10, 0x29, 3, 1, 3))
    a.branch(OP["BGT"], 3, "sum")
    a.emit(mfc(A.MISC["MB"]))
    a.emit(stq(15, D, ACK))
    a.emit(opl(0x10, 0x20, 15, 1, 15))
    a.emit(opr(0x10, 0x3D, 15, 13, 1))            # CMPULE round, N
    a.branch(OP["BNE"], 1, "recv")
    res(10, 14)
    a.emit(opl(0x10, 0x29, 15, 1, 15))
    res(11, 15)
    a.li(5, kseg(image.TR_PA))
    a.emit(ldq(1, 5, image.TR_TOTAL))
    res(12, 1)
    image.emit_print_text(a, "\n@@BEGIN\n")
    a.li(16, kseg(DATA_PA + RES))
    a.li(17, NRES)
    a.branch(OP["BSR"], 18, "print_hex")
    image.emit_print_text(a, "@@END\n")
    a.emit(call_pal(0x00))

    # Processor 1: VEN, then the messages.
    a.label("secondary")
    a.li(1, seg | VCTL_PA)
    a.emit(lda(2, R31, 1))
    a.emit(stq(2, 1, 0))
    a.emit(A.vc("setvl", lit=128))
    a.emit(A.vc("setvs", lit=8))
    a.li(1, kseg(IOTA_PA))
    a.emit(A.vmem("vloadq", 6, 1))                # 8 i
    a.li(16, kseg(BUF_PA))
    a.emit(lda(12, R31, 1))
    a.emit(stq(12, D, ALIVE))
    a.emit(lda(15, R31, 1))
    a.label("send")
    a.emit(opl(0x12, 0x39, 15, 10, 1))            # round x 1024
    a.emit(opl(0x11, 0x00, 15, 1, 2))
    a.branch(OP["BNE"], 2, "send_scalar")
    a.emit(A.vs("vsaddq", 6, 1, 9))
    a.emit(A.vmem("vstoreq", 9, 16))
    a.branch(OP["BR"], R31, "sent")
    a.label("send_scalar")
    a.emit(opr(0x11, 0x20, 16, 16, 3))
    a.emit(lda(4, R31, 128))
    a.label("st_loop")
    a.emit(stq(1, 3, 0))
    a.emit(lda(1, 1, 8))
    a.emit(lda(3, 3, 8))
    a.emit(opl(0x10, 0x29, 4, 1, 4))
    a.branch(OP["BGT"], 4, "st_loop")
    a.label("sent")
    a.emit(mfc(A.MISC["WMB"]))
    a.emit(stq(15, D, FLAG))
    wait_eq(ACK, 15, "w_ack")
    a.emit(opl(0x10, 0x20, 15, 1, 15))
    a.branch(OP["BR"], R31, "send")
    code = a.finish()

    img = bytearray(image.IMAGE_TOP)
    pal = image.build_pal(image.TR_PA, kseg(a.labels["main"]),
                          kseg(a.labels["fatal"]))
    h = handler()
    hw = h.finish()
    for i, w in enumerate(hw):
        pal[h.base + 4 * i] = w
    # The DTB-miss vector: a branch to the handler.
    vec = image.PAL_PA + 0x300
    pal[vec] = bra(OP["BR"], R31, (h.base - (vec + 4)) // 4)
    # CALL_PAL 90: DTB_IA.
    e = image.PAL_PA + 0x2000 + 0x1000 + (0x10 << 6)
    pal[e] = hw_mtpr(R31, IPR["DTB_IA"])
    pal[e + 4] = hw_ret(P23)
    for ad, w in pal.items():
        struct.pack_into("<I", img, ad, w)
    for i, w in enumerate(code):
        struct.pack_into("<I", img, image.CODE_PA + 4 * i, w)
    for i in range(128):
        struct.pack_into("<Q", img, IOTA_PA + 8 * i, 8 * i)
    for k, (va, p, p2) in enumerate(mapping()):
        struct.pack_into("<QQQ", img, MAP_PA + 32 * k, va, p, p2)
    return img


CFG = """sys0 = tsunami
{
  memory.bits = 26;
  rom.srm = "absent.exe";
  rom.decompressed = "oracle.rom";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  exit_on_pal_halt = true;

  cpu0 = %(cpu)s
  {
    speed = 800M;
  }
  cpu1 = %(cpu)s
  {
    speed = 800M;
  }

  serial0 = serial
  {
    address = "127.0.0.1";
    port = %(port)d;
  }

  pci0.7 = ali
  {
  }
}
"""


def expected():
    snap = POOL_PA ^ MAGIC
    return [
        ("1 vs 8 KB + 8: misses", 0, 128),
        ("1: page faults", 1, 0),
        ("1: elements wrong", 2, 0),
        ("2 vs 32 MB: misses", 3, 128),
        ("2: page faults", 4, 0),
        ("2: elements wrong", 5, 0),
        ("3 VSCATQ, page 64 absent: misses", 6, 129),
        ("3: page faults", 7, 1),
        ("3: element 0's quadword at the page fault", 8, snap),
        ("3: elements wrong after", 9, 0),
        ("messages: elements wrong", 10, 0),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="tarantula-mem")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--iterations", type=int, default=2000)
    ap.add_argument("--timeout", type=float, default=120)
    args = ap.parse_args()
    work = os.environ.get("ALPHABOX_WORK", os.path.join(HERE, "..", "..", "lab"))
    workdir = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    image.write_rom(os.path.join(workdir, "oracle.rom"), build(args.iterations))
    port = args.port or cpu_oracle.free_port()
    env = {"ALPHABOX_CPU_ANY_BOARD": "1"}
    orig = cpu_oracle.CFG
    cpu_oracle.CFG = CFG
    text, emu, secs = cpu_oracle.run(os.path.abspath(args.binary), workdir,
                                     "ev8", port, env, args.timeout)
    cpu_oracle.CFG = orig
    with open(os.path.join(workdir, "serial.txt"), "w") as f:
        f.write(text)
    print("== tarantula_mem %s: two ev8s on the ES40, %.1f s" % (args.label, secs))
    vals = []
    i = text.find("@@BEGIN")
    if i >= 0:
        for ln in text[i:].replace("\r", "").split("\n")[1:]:
            ln = ln.strip()
            if ln == "@@END":
                break
            if ln:
                vals.append(int(ln, 16))
    if len(vals) != NRES:
        print("  no results (see %s)" % workdir)
        for ln in (text[text.find("@@FATAL"):].split("\n")[:9]
                   if "@@FATAL" in text else []):
            print("   |", ln)
        for ln in emu.splitlines()[-10:]:
            print("   |", ln)
        print("  TARANTULA MEM: FAIL")
        return 1
    ok = True
    for name, k, want in expected():
        good = vals[k] == want
        ok = ok and good
        f = "%x" if "quadword" in name else "%d"
        print(("  %-44s " + f + " (expected " + f + "): %s") % (
            name, vals[k], want, "ok" if good else "FAIL"))
    for k, at in ((2, 13), (5, 14), (9, 15)):
        if vals[k]:
            print("  (scenario %d: the last element wrong is %d)" % (
                (2, 5, 9).index(k) + 1, 128 - vals[at]))
    rounds_ok = vals[11] == args.iterations
    print("  messages passed: %d (expected %d): %s" % (
        vals[11], args.iterations, "ok" if rounds_ok else "FAIL"))
    print("  exceptions taken outside the handler: %d (expected 0)" % vals[12])
    ok = ok and rounds_ok and vals[12] == 0
    print("  TARANTULA MEM: %s" % ("PASS" if ok else "FAIL"))
    if ok and not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print("  run directory: %s" % workdir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
