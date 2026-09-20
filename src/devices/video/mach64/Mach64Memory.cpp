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
 * Mach64 framebuffer access.
 *
 * Three ways in:
 *   - the 0xa0000 window. With the VGA aperture off (CONFIG_CNTL bit 2)
 *     it is a plain VGA's, through CVGA's planar path. With it on, the
 *     window is two 32 KB banks placed by MEM_VGA_WP_SEL/RP_SEL (separate
 *     read and write banks), the GC06 memory map still choosing which
 *     part of A0000-BFFFF answers, and the top 2 KB (0xbf800) are the
 *     register block;
 *   - the memory aperture (PCI BAR0): 8 MB little-endian, 8 MB
 *     big-endian (words and dwords byte-swapped), each half ending in a
 *     4 KB page that mirrors the register block twice. Beyond the
 *     installed memory the aperture reads all ones and drops writes,
 *     which is how the BIOS sizes the memory;
 *   - the GUI engine (Mach64Engine.cpp), which addresses pixels.
 **/

#include "Mach64.hpp"

using namespace mach64;

/**
 * The 0xa0000 window, whole accesses: the register block takes them at
 * their own width (a host-data write is one transfer whatever its size);
 * the rest goes a byte at a time through mem_r/mem_w.
 **/
u32 CMach64::legacy_read(u32 address, int dsize) {
  if (vga_aperture_enabled() && address >= 0x1f800)
    return reg_read(address - 0x1f800, dsize / 8);
  return CVGACard::legacy_read(address, dsize);
}

void CMach64::legacy_write(u32 address, int dsize, u32 data) {
  if (vga_aperture_enabled() && address >= 0x1f800) {
    reg_write(address - 0x1f800, dsize / 8, data);
    return;
  }
  CVGACard::legacy_write(address, dsize, data);
}

/**
 * Where a window offset (0..0x1ffff) lands in VRAM through the banks, or
 * ~0 when the GC06 memory map does not cover it.
 **/
u32 CMach64::window_offset(u32 offset, bool write) const {
  offset &= 0x1ffff;
  switch (vga.gc.memory_map_sel & 3) {
  case 0:
    break;
  case 1:
    if (offset >= 0x10000)
      return ~0u;
    break;
  case 2:
    offset -= 0x10000;
    if (offset >= 0x8000)
      return ~0u;
    break;
  default:
    offset -= 0x18000;
    if (offset >= 0x8000)
      return ~0u;
    break;
  }
  const u32 *bank = write ? r.bank_w : r.bank_r;
  return (offset & 0x7fff) + bank[(offset >> 15) & 1];
}

uint8_t CMach64::mem_r(offs_t offset) {
  if (!vga_aperture_enabled())
    return CVGA::mem_r(offset);
  if (offset >= 0x1f800)
    return reg_read8(offset - 0x1f800);
  const u32 addr = window_offset(offset, false);
  if (addr >= m_vram_bytes)
    return 0xff;
  return vga.memory[addr];
}

void CMach64::mem_w(offs_t offset, uint8_t data) {
  if (!vga_aperture_enabled()) {
    CVGA::mem_w(offset, data);
    state.vga_mem_updated = 1;
    return;
  }
  if (offset >= 0x1f800) {
    reg_write8(offset - 0x1f800, data);
    if ((offset - 0x1f800) & REG_BLOCK0)
      reg_written((offset - 0x1f800) & 0x3fc);
    return;
  }
  const u32 addr = window_offset(offset, true);
  if (addr >= m_vram_bytes)
    return;
  vga.memory[addr] = data;
  state.vga_mem_updated = 1;
}

/**
 * VRAM as bytes, words and dwords, little-endian, wrapping at the
 * installed size (the engine's addressing is modular; the aperture has
 * checked its bounds before coming here).
 **/
u32 CMach64::vram_read(u32 addr, int bytes) const {
  const u32 mask = vram_mask();
  u32 v = 0;
  for (int i = 0; i < bytes; i++)
    v |= u32(vga.memory[(addr + i) & mask]) << (8 * i);
  return v;
}

void CMach64::vram_write(u32 addr, int bytes, u32 data) {
  const u32 mask = vram_mask();
  for (int i = 0; i < bytes; i++)
    vga.memory[(addr + i) & mask] = u8(data >> (8 * i));
}

static inline u32 bswap(u32 v, int bytes) {
  switch (bytes) {
  case 2:
    return ((v & 0xff) << 8) | ((v >> 8) & 0xff);
  case 4:
    return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
  }
  return v;
}

u32 CMach64::aperture_read(u32 offset, int dsize) {
  const int bytes = dsize / 8;
  const bool big_endian = offset >= APERTURE_HALF;
  const u32 off = offset & (APERTURE_HALF - 1);
  if (off >= APERTURE_HALF - APERTURE_REG_PAGE)
    return reg_read(off & (REG_BLOCK_BYTES - 1), bytes);
  if (off + bytes > m_vram_bytes)
    return bytes == 1 ? 0xffu : bytes == 2 ? 0xffffu : 0xffffffffu;
  const u32 v = vram_read(off, bytes);
  return big_endian ? bswap(v, bytes) : v;
}

void CMach64::aperture_write(u32 offset, int dsize, u32 data) {
  const int bytes = dsize / 8;
  const bool big_endian = offset >= APERTURE_HALF;
  const u32 off = offset & (APERTURE_HALF - 1);
  if (off >= APERTURE_HALF - APERTURE_REG_PAGE) {
    reg_write(off & (REG_BLOCK_BYTES - 1), bytes, data);
    return;
  }
  if (off + bytes > m_vram_bytes)
    return;
  vram_write(off, bytes, big_endian ? bswap(data, bytes) : data);
  state.vga_mem_updated = 1;
}
