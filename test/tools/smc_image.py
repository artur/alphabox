#!/usr/bin/env python3
"""Write a boot block that modifies its own compiled code and halts.

The emulator skips an instruction-cache flush when nothing has been written
to memory any block was compiled from (CCodePageMap, CAlphaCPU::flush_icache).
That is safe only while every path that writes guest memory reports what it
wrote, and the way to be sure is a guest that actually does it:

    - a loop runs long enough that the JIT compiles the block it is in;
    - the guest then stores a different instruction over one INSIDE that
      compiled block, and executes CALL_PAL IMB, exactly as an operating
      system loading code does;
    - it re-enters the loop.

The instruction it writes is a branch to a CALL_PAL HALT. So the emulator
halts if and only if the new instruction is what runs. If the stale compiled
block runs instead, the original conditional branch takes the guest around
the loop and back to the patch, forever -- the run hangs, and the harness
times out. Success and failure are therefore not a printed claim but the
difference between a guest that stops and one that never does.

The loop body is deliberately plain integer arithmetic on registers the JIT
pins, so nothing but the patched instruction decides where control goes.
"""
import argparse
import struct
import sys

OP_CALL_PAL = 0x00
OP_LDA, OP_LDAH = 0x08, 0x09
OP_STL = 0x2C
OP_INTA, OP_INTL = 0x10, 0x11
OP_BR, OP_BNE = 0x30, 0x3D
F_ADDQ, F_SUBQ, F_XOR = 0x20, 0x29, 0x40

PAL_HALT, PAL_IMB = 0x0000, 0x0086


def operate(op, ra, rb, func, rc):
    return (op << 26) | (ra << 21) | (rb << 16) | (func << 5) | rc


def operate_lit(op, ra, lit, func, rc):
    return (op << 26) | (ra << 21) | ((lit & 0xFF) << 13) | (1 << 12) | (func << 5) | rc


def memfmt(op, ra, rb, disp):
    return (op << 26) | (ra << 21) | (rb << 16) | (disp & 0xFFFF)


def branch(op, ra, disp):
    return (op << 26) | (ra << 21) | (disp & 0x1FFFFF)


def call_pal(func):
    return (OP_CALL_PAL << 26) | (func & 0x3FFFFFF)


def build_code(iterations, body):
    """The image, as instruction words.

    Word 0 leaves the address of word 1 in r2, which is how the patch finds
    itself: the boot block does not know where SRM loaded it.
    """
    if body < 4:
        sys.exit("the loop body holds three operates, a decrement and a branch")

    code = [branch(OP_BR, 2, 0)]        # r2 = &word[1], then fall through
    HERE = 1                            # what r2 points at, in words
    code.append(memfmt(OP_LDA, 1, 31, iterations))  # r1 = iterations
    code.append(memfmt(OP_LDA, 3, 31, 0x111))
    code.append(memfmt(OP_LDA, 4, 31, 0x222))

    loop = len(code)
    for i in range(body - 2):
        if i % 3 == 0:
            code.append(operate(OP_INTA, 3, 4, F_ADDQ, 3))
        elif i % 3 == 1:
            code.append(operate(OP_INTL, 3, 4, F_XOR, 5))
        else:
            code.append(operate(OP_INTA, 5, 3, F_ADDQ, 6))
    code.append(operate_lit(OP_INTA, 1, 1, F_SUBQ, 1))  # subq r1, 1, r1

    patch = len(code)                   # the instruction the guest rewrites
    code.append(branch(OP_BNE, 1, loop - (patch + 1)))

    # r1 has reached zero. First an IMB with nothing to report: compiling
    # the loop marked its page, and marking a page counts as a write, so
    # this one settles that and leaves the emulator's own bookkeeping with
    # nothing outstanding. Without it the patch below would be covered by
    # that earlier bookkeeping rather than by its own store, and the test
    # could not tell a tracked store from an untracked one.
    code.append(call_pal(PAL_IMB))

    # Now rewrite the branch above into one that leaves the loop for the
    # halt, make it visible with IMB, and go back into the loop.
    tail = len(code)
    code.append(0)  # LDAH r7, hi   (filled in once `done` is known)
    code.append(0)  # LDA  r7, lo
    code.append(memfmt(OP_STL, 7, 2, (patch - HERE) * 4))
    code.append(call_pal(PAL_IMB))
    code.append(memfmt(OP_LDA, 1, 31, 5))           # a few more times round
    code.append(branch(OP_BR, 31, loop - (len(code) + 1)))

    done = len(code)
    code.append(call_pal(PAL_HALT))

    # The word the guest stores over `patch`: an unconditional branch to the
    # halt, as it would be encoded at the patch's own address.
    new_ins = branch(OP_BR, 31, done - (patch + 1)) & 0xFFFFFFFF
    hi, lo = divmod(new_ins, 1 << 16)
    if lo >= 0x8000:                    # LDA sign-extends its displacement
        hi += 1
    code[tail] = memfmt(OP_LDAH, 7, 31, hi & 0xFFFF)
    code[tail + 1] = memfmt(OP_LDA, 7, 7, lo)
    return code


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
    ap.add_argument("image")
    ap.add_argument("--iterations", type=int, default=4000,
                    help="times round the loop before the patch (default "
                         "4000: well past the JIT's compile threshold)")
    ap.add_argument("--body", type=int, default=8,
                    help="instructions per iteration (default 8)")
    ap.add_argument("--size", type=int, default=1474560, help="image bytes")
    ap.add_argument("--disasm", action="store_true",
                    help="print the words and what they are")
    args = ap.parse_args()

    code = build_code(args.iterations, args.body)
    if args.disasm:
        for i, w in enumerate(code):
            print("%3d  %08x" % (i, w))
    blocks = write_image(args.image, code, args.size)
    print("%s: %d instructions in %d block(s)" % (args.image, len(code), blocks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
