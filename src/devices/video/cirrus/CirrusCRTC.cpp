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
 * GD54xx extended CRTC registers (CR19..CR3F).
 *
 * CVGA::crtc_data_w stores every write in vga.crtc.data[] before
 * dispatching, so the handlers here only add side effects and read-only
 * behaviour.
 **/

#include "CirrusGD54xx.hpp"

using namespace cirrus;

void CCirrusGD54xx::crtc_reset() {
  vga.crtc.data[CRTC_CHIP_ID] = m_chip.chip_id;
}

bool CCirrusGD54xx::get_interlace_mode() {
  return (vga.crtc.data[CRTC_MISC_CONTROL] & 0x01) != 0;
}

void CCirrusGD54xx::crtc_map(address_map &map) {
  // CR19..CR3F read back what was written unless a handler below says
  // otherwise (CR19 interlace end, CR1C sync adjust, CR1E/CR1F, ...).
  map(0x19, 0x3f).lr8(NAME([this](offs_t offset) {
    return vga.crtc.data[0x19 + offset];
  }));

  // CR1A: miscellaneous control; bit 0 selects interlace.
  map(CRTC_MISC_CONTROL, CRTC_MISC_CONTROL)
      .lw8(NAME([this](offs_t offset, u8 data) { state.vga_mem_updated = 1; }));

  // CR1B: extended display control. Bit 4 is bit 8 of the CR13 offset,
  // bits 3..2 are display start address bits 17..16.
  map(CRTC_EXT_DISPLAY, CRTC_EXT_DISPLAY)
      .lw8(NAME([this](offs_t offset, u8 data) {
        vga.crtc.offset = (vga.crtc.offset & 0xff) | ((data & 0x10) << 4);
        vga.crtc.start_addr_latch =
            (vga.crtc.start_addr_latch & ~0x30000u) | ((data & 0x0c) << 14);
        state.vga_mem_updated = 1;
      }));

  // CR1D: bit 7 is display start address bit 18.
  map(CRTC_EXT_OVERLAY, CRTC_EXT_OVERLAY)
      .lw8(NAME([this](offs_t offset, u8 data) {
        vga.crtc.start_addr_latch =
            (vga.crtc.start_addr_latch & ~0x40000u) | ((data & 0x80) << 11);
        state.vga_mem_updated = 1;
      }));

  // CR22: graphics data latch read-back (read only).
  map(0x22, 0x22)
      .lrw8(NAME([this](offs_t offset) {
              return vga.gc.latch[vga.gc.read_map_sel & 3];
            }),
            NAME([this](offs_t offset, u8 data) {}));

  // CR25: part status (read only).
  map(CRTC_PART_STATUS, CRTC_PART_STATUS)
      .lrw8(NAME([this](offs_t offset) { return u8(0); }),
            NAME([this](offs_t offset, u8 data) {}));

  // CR26: attribute controller index read-back (read only).
  map(CRTC_ATC_INDEX, CRTC_ATC_INDEX)
      .lrw8(NAME([this](offs_t offset) {
              return u8(vga.attribute.index & 0x3f);
            }),
            NAME([this](offs_t offset, u8 data) {}));

  // CR27: chip identification (read only).
  map(CRTC_CHIP_ID, CRTC_CHIP_ID)
      .lrw8(NAME([this](offs_t offset) { return m_chip.chip_id; }),
            NAME([this](offs_t offset, u8 data) {}));
}
