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
#include "Ds25Aic7899.hpp"
#include "StdAfx.hpp"

CDs25Aic7899::CDs25Aic7899(CConfigurator *cfg, CSystem *c, int pcibus,
                           int pcidev)
    : CPCIDevice(cfg, c, pcibus, pcidev) {}

void CDs25Aic7899::init() {
  // Two functions, one per SCSI channel, as the AIC-7899 has (Adaptec
  // 9005:00cf, the console's own ID table at 0x1745e0 names it "Adaptec
  // AIC-7899"). I/O BAR0 (256 bytes) and memory BAR1 (4 KB) as the
  // AIC-789x parts have them; the expansion ROM size is not known [64 KB,
  // assumed].
  for (int f = 0; f < 2; f++) {
    u32 cfg_data[64] = {};
    u32 cfg_mask[64] = {};
    cfg_data[0x00 >> 2] = 0x00cf9005;
    cfg_data[0x04 >> 2] = 0x02100000;
    cfg_data[0x08 >> 2] = 0x01000001;
    cfg_data[0x0c >> 2] = 0x00800000; // multi-function
    cfg_data[0x10 >> 2] = 0x00000001;
    cfg_data[0x3c >> 2] = 0x281100ff | (u32(f + 1) << 8);
    cfg_mask[0x04 >> 2] = 0x00000157;
    cfg_mask[0x0c >> 2] = 0x0000ffff;
    cfg_mask[0x10 >> 2] = 0xffffff00;
    cfg_mask[0x14 >> 2] = 0xfffff000;
    cfg_mask[0x30 >> 2] = ~u32(0x10000 - 1) | PCI_ROM_ADDRESS_ENABLE;
    cfg_mask[0x3c >> 2] = 0x000000ff;
    add_function(f, cfg_data, cfg_mask);
  }
  ResetPCI();
}

/// The SCSI core is not modelled: its registers read 0. The flash behind the
/// expansion ROM reads as erased, so the console's flash probe finds no part
/// it knows and leaves it alone.
u32 CDs25Aic7899::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  (void)func;
  (void)address;
  if (bar == 6)
    return dsize >= 32 ? 0xffffffffu : (1u << dsize) - 1;
  return 0;
}

void CDs25Aic7899::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                                u32 data) {
  (void)func;
  (void)bar;
  (void)address;
  (void)dsize;
  (void)data;
}
