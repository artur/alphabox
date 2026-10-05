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
 * All but the 82559ER are PCI device 0x1229. The revision tells the
 * controller apart (82557 1-3, 82558 4-5, 82559 8); the subsystem IDs name
 * the board.
 * The ES40 SRM console names Compaq subsystem 0xb144 "DE600-AA" (an
 * 82559 board); its other Compaq entries, 0xb0dd "DE602-AA" and 0xb163,
 * 0xb0e1, 0xb164 ("DE602-B*", "-F*", "-T*"), are the DE602 family: dual-port
 * boards behind a PCI-PCI bridge, and their add-on modules.
 *
 * The 82559ER, the embedded 82559, is PCI device 0x1209: the AlphaServer
 * DS25's on-board controller (hose 0 device 8). Its console (SRM V7.3-2)
 * names 8086:1209 with any subsystem "Intel 82559ER Ethernet", and OpenVMS
 * 8.4's SYS$CONFIG.DAT "Intel 82559ER LOM (Fast Ethernet)" (EI,
 * SYS$EIDRIVER), again by the device ID alone. The EEPROM is the one that
 * console writes with "srom8255x_edit eia0 Default_values_set" (words 3 to
 * 12, the rest zero, and the checksum from "CRC_Compute_and_write"), read
 * back from the emulated part; the subsystem IDs, 0E11:00CE, are its words
 * 11 and 12, which the chip loads into configuration space. [Inferred: that
 * a DS25 left the factory with exactly these defaults; and the revision, 9,
 * which is Intel's for the 82559ER (the data sheet, and QEMU's eepro100
 * i82559ER row) -- nothing in the console or in OpenVMS depends on it. The
 * power management capability is the family's.]
 **/
#include "I8255x.hpp"

#include "I8255xRegs.hpp"

/// EEPROM words 3-12 of the DS25's 82559ER, as its console sets them:
/// compatibility, -, controller type and connectors, PHY (an 82555 at MII
/// address 1), -, the board's part number (two words), the ID word,
/// subsystem ID, subsystem vendor.
static const u16 ds25_82559er_eeprom[I8255X_EEPROM_BOARD_WORDS] = {
    0x0100, 0x0000, 0x0201, 0x4701, 0x0000,
    0x0095, 0x5101, 0x4880, 0x00ce, PCI_VENDOR_COMPAQ};

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
    {"i82559er", "82559ER", 0x1209, 0x09, PCI_VENDOR_COMPAQ, 0x00ce, 9, 0x7e21,
     ds25_82559er_eeprom},
};

const i8255x_chip_config *CI8255x::find_chip(const char *name) {
  for (const i8255x_chip_config &c : chips)
    if (!strcmp(c.name, name))
      return &c;
  return nullptr;
}
