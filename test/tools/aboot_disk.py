#!/usr/bin/env python3
"""Make a sparse disk image SRM boots Linux from with aboot (docs/linux.md).

usage: aboot_disk.py <image> <size_MiB> <bootlx> [boot_MiB]

<bootlx> is aboot's primary bootstrap, /boot/bootlx on a Linux/Alpha CD.
Layout, in 512-byte sectors:
  0          the SRM boot block (block count at 0x1E0, starting block at
             0x1E8, flags at 0x1F0, at 0x1F8 the sum of the first 63
             quadwords), with a BSD disklabel at byte 64 (the Alpha's
             LABELOFFSET), inside what that sum covers
  1..        bootlx
  2048..     partition a: aboot's partition 1, Linux's sda1, for /boot
             (boot_MiB, default 128; make it ext2 without extents)
  then       partition b: aboot's 2, Linux's sda2, the rest, for /
An existing file is never overwritten.
"""
import os
import struct
import sys

SECT = 512
DISKLABELMAGIC = 0x82564557
FS_EXT2 = 8


def disklabel(total, parts):
    """A BSD disklabel for <total> sectors with (offset, size) partitions."""
    nsect, ntrk = 63, 16
    spc = nsect * ntrk
    lab = struct.pack(
        "<IHH16s16sIIIIIIHHIHHHHIII5I5IIHHII",
        DISKLABELMAGIC, 4, 0, b"alphabox", b"linux",
        SECT, nsect, ntrk, total // spc, spc, total,
        0, 0, 0,  # spares per track and cylinder, alternate cylinders
        3600, 1, 0, 0,  # rpm, interleave, track and cylinder skew
        0, 0, 0,  # head switch, track seek, flags
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  # drive data, spare
        DISKLABELMAGIC, 0, len(parts), 8192, 8192)
    assert len(lab) == 148
    for off, size in parts:
        lab += struct.pack("<IIIBBH", size, off, 1024, FS_EXT2, 0, 0)
    # dkcksum: the XOR of the label's 16-bit words, with d_checksum zero.
    ck = 0
    for i in range(0, len(lab), 2):
        ck ^= struct.unpack_from("<H", lab, i)[0]
    return lab[:136] + struct.pack("<H", ck) + lab[138:]


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    img, size_mib, bootlx = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    boot_mib = int(sys.argv[4]) if len(sys.argv) == 5 else 128
    if os.path.exists(img):
        sys.exit("aboot_disk: refusing to overwrite " + img)
    total = size_mib * 2048
    bl = open(bootlx, "rb").read()
    bl_sects = (len(bl) + SECT - 1) // SECT
    a = (2048, boot_mib * 2048)
    b = (a[0] + a[1], total - a[0] - a[1])
    if 1 + bl_sects > a[0] or b[1] <= 0:
        sys.exit("aboot_disk: does not fit")

    s0 = bytearray(SECT)
    lab = disklabel(total, [a, b])
    s0[64:64 + len(lab)] = lab
    struct.pack_into("<QQQ", s0, 0x1E0, bl_sects, 1, 0)
    csum = sum(struct.unpack_from("<63Q", s0, 0)) & (2**64 - 1)
    struct.pack_into("<Q", s0, 0x1F8, csum)

    with open(img, "wb") as f:
        f.write(s0)
        f.write(bl)
        f.truncate(total * SECT)
    print("%s: %d sectors; bootlx %d sectors at 1; a %d+%d; b %d+%d"
          % (img, total, bl_sects, a[0], a[1], b[0], b[1]))


if __name__ == "__main__":
    main()
