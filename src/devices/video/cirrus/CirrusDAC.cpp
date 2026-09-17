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
 * GD54xx RAMDAC: the hidden DAC register, the extended (cursor) palette
 * and the pixel depth that SR07 and the hidden DAC select together.
 **/

#include "CirrusGD54xx.hpp"

using namespace cirrus;

/**
 * Pixel mask read (0x3c6). Four reads in a row arm the hidden DAC; the
 * fifth returns it instead of the mask.
 **/
u8 CCirrusGD54xx::dac_mask_read() {
  if (++m_dac_arm > DAC_HIDDEN_ARM_COUNT) {
    m_dac_arm = 0;
    return m_hidden_dac;
  }
  return ramdac_mask_r(0);
}

/**
 * Pixel mask write (0x3c6). Once armed, the write sets the hidden DAC;
 * either way the arming is spent.
 **/
void CCirrusGD54xx::dac_mask_write(u8 data) {
  if (m_dac_arm == DAC_HIDDEN_ARM_COUNT) {
    m_dac_arm = 0;
    m_hidden_dac = data;
    define_video_mode();
    state.vga_mem_updated = 1;
    return;
  }
  m_dac_arm = 0;
  ramdac_mask_w(0, data);
}

/**
 * Palette data read (0x3c9) while SR12 routes the DAC to the extended
 * palette. Returns false when the standard palette should answer.
 **/
bool CCirrusGD54xx::dac_data_read(u8 &data) {
  if (!(vga.sequencer.data[SEQ_CURSOR_ATTR] & CURSOR_EXT_PALETTE))
    return false;

  data = 0xff;
  if (vga.dac.read) {
    data = m_ext_palette[(vga.dac.read_index & 0x0f) * 3 + vga.dac.state];
    if (++vga.dac.state == 3) {
      vga.dac.state = 0;
      vga.dac.read_index++;
    }
  }
  return true;
}

/**
 * Palette data write (0x3c9) while SR12 routes the DAC to the extended
 * palette. Returns false when the standard palette should take it.
 **/
bool CCirrusGD54xx::dac_data_write(u8 data) {
  if (!(vga.sequencer.data[SEQ_CURSOR_ATTR] & CURSOR_EXT_PALETTE))
    return false;

  if (!vga.dac.read) {
    vga.dac.loading[vga.dac.state] = data;
    if (++vga.dac.state == 3) {
      const int i = (vga.dac.write_index & 0x0f) * 3;
      m_ext_palette[i] = vga.dac.loading[0];
      m_ext_palette[i + 1] = vga.dac.loading[1];
      m_ext_palette[i + 2] = vga.dac.loading[2];
      vga.dac.state = 0;
      vga.dac.write_index++;
      state.vga_mem_updated = 1;
    }
  }
  return true;
}

/// Extended palette entry as ARGB (6-bit DAC values expanded to 8 bits).
u32 CCirrusGD54xx::ext_palette_color(int index) const {
  const u8 *c = &m_ext_palette[(index & 0x0f) * 3];
  auto x = [](u8 v) -> u32 {
    v &= 0x3f;
    return u32((v << 2) | (v >> 4));
  };
  return 0xff000000u | (x(c[0]) << 16) | (x(c[1]) << 8) | x(c[2]);
}

/**
 * Select the renderer from SR07 (packed-pixel mode and depth) and, for the
 * 15/16 bpp depths, the hidden DAC's pixel format.
 **/
void CCirrusGD54xx::define_video_mode() {
  svga.rgb8_en = 0;
  svga.rgb15_en = 0;
  svga.rgb16_en = 0;
  svga.rgb24_en = 0;
  svga.rgb32_en = 0;

  const u8 sr07 = vga.sequencer.data[SEQ_EXT_MODE];
  if (!(sr07 & SEQ_EXT_MODE_SVGA))
    return;

  switch (sr07 & SEQ_EXT_MODE_DEPTH_MASK) {
  case SEQ_DEPTH_16_DOUBLE_VCLK:
  case SEQ_DEPTH_16:
    if ((m_hidden_dac & DAC_FORMAT_MASK) == DAC_FORMAT_565)
      svga.rgb16_en = 1;
    else
      svga.rgb15_en = 1;
    break;
  case SEQ_DEPTH_24:
    svga.rgb24_en = 1;
    break;
  case SEQ_DEPTH_32:
    svga.rgb32_en = 1;
    break;
  case SEQ_DEPTH_8:
  default:
    svga.rgb8_en = 1;
    break;
  }
}
