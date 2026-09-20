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
 * Mach64 display: the extended mode, the hardware cursor, the 8-bit DAC.
 *
 * With CRTC_GEN_CNTL's EXT_DISP_EN and CRTC_EN both set the Mach64's own
 * CRTC drives the screen: the size comes from CRTC_H_TOTAL_DISP and
 * CRTC_V_TOTAL_DISP, the framebuffer start and pitch from CRTC_OFF_PITCH,
 * the pixel format from CRTC_GEN_CNTL, all independent of the VGA
 * registers the BIOS left behind. Otherwise the card is a VGA and CVGA
 * draws it.
 *
 * The hardware cursor is 64x64, two bits a pixel from a 1 KB bitmap in
 * VRAM: 0 and 1 are the two cursor colours, 2 transparent, 3 inverts the
 * pixel under it. CUR_HORZ_VERT_OFF says where in the bitmap the screen
 * edge falls.
 **/

#include "Mach64.hpp"
#include "gui/gui.hpp"

using namespace mach64;

bool CMach64::native_crtc_active() const {
  return (r.crtc_gen_cntl & (CRTC_EXT_DISP_EN | CRTC_EN)) ==
         (CRTC_EXT_DISP_EN | CRTC_EN);
}

/// The extended CRTC's blanking is drawn as black, not gated, so the
/// screen goes dark rather than stale.
bool CMach64::display_enabled() const { return true; }

unsigned CMach64::native_width() const {
  return (((r.crtc_h_total_disp >> 16) & 0xff) + 1) * 8;
}

unsigned CMach64::native_height() const {
  return ((r.crtc_v_total_disp >> 16) & 0x7ff) + 1;
}

u8 CMach64::native_bpp_code() const {
  return u8((r.crtc_gen_cntl & CRTC_PIX_WIDTH_MASK) >> CRTC_PIX_WIDTH_SHIFT);
}

/// CRTC_OFF_PITCH's pitch is in units of eight pixels.
unsigned CMach64::native_pitch_bytes() const {
  const unsigned px = ((r.crtc_off_pitch >> 22) & 0x3ff) * 8;
  switch (native_bpp_code()) {
  case BPP_4:
    return px / 2;
  case BPP_8:
    return px;
  case BPP_15:
  case BPP_16:
    return px * 2;
  case BPP_24:
    return px * 3;
  case BPP_32:
    return px * 4;
  default:
    return px / 8;
  }
}

void CMach64::determine_screen_dimensions(unsigned *height, unsigned *width) {
  if (!native_crtc_active()) {
    CVGACard::determine_screen_dimensions(height, width);
    return;
  }
  unsigned w = native_width();
  unsigned h = native_height();
  if (w > 2048)
    w = 2048;
  if (h > 2048)
    h = 2048;
  *width = w;
  *height = h;
}

/**
 * The palette. With DAC_8BIT_EN the DAC takes its colours at eight bits;
 * otherwise it is a VGA's six-bit one.
 **/
void CMach64::palette_update() {
  if (!(r.dac_cntl & DAC_8BIT_EN)) {
    CVGACard::palette_update();
    return;
  }
  for (int i = 0; i < 256; i++) {
    const u8 *c = &vga.dac.color[3 * (i & vga.dac.mask)];
    set_pen_color(i, c[0], c[1], c[2]);
    bx_gui->palette_change((unsigned)i, c[0], c[1], c[2]);
  }
}

uint64_t CMach64::hw_cursor_signature() const {
  if (!(r.gen_test_cntl & GEN_CUR_EN))
    return 0;
  uint64_t sig = 1;
  sig = sig * 1000003u ^ r.cur_offset;
  sig = sig * 1000003u ^ r.cur_horz_vert_posn;
  sig = sig * 1000003u ^ r.cur_horz_vert_off;
  sig = sig * 1000003u ^ r.cur_clr0;
  sig = sig * 1000003u ^ r.cur_clr1;
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

/**
 * One frame of the extended mode. Double scan repeats every line.
 **/
void CMach64::render_native(bitmap_rgb32 &bitmap) {
  const int width = bitmap.width();
  const int height = bitmap.height();
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;

  if (r.crtc_gen_cntl & (CRTC_DISPLAY_DIS | CRTC_HSYNC_DIS | CRTC_VSYNC_DIS)) {
    bitmap.fill(black_pen(), bitmap.cliprect());
    return;
  }

  const u32 start = (r.crtc_off_pitch & 0xfffff) * 8;
  const u32 pitch = native_pitch_bytes();
  const int dbl = (r.crtc_gen_cntl & CRTC_DBL_SCAN_EN) ? 1 : 0;
  const u8 bpp = native_bpp_code();

  for (int y = 0; y < height; y++) {
    uint32_t *line = &bitmap.pix(y);
    const u32 row = start + u32(y >> dbl) * pitch;
    switch (bpp) {
    case BPP_4:
      for (int x = 0; x < width; x += 2) {
        const u8 d = vram[(row + (x >> 1)) & mask];
        line[x] = pen(d & 0x0f);
        if (x + 1 < width)
          line[x + 1] = pen(d >> 4);
      }
      break;
    case BPP_8:
      for (int x = 0; x < width; x++)
        line[x] = pen(vram[(row + x) & mask]);
      break;
    case BPP_15:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 2) & mask;
        const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
        line[x] = argb(expand5(p >> 10), expand5(p >> 5), expand5(p));
      }
      break;
    case BPP_16:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 2) & mask;
        const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
        line[x] = argb(expand5(p >> 11), expand6(p >> 5), expand5(p));
      }
      break;
    case BPP_24:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 3) & mask;
        line[x] = argb(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a]);
      }
      break;
    case BPP_32:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 4) & mask;
        line[x] = argb(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a]);
      }
      break;
    default: // 1 bpp: two pens
      for (int x = 0; x < width; x++) {
        const u8 d = vram[(row + (x >> 3)) & mask];
        line[x] = pen((d >> (7 - (x & 7))) & 1);
      }
      break;
    }
  }
}

void CMach64::draw_hw_cursor(bitmap_rgb32 &bitmap) {
  if (!(r.gen_test_cntl & GEN_CUR_EN))
    return;
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;
  const u32 col0 = argb(r.cur_clr0 >> 24, r.cur_clr0 >> 16, r.cur_clr0 >> 8);
  const u32 col1 = argb(r.cur_clr1 >> 24, r.cur_clr1 >> 16, r.cur_clr1 >> 8);
  const int x0 = int(r.cur_horz_vert_posn & 0x7ff);
  const int y0 = int((r.cur_horz_vert_posn >> 16) & 0x7ff);
  const int xoff = int(r.cur_horz_vert_off & 0x3f);
  const int yoff = int((r.cur_horz_vert_off >> 16) & 0x3f);
  u32 addr = ((r.cur_offset & 0xfffff) << 3) + u32(yoff) * 16;

  for (int row = yoff; row < 64; row++, addr += 16) {
    const int y = y0 + (row - yoff);
    if (y >= bitmap.height())
      break;
    uint32_t *line = &bitmap.pix(y);
    for (int col = xoff; col < 64; col++) {
      const int x = x0 + (col - xoff);
      if (x >= bitmap.width())
        break;
      const u32 a = (addr + (col >> 3) * 2) & mask;
      const u32 word = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
      switch ((word >> ((col & 7) * 2)) & 3) {
      case 0:
        line[x] = col0;
        break;
      case 1:
        line[x] = col1;
        break;
      case 3:
        line[x] ^= 0x00ffffff;
        break;
      }
    }
  }
}

uint32_t CMach64::screen_update(bitmap_rgb32 &bitmap,
                                const rectangle &cliprect) {
  if (native_crtc_active()) {
    render_native(bitmap);
    draw_hw_cursor(bitmap);
    return 0;
  }
  CVGA::screen_update(bitmap, cliprect);
  return 0;
}
