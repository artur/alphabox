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
 * S3 ViRGE S3d engine, 2D commands.
 *
 * A command is its register set plus the command register (0xa500 for
 * BitBLT and rectangle fill, 0xa900 for lines, 0xad00 for polygons, one
 * register behind three names). With autoexecute (bit 0) clear, writing
 * the command runs it; with it set, writing the set's last register
 * (0xa50c, 0xa97c, 0xad7c) does. A NOP command runs nothing and clears
 * autoexecute (the Windows 2000 driver relies on that).
 *
 * Every command draws pixels of one depth (8, 16 or 24 bits) at
 * destination base + y * stride + x * bytes, through the Windows ternary
 * raster operation of source, pattern and destination, inside the
 * clipping rectangle when bit 1 asks, and only with draw enable (bit 5)
 * set. The pattern is 8x8, aligned to the destination's coordinates:
 * monochrome (two dwords, a byte a row, the leftmost pixel in bit 7,
 * choosing the pattern foreground or background colour) or in colour
 * (the pattern RAM at 0xa000, rows of 8 pixels).
 *
 * BitBLT's source is video memory, or host data written anywhere into
 * the image transfer window (MMIO 0x0000-0x7fff): colour pixels, or one
 * bit a pixel choosing the source foreground or background colour, the
 * background left alone when transparency (bit 9) is set. Host data is a
 * byte stream: each line starts that many bytes into it (bits 13..12)
 * and is padded to a byte, word or doubleword (bits 11..10).
 *
 * Widths are the register's value plus one, heights the value itself.
 * Lines and polygons run bottom-up, one scanline at a time, their edges
 * in 12.20 fixed point.
 **/

#include "S3Virge.hpp"

using namespace virge;

namespace {

/// The Windows ternary raster operation: bit (P << 2 | S << 1 | D) of the
/// code is the result for that combination, bit by bit.
inline u32 ternary_rop(u8 rop, u32 p, u32 s, u32 d) {
  switch (rop) { // the common ones directly
  case 0x00:
    return 0;
  case 0xcc:
    return s;
  case 0xf0:
    return p;
  case 0xaa:
    return d;
  case 0x66:
    return s ^ d;
  case 0x5a:
    return p ^ d;
  case 0x55:
    return ~d;
  case 0x33:
    return ~s;
  case 0x88:
    return s & d;
  case 0xee:
    return s | d;
  case 0xff:
    return ~0u;
  }
  u32 out = 0;
  for (int i = 0; i < 8; i++) {
    if (!(rop & (1 << i)))
      continue;
    out |= ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
  }
  return out;
}

} // namespace

void CS3Virge::engine_reset() {
  memset(&r.e, 0, sizeof(r.e));
  std::lock_guard<std::mutex> lock(m_int_lock);
  r.subsys_status |= INT_3D_DONE | INT_FIFO_EMPTY | INT_3D_FIFO_EMPTY;
  update_int_line();
}

static virge_draw draw_from(const u32 *mmio, u32 cmd) {
  auto R = [mmio](u32 off) { return mmio[(off & 0x7ffc) >> 2]; };
  virge_draw d;
  d.cmd = cmd;
  const u32 fmt = (cmd >> CMD_FORMAT_SHIFT) & 7;
  d.bytes = fmt == 0 ? 1 : fmt == 1 ? 2 : 3;
  d.pmask = d.bytes == 1 ? 0xff : d.bytes == 2 ? 0xffff : 0xffffff;
  d.src_base = R(S2D_SRC_BASE) & 0x7ffff8;
  d.dest_base = R(S2D_DEST_BASE) & 0x7ffff8;
  d.src_stride = R(S2D_STRIDE) & 0xff8;
  d.dest_stride = (R(S2D_STRIDE) >> 16) & 0xff8;
  d.clip_l = (R(S2D_CLIP_LR) >> 16) & 0x7ff;
  d.clip_r = R(S2D_CLIP_LR) & 0x7ff;
  d.clip_t = (R(S2D_CLIP_TB) >> 16) & 0x7ff;
  d.clip_b = R(S2D_CLIP_TB) & 0x7ff;
  d.clip = (cmd & CMD_CLIP) != 0;
  d.rop = u8(cmd >> CMD_ROP_SHIFT);
  return d;
}

/**
 * A write to the engine's registers (the offset as written, before the
 * aliases fold together), which may start a command.
 **/
void CS3Virge::engine_register_written(u32 offset, u32 data) {
  switch (offset) {
  case 0xa500:
  case 0xa900:
  case 0xad00:
    if (((data >> CMD_COMMAND_SHIFT) & 15) == CMD2D_NOP) {
      M(S2D_CMD) = data & ~CMD_AUTOEXEC;
      r.e.active = false;
      return;
    }
    if (!(data & CMD_AUTOEXEC))
      s2d_start(data);
    return;
  case BLT_DEST_XY:
  case LINE_YCOUNT:
  case POLY_YCOUNT:
    if (M(S2D_CMD) & CMD_AUTOEXEC)
      s2d_start(M(S2D_CMD));
    return;
  case 0xb100:
  case 0xb500:
    if (((data >> CMD_COMMAND_SHIFT) & 15) == CMD2D_NOP) {
      M(S3D_CMD) = data & ~CMD_AUTOEXEC;
      return;
    }
    if (!(data & CMD_AUTOEXEC))
      s3d_start(data);
    return;
  case 0xb17c:
  case 0xb57c:
    if (M(S3D_CMD) & CMD_AUTOEXEC)
      s3d_start(M(S3D_CMD));
    return;
  }
}

void CS3Virge::s2d_start(u32 cmd) {
  r.e.active = false;
  m_draw = draw_from(r.mmio, cmd);
  const u32 op = (cmd >> CMD_COMMAND_SHIFT) & 15;
  if (m_trace_cmd) {
    const virge_draw d = draw_from(r.mmio, cmd);
    printf("%s: 2D %u cmd %08x rop %02x wh %08x src %08x dst %08x base "
           "%06x/%06x stride %03x/%03x\n",
           devid_string, op, cmd, d.rop, M(BLT_WIDTH_HEIGHT), M(BLT_SRC_XY),
           M(BLT_DEST_XY), d.src_base, d.dest_base, d.src_stride,
           d.dest_stride);
  }
  if (cmd & CMD_3D) {
    unimplemented("2D register set with the 3D bit");
    return;
  }
  switch (op) {
  case CMD2D_BITBLT:
    if (cmd & CMD_HOST_SRC) {
      const virge_draw d = draw_from(r.mmio, cmd);
      const u32 wh = M(BLT_WIDTH_HEIGHT);
      r.e.cmd = cmd;
      r.e.w = s32((wh >> 16) & 0x7ff) + 1;
      r.e.h = s32(wh & 0x7ff);
      r.e.x0 = r.e.dx = s32((M(BLT_DEST_XY) >> 16) & 0x7ff);
      r.e.dy = s32(M(BLT_DEST_XY) & 0x7ff);
      const u32 skip = (cmd >> CMD_SKIP_SHIFT) & 3;
      const u32 align = 1u << ((cmd >> CMD_ALIGN_SHIFT) & 3);
      const u32 data = (cmd & CMD_MONO_SRC) ? (u32(r.e.w) + 7) / 8
                                            : u32(r.e.w) * u32(d.bytes);
      r.e.line_bytes = (skip + data + align - 1) & ~(align - 1);
      r.e.line_fill = 0;
      r.e.active = r.e.h > 0 && r.e.line_bytes <= sizeof(r.e.line);
      return; // the rest happens as host data arrives
    }
    if (cmd & CMD_MONO_SRC)
      unimplemented("BitBLT from a monochrome source in video memory");
    s2d_bitblt_screen();
    break;
  case CMD2D_RECT:
    s2d_rect();
    break;
  case CMD2D_LINE:
    s2d_line();
    break;
  case CMD2D_POLY:
    s2d_poly();
    break;
  default: {
    char what[64];
    snprintf(what, sizeof(what), "2D command %u", op);
    unimplemented(what);
    break;
  }
  }
  state.vga_mem_updated = 1;
}

/// The pattern's pixel at destination (x, y).
u32 CS3Virge::s2d_pattern(s32 x, s32 y) const {
  const u32 cmd = m_draw.cmd;
  const u32 px = u32(x) & 7, py = u32(y) & 7;
  if (cmd & CMD_MONO_PAT) {
    const u32 row = py < 4 ? (M(S2D_MONO_PAT0) >> (8 * py))
                           : (M(S2D_MONO_PAT1) >> (8 * (py - 4)));
    return ((row >> (7 - px)) & 1) ? M(S2D_PAT_FG) : M(S2D_PAT_BG);
  }
  const u32 fmt = (cmd >> CMD_FORMAT_SHIFT) & 7;
  const u32 bytes = fmt == 0 ? 1 : fmt == 1 ? 2 : 3;
  const u32 at = (py * 8 + px) * bytes;
  const u8 *pat =
      reinterpret_cast<const u8 *>(&r.mmio[(PATTERN_RAM & 0x7fff) >> 2]);
  u32 v = 0;
  for (u32 i = 0; i < bytes; i++)
    v |= u32(pat[(at + i) & 0x1ff]) << (8 * i);
  return v;
}

/**
 * One pixel of a 2D command: clip, raster operation, store. `src` is the
 * source pixel (unused by operations without one).
 **/
void CS3Virge::s2d_pixel(s32 x, s32 y, u32 src, bool have_src) {
  const virge_draw &d = m_draw;
  const u32 cmd = d.cmd;
  (void)have_src;
  x &= 0x7ff;
  y &= 0x7ff;
  if (d.clip && (x < d.clip_l || x > d.clip_r || y < d.clip_t || y > d.clip_b))
    return;
  if (!(cmd & CMD_DRAW_ENABLE))
    return;
  const u32 a = d.dest_base + u32(y) * d.dest_stride + u32(x) * u32(d.bytes);
  const u32 dst = vram_read(a, d.bytes);
  const u32 out = ternary_rop(d.rop, s2d_pattern(x, y), src, dst) & d.pmask;
  vram_write(a, d.bytes, out);
}

/**
 * BitBLT from video memory. X Positive / Y Positive give the direction,
 * so that an overlapping copy reads each source pixel before it is
 * overwritten; the registers hold the corner the copy starts from.
 **/
void CS3Virge::s2d_bitblt_screen() {
  const u32 cmd = M(S2D_CMD);
  const virge_draw d = draw_from(r.mmio, cmd);
  const u32 wh = M(BLT_WIDTH_HEIGHT);
  const s32 w = s32((wh >> 16) & 0x7ff) + 1;
  const s32 h = s32(wh & 0x7ff);
  const s32 xinc = (cmd & CMD_X_POSITIVE) ? 1 : -1;
  const s32 yinc = (cmd & CMD_Y_POSITIVE) ? 1 : -1;
  s32 sy = s32(M(BLT_SRC_XY) & 0x7ff), dy = s32(M(BLT_DEST_XY) & 0x7ff);
  const s32 sx0 = s32((M(BLT_SRC_XY) >> 16) & 0x7ff);
  const s32 dx0 = s32((M(BLT_DEST_XY) >> 16) & 0x7ff);
  const bool transparent = (cmd & CMD_TRANSPARENT) != 0;
  const u32 key = M(S2D_SRC_FG) & d.pmask;
  for (s32 row = 0; row < h; row++, sy += yinc, dy += yinc) {
    s32 sx = sx0, dx = dx0;
    for (s32 col = 0; col < w; col++, sx += xinc, dx += xinc) {
      const u32 a = d.src_base + u32(sy & 0x7ff) * d.src_stride +
                    u32(sx & 0x7ff) * u32(d.bytes);
      const u32 s = vram_read(a, d.bytes);
      if (transparent && s == key)
        continue;
      s2d_pixel(dx, dy, s, true);
    }
  }
}

/// Rectangle fill: the pattern (or its foreground colour) alone.
void CS3Virge::s2d_rect() {
  const u32 cmd = M(S2D_CMD);
  const u32 wh = M(BLT_WIDTH_HEIGHT);
  const s32 w = s32((wh >> 16) & 0x7ff) + 1;
  const s32 h = s32(wh & 0x7ff);
  const s32 xinc = (cmd & CMD_X_POSITIVE) ? 1 : -1;
  const s32 yinc = (cmd & CMD_Y_POSITIVE) ? 1 : -1;
  const s32 dx0 = s32((M(BLT_DEST_XY) >> 16) & 0x7ff);
  s32 dy = s32(M(BLT_DEST_XY) & 0x7ff);
  for (s32 row = 0; row < h; row++, dy += yinc) {
    s32 dx = dx0;
    for (s32 col = 0; col < w; col++, dx += xinc)
      s2d_pixel(dx, dy, 0, false);
  }
}

/**
 * Host data for a BitBLT, a byte stream: collect a line's worth, then
 * draw it.
 **/
void CS3Virge::s2d_host_data(u32 data, int bytes) {
  if (!r.e.active) {
    unimplemented("host data with no BitBLT waiting for it");
    return;
  }
  for (int i = 0; i < bytes && r.e.active; i++) {
    r.e.line[r.e.line_fill++] = u8(data >> (8 * i));
    if (r.e.line_fill < r.e.line_bytes)
      continue;
    r.e.line_fill = 0;
    s2d_host_line(r.e.line);
    const s32 yinc = (r.e.cmd & CMD_Y_POSITIVE) ? 1 : -1;
    r.e.dy += yinc;
    if (--r.e.h <= 0)
      r.e.active = false;
  }
  state.vga_mem_updated = 1;
}

void CS3Virge::s2d_host_line(const u8 *line) {
  const u32 cmd = r.e.cmd;
  m_draw = draw_from(r.mmio, cmd);
  const virge_draw &d = m_draw;
  const u8 *p = line + ((cmd >> CMD_SKIP_SHIFT) & 3);
  const s32 xinc = (cmd & CMD_X_POSITIVE) ? 1 : -1;
  s32 x = r.e.x0;
  if (cmd & CMD_MONO_SRC) {
    const u32 fg = M(S2D_SRC_FG) & d.pmask, bg = M(S2D_SRC_BG) & d.pmask;
    const bool transparent = (cmd & CMD_TRANSPARENT) != 0;
    for (s32 i = 0; i < r.e.w; i++, x += xinc) {
      const bool bit = (p[i >> 3] >> (7 - (i & 7))) & 1;
      if (!bit && transparent)
        continue;
      s2d_pixel(x, r.e.dy, bit ? fg : bg, true);
    }
    return;
  }
  for (s32 i = 0; i < r.e.w; i++, x += xinc) {
    u32 s = 0;
    for (int b = 0; b < d.bytes; b++)
      s |= u32(p[i * d.bytes + b]) << (8 * b);
    s2d_pixel(x, r.e.dy, s, true);
  }
}

/**
 * A line, as the driver has cut it into scanlines: from YSTART upwards
 * for YCOUNT scanlines, the X of each scanline's run stepping by DX/DY
 * (12.20) from XSTART; XEND0 bounds the first scanline's run and XEND1
 * the last's. Bit 31 of the count register gives the X direction (set:
 * left to right).
 **/
void CS3Virge::s2d_line() {
  const u32 xend0 = (M(LINE_XEND) >> 16) & 0x7ff;
  const u32 xend1 = M(LINE_XEND) & 0x7ff;
  const s32 dxdy = s32(M(LINE_DXDY));
  s32 xs = s32(M(LINE_XSTART));
  s32 y = s32(M(LINE_YSTART) & 0x7ff);
  const s32 count = s32(M(LINE_YCOUNT) & 0x7ff);
  const bool right = (M(LINE_YCOUNT) & 0x80000000u) != 0;
  const s32 step = right ? 1 : -1;
  const s32 e0 = s32(xend0), e1 = s32(xend1);
  for (s32 n = count; n > 0; n--, y--) {
    const bool first = n == count, last = n == 1;
    s32 x = xs >> 20;
    const s32 next = last ? e1 + step : (xs + dxdy) >> 20;
    xs += dxdy;
    if (first && (right ? x < e0 : x > e0))
      x = e0;
    if (right ? x > next : x < next)
      continue;
    do {
      const bool before = right ? x < e0 : x > e0;
      const bool after = right ? x > e1 : x < e1;
      if (!before && !after)
        s2d_pixel(x, y, 0, false);
      if (x != next)
        x += step;
    } while (x != next);
  }
}

/**
 * A polygon, as trapezoids: from YSTART upwards for YCOUNT scanlines,
 * filled between the left and right edges (12.20, stepping by their
 * DX/DY). Bits 28 and 29 of the count register restart the right and
 * left edges from their XSTART registers; a clear bit carries the edge
 * on from where the previous trapezoid left it.
 **/
void CS3Virge::s2d_poly() {
  const u32 yc = M(POLY_YCOUNT);
  // The running edges live in the engine state so that the next trapezoid
  // can continue them.
  if (yc & (1u << 28))
    r.e.poly_right = s32(M(POLY_RIGHT_XSTART));
  if (yc & (1u << 29))
    r.e.poly_left = s32(M(POLY_LEFT_XSTART));
  s32 xr = r.e.poly_right, xl = r.e.poly_left;
  s32 y = s32(M(POLY_YSTART) & 0x7ff);
  const s32 count = s32(yc & 0x7ff);
  const s32 dr = s32(M(POLY_RIGHT_DX)), dl = s32(M(POLY_LEFT_DX));
  for (s32 n = 0; n < count; n++, y--) {
    s32 x0 = xl >> 20, x1 = xr >> 20;
    const s32 step = x0 <= x1 ? 1 : -1;
    for (s32 x = x0;; x += step) {
      s2d_pixel(x, y, 0, false);
      if (x == x1)
        break;
    }
    xl += dl;
    xr += dr;
  }
  r.e.poly_right = xr;
  r.e.poly_left = xl;
  M(POLY_YSTART) = u32(y) & 0x7ff;
}
