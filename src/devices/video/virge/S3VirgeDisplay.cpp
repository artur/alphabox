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
 * S3 ViRGE display: the extended modes, the clock synthesiser, the
 * hardware cursor and the streams processor.
 *
 * An extended mode (GR5 bit 6 with CR3A bit 4, the S3 "enhanced 256
 * colour" path) scans VRAM as packed pixels: the start address is CR0C/
 * CR0D with the high bits in CR69 (or the older CR31/CR51 fields), in
 * dwords; the pitch is CR13 with CR51 bits 5..4 (or CR43 bit 2) above it,
 * in units of 8 bytes; the pixel format is CR67's colour mode. On the
 * ViRGE and ViRGE/DX a 15/16-bit pixel takes two VCLKs, so the CRTC's
 * horizontal counts are twice the pixels.
 *
 * With CR67 bits 3..2 set the streams processor feeds the display
 * instead: the primary stream's address, stride and format are its own
 * registers (0x81c0, 0x81c8, 0x8180), and a secondary stream -- video,
 * in YUV or RGB, scaled -- can be laid over it.
 *
 * The cursor is 64x64, two bits a pixel, stored as sixteen bytes a row:
 * for each 16 pixels a big-endian word of the AND plane and one of the
 * XOR plane, at CR4C/CR4D * 1 KB.
 **/

#include "S3Virge.hpp"
#include "gui/gui.hpp"

using namespace virge;

bool CS3Virge::streams_active() const {
  return (cr(CR_EXT_MISC_2) & CR67_STREAMS) == CR67_STREAMS;
}

bool CS3Virge::native_crtc_active() const {
  if (streams_active())
    return true;
  return vga.gc.shift256 && (cr(CR_MISC_1) & CR3A_ENHANCED_256);
}

unsigned CS3Virge::native_bits_per_pixel() const {
  if (streams_active()) {
    switch ((M(PRI_STREAM_CTL) >> 24) & 7) {
    case 3:
      return 15;
    case 5:
      return 16;
    case 6:
      return 24;
    case 7:
      return 32;
    default:
      return 8;
    }
  }
  switch (cr(CR_EXT_MISC_2) >> 4) {
  case 0x2:
  case 0x3:
    return 15;
  case 0x4:
  case 0x5:
    return 16;
  case 0x7:
    return 24;
  case 0xd:
    return m_chip.vx ? 24 : 32;
  default:
    return 8;
  }
}

unsigned CS3Virge::native_width() const {
  if (streams_active())
    return ((M(PRI_SIZE) >> 16) & 0x7ff) + 1;
  const unsigned chars =
      ((vga.crtc.horz_disp_end & 0xff) | ((cr(CR_EXT_H_OVF) & 0x02) << 7)) + 1;
  unsigned w = chars * 8;
  const unsigned bpp = native_bits_per_pixel();
  if ((bpp == 15 || bpp == 16) && !m_chip.vx && !m_chip.gx2)
    w /= 2;
  return w;
}

unsigned CS3Virge::native_height() const {
  unsigned h =
      (vga.crtc.vert_disp_end & 0x3ff) | ((cr(CR_EXT_V_OVF) & 0x02) << 9);
  h += 1;
  if (streams_active()) {
    const unsigned ph = M(PRI_SIZE) & 0x7ff;
    if (ph && ph < h)
      h = ph;
  }
  return h;
}

u32 CS3Virge::native_start() const {
  if (streams_active()) {
    const u32 a = (M(DOUBLE_BUFFER) & 1) ? M(PRI_FB_ADDR1) : M(PRI_FB_ADDR0);
    return a & vram_mask();
  }
  const u32 ext = (cr(CR_EXT_SYS_CTL_3) & 0x1f) |
                  ((cr(CR_MEM_CONFIG) >> 4) & 0x03) |
                  ((cr(CR_EXT_SYS_CTL_2) & 0x03) << 2);
  return ((vga.crtc.start_addr_latch & 0xffff) | (ext << 16)) * 4;
}

u32 CS3Virge::native_pitch() const {
  if (streams_active())
    return M(PRI_STRIDE) & 0xfff;
  u32 off = vga.crtc.offset & 0xff;
  if (cr(CR_EXT_SYS_CTL_2) & 0x30)
    off |= u32(cr(CR_EXT_SYS_CTL_2) & 0x30) << 4;
  else if (cr(0x43) & 0x04)
    off |= 0x100;
  return off * 8;
}

void CS3Virge::determine_screen_dimensions(unsigned *height, unsigned *width) {
  if (!native_crtc_active()) {
    CVGACard::determine_screen_dimensions(height, width);
    return;
  }
  unsigned w = native_width();
  unsigned h = native_height();
  if (m_trace_mode) {
    char mode[160];
    snprintf(mode, sizeof(mode),
             "%ux%u %u bpp%s start %06x pitch %u (CR31 %02x CR3A %02x CR51 "
             "%02x CR67 %02x CR69 %02x)",
             w, h, native_bits_per_pixel(), streams_active() ? " streams" : "",
             native_start(), native_pitch(), cr(0x31), cr(0x3a), cr(0x51),
             cr(0x67), cr(0x69));
    if (m_last_mode != mode) {
      m_last_mode = mode;
      printf("%s: mode %s\n", devid_string, mode);
    }
  }
  if (w < 64 || h < 64) // not programmed yet: keep a sane window
    w = 640, h = 480;
  *width = w > 2048 ? 2048 : w;
  *height = h > 2048 ? 2048 : h;
}

uint64_t CS3Virge::direct_view_hash() const {
  if (!native_crtc_active())
    return CVGACard::direct_view_hash();
  uint64_t h = hash_vram(native_start(), native_pitch() * native_height());
  if (streams_active() && (M(BLEND_CTL) >> 24 & 7) != 1) {
    const u32 sec = (M(DOUBLE_BUFFER) & 2) ? M(SEC_FB_ADDR1) : M(SEC_FB_ADDR0);
    h = hash_vram(sec & vram_mask(),
                  (M(SEC_STRIDE) & 0xfff) * ((M(SEC_SIZE) & 0x7ff) + 1), h);
  }
  return h;
}

/**
 * The PLL: f = 14.318 MHz * (M + 2) / ((N + 2) * 2^R), with M in SR13
 * bits 6..0, N in SR12 bits 4..0 and R above it (two bits on the ViRGE,
 * three on the DX and VX). Only the refresh rate the status bits and the
 * redraw pace follow depends on it.
 **/
void CS3Virge::update_clock() {
  const u8 n = sr(SR_DCLK_N) & 0x1f;
  const u8 rr = (sr(SR_DCLK_N) >> 5) & ((m_chip.dx || m_chip.vx) ? 7 : 3);
  const u8 m = sr(SR_DCLK_M) & 0x7f;
  r.dclk_hz = 14318180.0 * (m + 2) / ((n + 2) * double(1u << rr));
  const unsigned ht =
      ((vga.crtc.horz_total & 0xff) | ((cr(CR_EXT_H_OVF) & 0x01) << 8)) + 5;
  const unsigned vt =
      (vga.crtc.vert_total & 0x3ff) + ((cr(CR_EXT_V_OVF) & 0x01) << 10) + 2;
  const double hz = r.dclk_hz / (ht * 8.0 * vt);
  if (((vga.miscellaneous_output >> 2) & 3) == 3 && hz > 40 && hz < 200) {
    timing.vrefresh_hz = hz;
    timing.refresh_interval_ms = uint64_t(1000.0 / hz);
  }
}

uint64_t CS3Virge::hw_cursor_signature() const {
  const u8 mode = cr(CR_CURSOR_MODE);
  if (!(mode & 1))
    return 0;
  uint64_t sig = mode | (u32(cr(CR_EXT_DAC_CTL)) << 8);
  for (u8 i = CR_CURSOR_X_HI; i <= CR_CURSOR_PAT_Y; i++)
    sig = sig * 1000003u ^ cr(i);
  for (int i = 0; i < 4; i++)
    sig = sig * 1000003u ^ (u32(r.cursor_fg[i]) << 8 | r.cursor_bg[i]);
  const u32 addr =
      (((u32(cr(CR_CURSOR_ADDR_HI)) << 8) | cr(CR_CURSOR_ADDR_LO)) & 0xfff) *
      1024;
  return hash_vram(addr & vram_mask(), 1024, sig);
}

static inline u32 argb(u32 rr, u32 g, u32 b) {
  return 0xff000000u | ((rr & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
}

/// An n-bit component widened to eight bits.
static inline u32 widen(u32 v, int bits) {
  v &= (1u << bits) - 1;
  u32 out = v << (8 - bits);
  for (int s = bits; s < 8; s += bits)
    out |= out >> s;
  return out & 0xff;
}

/// A pixel of the given depth as the display shows it.
static inline u32 pixel_rgb(const u8 *vram, u32 mask, u32 a, unsigned bpp,
                            const pen_t *pens) {
  switch (bpp) {
  case 8:
    return pens[vram[a & mask]];
  case 15: {
    const u32 p = vram[a & mask] | (u32(vram[(a + 1) & mask]) << 8);
    return argb(widen(p >> 10, 5), widen(p >> 5, 5), widen(p, 5));
  }
  case 16: {
    const u32 p = vram[a & mask] | (u32(vram[(a + 1) & mask]) << 8);
    return argb(widen(p >> 11, 5), widen(p >> 5, 6), widen(p, 5));
  }
  default: // 24 and 32: B, G, R in memory order
    return argb(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a & mask]);
  }
}

void CS3Virge::render_native(bitmap_rgb32 &bitmap) {
  const int width = bitmap.width();
  const int height = bitmap.height();
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;
  const unsigned bpp = native_bits_per_pixel();
  const u32 bytes = bpp == 15 ? 2 : bpp / 8;
  const u32 start = native_start();
  const u32 pitch = native_pitch();
  if (vga.dac.dirty) {
    palette_update();
    vga.dac.dirty = 0;
  }
  for (int y = 0; y < height; y++) {
    uint32_t *line = &bitmap.pix(y);
    const u32 row = start + u32(y) * pitch;
    for (int x = 0; x < width; x++)
      line[x] = pixel_rgb(vram, mask, row + u32(x) * bytes, bpp, m_pen_table);
  }
}

/**
 * The cursor's two colours are stacks of up to three bytes (CR4A, CR4B):
 * a palette index in 8-bit modes, a 5:6:5 or 5:5:5 pixel in 16-bit ones,
 * blue, green, red in 24/32-bit ones.
 **/
void CS3Virge::draw_hw_cursor(bitmap_rgb32 &bitmap) {
  const u8 mode = cr(CR_CURSOR_MODE);
  if (!(mode & 1))
    return;
  const unsigned bpp = native_crtc_active() ? native_bits_per_pixel() : 8;
  auto colour = [&](const u8 *c) {
    switch (bpp) {
    case 8:
      return pen(c[0]);
    case 15: {
      const u32 p = c[0] | (u32(c[1]) << 8);
      return argb(widen(p >> 10, 5), widen(p >> 5, 5), widen(p, 5));
    }
    case 16: {
      const u32 p = c[0] | (u32(c[1]) << 8);
      return argb(widen(p >> 11, 5), widen(p >> 5, 6), widen(p, 5));
    }
    default:
      return argb(c[2], c[1], c[0]);
    }
  };
  const u32 fg = colour(r.cursor_fg);
  const u32 bg = colour(r.cursor_bg);
  const bool x11 = (cr(CR_EXT_DAC_CTL) & CR55_CURSOR_X11) != 0;
  const int x0 =
      int(((u32(cr(CR_CURSOR_X_HI)) << 8) | cr(CR_CURSOR_X_LO)) & 0x7ff);
  const int y0 =
      int(((u32(cr(CR_CURSOR_Y_HI)) << 8) | cr(CR_CURSOR_Y_LO)) & 0x7ff);
  const int xoff = cr(CR_CURSOR_PAT_X) & 0x3f;
  const int yoff = cr(CR_CURSOR_PAT_Y) & 0x3f;
  const u32 base =
      (((u32(cr(CR_CURSOR_ADDR_HI)) << 8) | cr(CR_CURSOR_ADDR_LO)) & 0xfff) *
      1024;
  const u32 mask = vram_mask();

  for (int row = yoff; row < 64; row++) {
    const int y = y0 + row - yoff;
    if (y < 0 || y >= bitmap.height())
      continue;
    uint32_t *line = &bitmap.pix(y);
    const u32 a = base + u32(row) * 16;
    for (int col = xoff; col < 64; col++) {
      const int x = x0 + col - xoff;
      if (x < 0 || x >= bitmap.width())
        continue;
      const u32 g = a + u32(col >> 4) * 4;
      const u32 plane_a =
          (u32(vga.memory[g & mask]) << 8) | vga.memory[(g + 1) & mask];
      const u32 plane_b =
          (u32(vga.memory[(g + 2) & mask]) << 8) | vga.memory[(g + 3) & mask];
      const int bit = 15 - (col & 15);
      const bool pa = (plane_a >> bit) & 1, pb = (plane_b >> bit) & 1;
      if (x11) {
        if (pa)
          line[x] = pb ? fg : bg;
      } else if (!pa) {
        line[x] = pb ? fg : bg;
      } else if (pb) {
        line[x] ^= 0x00ffffff;
      }
    }
  }
}

void CS3Virge::draw_overlay(bitmap_rgb32 &bitmap) {}

uint32_t CS3Virge::screen_update(bitmap_rgb32 &bitmap,
                                 const rectangle &cliprect) {
  update_clock();
  if (native_crtc_active()) {
    render_native(bitmap);
    if (streams_active())
      draw_overlay(bitmap);
  } else {
    CVGA::screen_update(bitmap, cliprect);
  }
  draw_hw_cursor(bitmap);
  return 0;
}
