/* AXPbox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/axpbox
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
 * GD54xx extended graphics controller registers (GR00/GR01/GR05 widened,
 * GR09..GR3F).
 *
 * Unlike the sequencer and CRTC, CVGA does not keep a raw copy of GR
 * writes, so every extended register is stored in m_gr[].
 **/

#include "CirrusGD54xx.hpp"

using namespace cirrus;

void CCirrusGD54xx::graphics_reset() {
  memset(m_gr, 0, sizeof(m_gr));
  m_gr[GC_DRAM_EXT] = 0x0f; // fastest DRAM timing
}

void CCirrusGD54xx::gc_map(address_map &map) {
  // GR09..GR3F: storage unless a handler below says otherwise.
  map(GC_BANK_0, GC_BLT_LAST)
      .lrw8(NAME([this](offs_t offset) { return m_gr[GC_BANK_0 + offset]; }),
            NAME([this](offs_t offset, u8 data) {
              m_gr[GC_BANK_0 + offset] = data;
            }));

  // GR00/GR01: set/reset and its enable. With extended write modes on,
  // they are the full 8-bit background/foreground colours of modes 4/5.
  map(0x00, 0x01)
      .lrw8(NAME([this](offs_t offset) {
              return (m_gr[GC_BANK_MODE] & GC_BANK_MODE_EXT_WRITE)
                         ? m_gr[offset]
                         : u8(m_gr[offset] & 0x0f);
            }),
            NAME([this](offs_t offset, u8 data) {
              m_gr[offset] = data;
              if (offset == 0)
                vga.gc.set_reset = data & 0x0f;
              else
                vga.gc.enable_set_reset = data & 0x0f;
            }));

  // GR05: graphics mode. Write modes 4 and 5 widen the mode field to three
  // bits; the standard fields are decoded exactly as the VGA map does.
  map(0x05, 0x05)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.gc.shift256 & 1) << 6;
              res |= (vga.gc.shift_reg & 1) << 5;
              res |= (vga.gc.host_oe & 1) << 4;
              res |= (vga.gc.read_mode & 1) << 3;
              res |= m_gr[0x05] & 0x07;
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              m_gr[0x05] = data;
              vga.gc.shift256 = BIT(data, 6);
              vga.gc.shift_reg = BIT(data, 5);
              vga.gc.host_oe = BIT(data, 4);
              vga.gc.read_mode = BIT(data, 3);
              vga.gc.write_mode = data & 3;
            }));

  // GR09/GR0A/GR0B: the banked windows move.
  map(GC_BANK_0, GC_BANK_MODE)
      .lrw8(NAME([this](offs_t offset) { return m_gr[GC_BANK_0 + offset]; }),
            NAME([this](offs_t offset, u8 data) {
              m_gr[GC_BANK_0 + offset] = data;
              update_banks();
            }));

  // GR16: active display line, read only. Not tracked; reads as 0.
  map(0x16, 0x16)
      .lrw8(NAME([this](offs_t offset) { return u8(0); }),
            NAME([this](offs_t offset, u8 data) {}));

  // GR31: BitBLT start/status. The engine is not modelled yet: a start
  // request completes at once without drawing, so a driver polling the
  // busy bit does not hang, and the first request is reported.
  map(GC_BLT_STATUS, GC_BLT_STATUS)
      .lrw8(NAME([this](offs_t offset) {
              return u8(m_gr[GC_BLT_STATUS] & ~BLT_STATUS_BUSY);
            }),
            NAME([this](offs_t offset, u8 data) {
              if (data & BLT_START) {
                static bool reported = false;
                if (!reported) {
                  printf("%s: BitBLT requested; the blitter is not "
                         "implemented yet\n",
                         devid_string);
                  reported = true;
                }
                data &= ~BLT_START;
              }
              if (data & BLT_RESET)
                data &= ~(BLT_START | BLT_STATUS_BUSY);
              m_gr[GC_BLT_STATUS] = data;
            }));
}
