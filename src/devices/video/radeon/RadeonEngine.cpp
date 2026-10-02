/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

/**
 * \file
 * The Radeon's 2D engine, driven through its registers (programmed I/O;
 * the command processor's ring is not modelled).
 *
 * A command is set up by DP_GUI_MASTER_CNTL -- which pitch/offsets and
 * clip rectangle apply, the brush, destination and source types, the
 * ROP3 and where the source comes from -- and started by writing the
 * destination's size: DST_HEIGHT_WIDTH and its aliases draw a rectangle
 * (a fill, a screen-to-screen blit, or a source expanded from the host
 * data registers that follow), DST_LINE_END a line from DST_LINE_START.
 * The engine finishes the work inside the write that starts it (host
 * data: as the data arrives), so RBBM_STATUS always shows the FIFO empty
 * and the engine idle.
 *
 * Every pixel goes through the full raster operation: the ROP3 of
 * pattern, source and destination, the write mask, the clip rectangle.
 * Coordinates are pixels; offsets are memory-controller addresses (see
 * mc_to_vram). On the Radeon a pitch/offset register holds the offset in
 * 1 KB units in <21:0> and the pitch in 64-byte units in <29:22>.
 *
 * Which of a register's fields an engine command takes, and the aliasing
 * of DP_GUI_MASTER_CNTL onto DP_DATATYPE and DP_MIX, follow QEMU's
 * ati_2d.c and ati.c; the raster operation and the line engine are this
 * file's own.
 **/

#include "Radeon.hpp"

#include <cstdlib>

using namespace radeon;

namespace {
constexpr u32 DST_OFFSET = 0x1404;
constexpr u32 DST_PITCH = 0x1408;
constexpr u32 DST_WIDTH = 0x140c;
constexpr u32 DST_HEIGHT = 0x1410;
constexpr u32 SRC_X = 0x1414;
constexpr u32 SRC_Y = 0x1418;
constexpr u32 DST_X = 0x141c;
constexpr u32 DST_Y = 0x1420;
constexpr u32 SRC_PITCH_OFFSET = 0x1428;
constexpr u32 DST_PITCH_OFFSET = 0x142c;
constexpr u32 SRC_Y_X = 0x1434;
constexpr u32 DST_Y_X = 0x1438;
constexpr u32 DST_HEIGHT_WIDTH = 0x143c;
constexpr u32 DP_GUI_MASTER_CNTL = 0x146c;
constexpr u32 BRUSH_Y_X = 0x1474;
constexpr u32 DP_BRUSH_BKGD_CLR = 0x1478;
constexpr u32 DP_BRUSH_FRGD_CLR = 0x147c;
constexpr u32 BRUSH_DATA0 = 0x1480;
constexpr u32 DST_WIDTH_X = 0x1588;
constexpr u32 DST_HEIGHT_WIDTH_8 = 0x158c;
constexpr u32 SRC_X_Y = 0x1590;
constexpr u32 DST_X_Y = 0x1594;
constexpr u32 DST_WIDTH_HEIGHT = 0x1598;
constexpr u32 DST_WIDTH_X_INCY = 0x159c;
constexpr u32 DST_HEIGHT_Y = 0x15a0;
constexpr u32 SRC_OFFSET = 0x15ac;
constexpr u32 SRC_PITCH = 0x15b0;
constexpr u32 DST_HEIGHT_WIDTH_BW = 0x15b4;
constexpr u32 CLR_CMP_CNTL = 0x15c0;
constexpr u32 CLR_CMP_CLR_SRC = 0x15c4;
constexpr u32 CLR_CMP_CLR_DST = 0x15c8;
constexpr u32 CLR_CMP_MASK = 0x15cc;
constexpr u32 DP_SRC_FRGD_CLR = 0x15d8;
constexpr u32 DP_SRC_BKGD_CLR = 0x15dc;
constexpr u32 DST_LINE_START = 0x1600;
constexpr u32 DST_LINE_END = 0x1604;
constexpr u32 SC_LEFT = 0x1640;
constexpr u32 SC_RIGHT = 0x1644;
constexpr u32 SC_TOP = 0x1648;
constexpr u32 SC_BOTTOM = 0x164c;
constexpr u32 SRC_SC_RIGHT = 0x1654;
constexpr u32 SRC_SC_BOTTOM = 0x165c;
constexpr u32 DP_CNTL = 0x16c0;
constexpr u32 DP_DATATYPE = 0x16c4;
constexpr u32 DP_MIX = 0x16c8;
constexpr u32 DP_WRITE_MASK = 0x16cc;
constexpr u32 DP_CNTL_XDIR_YDIR_YMAJOR = 0x16d0;
constexpr u32 DEFAULT_PITCH_OFFSET = 0x16e0;
constexpr u32 DEFAULT_SC_BOTTOM_RIGHT = 0x16e8;
constexpr u32 SC_TOP_LEFT = 0x16ec;
constexpr u32 SC_BOTTOM_RIGHT = 0x16f0;
constexpr u32 SRC_SC_BOTTOM_RIGHT = 0x16f4;
constexpr u32 HOST_DATA0 = 0x17c0;
constexpr u32 HOST_DATA_LAST = 0x17e0;

// DP_GUI_MASTER_CNTL
constexpr u32 GMC_SRC_PITCH_OFFSET_CNTL = 1u << 0;
constexpr u32 GMC_DST_PITCH_OFFSET_CNTL = 1u << 1;
constexpr u32 GMC_SRC_CLIPPING = 1u << 2;
constexpr u32 GMC_DST_CLIPPING = 1u << 3;
constexpr u32 GMC_CLR_CMP_CNTL_DIS = 1u << 28;
constexpr u32 GMC_WR_MSK_DIS = 1u << 30;

// DP_DATATYPE as DP_GUI_MASTER_CNTL loads it: destination <3:0>, brush
// <11:8>, source <17:16>, byte pixel order <30>.
constexpr u32 DT_BYTE_PIX_ORDER = 1u << 30;

// sources (DP_MIX <10:8>)
enum { SRC_SOURCE_MEMORY = 2, SRC_SOURCE_HOST = 3, SRC_SOURCE_HOST_BYTE = 4 };
// brush types
enum {
  BRUSH_8X8_MONO_FG_BG = 0,
  BRUSH_8X8_MONO_FG_LA = 1,
  BRUSH_1X8_MONO_FG_BG = 4,
  BRUSH_1X8_MONO_FG_LA = 5,
  BRUSH_32X1_MONO_FG_BG = 6,
  BRUSH_32X1_MONO_FG_LA = 7,
  BRUSH_32X32_MONO_FG_BG = 8,
  BRUSH_32X32_MONO_FG_LA = 9,
  BRUSH_8X8_COLOR = 10,
  BRUSH_1X8_COLOR = 12,
  BRUSH_SOLID = 13,
  BRUSH_NONE = 15
};
// source types
enum { SRC_MONO_FG_BG = 0, SRC_MONO_FG_LA = 1, SRC_COLOR = 3 };

int bytes_of_datatype(u32 dt) {
  switch (dt & 0xf) {
  case 3:  // 15 bpp
  case 4:  // 16 bpp
  case 15: // ARGB 4444
  case 11: // VYUY
  case 12: // YVYU
    return 2;
  case 6:  // 32 bpp
  case 14: // AYUV 4444
    return 4;
  default: // 8 bpp (CI, RGB 332, Y8, RGB8); 24 bpp is drawn as 8 bpp
    return 1;
  }
}

/// The ROP3 of pattern, source and destination (Windows encoding: bit
/// P*4 + S*2 + D of the code is the result).
inline u32 rop3(u32 rop, u32 p, u32 s, u32 d) {
  u32 r = 0;
  if (rop & 0x01)
    r |= ~p & ~s & ~d;
  if (rop & 0x02)
    r |= ~p & ~s & d;
  if (rop & 0x04)
    r |= ~p & s & ~d;
  if (rop & 0x08)
    r |= ~p & s & d;
  if (rop & 0x10)
    r |= p & ~s & ~d;
  if (rop & 0x20)
    r |= p & ~s & d;
  if (rop & 0x40)
    r |= p & s & ~d;
  if (rop & 0x80)
    r |= p & s & d;
  return r;
}

inline int sext14(u32 v) { return int(v << 18) >> 18; }
} // namespace

bool CRadeon::is_engine_reg(u32 reg) { return reg >= 0x1400 && reg < 0x1800; }

void CRadeon::engine_reset() {
  eng.host_active = false;
  eng.hx = eng.hy = 0;
}

/**
 * Registers that read back differently from what was last written.
 * DP_GUI_MASTER_CNTL shows the brush, destination and source types and
 * the ROP3 and source that DP_DATATYPE and DP_MIX now hold.
 **/
u32 CRadeon::engine_read(u32 reg) {
  switch (reg) {
  case DP_GUI_MASTER_CNTL: {
    const u32 dt = R(DP_DATATYPE), mix = R(DP_MIX);
    return (R(reg) & 0xf800000fu) | ((dt & 0xf00) >> 4) | ((dt & 0xf) << 8) |
           ((dt & 0x30000) >> 4) | ((dt & DT_BYTE_PIX_ORDER) >> 16) |
           (mix & 0x00ff0000u) | ((mix & 0x700) << 16);
  }
  case 0x1714: // DSTCACHE_CTLSTAT: never busy
  case 0x1720: // WAIT_UNTIL
    return 0;
  }
  return R(reg);
}

/// The pitch/offset register format: offset <21:0> in KB, pitch <29:22> in
/// 64-byte units.
static void split_pitch_offset(u32 v, u32 *offset, u32 *pitch) {
  *offset = (v & 0x3fffff) << 10;
  *pitch = ((v >> 22) & 0xff) * 64;
}

void CRadeon::engine_master_cntl(u32 data) {
  // DP_DATATYPE and DP_MIX take their fields from the master control.
  R(DP_DATATYPE) = (R(DP_DATATYPE) & ~0x40030f0fu) | ((data >> 8) & 0xf) |
                   ((data & 0xf0) << 4) | ((data & 0x3000) << 4) |
                   ((data & 0x4000) << 16);
  R(DP_MIX) = (R(DP_MIX) & ~0x00ff0700u) | (data & 0x00ff0000u) |
              ((data >> 16) & 0x700);
  // Unless told to use their own, source and destination take the default
  // pitch/offset and the destination the default clip rectangle.
  if (!(data & GMC_SRC_PITCH_OFFSET_CNTL))
    split_pitch_offset(R(DEFAULT_PITCH_OFFSET), &eng.src_offset,
                       &eng.src_pitch);
  if (!(data & GMC_DST_PITCH_OFFSET_CNTL))
    split_pitch_offset(R(DEFAULT_PITCH_OFFSET), &eng.dst_offset,
                       &eng.dst_pitch);
  if (!(data & GMC_DST_CLIPPING)) {
    eng.sc_left = 0;
    eng.sc_top = 0;
    eng.sc_right = int(R(DEFAULT_SC_BOTTOM_RIGHT) & 0x3fff);
    eng.sc_bottom = int((R(DEFAULT_SC_BOTTOM_RIGHT) >> 16) & 0x3fff);
  }
  eng.gmc = data;
}

void CRadeon::engine_write(u32 reg, u32 data) {
  if (reg >= HOST_DATA0 && reg <= HOST_DATA_LAST) {
    engine_host_data(data, reg == HOST_DATA_LAST);
    return;
  }
  switch (reg) {
  case DP_GUI_MASTER_CNTL:
    engine_master_cntl(data);
    return;
  case DST_OFFSET:
    eng.dst_offset = data & 0xfffffff0u;
    return;
  case DST_PITCH:
    eng.dst_pitch = data & 0x3fff;
    return;
  case SRC_OFFSET:
    eng.src_offset = data & 0xfffffff0u;
    return;
  case SRC_PITCH:
    eng.src_pitch = data & 0x3fff;
    return;
  case DST_PITCH_OFFSET:
    split_pitch_offset(data, &eng.dst_offset, &eng.dst_pitch);
    return;
  case SRC_PITCH_OFFSET:
    split_pitch_offset(data, &eng.src_offset, &eng.src_pitch);
    return;
  case DST_X:
    eng.dst_x = sext14(data);
    return;
  case DST_Y:
    eng.dst_y = sext14(data);
    return;
  case SRC_X:
    eng.src_x = sext14(data);
    return;
  case SRC_Y:
    eng.src_y = sext14(data);
    return;
  case DST_Y_X:
    eng.dst_x = sext14(data);
    eng.dst_y = sext14(data >> 16);
    return;
  case DST_X_Y:
    eng.dst_y = sext14(data);
    eng.dst_x = sext14(data >> 16);
    return;
  case SRC_Y_X:
    eng.src_x = sext14(data);
    eng.src_y = sext14(data >> 16);
    return;
  case SRC_X_Y:
    eng.src_y = sext14(data);
    eng.src_x = sext14(data >> 16);
    return;
  case DST_HEIGHT_Y:
    eng.dst_y = sext14(data);
    eng.height = int((data >> 16) & 0x3fff);
    return;
  case DST_HEIGHT:
    eng.height = int(data & 0x3fff);
    return;
  case SC_LEFT:
    eng.sc_left = int(data & 0x3fff);
    return;
  case SC_TOP:
    eng.sc_top = int(data & 0x3fff);
    return;
  case SC_RIGHT:
    eng.sc_right = int(data & 0x3fff);
    return;
  case SC_BOTTOM:
    eng.sc_bottom = int(data & 0x3fff);
    return;
  case SC_TOP_LEFT:
    eng.sc_left = int(data & 0x3fff);
    eng.sc_top = int((data >> 16) & 0x3fff);
    return;
  case SC_BOTTOM_RIGHT:
    eng.sc_right = int(data & 0x3fff);
    eng.sc_bottom = int((data >> 16) & 0x3fff);
    return;
  case DP_CNTL:
    eng.dp_cntl = data & 3;
    return;
  case DP_CNTL_XDIR_YDIR_YMAJOR:
    eng.dp_cntl = ((data >> 31) & 1) | (((data >> 15) & 1) << 1);
    return;

  // The commands.
  case DST_WIDTH:
    eng.width = int(data & 0x3fff);
    engine_rect();
    return;
  case DST_HEIGHT_WIDTH:
  case DST_HEIGHT_WIDTH_8:
  case DST_HEIGHT_WIDTH_BW:
    eng.width = int(data & 0x3fff);
    eng.height = int((data >> 16) & 0x3fff);
    engine_rect();
    return;
  case DST_WIDTH_HEIGHT:
    eng.height = int(data & 0x3fff);
    eng.width = int((data >> 16) & 0x3fff);
    engine_rect();
    return;
  case DST_WIDTH_X:
    eng.dst_x = sext14(data);
    eng.width = int((data >> 16) & 0x3fff);
    engine_rect();
    return;
  case DST_WIDTH_X_INCY:
    eng.dst_x = sext14(data);
    eng.width = int((data >> 16) & 0x3fff);
    engine_rect();
    eng.dst_y += eng.height;
    return;
  case DST_LINE_END:
    engine_line(reg);
    return;
  }
}

/**
 * One pixel of the destination through the raster operation. `src` is
 * the source pixel (memory, host colour or the expanded mono colour);
 * `host_bit` false with a mono FG/LA source leaves the pixel alone.
 **/
void CRadeon::engine_pixel(int x, int y, u32 src, bool src_opaque) {
  if (x < eng.sc_left || x > eng.sc_right || y < eng.sc_top ||
      y > eng.sc_bottom || x < 0 || y < 0)
    return;
  if (!src_opaque)
    return;
  const int bpp = eng.bpp;
  const u32 addr =
      mc_to_vram(eng.dst_offset + u32(y) * eng.dst_pitch + u32(x) * u32(bpp));
  const u32 d = vram_read(addr, bpp);

  // The brush.
  const u32 dt = R(DP_DATATYPE);
  const u32 btype = (dt >> 8) & 0xf;
  u32 p = R(DP_BRUSH_FRGD_CLR);
  switch (btype) {
  case BRUSH_SOLID:
  case BRUSH_NONE:
    break;
  case BRUSH_8X8_MONO_FG_BG:
  case BRUSH_8X8_MONO_FG_LA:
  case BRUSH_1X8_MONO_FG_BG:
  case BRUSH_1X8_MONO_FG_LA: {
    const u32 org = R(BRUSH_Y_X);
    const int bx = (x - int(org & 0x1f)) & 7;
    const int by = (btype >= BRUSH_1X8_MONO_FG_BG)
                       ? 0
                       : ((y - int((org >> 8) & 0x1f)) & 7);
    const u8 row = u8(R(BRUSH_DATA0 + (by >> 2) * 4) >> (8 * (by & 3)));
    if ((row >> bx) & 1)
      p = R(DP_BRUSH_FRGD_CLR);
    else if (btype == BRUSH_8X8_MONO_FG_LA || btype == BRUSH_1X8_MONO_FG_LA)
      return;
    else
      p = R(DP_BRUSH_BKGD_CLR);
    break;
  }
  case BRUSH_32X1_MONO_FG_BG:
  case BRUSH_32X1_MONO_FG_LA:
  case BRUSH_32X32_MONO_FG_BG:
  case BRUSH_32X32_MONO_FG_LA: {
    const int bx = x & 31;
    const int by = btype >= BRUSH_32X32_MONO_FG_BG ? (y & 31) : 0;
    if ((R(BRUSH_DATA0 + by * 4) >> bx) & 1)
      p = R(DP_BRUSH_FRGD_CLR);
    else if (btype & 1)
      return;
    else
      p = R(DP_BRUSH_BKGD_CLR);
    break;
  }
  case BRUSH_8X8_COLOR: {
    const int bx = x & 7, by = y & 7;
    const u32 a = BRUSH_DATA0 + u32(by * 8 + bx) * u32(bpp);
    // 8x8 pixels of the destination's size in BRUSH_DATA0-63
    p = 0;
    for (int i = 0; i < bpp; i++)
      p |= ((R((a + i) & ~3u) >> (8 * ((a + i) & 3))) & 0xff) << (8 * i);
    break;
  }
  default:
    break;
  }

  if (!(eng.gmc & GMC_CLR_CMP_CNTL_DIS) && (R(CLR_CMP_CNTL) & 7)) {
    // Colour compare: <2:0> on the source, <10:8> on the destination;
    // 4 "not equal" and 5 "equal" leave the pixel alone when they hold.
    const u32 m = R(CLR_CMP_MASK);
    const u32 sf = R(CLR_CMP_CNTL) & 7, df = (R(CLR_CMP_CNTL) >> 8) & 7;
    const bool src_eq = (src & m) == (R(CLR_CMP_CLR_SRC) & m);
    const bool dst_eq = (d & m) == (R(CLR_CMP_CLR_DST) & m);
    if ((sf == 4 && !src_eq) || (sf == 5 && src_eq) || (df == 4 && !dst_eq) ||
        (df == 5 && dst_eq))
      return;
  }

  u32 out = rop3((R(DP_MIX) >> 16) & 0xff, p, src, d);
  if (!(eng.gmc & GMC_WR_MSK_DIS)) {
    const u32 wm = R(DP_WRITE_MASK);
    out = (d & ~wm) | (out & wm);
  }
  vram_write(addr, bpp, out);
}

/**
 * A rectangle: DST_X/DST_Y, DST_WIDTH x DST_HEIGHT, drawn in the direction
 * DP_CNTL gives (from the right or the bottom the start is the last
 * column or row, as the drivers program an overlapping blit). The source
 * is memory, the host (data to follow), or nothing.
 **/
void CRadeon::engine_rect() {
  eng.host_active = false;
  eng.bpp = bytes_of_datatype(R(DP_DATATYPE));
  if (eng.width <= 0 || eng.height <= 0)
    return;
  const u32 source = (R(DP_MIX) >> 8) & 7;
  if (source == SRC_SOURCE_HOST || source == SRC_SOURCE_HOST_BYTE) {
    eng.host_active = true;
    eng.hx = eng.hy = 0;
    eng.host_mono = ((R(DP_DATATYPE) >> 16) & 3) != SRC_COLOR;
    return;
  }
  const bool ltr = eng.dp_cntl & 1, ttb = eng.dp_cntl & 2;
  const int x0 = ltr ? eng.dst_x : eng.dst_x - eng.width + 1;
  const int y0 = ttb ? eng.dst_y : eng.dst_y - eng.height + 1;
  const int sx0 = ltr ? eng.src_x : eng.src_x - eng.width + 1;
  const int sy0 = ttb ? eng.src_y : eng.src_y - eng.height + 1;
  const bool mem_src = source == SRC_SOURCE_MEMORY;
  const u32 stype = (R(DP_DATATYPE) >> 16) & 3;
  const int bpp = eng.bpp;

  for (int j = 0; j < eng.height; j++) {
    const int row = ttb ? j : eng.height - 1 - j;
    for (int i = 0; i < eng.width; i++) {
      const int col = ltr ? i : eng.width - 1 - i;
      u32 s = 0;
      bool opaque = true;
      if (mem_src) {
        const int sx = sx0 + col, sy = sy0 + row;
        if (stype == SRC_COLOR) {
          s = vram_read(mc_to_vram(eng.src_offset + u32(sy) * eng.src_pitch +
                                   u32(sx) * u32(bpp)),
                        bpp);
        } else { // mono bits in memory, expanded
          const u8 b =
              u8(vram_read(mc_to_vram(eng.src_offset + u32(sy) * eng.src_pitch +
                                      u32(sx >> 3)),
                           1));
          const int bit =
              (R(DP_DATATYPE) & DT_BYTE_PIX_ORDER) ? (sx & 7) : 7 - (sx & 7);
          const bool on = (b >> bit) & 1;
          s = on ? R(DP_SRC_FRGD_CLR) : R(DP_SRC_BKGD_CLR);
          opaque = on || stype != SRC_MONO_FG_LA;
        }
      } else {
        s = R(DP_SRC_FRGD_CLR);
      }
      engine_pixel(x0 + col, y0 + row, s, opaque);
    }
  }
}

/**
 * Host data: the source of a rectangle, a dword at a time, filling it
 * left to right and top to bottom. Colour data packs pixels of the
 * destination's size; mono data a bit a pixel, most significant bit of
 * each byte first unless DP_DATATYPE's byte pixel order says otherwise.
 * Rows follow each other without padding, except that with the
 * byte-aligned source each row starts on a new byte. HOST_DATA_LAST ends
 * the transfer.
 **/
void CRadeon::engine_host_data(u32 data, bool last) {
  if (!eng.host_active)
    return;
  const int bpp = eng.bpp;
  const u32 stype = (R(DP_DATATYPE) >> 16) & 3;
  const bool lsb_first = (R(DP_DATATYPE) & DT_BYTE_PIX_ORDER) != 0;
  const bool byte_align = ((R(DP_MIX) >> 8) & 7) == SRC_SOURCE_HOST_BYTE;
  const int count = eng.host_mono ? 32 : 4 / bpp;

  for (int k = 0; k < count && eng.hy < eng.height; k++) {
    u32 s;
    bool opaque = true;
    if (eng.host_mono) {
      const int byte = k >> 3, bit = k & 7;
      const bool on = (data >> (8 * byte + (lsb_first ? bit : 7 - bit))) & 1;
      s = on ? R(DP_SRC_FRGD_CLR) : R(DP_SRC_BKGD_CLR);
      opaque = on || stype != SRC_MONO_FG_LA;
    } else {
      s = bpp == 4 ? data : (data >> (8 * bpp * k)) & ((1u << (8 * bpp)) - 1);
    }
    engine_pixel(eng.dst_x + eng.hx, eng.dst_y + eng.hy, s, opaque);
    if (++eng.hx >= eng.width) {
      eng.hx = 0;
      eng.hy++;
      if (byte_align && eng.host_mono)
        k |= 7; // the rest of this byte is padding
    }
  }
  if (last || eng.hy >= eng.height)
    eng.host_active = false;
}

/**
 * A line from DST_LINE_START to DST_LINE_END (y in <31:16>, x in <15:0>),
 * the end pixel left out, in the brush colour through the ROP3 (the
 * source is the source foreground colour). The next line of a polyline
 * starts where this one ended.
 **/
void CRadeon::engine_line(u32 reg) {
  (void)reg;
  eng.bpp = bytes_of_datatype(R(DP_DATATYPE));
  const u32 a = R(DST_LINE_START), b = R(DST_LINE_END);
  int x0 = sext14(a), y0 = sext14(a >> 16);
  const int x1 = sext14(b), y1 = sext14(b >> 16);
  const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (x0 == x1 && y0 == y1)
      break;
    engine_pixel(x0, y0, R(DP_SRC_FRGD_CLR), true);
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
  R(DST_LINE_START) = b;
}
