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
 * GD54xx extended sequencer registers (SR05..SR1F).
 *
 * CVGA::sequencer_data_w stores every write in vga.sequencer.data[] before
 * dispatching and calls recompute_params() after it, and the standard map
 * reads unclaimed indices back from that array. Plain storage registers
 * (SR08, SR09/SR0A scratch, the dot clocks, SR12/SR13, ...) therefore need
 * no handler; the ones here have side effects or read-only bits.
 **/

#include "CirrusGD54xx.hpp"

using namespace cirrus;

void CCirrusGD54xx::sequencer_reset() {
  u8 *sr = vga.sequencer.data;

  sr[SEQ_UNLOCK] = SEQ_UNLOCK_LOCKED_READ;
  sr[SEQ_DRAM_CONTROL] = m_chip.sr0f_strap;
  sr[SEQ_CONFIG] = m_chip.sr17_strap;
  sr[SEQ_MCLK] = m_chip.sr1f_mclk;
  // The 543x BIOS reads the memory size from here.
  sr[SEQ_MEMORY_SIZE] = (m_chip.vram_bytes >= (4u << 20))   ? 0x04
                        : (m_chip.vram_bytes >= (2u << 20)) ? 0x03
                                                            : 0x02;

  // Power-on dot clocks (numerator, denominator/post-scalar):
  // 25.227, 28.325, 41.165 and 36.082 MHz from the 14.31818 MHz reference.
  static const u8 vclk[4][2] = {
      {0x4a, 0x2b}, {0x5b, 0x2f}, {0x45, 0x30}, {0x7e, 0x33}};
  for (int i = 0; i < 4; i++) {
    sr[SEQ_VCLK0_NUM + i] = vclk[i][0];
    sr[SEQ_VCLK0_DENOM + i] = vclk[i][1];
  }
}

void CCirrusGD54xx::sequencer_map(address_map &map) {
  // Everything from SR05 up reads back what was written, unless a handler
  // below says otherwise. (The standard map already reads SR00..SR04 back.)

  // SR06: extension unlock. Only 0x12 (ignoring bits 7..5 and 3) unlocks;
  // the register then reads 0x12, otherwise 0x0F. The GD5429 and later
  // leave the extensions decoded regardless, so nothing else is gated.
  map(SEQ_UNLOCK, SEQ_UNLOCK).lw8(NAME([this](offs_t offset, u8 data) {
    vga.sequencer.data[SEQ_UNLOCK] = ((data & 0x17) == SEQ_UNLOCK_MAGIC)
                                         ? SEQ_UNLOCK_MAGIC
                                         : SEQ_UNLOCK_LOCKED_READ;
  }));

  // SR07: extended mode -- packed-pixel memory path and pixel depth.
  map(SEQ_EXT_MODE, SEQ_EXT_MODE).lw8(NAME([this](offs_t offset, u8 data) {
    vga.sequencer.data[SEQ_EXT_MODE] = data;
    define_video_mode();
    state.vga_mem_updated = 1;
  }));

  // SR10/SR11: cursor position, answering at every index whose low five
  // bits match; the top three index bits are the position's low bits.
  map(SEQ_CURSOR_X, SEQ_CURSOR_X)
      .mirror(0xe0)
      .lrw8(NAME([this](offs_t offset) {
              return vga.sequencer.data[SEQ_CURSOR_X];
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.sequencer.data[SEQ_CURSOR_X] = data;
              m_cursor_x_low = vga.sequencer.index >> 5;
            }));
  map(SEQ_CURSOR_Y, SEQ_CURSOR_Y)
      .mirror(0xe0)
      .lrw8(NAME([this](offs_t offset) {
              return vga.sequencer.data[SEQ_CURSOR_Y];
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.sequencer.data[SEQ_CURSOR_Y] = data;
              m_cursor_y_low = vga.sequencer.index >> 5;
            }));

  // SR17: bus-type straps are read only.
  map(SEQ_CONFIG, SEQ_CONFIG).lw8(NAME([this](offs_t offset, u8 data) {
    vga.sequencer.data[SEQ_CONFIG] =
        (m_chip.sr17_strap & SEQ_CONFIG_STRAPS) | (data & ~SEQ_CONFIG_STRAPS);
  }));
}
