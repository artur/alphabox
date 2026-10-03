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
 * The Radeon memory controller's view of the bus: what an address the
 * engine, the CP or the 3D engine's fetches use reaches.
 *
 * The memory controller's space holds, by Linux's r100_mc_program and
 * the legacy DRM's radeon_cp.c:
 *   - the framebuffer, MC_FB_LOCATION (<15:0> start, <31:16> end, 64 KB
 *     units);
 *   - the AGP window, MC_AGP_LOCATION in the same form, reaching the bus
 *     at AGP_BASE + (address - start). The window's addresses are bus
 *     addresses in the AGP aperture: the GART behind it is the host
 *     bridge's. On the Alpha machines with an AGP port that is the
 *     chipset's scatter-gather DMA window -- Linux builds the AGP
 *     aperture of the Titan (titan_agp_*) and of the Marvel IO7 port 3
 *     (marvel_agp_*) out of the port's SG window's PTEs (iommu_bind) --
 *     which the chipset model already translates (CPCIDevice::do_pci_read
 *     through PCI_Phys). The Tsunami has no AGP port; the AGP board in an
 *     ES40 slot reaches the PCI bus through the window the same way. A
 *     driver that turns the window off writes an end below the start
 *     (r100_mc_program: 0x0FFFFFFF);
 *   - the card's own PCI GART, used by the PCI board and by Linux on any
 *     R100 without AGP (r100_pci_gart_enable; radeon_cp.c
 *     radeon_set_pcigart): AIC_CNTL PCIGART_TRANSLATE_EN <0> turns it on,
 *     AIC_LO_ADDR..AIC_HI_ADDR (inclusive) is its range in the memory
 *     controller's space, AIC_PT_BASE the page table: one dword a 4 KB
 *     page, the page's bus address (r100_pci_gart_set_page; the legacy
 *     ati_pcigart.c writes the same for DRM_ATI_GART_PCI). The table is
 *     itself read through the memory controller: in host memory (the
 *     address is a bus address, the KMS driver's and the legacy DRM's
 *     default) or in the framebuffer (the legacy DRM's pcigart_offset,
 *     AIC_PT_BASE = fb_location + offset). AIC_CNTL
 *     DIS_OUT_OF_PCI_GART_ACCESS <1> forbids bus mastering outside the
 *     window; here nothing outside the three ranges is decoded either
 *     way [inference: no source says what the chip does with such an
 *     address with the bit clear]. Linux notes that the chip caches one
 *     translation and has no documented way to flush it
 *     (r100_pci_gart_tlb_flush); every access is translated afresh here
 *     [inference]. AIC_TLB_ADDR/AIC_TLB_DATA (debug) read back what was
 *     written.
 **/

#include "Radeon.hpp"
#include "System.hpp"

using namespace radeon;

namespace {
constexpr u32 AGP_BASE = 0x0170;
constexpr u32 AIC_CNTL = 0x01d0;
constexpr u32 AIC_PT_BASE = 0x01d8;
constexpr u32 AIC_LO_ADDR = 0x01dc;
constexpr u32 AIC_HI_ADDR = 0x01e0;
constexpr u32 PCIGART_TRANSLATE_EN = 1u << 0;
} // namespace

bool CRadeon::gart_translate(u32 mc, u32 *bus) {
  if (!(R(AIC_CNTL) & PCIGART_TRANSLATE_EN))
    return false;
  const u32 lo = R(AIC_LO_ADDR), hi = R(AIC_HI_ADDR);
  if (hi < lo || mc < lo || mc > hi)
    return false;
  const u32 page = (mc - lo) >> 12;
  const u32 pte_mc = (R(AIC_PT_BASE) & ~3u) + page * 4;
  // The table: in the framebuffer, or host memory at a bus address.
  const u32 fb = R(MC_FB_LOCATION);
  const u32 fb_start = (fb & 0xffff) << 16,
            fb_end = (fb & 0xffff0000u) | 0xffff;
  u32 pte;
  if (pte_mc >= fb_start && pte_mc <= fb_end) {
    pte = vram_read((pte_mc - fb_start) & vram_mask(), 4);
  } else {
    pte = 0;
    do_pci_read(pte_mc, &pte, 4, 1);
  }
  *bus = (pte & ~0xfffu) | (mc & 0xfffu);
  return true;
}

bool CRadeon::cp_translate(u32 mc, bool *is_vram, u32 *addr) {
  const u32 fb = R(MC_FB_LOCATION), agp = R(MC_AGP_LOCATION);
  const u32 fb_start = (fb & 0xffff) << 16,
            fb_end = (fb & 0xffff0000u) | 0xffff;
  const u32 agp_start = (agp & 0xffff) << 16,
            agp_end = (agp & 0xffff0000u) | 0xffff;
  if (mc >= fb_start && mc <= fb_end) {
    *is_vram = true;
    *addr = (mc - fb_start) & vram_mask();
    return true;
  }
  if (agp_end > agp_start && mc >= agp_start && mc <= agp_end) {
    *is_vram = false;
    *addr = R(AGP_BASE) + (mc - agp_start);
    return true;
  }
  if (gart_translate(mc, addr)) {
    *is_vram = false;
    return true;
  }
  return false;
}
