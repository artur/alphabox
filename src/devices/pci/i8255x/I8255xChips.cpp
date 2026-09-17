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
 * Intel 8255x family: the parts and boards.
 *
 * All of them are PCI device 0x1229. The revision tells the controller
 * apart (82557 1-3, 82558 4-5, 82559 8); the subsystem IDs name the board.
 * The ES40 SRM console names Compaq subsystem 0xb144 "DE600-AA" (an
 * 82559 board); its other Compaq entries, 0xb0dd "DE602-AA" and 0xb163,
 * 0xb0e1, 0xb164 ("DE602-B*", "-F*", "-T*"), are the DE602 family: dual-port
 * boards behind a PCI-PCI bridge, and their add-on modules.
 **/
#include "I8255x.hpp"

#include "I8255xRegs.hpp"

static const i8255x_chip_config chips[] = {
    // name     part     device  rev   subsystem vendor, id    gen  PMC
    {"de600", "82559", 0x1229, 0x08, PCI_VENDOR_COMPAQ, 0xb144, 9, 0x7e21},
    // The ports of the dual-port DE602 boards (see PCIBridgeChips.cpp).
    {"de602_port", "82558B", 0x1229, 0x05, PCI_VENDOR_COMPAQ, 0xb0dd, 8,
     0x7e21},
    {"de602b_port", "82559", 0x1229, 0x08, PCI_VENDOR_COMPAQ, 0xb163, 9,
     0x7e21},
    {"i82557", "82557C", 0x1229, 0x03, 0, 0, 7, 0},
    {"i82558", "82558B", 0x1229, 0x05, PCI_VENDOR_INTEL, 0x0009, 8, 0x7e21},
    {"i82559", "82559", 0x1229, 0x08, PCI_VENDOR_INTEL, 0x000c, 9, 0x7e21},
};

const i8255x_chip_config *CI8255x::find_chip(const char *name) {
  for (const i8255x_chip_config &c : chips)
    if (!strcmp(c.name, name))
      return &c;
  return nullptr;
}
