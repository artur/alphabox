#!/usr/bin/env python3
"""Build and inspect small FAT16 disk images for storage tests.

  fat_disk.py make <image> [--size-mb N] [--data-kb N] [--seed S]
      Create an MBR disk with one FAT16 partition holding HELLO.TXT and
      DATA.BIN (N KB of seeded pseudo-random bytes), so a guest can read,
      copy and write files on it.

  fat_disk.py get <image> <NAME.EXT> [<out>]
      Extract a root-directory file (8.3 name) from such an image, e.g. a
      copy a guest wrote. Prints its size and MD5.

  fat_disk.py check <image> <NAME.EXT> <REFERENCE.EXT>
      Compare two root-directory files; exit status 0 when identical.

Only what these tests need: one partition, root directory entries, 8.3
names, FAT16 cluster chains. Uses only the standard library.
"""
import argparse
import hashlib
import random
import struct
import sys

SECTOR = 512
PART_START = 63          # classic CHS-aligned first partition
SEC_PER_CLUSTER = 4
RESERVED = 1
NUM_FATS = 2
ROOT_ENTRIES = 512


def geometry(total_sectors):
    part_sectors = total_sectors - PART_START
    # FAT size: enough 16-bit entries for every cluster
    fat_sectors = 1
    while True:
        data = part_sectors - RESERVED - NUM_FATS * fat_sectors - ROOT_ENTRIES * 32 // SECTOR
        clusters = data // SEC_PER_CLUSTER
        if (clusters + 2) * 2 <= fat_sectors * SECTOR:
            return part_sectors, fat_sectors, clusters
        fat_sectors += 1


def layout(img):
    """Parse the partition and boot sector; return the offsets we need."""
    mbr = img[:SECTOR]
    start = struct.unpack_from("<I", mbr, 446 + 8)[0]
    bs = start * SECTOR
    bps, spc, rsv, nfats, nroot = struct.unpack_from("<HBHBH", img, bs + 11)
    fatsz = struct.unpack_from("<H", img, bs + 22)[0]
    fat = bs + rsv * bps
    root = fat + nfats * fatsz * bps
    data = root + nroot * 32
    return dict(fat=fat, root=root, nroot=nroot, data=data, csize=spc * bps)


def make(args):
    total = args.size_mb * 1024 * 1024 // SECTOR
    part_sectors, fat_sectors, clusters = geometry(total)
    img = bytearray(total * SECTOR)

    # MBR: one active FAT16 (type 0x06) partition
    entry = struct.pack("<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x06, b"\xfe\xff\xff",
                        PART_START, part_sectors)
    img[446:446 + 16] = entry
    img[510:512] = b"\x55\xaa"

    bs = PART_START * SECTOR
    boot = bytearray(SECTOR)
    boot[0:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"AXPBOX  "
    struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, SEC_PER_CLUSTER, RESERVED,
                     NUM_FATS, ROOT_ENTRIES, 0 if part_sectors > 0xFFFF else part_sectors,
                     0xF8, fat_sectors, 63, 255, PART_START,
                     part_sectors if part_sectors > 0xFFFF else 0)
    struct.pack_into("<BBBI11s8s", boot, 36, 0x80, 0, 0x29, 0x1234ABCD,
                     b"AXPBOXTEST ", b"FAT16   ")
    boot[510:512] = b"\x55\xaa"
    img[bs:bs + SECTOR] = boot

    lay = layout(img)
    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)
    root = bytearray(ROOT_ENTRIES * 32)
    root[0:11] = b"AXPBOXTEST "
    root[11] = 0x08  # volume label
    next_cluster = [2]

    def add_file(slot, name, content):
        n = max(1, -(-len(content) // lay["csize"]))
        first = next_cluster[0]
        for i in range(n):
            c = first + i
            struct.pack_into("<H", fat, c * 2, 0xFFFF if i == n - 1 else c + 1)
            off = lay["data"] + (c - 2) * lay["csize"]
            chunk = content[i * lay["csize"]:(i + 1) * lay["csize"]]
            img[off:off + len(chunk)] = chunk
        next_cluster[0] += n
        base, ext = name.split(".")
        e = slot * 32
        root[e:e + 11] = base.ljust(8).encode() + ext.ljust(3).encode()
        root[e + 11] = 0x20
        struct.pack_into("<HHHI", root, e + 22, 0, 0x5A21, first, len(content))

    add_file(1, "HELLO.TXT", b"Hello from the AXPbox storage test.\r\n")
    rnd = random.Random(args.seed)
    add_file(2, "DATA.BIN", bytes(rnd.getrandbits(8) for _ in range(args.data_kb * 1024)))

    for i in range(NUM_FATS):
        off = lay["fat"] + i * fat_sectors * SECTOR
        img[off:off + len(fat)] = fat
    img[lay["root"]:lay["root"] + len(root)] = root
    with open(args.image, "wb") as f:
        f.write(img)
    print("%s: %d MB, FAT16, %d clusters, DATA.BIN %d KB" %
          (args.image, args.size_mb, clusters, args.data_kb))


def read_file(img, name):
    lay = layout(img)
    want = name.upper().split(".")
    key = want[0].ljust(8).encode() + (want[1] if len(want) > 1 else "").ljust(3).encode()
    for i in range(lay["nroot"]):
        e = lay["root"] + i * 32
        if img[e] in (0x00, 0xE5):
            continue
        if bytes(img[e:e + 11]) == key and not img[e + 11] & 0x18:
            cluster = struct.unpack_from("<H", img, e + 26)[0]
            size = struct.unpack_from("<I", img, e + 28)[0]
            out = bytearray()
            while 2 <= cluster < 0xFFF8 and len(out) < size:
                off = lay["data"] + (cluster - 2) * lay["csize"]
                out += img[off:off + lay["csize"]]
                cluster = struct.unpack_from("<H", img, lay["fat"] + cluster * 2)[0]
            return bytes(out[:size])
    return None


def get(args):
    img = open(args.image, "rb").read()
    data = read_file(img, args.name)
    if data is None:
        print("%s: %s not found" % (args.image, args.name))
        return 1
    print("%s: %d bytes, md5 %s" % (args.name, len(data), hashlib.md5(data).hexdigest()))
    if args.out:
        open(args.out, "wb").write(data)
    return 0


def check(args):
    img = open(args.image, "rb").read()
    a, b = read_file(img, args.name), read_file(img, args.reference)
    if a is None or b is None:
        print("missing: %s" % ", ".join(n for n, d in ((args.name, a), (args.reference, b)) if d is None))
        return 1
    same = a == b
    print("%s (%d bytes) %s %s (%d bytes)" %
          (args.name, len(a), "==" if same else "!=", args.reference, len(b)))
    return 0 if same else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("image")
    m.add_argument("--size-mb", type=int, default=64)
    m.add_argument("--data-kb", type=int, default=2048)
    m.add_argument("--seed", type=int, default=1)
    g = sub.add_parser("get")
    g.add_argument("image")
    g.add_argument("name")
    g.add_argument("out", nargs="?")
    c = sub.add_parser("check")
    c.add_argument("image")
    c.add_argument("name")
    c.add_argument("reference")
    args = ap.parse_args()
    return {"make": make, "get": get, "check": check}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
