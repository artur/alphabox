#!/usr/bin/env python3
"""Write a bootable image whose code runs a known number of Alpha
instructions and halts.

A guest at its desktop tells you almost nothing about how fast the emulator
executes code: it is asleep, and what little it runs is its own idle loop.
This builds the other kind of measurement -- a loop of exactly the shape and
length we ask for, with the instruction count known in advance, so the time
it takes is a throughput number and nothing else.

The image is an SRM boot block (see make_halt_floppy.py for the format) whose
payload is:

    prologue    put the iteration count in r1, clear the scratch registers
    loop:       <body-1 integer operate instructions>
                subq r1, 1, r1
                bne  r1, loop
    call_pal 0  HALT

so the instruction count is prologue + iterations * body + 1, and `--count`
prints it without writing anything.

The body length is the interesting knob. A long body measures how well the
translated code itself runs; a short one measures what it costs to leave a
block and enter the next, which is where a block-at-a-time JIT spends its
overhead. Comparing the two says which of the two to work on.
"""
import argparse
import struct
import sys

# Alpha instruction encodings, from the Architecture Handbook's formats.
OP_LDA, OP_LDAH = 0x08, 0x09
OP_LDQ, OP_STQ = 0x29, 0x2D
OP_INTA, OP_INTL = 0x10, 0x11
OP_BR, OP_BNE = 0x30, 0x3D
F_ADDQ, F_SUBQ, F_CMPULT = 0x20, 0x29, 0x1D
F_AND, F_BIS, F_XOR = 0x00, 0x20, 0x40


def operate(op, ra, rb, func, rc):
    return (op << 26) | (ra << 21) | (rb << 16) | (func << 5) | rc


def operate_lit(op, ra, lit, func, rc):
    return (op << 26) | (ra << 21) | ((lit & 0xFF) << 13) | (1 << 12) | (func << 5) | rc


def memfmt(op, ra, rb, disp):
    return (op << 26) | (ra << 21) | (rb << 16) | (disp & 0xFFFF)


def branch(op, ra, disp):
    return (op << 26) | (ra << 21) | (disp & 0x1FFFFF)


# The JIT keeps a fixed set of guest registers in host registers (see
# kA64Pins / kA64CallerPins in jitemit_a64.hpp); every other register is a
# load and a store around each use. Which set a loop uses therefore changes
# what it measures, so both are available: "pinned" is the best the register
# allocator does today, "spilled" the worst, and real code is in between.
# r1 is the loop counter and belongs to neither set.
REGISTER_SETS = {
    "pinned": [2, 3, 9, 10, 11, 17, 18, 26],
    "spilled": [4, 5, 6, 7, 8, 12, 13, 14],
}


def build_code(iterations, body, regs="pinned", mix="alu"):
    """The loop, as a list of instruction words, and the count it executes."""
    if body < 2:
        sys.exit("a loop body is at least a decrement and a branch")

    # The iteration count, built high half first. LDA sign-extends its
    # displacement, so a low half of 0x8000 or more borrows from the high.
    high, low = divmod(iterations, 1 << 16)
    if low >= 0x8000:
        high += 1
    if high >= 0x8000:
        sys.exit("iteration count too large for one LDAH/LDA pair")
    code = [
        memfmt(OP_LDAH, 1, 31, high),
        memfmt(OP_LDA, 1, 1, low),
        # Scratch registers with values the arithmetic below cannot trap on
        # (integer operates never trap, but keep them small and readable).
    ]
    for i, r in enumerate(REGISTER_SETS[regs]):
        code.append(memfmt(OP_LDA, r, 31, 0x1234 + i * 0x111))
    prologue = len(code)

    # The body: a repeating pattern of integer operates that depend on each
    # other just enough not to be trivially reorderable, then the counter and
    # the branch back.
    a, b, c, d, e, f, g, h = REGISTER_SETS[regs]
    pattern = [
        operate(OP_INTA, a, b, F_ADDQ, a),
        operate(OP_INTL, c, d, F_XOR, c),
        operate(OP_INTA, a, c, F_ADDQ, e),
        operate(OP_INTL, e, b, F_BIS, f),
        operate(OP_INTA, f, d, F_CMPULT, g),
        operate(OP_INTL, g, a, F_AND, h),
        operate(OP_INTA, h, e, F_ADDQ, b),
        operate(OP_INTL, b, f, F_XOR, d),
    ]
    if mix == "mem":
        # Real guest code is full of loads and stores, and each one costs the
        # translated code an address calculation and a probe of the page cache
        # before the access itself. This pattern alternates a load, a store
        # and an operate on what was loaded, all within one page, so the probe
        # always hits and what is measured is the fast path rather than the
        # helper behind it.
        #
        # The scratch page is our own code's, a kilobyte past the loop: a
        # branch-to-self puts the address of the next instruction in a
        # register, which is the only way this code can learn where it was
        # loaded. It is written to but never executed.
        scratch = h
        code.append(branch(OP_BR, scratch, 0))       # scratch = &next
        code.append(memfmt(OP_LDA, scratch, scratch, 1024))
        prologue += 2
        pattern = [
            memfmt(OP_LDQ, a, scratch, 0),
            operate(OP_INTA, a, b, F_ADDQ, a),
            memfmt(OP_STQ, a, scratch, 8),
            memfmt(OP_LDQ, c, scratch, 16),
            operate(OP_INTL, c, d, F_XOR, c),
            memfmt(OP_STQ, c, scratch, 24),
            memfmt(OP_LDQ, e, scratch, 32),
            operate(OP_INTA, e, a, F_ADDQ, e),
        ]

    loop = [pattern[i % len(pattern)] for i in range(body - 2)]
    loop.append(operate_lit(OP_INTA, 1, 1, F_SUBQ, 1))
    # The branch is the last instruction of the loop, so it jumps back over
    # everything before it: displacement counts instructions from the one
    # after the branch.
    loop.append(branch(OP_BNE, 1, -(body)))

    code += loop
    code.append(0x00000000)  # CALL_PAL HALT
    return code, prologue + iterations * body + 1


def write_image(path, code, size, block_size=512):
    per_block = block_size // 4
    blocks = (len(code) + per_block - 1) // per_block
    img = bytearray(max(size, block_size * (blocks + 1)))

    boot = bytearray(block_size)
    struct.pack_into("<QQQ", boot, 0x1E0, blocks, 1, 0)
    csum = sum(struct.unpack_from("<63Q", boot, 0)) & ((1 << 64) - 1)
    struct.pack_into("<Q", boot, 0x1F8, csum)
    img[0:block_size] = boot

    for i, word in enumerate(code):
        struct.pack_into("<I", img, block_size + i * 4, word)

    with open(path, "wb") as f:
        f.write(img)
    return blocks


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", nargs="?", help="image to write (a floppy by default)")
    ap.add_argument("--iterations", type=int, default=20_000_000)
    ap.add_argument("--mix", choices=("alu", "mem"), default="alu",
                    help="integer operates only, or a load/store/operate "
                         "pattern in one page (default alu)")
    ap.add_argument("--regs", choices=sorted(REGISTER_SETS), default="pinned",
                    help="use guest registers the JIT pins to host registers, "
                         "or ones it does not (default pinned)")
    ap.add_argument("--body", type=int, default=32,
                    help="instructions per iteration, the decrement and "
                         "branch included (default 32)")
    ap.add_argument("--size-mb", type=float, default=1.44 * 1024 * 1024 / 1e6,
                    help="image size; the default is a 1.44 MB floppy")
    ap.add_argument("--count", action="store_true",
                    help="print the instruction count and write nothing")
    args = ap.parse_args()

    code, count = build_code(args.iterations, args.body, args.regs, args.mix)
    if args.count:
        print(count)
        return 0
    if not args.image:
        ap.error("an image path is required unless --count is given")

    size = 1474560 if abs(args.size_mb - 1.47456) < 0.001 else int(args.size_mb * 1e6)
    blocks = write_image(args.image, code, size)
    print("%s: %d instructions in %d block(s), %d per iteration, %s registers,"
          " %s mix" % (args.image, count, blocks, args.body, args.regs, args.mix))
    return 0


if __name__ == "__main__":
    sys.exit(main())
