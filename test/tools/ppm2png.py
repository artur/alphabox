#!/usr/bin/env python3
# ppm2png.py in.ppm out.png -- convert an AXPBOX_DUMP_FB P6 dump to PNG
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
p = d.split(b'\n', 3)
w, h = map(int, p[1].split())
px = p[3]
raw = b''.join(b'\x00' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
def ch(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
