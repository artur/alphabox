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
 * Radeon display: the primary CRTC's extended modes, the hardware cursor,
 * the 8-bit palette.
 *
 * With CRTC_GEN_CNTL's EXT_DISP_EN and CRTC_EN both set the Radeon's own
 * CRTC drives the screen: the size from CRTC_H_TOTAL_DISP (<24:16>
 * characters less one) and CRTC_V_TOTAL_DISP (<27:16> lines less one),
 * the format from CRTC_GEN_CNTL's PIX_WIDTH, the start from CRTC_OFFSET
 * (from DISPLAY_BASE_ADDR in the memory controller's space) and the pitch
 * from CRTC_PITCH (<10:0>, eight pixels a unit). Otherwise the card is a
 * VGA and CVGA draws it.
 *
 * The hardware cursor is 64x64. In mono mode (CUR_MODE 0) a row is 16
 * bytes, an AND mask then an XOR mask, most significant bit first
 * (QEMU's reading): AND 0 draws CUR_CLR0 or CUR_CLR1 by the XOR bit, AND 1
 * leaves the pixel or inverts it. In ARGB mode (CUR_MODE 2) a row is 64
 * dwords, premultiplied by their alpha, blended over the screen. CUR_OFFSET is
 * where the first row shown starts (the driver adds the vertical clip itself);
 * CUR_HORZ_VERT_OFF's <21:16> skips pixels of each row and <5:0> rows at the
 * bottom.
 **/

#include "Radeon.hpp"
#include "gui/gui.hpp"

#include <algorithm>

using namespace radeon;

bool CRadeon::native_crtc_active() const {
  return (R(CRTC_GEN_CNTL) & (CRTC_EXT_DISP_EN | CRTC_EN)) ==
         (CRTC_EXT_DISP_EN | CRTC_EN);
}

unsigned CRadeon::native_width() const {
  return (((R(CRTC_H_TOTAL_DISP) >> 16) & 0x1ff) + 1) * 8;
}

unsigned CRadeon::native_height() const {
  return ((R(CRTC_V_TOTAL_DISP) >> 16) & 0xfff) + 1;
}

unsigned CRadeon::native_bytes_pp() const {
  switch ((R(CRTC_GEN_CNTL) & CRTC_PIX_WIDTH_MASK) >> CRTC_PIX_WIDTH_SHIFT) {
  case PIX_15BPP:
  case PIX_16BPP:
    return 2;
  case PIX_24BPP:
    return 3;
  case PIX_32BPP:
    return 4;
  default:
    return 1;
  }
}

unsigned CRadeon::native_pitch_bytes() const {
  return (R(CRTC_PITCH) & 0x7ff) * 8 * native_bytes_pp();
}

u32 CRadeon::native_start() const {
  return mc_to_vram(R(DISPLAY_BASE_ADDR) + (R(CRTC_OFFSET) & 0x07ffffffu));
}

void CRadeon::determine_screen_dimensions(unsigned *height, unsigned *width) {
  if (!native_crtc_active()) {
    CVGACard::determine_screen_dimensions(height, width);
    return;
  }
  unsigned w = native_width(), h = native_height();
  if (R(CRTC_GEN_CNTL) & CRTC_DBL_SCAN_EN)
    h *= 2;
  *width = w > 2048 ? 2048 : w;
  *height = h > 2048 ? 2048 : h;
}

/**
 * With DAC_8BIT_EN the DAC holds its colours at eight bits; otherwise it
 * is a VGA's six-bit one.
 **/
void CRadeon::palette_update() {
  if (!(R(DAC_CNTL) & DAC_8BIT_EN)) {
    CVGACard::palette_update();
    return;
  }
  for (int i = 0; i < 256; i++) {
    const u8 *c = &vga.dac.color[3 * (i & vga.dac.mask)];
    set_pen_color(i, c[0], c[1], c[2]);
    bx_gui->palette_change((unsigned)i, c[0], c[1], c[2]);
  }
}

uint64_t CRadeon::hw_cursor_signature() const {
  if (!(R(CRTC_GEN_CNTL) & CRTC_CUR_EN))
    return 0;
  uint64_t sig = 1;
  for (u32 reg : {CRTC_GEN_CNTL, CUR_OFFSET, CUR_HORZ_VERT_POSN,
                  CUR_HORZ_VERT_OFF, CUR_CLR0, CUR_CLR1})
    sig = sig * 1000003u ^ R(reg);
  return sig;
}

static inline u32 argb(u32 rr, u32 g, u32 b) {
  return 0xff000000u | ((rr & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
}
static inline u32 expand5(u32 v) {
  v &= 0x1f;
  return (v << 3) | (v >> 2);
}
static inline u32 expand6(u32 v) {
  v &= 0x3f;
  return (v << 2) | (v >> 4);
}

void CRadeon::render_native(bitmap_rgb32 &bitmap) {
  const int width = bitmap.width();
  const int height = bitmap.height();
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;

  if (R(CRTC_EXT_CNTL) & CRTC_DISPLAY_DIS) {
    bitmap.fill(black_pen(), bitmap.cliprect());
    return;
  }
  const u32 start = native_start();
  const u32 pitch = native_pitch_bytes();
  const int dbl = (R(CRTC_GEN_CNTL) & CRTC_DBL_SCAN_EN) ? 1 : 0;
  const u32 code =
      (R(CRTC_GEN_CNTL) & CRTC_PIX_WIDTH_MASK) >> CRTC_PIX_WIDTH_SHIFT;

  for (int y = 0; y < height; y++) {
    uint32_t *line = &bitmap.pix(y);
    const u32 row = start + u32(y >> dbl) * pitch;
    switch (code) {
    case PIX_8BPP:
      for (int x = 0; x < width; x++)
        line[x] = pen(vram[(row + x) & mask]);
      break;
    case PIX_15BPP:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 2) & mask;
        const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
        line[x] = argb(expand5(p >> 10), expand5(p >> 5), expand5(p));
      }
      break;
    case PIX_16BPP:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 2) & mask;
        const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
        line[x] = argb(expand5(p >> 11), expand6(p >> 5), expand5(p));
      }
      break;
    case PIX_24BPP:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 3) & mask;
        line[x] = argb(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a]);
      }
      break;
    case PIX_32BPP:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 4) & mask;
        line[x] = argb(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a]);
      }
      break;
    default: // 4 bpp
      for (int x = 0; x < width; x += 2) {
        const u8 d = vram[(row + (x >> 1)) & mask];
        line[x] = pen(d & 0x0f);
        if (x + 1 < width)
          line[x + 1] = pen(d >> 4);
      }
      break;
    }
  }
}

void CRadeon::draw_hw_cursor(bitmap_rgb32 &bitmap) {
  const u32 gen = R(CRTC_GEN_CNTL);
  if (!(gen & CRTC_CUR_EN))
    return;
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;
  const u32 mode = (gen & CRTC_CUR_MODE_MASK) >> CRTC_CUR_MODE_SHIFT;
  const int x0 = int((R(CUR_HORZ_VERT_POSN) >> 16) & 0xfff);
  const int y0 = int(R(CUR_HORZ_VERT_POSN) & 0xfff);
  const int xoff = int((R(CUR_HORZ_VERT_OFF) >> 16) & 0x3f);
  const int yoff = int(R(CUR_HORZ_VERT_OFF) & 0x3f);
  const u32 base =
      mc_to_vram(R(DISPLAY_BASE_ADDR) + (R(CUR_OFFSET) & 0x07fffff0u));
  const u32 c0 = argb(R(CUR_CLR0) >> 16, R(CUR_CLR0) >> 8, R(CUR_CLR0));
  const u32 c1 = argb(R(CUR_CLR1) >> 16, R(CUR_CLR1) >> 8, R(CUR_CLR1));
  const int dbl = (gen & CRTC_DBL_SCAN_EN) ? 1 : 0;

  for (int row = 0; row < 64 - yoff; row++) {
    for (int rep = 0; rep <= dbl; rep++) {
      const int y = ((y0 + row) << dbl) + rep;
      if (y >= bitmap.height())
        return;
      uint32_t *line = &bitmap.pix(y);
      for (int col = xoff; col < 64; col++) {
        const int x = x0 + (col - xoff);
        if (x >= bitmap.width())
          break;
        if (mode == 0) {
          const u32 a = base + u32(row) * 16 + u32(col >> 3);
          const int bit = 7 - (col & 7);
          const bool andb = (vram[a & mask] >> bit) & 1;
          const bool xorb = (vram[(a + 8) & mask] >> bit) & 1;
          if (!andb)
            line[x] = xorb ? c1 : c0;
          else if (xorb)
            line[x] ^= 0x00ffffff;
        } else {
          const u32 a = base + u32(row) * 256 + u32(col) * 4;
          const u32 p = vram[a & mask] | (u32(vram[(a + 1) & mask]) << 8) |
                        (u32(vram[(a + 2) & mask]) << 16) |
                        (u32(vram[(a + 3) & mask]) << 24);
          // premultiplied alpha: X.org loads ARGB cursor images (which X
          // keeps premultiplied) unchanged, and colours its mono cursors
          // without premultiplying only because their pixels are opaque
          // or clear (xf86-video-ati radeon_cursor.c)
          const u32 al = p >> 24;
          if (p == 0)
            continue;
          const u32 d = line[x];
          u32 out = 0xff000000u;
          for (int s = 0; s < 24; s += 8) {
            const u32 sc = (p >> s) & 0xff, dc = (d >> s) & 0xff;
            out |= std::min(255u, sc + (dc * (255 - al) + 127) / 255) << s;
          }
          line[x] = out;
        }
      }
    }
  }
}

uint32_t CRadeon::screen_update(bitmap_rgb32 &bitmap,
                                const rectangle &cliprect) {
  if (native_crtc_active()) {
    render_native(bitmap);
    draw_hw_cursor(bitmap);
    return 0;
  }
  CVGA::screen_update(bitmap, cliprect);
  return 0;
}
