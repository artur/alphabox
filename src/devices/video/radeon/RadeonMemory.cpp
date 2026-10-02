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
 * Radeon framebuffer access.
 *
 *   - the 0xa0000 window: a plain VGA's, through CVGA's planar path;
 *   - the framebuffer BAR: two 64 MB apertures over the same memory, each
 *     with its own byte swapping (SURFACE_CNTL: NONSURF_AP0_SWP_16BPP
 *     swaps the bytes of each word, _32BPP those of each dword; AP1 the
 *     same). Beyond the installed memory an aperture reads all ones;
 *   - MM_INDEX with bit 31 set, through MM_DATA (RadeonControl.cpp);
 *   - the memory controller's own addresses, as the CRTC, the cursor and
 *     the engine take them: MC_FB_LOCATION <15:0> is where the framebuffer
 *     starts in that space, in 64 KB units.
 **/

#include "Radeon.hpp"
#include "System.hpp"

using namespace radeon;

u32 CRadeon::vram_read(u32 addr, int bytes) const {
  const u32 mask = vram_mask();
  u32 v = 0;
  for (int i = 0; i < bytes; i++)
    v |= u32(vga.memory[(addr + i) & mask]) << (8 * i);
  return v;
}

void CRadeon::vram_write(u32 addr, int bytes, u32 data) {
  const u32 mask = vram_mask();
  for (int i = 0; i < bytes; i++)
    vga.memory[(addr + i) & mask] = u8(data >> (8 * i));
  state.vga_mem_updated = 1;
}

u32 CRadeon::mc_to_vram(u32 mc) const {
  const u32 fb_start = (R(MC_FB_LOCATION) & 0xffffu) << 16;
  return (mc - fb_start) & vram_mask();
}

/// The swap an aperture applies to an access of `bytes` (SURFACE_CNTL).
static u32 aperture_swap(u32 surface_cntl, bool ap1, int bytes, u32 v) {
  const u32 sw16 = 1u << (ap1 ? 22 : 20), sw32 = 1u << (ap1 ? 23 : 21);
  if ((surface_cntl & sw32) && bytes == 4)
    return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
  if (surface_cntl & sw16) {
    if (bytes == 2)
      return ((v & 0xff) << 8) | ((v >> 8) & 0xff);
    if (bytes == 4)
      return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
  }
  return v;
}

u32 CRadeon::fb_read(u32 offset, int dsize) {
  const int bytes = dsize / 8;
  const bool ap1 = offset >= FB_APERTURE_HALF;
  const u32 off = offset & (FB_APERTURE_HALF - 1);
  if (off + bytes > m_vram_bytes)
    return bytes == 1 ? 0xffu : bytes == 2 ? 0xffffu : 0xffffffffu;
  return aperture_swap(R(SURFACE_CNTL), ap1, bytes, vram_read(off, bytes));
}

void CRadeon::fb_write(u32 offset, int dsize, u32 data) {
  const int bytes = dsize / 8;
  const bool ap1 = offset >= FB_APERTURE_HALF;
  const u32 off = offset & (FB_APERTURE_HALF - 1);
  if (off + bytes > m_vram_bytes)
    return;
  vram_write(off, bytes, aperture_swap(R(SURFACE_CNTL), ap1, bytes, data));
}

/**
 * Offer the first aperture's memory to the CPUs as plain memory while it
 * is decoded (COMMAND memory enable, BAR0 placed), the card sits on the
 * hose (a bridge forwards its ranges elsewhere) and the aperture does not
 * swap bytes; withdraw it the moment any of that changes.
 * ALPHABOX_LFB_DIRECT=0 keeps every access trapping.
 **/
void CRadeon::refresh_direct_aperture() {
  static const bool enabled = [] {
    const char *e = getenv("ALPHABOX_LFB_DIRECT");
    return !(e && e[0] == '0');
  }();
  const u32 cmd = config_read(0, 0x04, 16);
  const u32 bar0 = config_read(0, 0x10, 32) & 0xfffffff0u;
  u64 base = 0, size = 0;
  if (enabled && !myBridge && (cmd & 0x0002) && bar0 != 0 && vga.memory &&
      !(R(SURFACE_CNTL) & (3u << 20))) {
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
 * While the CPUs write VRAM directly: what the extended mode shows (its
 * frame, the cursor image) or, in a VGA mode, the first 256 KB the VGA
 * draws from.
 **/
uint64_t CRadeon::direct_view_hash() const {
  if (!native_crtc_active())
    return hash_vram(0, 256 * 1024);
  uint64_t h =
      hash_vram(native_start(), native_pitch_bytes() * (native_height() + 1));
  if (R(CRTC_GEN_CNTL) & CRTC_CUR_EN)
    h = hash_vram(mc_to_vram(R(CUR_OFFSET) & 0x07fffff0u), 64 * 256, h);
  return h;
}
