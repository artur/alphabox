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
 * Permedia 2 memory access.
 *
 * Two ways in besides the graphics processor:
 *   - the 0xa0000 window, the SVGA's: planar VGA memory through CVGA, or,
 *     with Mode640 enabled, SGRAM addressed as bytes through two 64 KB
 *     banks (A for 0xa0000, B for 0xb0000). The SVGA reaches it only while
 *     VGAControl lets the host at memory;
 *   - the two 8 MB apertures (regions 1 and 2), each with its own control
 *     register: straight onto the SGRAM, byte- or halfword-swapped, or
 *     turned to the SVGA's 128 KB (the address's low 17 bits) or to the
 *     expansion ROM. The packed 16-bit views of a double-buffered frame
 *     are not modelled: an aperture set to one reads and writes plainly.
 *     Beyond the installed memory an aperture reads all ones and drops
 *     writes.
 **/

#include "Permedia2.hpp"
#include "System.hpp"

using namespace permedia2;

u32 CPermedia2::vram_read(u32 addr, int bytes) const {
  const u32 mask = vram_mask();
  u32 v = 0;
  for (int i = 0; i < bytes; i++)
    v |= u32(vga.memory[(addr + i) & mask]) << (8 * i);
  return v;
}

void CPermedia2::vram_write(u32 addr, int bytes, u32 data) {
  const u32 mask = vram_mask();
  for (int i = 0; i < bytes; i++)
    vga.memory[(addr + i) & mask] = u8(data >> (8 * i));
}

/// The aperture's byte control: 1 swaps a dword's bytes, 2 its halfwords.
static u32 swap_lanes(u32 ctl, int bytes, u32 v) {
  switch (ctl & AP_BYTE_CONTROL) {
  case 1:
    if (bytes == 4)
      return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
    if (bytes == 2)
      return ((v & 0xff) << 8) | ((v >> 8) & 0xff);
    return v;
  case 2:
    if (bytes == 4)
      return (v << 16) | (v >> 16);
    return v;
  }
  return v;
}

/// Where a narrow access lands in the dword once the lanes are swapped.
static u32 swap_address(u32 ctl, u32 offset, int bytes) {
  switch (ctl & AP_BYTE_CONTROL) {
  case 1:
    return bytes == 4 ? offset : bytes == 2 ? offset ^ 2 : offset ^ 3;
  case 2:
    return bytes == 4 ? offset : offset ^ 2;
  }
  return offset;
}

u32 CPermedia2::aperture_read(int which, u32 offset, int dsize) {
  const int bytes = dsize / 8;
  const u32 ctl = reg(which == 1 ? APERTURE_ONE : APERTURE_TWO);
  if (ctl & AP_ROM)
    return rom_read(offset & (rom_max - 1), dsize);
  if (ctl & AP_SVGA) {
    u32 v = 0;
    for (int i = 0; i < bytes; i++)
      v |= u32(mem_r((offset + i) & 0x1ffff)) << (8 * i);
    return v;
  }
  const u32 a = swap_address(ctl, offset, bytes);
  if (a + bytes > m_vram_bytes)
    return bytes == 1 ? 0xffu : bytes == 2 ? 0xffffu : 0xffffffffu;
  return swap_lanes(ctl, bytes, vram_read(a, bytes));
}

void CPermedia2::aperture_write(int which, u32 offset, int dsize, u32 data) {
  const int bytes = dsize / 8;
  const u32 ctl = reg(which == 1 ? APERTURE_ONE : APERTURE_TWO);
  if (ctl & AP_ROM) // a flash part would take a program sequence; this is ROM
    return;
  if (ctl & AP_SVGA) {
    for (int i = 0; i < bytes; i++)
      mem_w((offset + i) & 0x1ffff, u8(data >> (8 * i)));
    return;
  }
  const u32 a = swap_address(ctl, offset, bytes);
  if (a + bytes > m_vram_bytes)
    return;
  vram_write(a, bytes, swap_lanes(ctl, bytes, data));
  state.vga_mem_updated = 1;
}

/**
 * Offer aperture one's SGRAM to the CPUs as plain memory while it is
 * decoded (COMMAND memory enable, BAR1 placed), the card sits on the hose
 * and the aperture is a straight view -- no swapping, packing, SVGA or
 * ROM -- and withdraw it the moment any of that changes.
 * ALPHABOX_LFB_DIRECT=0 keeps every access trapping.
 **/
void CPermedia2::refresh_direct_aperture() {
  static const bool enabled = [] {
    const char *e = getenv("ALPHABOX_LFB_DIRECT");
    return !(e && e[0] == '0');
  }();
  const u32 cmd = config_read(0, 0x04, 16);
  const u32 bar1 = config_read(0, 0x14, 32) & 0xfffffff0u;
  const u32 ctl = reg(APERTURE_ONE);
  u64 base = 0, size = 0;
  if (enabled && !myBridge && (cmd & 0x0002) && bar1 != 0 && vga.memory &&
      (ctl & (AP_BYTE_CONTROL | AP_PACKED16 | AP_SVGA | AP_ROM)) == 0) {
    base = bus_address(false, bar1);
    size = m_vram_bytes;
  }
  if (base == m_direct_base && size == m_direct_size)
    return;
  m_direct_base = base;
  m_direct_size = size;
  printf("%s: aperture %s for direct access (%llx + %llx)\n", devid_string,
         size ? "offered" : "withdrawn", (unsigned long long)base,
         (unsigned long long)size);
  cSystem->set_direct_memory(base, size, size ? vga.memory : nullptr);
}

/**
 * Mode640's byte-addressed banks, or the VGA's planes.
 **/
uint8_t CPermedia2::mem_r(offs_t offset) {
  if (!(r.vga_control & VGACTL_HOST_MEMORY))
    return 0xff;
  if (!(r.mode640 & M640_ENABLE))
    return CVGA::mem_r(offset);
  const u32 bank = (offset & 0x10000) ? (r.mode640 >> M640_BANK_B_SHIFT) & 7
                                      : r.mode640 & M640_BANK_A;
  return vga.memory[((bank << 16) | (offset & 0xffff)) & vram_mask()];
}

void CPermedia2::mem_w(offs_t offset, uint8_t data) {
  if (!(r.vga_control & VGACTL_HOST_MEMORY))
    return;
  if (!(r.mode640 & M640_ENABLE)) {
    CVGA::mem_w(offset, data);
    state.vga_mem_updated = 1;
    return;
  }
  const u32 bank = (offset & 0x10000) ? (r.mode640 >> M640_BANK_B_SHIFT) & 7
                                      : r.mode640 & M640_BANK_A;
  vga.memory[((bank << 16) | (offset & 0xffff)) & vram_mask()] = data;
  state.vga_mem_updated = 1;
}
