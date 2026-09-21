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

The instruction it writes sets a register the loop then carries to a halt.
So the emulator halts if and only if the new instruction is what ran; if the
block compiled from the old words runs instead, the register stays clear and
the guest spins forever. Success and failure are therefore not a printed
claim but the difference between a guest that stops and one that never does.

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
OP_BR, OP_BEQ, OP_BNE = 0x30, 0x39, 0x3D
F_ADDQ, F_SUBQ, F_XOR, F_BIS = 0x20, 0x29, 0x40, 0x20

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

    The shape matters. An earlier version patched the loop's own exit branch
    into a branch to the halt, which the INTERPRETER would carry out as soon
    as it re-read the word -- and since an IMB always invalidates the
    instruction cache, that is now what happens, whatever the compiled block
    cache believes. It proved nothing about compiled code. So the patched
    word writes a REGISTER instead, the loop runs again long enough to be
    dispatched as compiled code, and the register decides the ending:

        r5 == 0  every turn of the loop ran the patched word  -> halt
        r5 != 0  some turn ran the words compiled before it   -> spin

    The counting matters. An earlier version had the patched word SET the
    flag, so a single interpreted turn out of two thousand was enough to
    report success while the compiled block went on running stale code. The
    original word counts and the patched one does nothing, so one stale turn
    out of two thousand is enough to report failure.
    """
    if body < 5:
        sys.exit("the loop body holds the flag store, an operate, a decrement "
                 "and a branch")

    code = [branch(OP_BR, 2, 0)]        # r2 = &word[1], then fall through
    HERE = 1                            # what r2 points at, in words
    code.append(memfmt(OP_LDA, 1, 31, iterations))  # r1 = iterations
    code.append(memfmt(OP_LDA, 3, 31, 0x111))
    code.append(memfmt(OP_LDA, 4, 31, 0x222))
    code.append(operate(OP_INTL, 31, 31, F_BIS, 8))  # r8 = 0 (first pass)

    loop = len(code)
    flag = None
    for i in range(body - 2):
        if i == 0:
            flag = len(code)            # the word the guest rewrites
            code.append(memfmt(OP_LDA, 5, 5, 1))  # r5 += 1 (the old word)
        elif i % 2:
            code.append(operate(OP_INTA, 3, 4, F_ADDQ, 3))
        else:
            code.append(operate(OP_INTL, 3, 4, F_XOR, 6))
    code.append(operate_lit(OP_INTA, 1, 1, F_SUBQ, 1))  # subq r1, 1, r1
    code.append(branch(OP_BNE, 1, loop - (len(code) + 1)))

    # Second time round we are here to read the verdict, not to patch again.
    check_br = len(code)
    code.append(0)                      # bne r8, check   (filled in below)

    # First pass: settle the emulator's own bookkeeping with an IMB that has
    # nothing to report (compiling the loop marked its page, and marking a
    # page counts as a write), then rewrite the flag instruction, make it
    # visible, and run the loop again -- long enough to be dispatched as the
    # compiled block it already is.
    code.append(memfmt(OP_LDA, 8, 31, 1))           # r8 = 1 (second pass)
    code.append(call_pal(PAL_IMB))
    tail = len(code)
    code.append(0)  # LDAH r7, hi
    code.append(0)  # LDA  r7, lo
    code.append(memfmt(OP_STL, 7, 2, (flag - HERE) * 4))
    code.append(call_pal(PAL_IMB))
    code.append(operate(OP_INTL, 31, 31, F_BIS, 5))  # r5 = 0: count afresh
    code.append(memfmt(OP_LDA, 1, 31, 2000))
    code.append(branch(OP_BR, 31, loop - (len(code) + 1)))

    check = len(code)
    code[check_br] = branch(OP_BNE, 8, check - (check_br + 1))
    halt_from = len(code)
    code.append(0)                      # beq r5, halt  (filled in below)
    code.append(branch(OP_BR, 31, -1))  # r5 != 0: a stale turn; spin here
    halt = len(code)
    code[halt_from] = branch(OP_BEQ, 5, halt - (halt_from + 1))
    code.append(call_pal(PAL_HALT))     # r5 == 0: the patched word ran

    # The word the guest stores over the flag instruction: do nothing.
    new_ins = operate(OP_INTL, 31, 31, F_BIS, 31) & 0xFFFFFFFF
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
