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
 * The IBM RGB561 RAMDAC of the PowerStorm 4D20 (TGA2), reached through the
 * TGA2's external-device window: four ports, picked by the window
 * address's register field. Port 0 and 1 load the low and high byte of a
 * 16-bit address; port 2 reads or writes the byte there and steps the
 * address; port 3 does the same for the tables whose entries are wider
 * than a byte -- the window type tables and gamma ramps take two writes an
 * entry (the high eight bits, then the low two in <7:6>), the colour map
 * three (red, green, blue). The address map and register fields are
 * NetBSD's ibm561reg.h; how the driver uses them is ibm561.c's and
 * tga2.sys's.
 *
 * The display: each 32-bit pixel's window ID -- its top bits, as many as
 * configuration register 1 asks for -- picks a frame buffer window type,
 * which says how the rest of the pixel is shown: 8-bit indexed through a
 * 64-entry block of the colour map, 24-bit through the map (direct) or
 * straight (true colour), 12- or 16-bit. The gamma ramps are stored but
 * not applied: NetBSD loads them all ones, which would show white.
 *
 * The cursor: 64x64, two bits a pixel packed MSB first at 0x2000; colour 0
 * transparent and 1-3 from the cursor colour table at 0x0a10; enabled by
 * the cursor control register, placed at the cursor position less the
 * hot spot.
 **/

#include "StdAfx.hpp"
#include "Tga.hpp"

using namespace tga;

namespace {
constexpr u16 R561_CONFIG1 = 0x0001;
constexpr u16 R561_CURS_CNTL = 0x0030;
constexpr u16 R561_HOTSPOT_X = 0x0034;
constexpr u16 R561_HOTSPOT_Y = 0x0035;
constexpr u16 R561_CURSOR_X = 0x0036;
constexpr u16 R561_CURSOR_Y = 0x0038;
constexpr u16 R561_CURSOR_LUT = 0x0a10;
constexpr u16 R561_AUXFB_WAT = 0x0e00;
constexpr u16 R561_AUXOL_WAT = 0x0f00;
constexpr u16 R561_FB_WAT = 0x1000;
constexpr u16 R561_OL_WAT = 0x1400;
constexpr u16 R561_CURSOR_BITMAP = 0x2000;
constexpr u16 R561_GAMMA_R = 0x3000;
constexpr u16 R561_CMAP = 0x4000;
} // namespace

void CTga::rgb561_reset() { memset(&ibm, 0, sizeof(ibm)); }

/// Byte-wide locations (port 2).
static u8 *rgb561_byte(u16 a, u8 *regs, u8 *auxfb, u8 *auxol, u8 *cursor,
                       u8 *lut) {
  if (a < 0x100)
    return &regs[a];
  if (a >= R561_AUXFB_WAT && a < R561_AUXFB_WAT + 256)
    return &auxfb[a - R561_AUXFB_WAT];
  if (a >= R561_AUXOL_WAT && a < R561_AUXOL_WAT + 256)
    return &auxol[a - R561_AUXOL_WAT];
  if (a >= R561_CURSOR_BITMAP && a < R561_CURSOR_BITMAP + 1024)
    return &cursor[a - R561_CURSOR_BITMAP];
  if (a >= R561_CURSOR_LUT && a < R561_CURSOR_LUT + 48)
    return &lut[a - R561_CURSOR_LUT];
  return nullptr;
}

/// The 10-bit table entry at an address, if it is one.
static u16 *rgb561_wide(u16 a, u16 *fb_wat, u16 *ol_wat, u16 (*gamma)[256]) {
  if (a >= R561_FB_WAT && a < R561_FB_WAT + 256)
    return &fb_wat[a - R561_FB_WAT];
  if (a >= R561_OL_WAT && a < R561_OL_WAT + 256)
    return &ol_wat[a - R561_OL_WAT];
  if (a >= R561_GAMMA_R && a < R561_GAMMA_R + 0xc00 && (a & 0x3ff) < 256)
    return &gamma[(a - R561_GAMMA_R) >> 10][a & 0xff];
  return nullptr;
}

void CTga::rgb561_write(unsigned port, u8 v) {
  trace("RGB561 write port %u addr %04x = %02x", port, ibm.addr, v);
  switch (port) {
  case 0:
    ibm.addr = u16((ibm.addr & 0xff00) | v);
    ibm.sub = 0;
    return;
  case 1:
    ibm.addr = u16((ibm.addr & 0x00ff) | (v << 8));
    ibm.sub = 0;
    return;
  case 3:
    if (ibm.addr >= R561_CMAP && ibm.addr < R561_CMAP + 1024) {
      ibm.latch[ibm.sub++] = v;
      if (ibm.sub == 3) {
        memcpy(ibm.cmap[ibm.addr - R561_CMAP], ibm.latch, 3);
        ibm.sub = 0;
        ibm.addr++;
        m_generation++;
      }
      return;
    }
    if (u16 *w = rgb561_wide(ibm.addr, ibm.fb_wat, ibm.ol_wat, ibm.gamma)) {
      if (ibm.sub == 0) {
        ibm.latch[0] = v;
        ibm.sub = 1;
      } else {
        *w = u16((ibm.latch[0] << 2) | (v >> 6));
        ibm.sub = 0;
        ibm.addr++;
        m_generation++;
      }
      return;
    }
    // Anything else takes port 3 as it takes port 2.
    [[fallthrough]];
  case 2:
    if (u8 *b = rgb561_byte(ibm.addr, ibm.regs, ibm.auxfb_wat, ibm.auxol_wat,
                            ibm.cursor, &ibm.cursor_lut[0][0]))
      *b = v;
    else if (m_trace_first)
      first_use("RGB561 write to %04x", ibm.addr);
    ibm.addr++;
    m_generation++;
    return;
  }
}

u8 CTga::rgb561_read(unsigned port) {
  u8 v = 0;
  switch (port) {
  case 0:
    v = u8(ibm.addr);
    break;
  case 1:
    v = u8(ibm.addr >> 8);
    break;
  case 3:
    if (ibm.addr >= R561_CMAP && ibm.addr < R561_CMAP + 1024) {
      v = ibm.cmap[ibm.addr - R561_CMAP][ibm.sub++];
      if (ibm.sub == 3) {
        ibm.sub = 0;
        ibm.addr++;
      }
      break;
    }
    if (u16 *w = rgb561_wide(ibm.addr, ibm.fb_wat, ibm.ol_wat, ibm.gamma)) {
      if (ibm.sub == 0) {
        v = u8(*w >> 2);
        ibm.sub = 1;
      } else {
        v = u8((*w & 3) << 6);
        ibm.sub = 0;
        ibm.addr++;
      }
      break;
    }
    [[fallthrough]];
  case 2:
    if (u8 *b = rgb561_byte(ibm.addr, ibm.regs, ibm.auxfb_wat, ibm.auxol_wat,
                            ibm.cursor, &ibm.cursor_lut[0][0]))
      v = *b;
    ibm.addr++;
    break;
  }
  trace("RGB561 read  port %u = %02x", port, v);
  return v;
}

/**
 * One 32-bit pixel as 0xffRRGGBB, through its window type.
 **/
u32 CTga::rgb561_pixel(u32 p) const {
  static const unsigned wid_bits[8] = {0, 2, 4, 6, 8, 8, 8, 8};
  const unsigned nwid = wid_bits[ibm.regs[R561_CONFIG1] & 7];
  const unsigned wid = nwid ? (p >> (32 - nwid)) & 0xff : 0;
  const u16 wat = ibm.fb_wat[wid];
  const unsigned block = (wat >> 6) & 0xf;
  const bool buf_b = (wat & 0x08) != 0;
  const unsigned mode = (wat >> 1) & 3; // indexed, grey, direct, true colour
  u32 rr, gg, bb;
  switch ((wat >> 4) & 3) {
  case 0: { // 8 bits, through a block of the colour map
    const unsigned idx = buf_b ? (p >> 8) & 0xff : p & 0xff;
    if (mode == 1) {
      rr = gg = bb = idx;
      break;
    }
    const u8 *c = ibm.cmap[(block * 64 + idx) & 1023];
    rr = c[0];
    gg = c[1];
    bb = c[2];
    break;
  }
  case 1: { // 12 bits: a nibble of each component, buffer A high
    const unsigned s = buf_b ? 0 : 4;
    rr = ((p >> (16 + s)) & 0xf) * 17;
    gg = ((p >> (8 + s)) & 0xf) * 17;
    bb = ((p >> s) & 0xf) * 17;
    break;
  }
  case 2: // 16 bits, 5-6-5
    rr = ((p >> 11) & 0x1f) * 255 / 31;
    gg = ((p >> 5) & 0x3f) * 255 / 63;
    bb = (p & 0x1f) * 255 / 31;
    break;
  default: // 24 bits
    rr = (p >> 16) & 0xff;
    gg = (p >> 8) & 0xff;
    bb = p & 0xff;
    break;
  }
  if (((wat >> 4) & 3) != 0 && mode == 2) {
    // Direct colour: each component indexes its column of the map.
    const unsigned base = block * 64;
    rr = ibm.cmap[(base + rr) & 1023][0];
    gg = ibm.cmap[(base + gg) & 1023][1];
    bb = ibm.cmap[(base + bb) & 1023][2];
  }
  return 0xff000000u | (rr << 16) | (gg << 8) | bb;
}

void CTga::draw_rgb561_cursor(unsigned w, unsigned h) {
  const u8 ctl = ibm.regs[R561_CURS_CNTL];
  if (!(ctl & 1))
    return;
  const int x0 =
      int(ibm.regs[R561_CURSOR_X] | (ibm.regs[R561_CURSOR_X + 1] << 8)) -
      int(ibm.regs[R561_HOTSPOT_X]);
  const int y0 =
      int(ibm.regs[R561_CURSOR_Y] | (ibm.regs[R561_CURSOR_Y + 1] << 8)) -
      int(ibm.regs[R561_HOTSPOT_Y]);
  for (unsigned cy = 0; cy < 64; cy++) {
    const int y = y0 + int(cy);
    if (y < 0 || y >= int(h))
      continue;
    for (unsigned cx = 0; cx < 64; cx++) {
      const int x = x0 + int(cx);
      if (x < 0 || x >= int(w))
        continue;
      const unsigned v =
          (ibm.cursor[cy * 16 + cx / 4] >> (6 - 2 * (cx & 3))) & 3;
      if (!v || (v == 3 && (ctl & 0x80)))
        continue;
      const u8 *c = ibm.cursor_lut[v];
      m_frame[size_t(y) * w + unsigned(x)] =
          0xff000000u | (u32(c[0]) << 16) | (u32(c[1]) << 8) | c[2];
    }
  }
}
