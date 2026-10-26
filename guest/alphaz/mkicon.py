#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# mkicon.py OUT.ico: AlphaZ's icon, a white Z on a teal rounded square, at
# 32x32 and 16x16, 24 bits a pixel with a transparency mask, which every
# Windows from NT 4.0 on draws. Drawn here rather than kept as a binary in
# the tree; the same bytes every run.
# Copyright (C) 2026 Artur Goulão
import struct
import sys

TEAL = (0, 128, 128)
EDGE = (0, 80, 80)
WHITE = (255, 255, 255)


def pixel(x, y):
    """The colour at (x, y) of the 32x32 design (fractions allowed), or
    None outside the rounded square."""
    r = 5.0
    cx = min(max(x, 1 + r), 31 - r)
    cy = min(max(y, 1 + r), 31 - r)
    d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
    if x < 1 or x > 31 or y < 1 or y > 31 or d > r:
        return None
    if d > r - 1.2 or x < 2.2 or x > 29.8 or y < 2.2 or y > 29.8:
        return EDGE
    if 8 <= x <= 24 and (7 <= y <= 10.5 or 21.5 <= y <= 25):
        return WHITE
    if 10.5 < y < 21.5:
        xl = 24 - (y - 10.5) * (16.0 / 11.0)
        if abs(x - xl) <= 2.4:
            return WHITE
    return TEAL


def image(size):
    s = 32.0 / size
    rows, mask = [], []
    for y in range(size - 1, -1, -1):      # bottom-up, as a DIB is
        row, bits = b"", 0
        for x in range(size):
            c = pixel((x + 0.5) * s, (y + 0.5) * s)
            if c is None:
                row += b"\0\0\0"
                bits |= 1 << (size - 1 - x)
            else:
                row += bytes((c[2], c[1], c[0]))
        rows.append(row + b"\0" * (-len(row) % 4))
        m = bits.to_bytes(size // 8, "big")
        mask.append(m + b"\0" * (-len(m) % 4))
    xor, andm = b"".join(rows), b"".join(mask)
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 24, 0, len(xor) + len(andm), 0, 0, 0, 0)
    return hdr + xor + andm


def main():
    imgs = [image(32), image(16)]
    out = struct.pack("<HHH", 0, 1, len(imgs))
    off = 6 + 16 * len(imgs)
    for size, im in zip((32, 16), imgs):
        out += struct.pack("<BBBBHHII", size, size, 0, 0, 1, 24, len(im), off)
        off += len(im)
    out += b"".join(imgs)
    with open(sys.argv[1], "wb") as f:
        f.write(out)


main()
