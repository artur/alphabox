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
 * GD54xx hardware cursor.
 *
 * Position: SR10/SR11 (plus the low bits carried in their index), top-left
 * corner, no hot spot. Attributes: SR12. The pattern lives at the top of
 * VRAM as two bit planes -- plane 0 and plane 1 select, per pixel:
 * transparent, inverted, background (extended palette 0) or foreground
 * (extended palette 15).
 *   32x32: 256-byte slots from VRAM end - 16 KB, chosen by SR13; plane 0 is
 *          the first 128 bytes (4 per row), plane 1 the next 128.
 *   64x64: VRAM end - 1 KB; each row is 8 bytes of plane 0 then 8 of plane 1.
 **/

#include "CirrusGD54xx.hpp"

using namespace cirrus;

u16 CCirrusGD54xx::cursor_x() const {
  return u16((vga.sequencer.data[SEQ_CURSOR_X] << 3) | m_cursor_x_low);
}

u16 CCirrusGD54xx::cursor_y() const {
  return u16((vga.sequencer.data[SEQ_CURSOR_Y] << 3) | m_cursor_y_low);
}

/// The cursor is not a VRAM write, so its registers feed the dirty gate.
uint64_t CCirrusGD54xx::hw_cursor_signature() const {
  return (uint64_t(vga.sequencer.data[SEQ_CURSOR_ATTR]) << 48) |
         (uint64_t(vga.sequencer.data[SEQ_CURSOR_PATTERN]) << 40) |
         (uint64_t(cursor_x()) << 16) | uint64_t(cursor_y());
}

void CCirrusGD54xx::draw_hw_cursor(bitmap_rgb32 &bitmap,
                                   const rectangle &cliprect) {
  const u8 attr = vga.sequencer.data[SEQ_CURSOR_ATTR];
  if (!(attr & CURSOR_ENABLE))
    return;

  const bool large = (attr & CURSOR_LARGE) != 0;
  const int size = large ? 64 : 32;
  const u32 base =
      large ? m_chip.vram_bytes - 0x400
            : m_chip.vram_bytes - 0x4000 +
                  (vga.sequencer.data[SEQ_CURSOR_PATTERN] & 0x3f) * 256u;
  const int row_step = large ? 16 : 4; // bytes per row of plane 0
  const u32 plane1 = large ? 8 : 128;  // plane 1 offset from plane 0
  const u32 bg = ext_palette_color(0);
  const u32 fg = ext_palette_color(15);
  const int x0 = cursor_x();
  const int y0 = cursor_y();
  const u8 *vram = vga.memory;

  for (int row = 0; row < size; row++) {
    const int y = y0 + row;
    if (y >= bitmap.height())
      break;
    const u32 p0 = base + row * row_step;
    for (int col = 0; col < size; col++) {
      const int x = x0 + col;
      if (x >= bitmap.width())
        break;
      const int shift = 7 - (col & 7);
      const int b0 = (vram[p0 + (col >> 3)] >> shift) & 1;
      const int b1 = (vram[p0 + plane1 + (col >> 3)] >> shift) & 1;
      uint32_t &pix = bitmap.pix(y, x);
      switch (b0 | (b1 << 1)) {
      case 1:
        pix ^= 0x00ffffff;
        break;
      case 2:
        pix = bg;
        break;
      case 3:
        pix = fg;
        break;
      }
    }
  }
}

uint32_t CCirrusGD54xx::screen_update(bitmap_rgb32 &bitmap,
                                      const rectangle &cliprect) {
  CVGA::screen_update(bitmap, cliprect);
  if (svga.rgb8_en || svga.rgb15_en || svga.rgb16_en || svga.rgb24_en ||
      svga.rgb32_en)
    draw_hw_cursor(bitmap, cliprect);
  return 0;
}
