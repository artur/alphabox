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
 * GD54xx framebuffer access: the banked 0xa0000 window, the PCI linear
 * aperture, extended write modes 4/5, and the memory-mapped BitBLT
 * registers.
 *
 * With SR07 bit 0 clear the chip is a plain VGA and the window goes through
 * CVGA's planar path. With it set, memory is packed pixels: the window is
 * two 32 KB banks (0xa0000 and 0xa8000) into VRAM, placed by GR09/GR0A.
 **/

#include "CirrusGD54xx.hpp"

using namespace cirrus;

/**
 * Recompute both bank windows from GR09/GR0A/GR0B. In single-bank mode
 * GR09 places a 64 KB window, so the second half is the first bank plus
 * 32 KB.
 **/
void CCirrusGD54xx::update_banks() {
  const u8 mode = m_gr[GC_BANK_MODE];
  for (int i = 0; i < 2; i++) {
    u32 base =
        (mode & GC_BANK_MODE_DUAL) ? m_gr[GC_BANK_0 + i] : m_gr[GC_BANK_0];
    base <<= (mode & GC_BANK_GRANULARITY_16K) ? 14 : 12;

    u32 limit = (base < m_chip.vram_bytes) ? m_chip.vram_bytes - base : 0;
    if (!(mode & GC_BANK_MODE_DUAL) && i == 1) {
      if (limit > 0x8000) {
        base += 0x8000;
        limit -= 0x8000;
      } else {
        limit = 0;
      }
    }

    m_bank_base[i] = limit ? base : 0;
    m_bank_limit[i] = limit;
  }
}

/**
 * VRAM offset for an aperture address. GR0B can make each aperture byte
 * stand for 8 or 16 VRAM bytes (used with extended write modes).
 **/
u32 CCirrusGD54xx::aperture_offset(u32 offset) const {
  const u8 mode = m_gr[GC_BANK_MODE];
  if ((mode & GC_BANK_MODE_BY16) == GC_BANK_MODE_BY16)
    offset <<= 4;
  else if (mode & GC_BANK_MODE_BY8)
    offset <<= 3;
  return offset & vram_mask();
}

/**
 * Store a byte in packed-pixel mode. Write modes 4 and 5 (with GR0B bit 2)
 * expand the byte into eight pixels: set bits take the foreground colour
 * (GR01/GR11), clear bits the background (GR00/GR10) in mode 5 or are left
 * alone in mode 4.
 **/
void CCirrusGD54xx::write_packed(u32 offset, u8 data) {
  const u8 mode = m_gr[0x05] & 0x07;
  const u8 bank_mode = m_gr[GC_BANK_MODE];
  const u32 mask = vram_mask();
  u8 *vram = vga.memory;

  state.vga_mem_updated = 1;

  if (mode < 4 || mode > 5 || !(bank_mode & GC_BANK_MODE_EXT_WRITE)) {
    vram[offset] = data;
    return;
  }

  if ((bank_mode & GC_BANK_MODE_BY16) != GC_BANK_MODE_BY16) {
    for (int x = 0; x < 8; x++, data <<= 1) {
      const u32 d = (offset + x) & mask;
      if (data & 0x80)
        vram[d] = m_gr[0x01];
      else if (mode == 5)
        vram[d] = m_gr[0x00];
    }
  } else {
    for (int x = 0; x < 8; x++, data <<= 1) {
      const u32 d = (offset + 2 * x) & mask & ~1u;
      if (data & 0x80) {
        vram[d] = m_gr[0x01];
        vram[d + 1] = m_gr[GC_FG_COLOR_1];
      } else if (mode == 5) {
        vram[d] = m_gr[0x00];
        vram[d + 1] = m_gr[GC_BG_COLOR_1];
      }
    }
  }
}

bool CCirrusGD54xx::mmio_enabled_legacy() const {
  return (vga.sequencer.data[SEQ_CONFIG] &
          (SEQ_CONFIG_MMIO | SEQ_CONFIG_MMIO_LINEAR)) == SEQ_CONFIG_MMIO;
}

bool CCirrusGD54xx::mmio_enabled_linear() const {
  return (vga.sequencer.data[SEQ_CONFIG] &
          (SEQ_CONFIG_MMIO | SEQ_CONFIG_MMIO_LINEAR)) ==
         (SEQ_CONFIG_MMIO | SEQ_CONFIG_MMIO_LINEAR);
}

/**
 * Memory-mapped BitBLT register block: each byte is an alias of a GR
 * register, so reads and writes go through the GR map and keep its side
 * effects. -1 marks bytes with no register behind them.
 **/
// clang-format off
static const int16_t mmio_to_gr[0x41] = {
    0x00, 0x10, 0x12, 0x14, // 00: background colour
    0x01, 0x11, 0x13, 0x15, // 04: foreground colour
    0x20, 0x21,             // 08: width
    0x22, 0x23,             // 0a: height
    0x24, 0x25,             // 0c: destination pitch
    0x26, 0x27,             // 0e: source pitch
    0x28, 0x29, 0x2a, -1,   // 10: destination address
    0x2c, 0x2d, 0x2e,       // 14: source address
    0x2f,                   // 17: write mask
    0x30, -1,               // 18: mode
    0x32,                   // 1a: raster operation
    0x33,                   // 1b: mode extensions
    0x34, 0x35, -1, -1,     // 1c: transparent colour
    0x38, 0x39, -1, -1,     // 20: transparent colour mask
    -1, -1, -1, -1, -1, -1, -1, -1, // 24
    -1, -1, -1, -1, -1, -1, -1, -1, // 2c
    -1, -1, -1, -1, -1, -1, -1, -1, // 34
    -1, -1, -1, -1,                 // 3c
    0x31,                           // 40: start/status
};
// clang-format on

u8 CCirrusGD54xx::mmio_read(u32 offset) {
  if (offset < sizeof(mmio_to_gr) / sizeof(mmio_to_gr[0]) &&
      mmio_to_gr[offset] >= 0)
    return m_gc_map.read_byte((u8)mmio_to_gr[offset]);
  return 0xff;
}

void CCirrusGD54xx::mmio_write(u32 offset, u8 data) {
  if (offset < sizeof(mmio_to_gr) / sizeof(mmio_to_gr[0]) &&
      mmio_to_gr[offset] >= 0)
    m_gc_map.write_byte((u8)mmio_to_gr[offset], data);
}

/**
 * Read from the 0xa0000 window (offset 0..0x1ffff).
 **/
uint8_t CCirrusGD54xx::mem_r(offs_t offset) {
  if (!(vga.sequencer.data[SEQ_EXT_MODE] & SEQ_EXT_MODE_SVGA))
    return CVGA::mem_r(offset);

  if (offset < 0x10000) {
    const int bank = offset >> 15;
    const u32 in_bank = offset & 0x7fff;
    if (in_bank >= m_bank_limit[bank])
      return 0xff;
    return vga.memory[aperture_offset(in_bank + m_bank_base[bank])];
  }

  if (offset >= 0x18000 && offset < 0x18100 && mmio_enabled_legacy())
    return mmio_read(offset & 0xff);

  return 0xff;
}

/**
 * Write to the 0xa0000 window (offset 0..0x1ffff).
 **/
void CCirrusGD54xx::mem_w(offs_t offset, uint8_t data) {
  if (!(vga.sequencer.data[SEQ_EXT_MODE] & SEQ_EXT_MODE_SVGA)) {
    CVGA::mem_w(offset, data);
    state.vga_mem_updated = 1;
    return;
  }

  if (offset < 0x10000) {
    const int bank = offset >> 15;
    const u32 in_bank = offset & 0x7fff;
    if (in_bank < m_bank_limit[bank])
      write_packed(aperture_offset(in_bank + m_bank_base[bank]), data);
    return;
  }

  if (offset >= 0x18000 && offset < 0x18100 && mmio_enabled_legacy())
    mmio_write(offset & 0xff, data);
}

/**
 * Read from the PCI linear aperture (BAR0). VRAM repeats through the
 * aperture; with SR17 bits 6 and 2 set, the last 256 bytes of each VRAM
 * image are the BitBLT registers instead.
 **/
uint8_t CCirrusGD54xx::mem_linear_r(offs_t offset) {
  const u32 addr = offset & vram_mask();
  const u32 mmio = m_chip.vram_bytes - 0x100;
  if (mmio_enabled_linear() && (addr & mmio) == mmio)
    return mmio_read(addr & 0xff);
  return vga.memory[aperture_offset(addr)];
}

void CCirrusGD54xx::mem_linear_w(offs_t offset, uint8_t data) {
  const u32 addr = offset & vram_mask();
  const u32 mmio = m_chip.vram_bytes - 0x100;
  if (mmio_enabled_linear() && (addr & mmio) == mmio) {
    mmio_write(addr & 0xff, data);
    return;
  }
  write_packed(aperture_offset(addr), data);
}
