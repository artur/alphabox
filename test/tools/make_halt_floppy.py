#!/usr/bin/env python3
"""Write a 1.44 MB floppy image whose SRM boot block runs a CALL_PAL HALT.

SRM's "boot dva0" reads block 0, loads <count> blocks starting at <LBA> and
jumps to them in kernel mode. Block 0 holds the Alpha boot block fields:
count at 0x1E0, starting LBA at 0x1E8, flags at 0x1F0 and, at 0x1F8, the sum
of the first 63 quadwords. Block 1 is all zeros, and longword 0 is
CALL_PAL 0x0000 (HALT).

Use: exit_on_pal_halt = true makes the emulator exit gracefully a few seconds
after "boot dva0"; without it SRM prints "HALT instruction executed".
"""
import struct
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make_halt_floppy.py <image>")
    img = bytearray(1474560)
    b0 = bytearray(512)
    struct.pack_into("<QQQ", b0, 0x1E0, 1, 1, 0)  # 1 block from LBA 1, flags 0
    csum = sum(struct.unpack_from("<63Q", b0, 0)) & (2**64 - 1)
    struct.pack_into("<Q", b0, 0x1F8, csum)
    img[0:512] = b0
    with open(sys.argv[1], "wb") as f:
        f.write(img)
    return 0


if __name__ == "__main__":
    sys.exit(main())
