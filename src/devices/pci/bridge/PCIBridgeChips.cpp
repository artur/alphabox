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
 * PCI-PCI bridges: the parts the ES40 SRM console names, and the
 * multi-port boards built on them.
 *
 * The DE602 boards are Compaq's dual-port NICs: the ES40 console names
 * subsystem 0xb0dd (Compaq NC3131, two 82558s) "DE602-AA" and 0xb163
 * (NC3134, two 82559s) "DE602-B*". Which bridge each board carries is not
 * recorded in any source at hand; the 21152 and Intel's 21154 are the
 * parts of their generations.
 **/
#include "PCIBridge.hpp"

static const pci_bridge_config chips[] = {
    // name  part  vendor device rev  io32 pref64 PMC  port class, ports, board
    {"dec21050", "DECchip 21050", 0x1011, 0x0001, 0x02, false, false, 0,
     nullptr, 0, nullptr},
    {"dec21052", "DECchip 21052", 0x1011, 0x0021, 0x02, false, false, 0,
     nullptr, 0, nullptr},
    {"dec21152", "DECchip 21152", 0x1011, 0x0024, 0x03, true, false, 0, nullptr,
     0, nullptr},
    {"dec21153", "DECchip 21153", 0x1011, 0x0025, 0x01, true, true, 0x7e02,
     nullptr, 0, nullptr},
    {"dec21154", "DECchip 21154", 0x1011, 0x0026, 0x02, true, true, 0x7e02,
     nullptr, 0, nullptr},
    {"de602", "DECchip 21152", 0x1011, 0x0024, 0x03, true, false, 0,
     "de602_port", 2, "DE602-AA"},
    // Revision unknown: the console only matches "Intel 21154-*E".
    {"de602b", "Intel 21154", 0x8086, 0xb154, 0x00, true, true, 0x7e02,
     "de602b_port", 2, "DE602-B*"},
};

const pci_bridge_config *CPCIBridge::find_chip(const char *name) {
  for (const pci_bridge_config &c : chips)
    if (!strcmp(c.name, name))
      return &c;
  return nullptr;
}
