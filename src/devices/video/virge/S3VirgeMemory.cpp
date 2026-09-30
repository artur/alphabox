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
 * S3 ViRGE memory access.
 *
 * BAR0 is the chip's one memory range. Its first 16 MB are the linear
 * framebuffer -- answering only while the linear window is enabled (CR58
 * bit 4, or the same bit in advanced function control), the VRAM repeated
 * across the 16 MB -- and the next 16 MB the MMIO window, whose first 64 KB
 * are the registers, while "new MMIO" (CR53 bit 3) is on. On the DX/GX the
 * upper 32 MB repeat both with the bytes of each dword swapped, for
 * big-endian hosts.
 *
 * The 0xa0000 window is the VGA's, or, in the S3 enhanced mapping (CR31
 * bit 3) and in the extended modes, 64 KB of VRAM at the bank CR35/CR51
 * (or CR6A) select. With "old MMIO" (CR53 bit 4) it is instead the MMIO
 * window's first 64 KB: 0xa0000-0xa7fff take host data for the engine,
 * 0xa8000-0xaffff are registers 0x8000-0xffff.
 **/

#include "S3Virge.hpp"
#include "System.hpp"

using namespace virge;

u32 CS3Virge::vram_read(u32 addr, int bytes) const {
  const u32 mask = vram_mask();
  u32 v = 0;
  for (int i = 0; i < bytes; i++)
    v |= u32(vga.memory[(addr + i) & mask]) << (8 * i);
  return v;
}

void CS3Virge::vram_write(u32 addr, int bytes, u32 data) {
  const u32 mask = vram_mask();
  for (int i = 0; i < bytes; i++)
    vga.memory[(addr + i) & mask] = u8(data >> (8 * i));
}

bool CS3Virge::lfb_enabled() const {
  return (cr(CR_LAW_CTL) & CR58_LAW_ENABLE) ||
         (M(ADV_FUNC_CTL) & AFC_LAW_ENABLE);
}

bool CS3Virge::new_mmio_enabled() const {
  return (cr(CR_EXT_MEM_CTL_1) & CR53_NEW_MMIO) != 0;
}

bool CS3Virge::old_mmio_enabled() const {
  return (cr(CR_EXT_MEM_CTL_1) & CR53_OLD_MMIO) ||
         (M(ADV_FUNC_CTL) & AFC_MMIO_ENABLE);
}

static u32 swap_dword_lanes(u32 v, int bytes) {
  if (bytes == 4)
    return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
  if (bytes == 2)
    return ((v & 0xff) << 8) | ((v >> 8) & 0xff);
  return v;
}

/// Where a narrow access lands once a dword's bytes are swapped.
static u32 swap_address(u32 offset, int bytes) {
  return bytes == 4 ? offset : bytes == 2 ? offset ^ 2 : offset ^ 3;
}

u32 CS3Virge::bar0_read(u32 offset, int dsize) {
  const int bytes = dsize / 8;
  const bool swapped = m_chip.dx && (offset & BIG_ENDIAN_HALF);
  offset &= BIG_ENDIAN_HALF - 1;
  if (swapped)
    offset = swap_address(offset, bytes);
  u32 v;
  if (offset & MMIO_OFFSET) {
    if (!new_mmio_enabled() || (offset & (LFB_SPAN - 1)) >= MMIO_BYTES)
      return bytes == 4 ? 0xffffffffu : (1u << dsize) - 1;
    v = mmio_read(offset & (MMIO_BYTES - 1), dsize);
  } else {
    if (!lfb_enabled())
      return bytes == 4 ? 0xffffffffu : (1u << dsize) - 1;
    v = vram_read(offset, bytes);
  }
  return swapped ? swap_dword_lanes(v, bytes) : v;
}

void CS3Virge::bar0_write(u32 offset, int dsize, u32 data) {
  const int bytes = dsize / 8;
  const bool swapped = m_chip.dx && (offset & BIG_ENDIAN_HALF);
  offset &= BIG_ENDIAN_HALF - 1;
  if (swapped) {
    offset = swap_address(offset, bytes);
    data = swap_dword_lanes(data, bytes);
  }
  if (offset & MMIO_OFFSET) {
    if (new_mmio_enabled() && (offset & (LFB_SPAN - 1)) < MMIO_BYTES)
      mmio_write(offset & (MMIO_BYTES - 1), dsize, data);
    return;
  }
  if (!lfb_enabled())
    return;
  vram_write(offset, bytes, data);
  state.vga_mem_updated = 1;
}

/**
 * Offer the linear framebuffer's VRAM to the CPUs as plain memory while it
 * is decoded (COMMAND memory enable, BAR0 placed, the window enabled) and
 * the card sits on the hose, and withdraw it the moment any of that
 * changes. Only the first copy of VRAM is offered; the MMIO window and the
 * repeats above stay on the device path. ALPHABOX_LFB_DIRECT=0 keeps every
 * access trapping.
 **/
void CS3Virge::refresh_direct_lfb() {
  static const bool enabled = [] {
    const char *e = getenv("ALPHABOX_LFB_DIRECT");
    return !(e && e[0] == '0');
  }();
  const u32 cmd = config_read(0, 0x04, 16);
  const u32 bar0 = config_read(0, 0x10, 32) & 0xfffffff0u;
  u64 base = 0, size = 0;
  if (enabled && !myBridge && (cmd & 0x0002) && bar0 != 0 && vga.memory &&
      lfb_enabled()) {
    base = bus_address(false, bar0);
    size = m_vram_bytes;
  }
  if (base == m_direct_base && size == m_direct_size)
    return;
  m_direct_base = base;
  m_direct_size = size;
  printf("%s: framebuffer %s for direct access (%llx + %llx)\n", devid_string,
         size ? "offered" : "withdrawn", (unsigned long long)base,
         (unsigned long long)size);
  cSystem->set_direct_memory(base, size, size ? vga.memory : nullptr);
}

/**
 * The 0xa0000 window with old MMIO on: the MMIO window's first 64 KB at
 * 0xa0000 (or its upper 32 KB at 0xb8000, CR53 bit 5). Otherwise memory.
 **/
u32 CS3Virge::legacy_read(u32 address, int dsize) {
  if (old_mmio_enabled()) {
    if (cr(CR_EXT_MEM_CTL_1) & CR53_OLD_MMIO_B8) {
      if (address >= 0x18000)
        return mmio_read(0x8000 + (address - 0x18000), dsize);
    } else if (address < 0x10000) {
      return mmio_read(address, dsize);
    }
  }
  return CVGACard::legacy_read(address, dsize);
}

void CS3Virge::legacy_write(u32 address, int dsize, u32 data) {
  if (old_mmio_enabled()) {
    if (cr(CR_EXT_MEM_CTL_1) & CR53_OLD_MMIO_B8) {
      if (address >= 0x18000) {
        mmio_write(0x8000 + (address - 0x18000), dsize, data);
        return;
      }
    } else if (address < 0x10000) {
      mmio_write(address, dsize, data);
      return;
    }
  }
  CVGACard::legacy_write(address, dsize, data);
}

/**
 * The banked window: 64 KB of VRAM at the selected bank in the enhanced
 * mapping and the extended modes (packed pixels with chain 4, four planes'
 * bytes side by side without), the VGA's planes otherwise.
 **/
uint8_t CS3Virge::mem_r(offs_t offset) {
  if ((cr(CR_MEM_CONFIG) & CR31_ENHANCED_MAP) || native_crtc_active()) {
    if (offset & 0x10000)
      return 0xff;
    const u32 bank = u32(r.bank) << 16;
    if (vga.sequencer.data[4] & 0x08)
      return vga.memory[(bank + offset) & vram_mask()];
    u8 v = 0;
    for (int i = 0; i < 4; i++)
      if (vga.sequencer.map_mask & (1 << i))
        v |= vga.memory[(bank + offset * 4 + i) & vram_mask()];
    return v;
  }
  return CVGA::mem_r(offset);
}

void CS3Virge::mem_w(offs_t offset, uint8_t data) {
  if ((cr(CR_MEM_CONFIG) & CR31_ENHANCED_MAP) || native_crtc_active()) {
    if (offset & 0x10000)
      return;
    const u32 bank = u32(r.bank) << 16;
    if (vga.sequencer.data[4] & 0x08) {
      vga.memory[(bank + offset) & vram_mask()] = data;
    } else {
      for (int i = 0; i < 4; i++)
        if (vga.sequencer.map_mask & (1 << i))
          vga.memory[(bank + offset * 4 + i) & vram_mask()] = data;
    }
    state.vga_mem_updated = 1;
    return;
  }
  CVGA::mem_w(offset, data);
  state.vga_mem_updated = 1;
}
