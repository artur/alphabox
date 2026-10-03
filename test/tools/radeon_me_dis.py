#!/usr/bin/env python3
"""Annotated listing of a Radeon CP micro-engine (ME) image.

ATI never documented the micro-engine's instruction set. What this tool
prints is the structure docs/radeon-microcode.md establishes, with every
annotation marked by how well it is known:

  [doc]   documented (AMD's R5xx Acceleration guide, the drivers' sources)
  [inf]   inferred from strong, repeated evidence in the images
  [spec]  speculative: a reading the evidence allows, not one it forces

Input: a microcode image in the linux-firmware layout (radeon/R100_cp.bin:
N entries, each the DATAH word then the DATAL word, big-endian, the order
r100_cp_load_microcode writes them), or the dump ALPHABOX_RADEON_ME_DUMP
writes (the same layout). The microcode itself is not in this repository.

    radeon_me_dis.py R100_cp.bin            # annotated listing
    radeon_me_dis.py --table R100_cp.bin    # the packet dispatch table only
    radeon_me_dis.py --raw R100_cp.bin      # fields only, no annotations
"""
import argparse
import struct
import sys
import zlib

# The three generations of the ME word seen in the images: the register
# field widens by one bit per step, everything above it moves up [inf].
# Entries 2 and 3 are jumps in every image; the jump opcode's bit (DATAH
# 0x02, 0x04, 0x08) gives the width [inf].
GENERATIONS = [
    # (name, register-field bits, DATAH of a jump)
    ("Rage 128 (CCE)", 11, 0x02),
    ("R100/R200 (RV100, RV200, RS100, RS200, RV250, RV280, RS300)", 12, 0x04),
    ("R300 and later (R420, R520, RS600, RS690)", 13, 0x08),
]

# Type-3 opcodes, IT_OPCODE <14:8> (bit 15 = GUI_CONTROL present) [doc]:
# R5xx Acceleration v1.5 6.2.1, Linux radeon_drv.h (legacy DRM).
PACKET3 = {
    0x10: "NOP", 0x11: "PAINT", 0x12: "BITBLT", 0x13: "SMALLTEXT",
    0x14: "HOSTDATA_BLT", 0x15: "POLYLINE", 0x18: "POLYSCANLINES",
    0x19: "NEXT_CHAR", 0x1A: "PAINT_MULTI", 0x1B: "BITBLT_MULTI",
    0x1C: "TRANS_BITBLT", 0x1D: "PLY_NEXTSCAN", 0x1E: "SET_SCISSORS",
    0x20: "PRED_EXEC (R5xx)", 0x21: "COND_EXEC (R5xx)",
    0x22: "WAIT_SEMAPHORE (R5xx)",
    0x23: "3D_RNDR_GEN_INDX_PRIM (R100) / WAIT_MEM (R5xx)",
    0x24: "LOAD_MICROCODE", 0x26: "WAIT_FOR_IDLE", 0x28: "3D_DRAW_VBUF",
    0x29: "3D_DRAW_IMMD", 0x2A: "3D_DRAW_INDX", 0x2C: "LOAD_PALETTE",
    0x2F: "3D_LOAD_VBPNTR", 0x30: "MPEG_IDCT_MACROBLOCK",
    0x31: "MPEG_IDCT_MACROBLOCK_REV", 0x32: "3D_CLEAR_ZMASK",
    0x33: "INDX_BUFFER", 0x34: "3D_DRAW_VBUF_2", 0x35: "3D_DRAW_IMMD_2",
    0x36: "3D_DRAW_INDX_2", 0x37: "3D_CLEAR_HIZ", 0x38: "3D_CLEAR_CMASK",
    0x39: "3D_DRAW_128 (R5xx)", 0x3A: "MPEG_INDEX (R5xx)",
}

# Register offsets the R100/R200 images name in their register field
# (radeon_reg.h, r200_reg.h; '?' = no public name).
REGS = {
    0x1428: "SRC_PITCH_OFFSET", 0x142C: "DST_PITCH_OFFSET",
    0x1438: "DST_Y_X", 0x143C: "DST_HEIGHT_WIDTH",
    0x146C: "DP_GUI_MASTER_CNTL", 0x1474: "BRUSH_Y_X",
    0x1588: "DST_WIDTH_X", 0x158C: "DST_HEIGHT_WIDTH_8",
    0x1590: "SRC_X_Y", 0x1594: "DST_X_Y", 0x1598: "DST_WIDTH_HEIGHT",
    0x15A0: "DST_HEIGHT_Y", 0x15C0: "CLR_CMP_CNTL",
    0x15C4: "CLR_CMP_CLR_SRC", 0x15C8: "CLR_CMP_CLR_DST",
    0x15D8: "DP_SRC_FRGD_CLR", 0x15DC: "DP_SRC_BKGD_CLR",
    0x1600: "DST_LINE_START", 0x1604: "DST_LINE_END",
    0x16EC: "SC_TOP_LEFT", 0x16F0: "SC_BOTTOM_RIGHT",
    0x16F4: "SRC_SC_BOTTOM_RIGHT", 0x1714: "DSTCACHE_CTLSTAT",
    0x17C0: "HOST_DATA0", 0x17E0: "HOST_DATA_LAST",
    0x1D04: "PP_TEX_SIZE_0", 0x1D0C: "PP_TEX_SIZE_1",
    0x1F80: "IDCT_RUNS", 0x1F84: "IDCT_LEVELS", 0x1FBC: "IDCT_CONTROL",
    0x2000: "SE_PORT_DATA0", 0x2080: "SE_VTX_FMT", 0x2084: "SE_VF_CNTL",
    0x20C8: "SE_VTX_AOS_ADDR0", 0x325C: "RB3D_DSTCACHE_CTLSTAT",
}

# The opcode field (R100 numbering, which the R128 and R300 images share
# once the register field is accounted for) and the readings the evidence
# supports; docs/radeon-microcode.md gives the evidence for each.
#   00  a packet's last step: the rest of the body to register(s) (M=7),
#       or dropped (M=5)                                        [inf]
#   01  a step: one body dword to register A (M=7), one dropped (M=5),
#       an ALU result to register A (M=2/6), other forms        [inf]/[spec]
#   02  jump to A                                               [inf]
#   03  return (after an optional register write as 01's)       [inf]
#   0b  indexed jump through the next entry's four bytes        [inf]
#   0c  call A (F=0, M=0; with F/M set, a condition?)           [inf]/[spec]
#   04 06 09 0d 0e  conditional branch to A                     [inf]
COND_BRANCH = {0x04, 0x06, 0x08, 0x09, 0x0D, 0x0E}


def load(path):
    b = open(path, "rb").read()
    if len(b) % 8:
        sys.exit("%s: %d bytes, not a whole number of 8-byte entries" % (path, len(b)))
    return [struct.unpack(">II", b[i:i + 8]) for i in range(0, len(b), 8)]


def crcs(ents):
    f = zlib.crc32(b"".join(struct.pack(">II", h, l) for h, l in ents)) & 0xFFFFFFFF
    # what Alphabox logs: each entry as {DATAH, DATAL}, little-endian words
    m = zlib.crc32(b"".join(struct.pack("<II", h, l) for h, l in ents)) & 0xFFFFFFFF
    return f, m


def generation(ents):
    if len(ents) > 3 and ents[2][0] == ents[3][0] and ents[2][1] < 0x100 and ents[3][1] < 0x100:
        for name, rb, h in GENERATIONS:
            if ents[2][0] == h:
                return name, rb
    return None, None


def fields(w, rb):
    """A = register/target field, M = the 3 bits above it, F = the middle
    field, OP = the top bits (normalised to the R100 numbering)."""
    a = w & ((1 << rb) - 1)
    up = w >> rb
    m = up & 7
    f = (up >> 3) & 0x3FFFF
    op = up >> 21
    return op, f, m, a


def dispatch_table(ents):
    """Entries 4.. hold one byte per type-3 opcode from 0x10, low byte
    first; the table ends at the first entry whose opcode field is not 0
    (true of every image found) [inf]."""
    words = []
    i = 4
    while i < len(ents) and ents[i][0] == 0 and i < 32:
        words.append(ents[i][1])
        i += 1
    tb = []
    for w in words:
        tb += [(w >> (8 * k)) & 0xFF for k in range(4)]
    return tb, 4, i  # bytes, first entry, entry after the table


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--table", action="store_true", help="the dispatch table only")
    ap.add_argument("--raw", action="store_true", help="fields only")
    args = ap.parse_args()
    ents = load(args.image)
    fcrc, mcrc = crcs(ents)
    gname, rb = generation(ents)
    print("; %s: %d entries, CRC-32 %08x (file) / %08x (as Alphabox logs it)" % (
        args.image, len(ents), fcrc, mcrc))
    if gname is None:
        print("; entries 2 and 3 are not jumps of a known width: generation "
              "unknown; fields decoded as R100")
        rb = 12
    else:
        print("; word width: %s, register field %d bits [inf]" % (gname, rb))

    tb, t0, t1 = dispatch_table(ents)
    default = tb[0] if tb else None
    handlers = {}
    for k, t in enumerate(tb):
        handlers.setdefault(t, []).append(0x10 + k)
    print("; dispatch table: entries %d..%d, type-3 opcodes 0x10..0x%02x, "
          "the default (NOP's) handler %d [inf]" % (t0, t1 - 1, 0x10 + len(tb) - 1, default))
    for k, t in enumerate(tb):
        op = 0x10 + k
        if t != default or op == 0x10:
            print(";   0x%02x %-48s -> %3d" % (op, PACKET3.get(op, "?"), t))
    if args.table:
        return

    labels = {}
    for t, ops in handlers.items():
        if t == default:
            labels.setdefault(t, []).append("default/NOP")
        else:
            labels.setdefault(t, []).append("/".join(PACKET3.get(o, "op%02x" % o).split(" ")[0] for o in ops))
    labels.setdefault(0, []).append("vector 0")
    labels.setdefault(1, []).append("vector 1")
    # the two jumps after them: entry points [inf] (their roles [spec])
    for v in (2, 3):
        w = (ents[v][0] << 32) | ents[v][1]
        op, f, m, a = fields(w, rb)
        if op == 0x02:
            labels.setdefault(a, []).append("via vector %d" % v)
    # inline tables after a 'jtab' (op 0x0B with M=0, F=0x1e) [inf]
    inline = {}
    for i, (h, l) in enumerate(ents):
        w = (h << 32) | l
        op, f, m, a = fields(w, rb)
        if op == 0x0B and f == 0x1E and i + 1 < len(ents):
            inline[i + 1] = [(ents[i + 1][1] >> (8 * k)) & 0xFF for k in range(4)]
            for k, t in enumerate(inline[i + 1]):
                labels.setdefault(t, []).append("jtab@%d[%d]" % (i, k))
    print(";")
    print("; addr  raw          op  F      M  A      annotation")
    last = len(ents)
    while last > 0 and ents[last - 1] == (0, 0):
        last -= 1
    for i, (h, l) in enumerate(ents):
        if i >= last:
            print("; %d..%d: zero" % (last, len(ents) - 1))
            break
        w = (h << 32) | l
        op, f, m, a = fields(w, rb)
        for lab in labels.get(i, []):
            print("%s:" % lab)
        raw = "%02x:%08x" % (h, l)
        if t0 <= i < t1:
            k = (i - t0) * 4
            print("  %3d  %s  table: %s" % (i, raw, ", ".join(
                "%02x->%d" % (0x10 + k + j, tb[k + j]) for j in range(4))))
            continue
        if i in inline:
            print("  %3d  %s  inline table: %s [inf]" % (i, raw, " ".join("%d" % t for t in inline[i])))
            continue
        line = "  %3d  %s  %02x  %05x  %x  %03x" % (i, raw, op, f, m, a)
        if args.raw:
            print(line)
            continue
        note = []
        reg = "0x%04x %s" % (a * 4, REGS.get(a * 4, "?"))
        if op == 0x02:
            note.append("jump %d [inf]%s" % (a, "" if (f, m) == (0, 0) else " (F/M set: conditional? [spec])"))
        elif op == 0x0C:
            note.append("call %d [inf]%s" % (a, "" if (f, m) == (0, 0) else " (F/M set: conditional? [spec])"))
        elif op in COND_BRANCH:
            note.append("branch if ? -> %d [inf; condition spec]" % a)
        elif op == 0x0B and f == 0x1E:
            note.append("indexed jump, byte selector %d [inf]" % a)
        elif op == 0x00 and m == 7 and a == 0 and f in (0x4200, 0x4000):
            note.append("rest of the body to consecutive registers from the index register%s [inf]" % (
                "" if f == 0x4200 else ", no increment (ONE_REG_WR)"))
        elif op == 0x00 and m == 7 and a * 4 >= 0x700:
            note.append("rest of the body -> %s, end of packet [inf]" % reg)
        elif op == 0x00 and (f, m, a) == (0, 5, 0):
            note.append("drop the rest of the body, end of packet [inf]")
        elif op in (0x01, 0x03, 0x10) and m == 7 and a * 4 >= 0x700:
            note.append("body dword -> %s [inf]" % reg)
        elif op in (0x01, 0x03) and m == 5 and a == 0 and f == 0:
            note.append("drop one body dword [inf]")
        elif op in (0x01, 0x03, 0x10) and m in (2, 6) and a * 4 >= 0x700:
            note.append("ALU -> %s [spec]" % reg)
        elif op in (0x01, 0x03) and m == 3 and a * 4 >= 0x700:
            note.append("(modified?) body dword -> %s [spec]" % reg)
        if op == 0x03:
            note.append("return [inf]")
        print(line + ("  " + "; ".join(note) if note else ""))


if __name__ == "__main__":
    main()
