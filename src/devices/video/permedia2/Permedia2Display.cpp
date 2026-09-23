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
 * Permedia 2 display: the graphics processor's frame and the RAMDAC's
 * hardware cursor.
 *
 * With VGAControl's EnableVGADisplay clear, the timing generator (region
 * 0, 0x3000) scans the frame out of SGRAM and the RAMDAC turns it into
 * colour, whatever the SVGA's registers say. The visible width is HTotal
 * + 1 - HbEnd units of 32 bits (64 with Data64Enable), the height VTotal
 * + 1 - VbEnd lines (5.1, 5.2); ScreenBase and ScreenStride are in 64-bit
 * units. The pixel format is the RAMDAC's Color Mode register.
 *
 * The cursor is the RAMDAC's: 1 KB of two bit planes, a 64x64 image or
 * four 32x32 ones, positioned by its bottom-right corner plus (64, 64)
 * (5.8), in one of three colour schemes (5.6.1).
 **/

#include "Permedia2.hpp"
#include "gui/gui.hpp"

using namespace permedia2;

bool CPermedia2::native_crtc_active() const {
  return !(r.vga_control & VGACTL_VGA_DISPLAY);
}

unsigned CPermedia2::native_bits_per_pixel() const {
  const u8 cm = r.rd_indexed[RDI_COLOR_MODE];
  if (!(cm & CM_GUI))
    return 8;
  switch (cm & CM_FORMAT) {
  case PF_RGBA5551:
  case PF_RGBA4444:
  case PF_RGB565:
    return 16;
  case PF_RGBA8888:
    return 32;
  case PF_RGB888:
    return 24;
  }
  return 8;
}

unsigned CPermedia2::native_width() const {
  const u32 units = (reg(H_TOTAL) & 0x7ff) + 1 - (reg(HB_END) & 0x7ff);
  const u32 unit_bits = (reg(VIDEO_CONTROL) & VC_DATA64) ? 64 : 32;
  return (units & 0x7ff) * unit_bits / native_bits_per_pixel();
}

unsigned CPermedia2::native_height() const {
  return ((reg(V_TOTAL) & 0x7ff) + 1 - (reg(VB_END) & 0x7ff)) & 0x7ff;
}

void CPermedia2::determine_screen_dimensions(unsigned *height,
                                             unsigned *width) {
  if (!native_crtc_active()) {
    CVGACard::determine_screen_dimensions(height, width);
    return;
  }
  unsigned w = native_width();
  unsigned h = native_height();
  if (w < 64 || h < 64) // not programmed yet: keep a sane window
    w = 640, h = 480;
  *width = w > 2048 ? 2048 : w;
  *height = h > 2048 ? 2048 : h;
}

/**
 * The palette: eight bits a component with MiscControl's PaletteWidth set,
 * otherwise a VGA's six.
 **/
void CPermedia2::palette_update() {
  if (!(r.rd_indexed[RDI_MISC_CONTROL] & MISC_PALETTE_8BIT)) {
    CVGACard::palette_update();
    return;
  }
  for (int i = 0; i < 256; i++) {
    const u8 *c = &vga.dac.color[3 * (i & vga.dac.mask)];
    set_pen_color(i, c[0], c[1], c[2]);
    bx_gui->palette_change((unsigned)i, c[0], c[1], c[2]);
  }
}

uint64_t CPermedia2::direct_view_hash() const {
  if (!native_crtc_active())
    return CVGACard::direct_view_hash();
  const u32 start = (reg(SCREEN_BASE) & 0xfffff) * 8;
  const u32 pitch = (reg(SCREEN_STRIDE) & 0xfffff) * 8;
  return hash_vram(start, pitch * (native_height() + 1));
}

uint64_t CPermedia2::hw_cursor_signature() const {
  const u8 cr = r.rd_indexed[RDI_CURSOR_CONTROL];
  if (!(cr & CUR_MODE))
    return 0;
  uint64_t sig = cr;
  sig = sig * 1000003u ^ (u32(r.cursor_x_hi) << 8 | r.cursor_x_lo);
  sig = sig * 1000003u ^ (u32(r.cursor_y_hi) << 8 | r.cursor_y_lo);
  for (const auto &c : r.cursor_color)
    sig = sig * 1000003u ^ (u32(c[0]) << 16 | u32(c[1]) << 8 | c[2]);
  for (u8 b : r.cursor_ram)
    sig = sig * 131u ^ b;
  return sig;
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

/**
 * One frame of the graphics processor's display. A true-colour pixel's
 * fields run red to blue from the top with Color Mode's RGB bit set, blue
 * to red without; with its TrueColor bit set each component is then looked
 * up in the palette (gamma correction).
 **/
void CPermedia2::render_native(bitmap_rgb32 &bitmap) {
  const int width = bitmap.width();
  const int height = bitmap.height();
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;
  const u32 vc = reg(VIDEO_CONTROL);

  if (!(vc & VC_ENABLE)) {
    bitmap.fill(black_pen(), bitmap.cliprect());
    return;
  }

  const u32 start = (reg(SCREEN_BASE) & 0xfffff) * 8;
  const u32 pitch = (reg(SCREEN_STRIDE) & 0xfffff) * 8;
  const int dbl = (vc & VC_LINE_DOUBLE) ? 1 : 0;
  const u8 cm = r.rd_indexed[RDI_COLOR_MODE];
  const int fmt = (cm & CM_GUI) ? (cm & CM_FORMAT) : PF_CI8;
  const bool rgb = (cm & CM_RGB) != 0;
  const bool gamma = (cm & CM_TRUECOLOR_PALETTE) != 0;
  const bool pal8 = (r.rd_indexed[RDI_MISC_CONTROL] & MISC_PALETTE_8BIT) != 0;
  const u8 *pal = vga.dac.color;

  // Red, green and blue as the frame holds them, to the screen's colour.
  auto out = [&](u32 hi, u32 mid, u32 lo) {
    u32 rr = rgb ? hi : lo, b = rgb ? lo : hi;
    u32 g = mid;
    if (gamma) {
      rr = pal[3 * rr];
      g = pal[3 * g + 1];
      b = pal[3 * b + 2];
      if (!pal8)
        rr = widen(rr, 6), g = widen(g, 6), b = widen(b, 6);
    }
    return argb(rr, g, b);
  };

  for (int y = 0; y < height; y++) {
    uint32_t *line = &bitmap.pix(y);
    const u32 row = start + u32(y >> dbl) * pitch;
    for (int x = 0; x < width; x++) {
      switch (fmt) {
      case PF_CI8:
        line[x] = pen(vram[(row + x) & mask]);
        break;
      case PF_RGB332: {
        const u32 p = vram[(row + x) & mask];
        line[x] = out(widen(p >> 5, 3), widen(p >> 2, 3), widen(p, 2));
        break;
      }
      case PF_RGB232_OFFSET:
      case PF_RGBA2321: {
        const u32 p = vram[(row + x) & mask];
        line[x] = out(widen(p >> 5, 2), widen(p >> 2, 3), widen(p, 2));
        break;
      }
      case PF_RGBA5551:
      case PF_RGBA4444:
      case PF_RGB565: {
        const u32 a = (row + x * 2) & mask;
        const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
        if (fmt == PF_RGB565)
          line[x] = out(widen(p >> 11, 5), widen(p >> 5, 6), widen(p, 5));
        else if (fmt == PF_RGBA5551)
          line[x] = out(widen(p >> 10, 5), widen(p >> 5, 5), widen(p, 5));
        else
          line[x] = out(widen(p >> 8, 4), widen(p >> 4, 4), widen(p, 4));
        break;
      }
      case PF_RGB888: {
        const u32 a = (row + x * 3) & mask;
        line[x] = out(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a]);
        break;
      }
      default: { // RGBA8888
        const u32 a = (row + x * 4) & mask;
        line[x] = out(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a]);
        break;
      }
      }
    }
  }
}

void CPermedia2::draw_hw_cursor(bitmap_rgb32 &bitmap) {
  const u8 cr = r.rd_indexed[RDI_CURSOR_CONTROL];
  const int mode = cr & CUR_MODE;
  if (!mode)
    return;
  const bool big = (cr & CUR_SIZE_64) != 0;
  const int size = big ? 64 : 32;
  const int sel = (cr >> CUR_SELECT_SHIFT) & 3;
  const int qx = big ? 0 : (sel & 1) * 32, qy = big ? 0 : (sel >> 1) * 32;
  const int x0 = int((u32(r.cursor_x_hi) << 8) | r.cursor_x_lo) - 64;
  const int y0 = int((u32(r.cursor_y_hi) << 8) | r.cursor_y_lo) - 64;
  auto colour = [&](int k) {
    const u8 *c = r.cursor_color[k + 1];
    return argb(c[0], c[1], c[2]);
  };

  for (int row = 0; row < size; row++) {
    const int y = y0 + row;
    if (y < 0 || y >= bitmap.height())
      continue;
    uint32_t *line = &bitmap.pix(y);
    for (int col = 0; col < size; col++) {
      const int x = x0 + col;
      if (x < 0 || x >= bitmap.width())
        continue;
      const u32 bit = u32(qy + row) * 64 + u32(qx + col);
      const int shift = 7 - int(bit & 7);
      const int p0 = (r.cursor_ram[bit >> 3] >> shift) & 1;
      const int p1 = (r.cursor_ram[0x200 + (bit >> 3)] >> shift) & 1;
      const int v = (p1 << 1) | p0;
      switch (mode) {
      case 1: // three colour
        if (v)
          line[x] = colour(v - 1);
        break;
      case 2: // XGA
        if (v == 0 || v == 1)
          line[x] = colour(v);
        else if (v == 3)
          line[x] ^= 0x00ffffff;
        break;
      default: // X Windows
        if (v >= 2)
          line[x] = colour(v - 2);
        break;
      }
    }
  }
}

uint32_t CPermedia2::screen_update(bitmap_rgb32 &bitmap,
                                   const rectangle &cliprect) {
  if (native_crtc_active())
    render_native(bitmap);
  else
    CVGA::screen_update(bitmap, cliprect);
  draw_hw_cursor(bitmap);
  return 0;
}
