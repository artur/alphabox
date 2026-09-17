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
 * Transparent PCI-PCI bridge: the type 1 header, bus numbering and the
 * devices on the secondary bus.
 **/
#include "PCIBridge.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

// Type 1 header offsets
#define PPB_BUS_NUMBERS 0x18 // primary, secondary, subordinate, sec. latency
#define PPB_SECONDARY 0x19
#define PPB_IO 0x1c       // I/O base, limit; secondary status
#define PPB_MEMORY 0x20   // memory base, limit
#define PPB_PREFETCH 0x24 // prefetchable base, limit
#define PPB_PREFETCH_BASE_UPPER 0x28
#define PPB_PREFETCH_LIMIT_UPPER 0x2c
#define PPB_IO_UPPER 0x30 // I/O base and limit, upper 16 bits
#define PPB_CAPABILITIES 0x34
#define PPB_INTERRUPT 0x3c // line, pin; bridge control
#define PPB_BRIDGE_CONTROL 0x3e
#define PPB_CHIP_REGS 0x40 // chip, diagnostic and arbiter control, ...
#define PPB_CHIP_REGS_END 0x70
#define PPB_PM_CAP 0xdc

#define BRIDGE_CONTROL_SECONDARY_RESET 0x0040

CPCIBridge::CPCIBridge(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                       const pci_bridge_config &chip)
    : CPCIDevice(cfg, c, pcibus, pcidev), m_chip(chip) {}

CPCIBridge::~CPCIBridge() {}

void CPCIBridge::init() {
  u32 data[64] = {};
  u32 mask[64] = {};

  data[0x00 >> 2] = u32(m_chip.device_id) << 16 | m_chip.vendor_id;
  data[0x04 >> 2] = 0x02900000; // medium DEVSEL, fast back-to-back
  data[0x08 >> 2] = 0x06040000 | m_chip.revision; // PCI-PCI bridge
  data[0x0c >> 2] = 0x00010000;                   // type 1 header
  mask[0x04 >> 2] = 0x000003ff;
  mask[0x0c >> 2] = 0x0000ffff;
  mask[PPB_BUS_NUMBERS >> 2] = 0xffffffff;
  data[PPB_IO >> 2] = m_chip.io32 ? 0x00000101 : 0;
  mask[PPB_IO >> 2] = 0x0000f0f0;
  mask[PPB_MEMORY >> 2] = 0xfff0fff0;
  data[PPB_PREFETCH >> 2] = m_chip.pref64 ? 0x00010001 : 0;
  mask[PPB_PREFETCH >> 2] = 0xfff0fff0;
  if (m_chip.pref64) {
    mask[PPB_PREFETCH_BASE_UPPER >> 2] = 0xffffffff;
    mask[PPB_PREFETCH_LIMIT_UPPER >> 2] = 0xffffffff;
  }
  if (m_chip.io32)
    mask[PPB_IO_UPPER >> 2] = 0xffffffff;
  mask[PPB_INTERRUPT >> 2] = 0x0fff00ff; // line; bridge control
  for (int r = PPB_CHIP_REGS; r < PPB_CHIP_REGS_END; r += 4)
    mask[r >> 2] = 0xffffffff;
  if (m_chip.pm_capabilities) {
    data[0x04 >> 2] |= 0x00100000; // capability list
    data[PPB_CAPABILITIES >> 2] = PPB_PM_CAP;
    data[PPB_PM_CAP >> 2] = u32(m_chip.pm_capabilities) << 16 | 0x0001;
    mask[(PPB_PM_CAP + 4) >> 2] = 0x00000103;
  }
  add_function(0, data, mask);

  ResetPCI();

  if (m_chip.board)
    printf("%s: %s (%s, %d ports)\n", devid_string, m_chip.board, m_chip.part,
           m_chip.ports);
  else
    printf("%s: %s PCI-PCI bridge\n", devid_string, m_chip.part);
}

int CPCIBridge::secondary_bus() const {
  return (endian_32(pci_state.config_data[0][PPB_BUS_NUMBERS >> 2]) >> 8) &
         0xff;
}

void CPCIBridge::attach(CPCIDevice *child) { m_children.push_back(child); }

void CPCIBridge::remap_children() {
  for (CPCIDevice *child : m_children)
    child->map_config_space();
}

/**
 * Bus reset: the bus numbers clear, so everything behind the bridge drops
 * out of configuration space until the firmware numbers it again.
 **/
void CPCIBridge::ResetPCI() {
  CPCIDevice::ResetPCI();
  remap_children();
}

void CPCIBridge::config_write_custom(int func, u32 address, int dsize,
                                     u32 old_data, u32 new_data, u32 data) {
  const u32 end = address + dsize / 8;

  if (address <= PPB_SECONDARY && PPB_SECONDARY < end) {
    if (secondary_bus())
      printf("%s: secondary bus is now %d\n", devid_string, secondary_bus());
    remap_children();
  }

  // Asserting the secondary reset resets every device behind the bridge.
  if (address <= PPB_BRIDGE_CONTROL && PPB_BRIDGE_CONTROL < end) {
    const int shift = 8 * (PPB_BRIDGE_CONTROL - address);
    const u32 was = old_data >> shift;
    const u32 now = new_data >> shift;
    if ((now & BRIDGE_CONTROL_SECONDARY_RESET) &&
        !(was & BRIDGE_CONTROL_SECONDARY_RESET))
      for (CPCIDevice *child : m_children)
        child->ResetPCI();
  }
}

int CPCIBridge::RestoreState(FILE *f) {
  int res = CPCIDevice::RestoreState(f);
  if (!res)
    remap_children();
  return res;
}
