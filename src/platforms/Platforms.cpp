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
 * The machines this emulator knows.
 *
 * Only the ES40 so far; docs/platforms.md describes how another is added
 * and docs/platforms/ds20e.md is the first one queued.
 **/
#include "StdAfx.hpp"

#include "Platform.hpp"

/**
 * ES40 interrupts: the chipset's 64 interrupt inputs are wired to the PCI
 * slots by the board, in the pattern the console programs on its first
 * pass -- input (slot + 1) * 4 + hose * 16 + (pin - 1), sixty-four inputs
 * wide. Devices behind a bridge arrive here with the bridge's slot and
 * their pin already rotated (CPCIDevice::do_pci_interrupt), which is what
 * the console's own numbering does.
 */
static int es40_pci_interrupt(int hose, int slot, int intx) {
  return ((slot + 1) * 4 + (hose & 3) * 0x10 + (intx & 3)) & 0x3f;
}

/**
 * ES40 slots: hose 0 device 0 is the bridge's own place, and 7, 15, 17 and
 * 19 hold the M1543C's functions. Firmware that meets an add-in device
 * there fails in obscure ways (SCSI disks going missing, device conflicts),
 * so the configuration is refused instead.
 */
static const char *es40_slot_refusal(int hose, int slot) {
  if (hose != 0)
    return nullptr;
  if (slot == 0)
    return "PCI slot pci0.0 is reserved and cannot be used for add-in "
           "devices. Use pci0.1 through pci0.4";
  if (slot == 7 || slot == 15 || slot == 17 || slot == 19)
    return "that PCI slot is reserved for a system-internal device. Use "
           "pci0.1 through pci0.4 for add-in devices";
  return nullptr;
}

static const platform_config platforms[] = {
    {"es40", "AlphaServer ES40", "ev68cb", 4, 26, 35, "cl67srmrom.exe",
     FW_LFU_BUNDLE, 2, es40_pci_interrupt, es40_slot_refusal},
};

const platform_config *find_platform(const char *name) {
  for (const platform_config &p : platforms)
    if (!strcmp(p.name, name))
      return &p;
  return nullptr;
}
