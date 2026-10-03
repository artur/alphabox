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
 * The AlphaServer DS10 board (docs/platforms/ds10.md): its interrupt
 * wiring, its slots, and its I2C bus behind a PCF8584 (PCF8584.cpp, which
 * the DS20E uses too).
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "PCF8584.hpp"
#include "i2c_spd.hpp"

#include <memory>
#include <vector>

/**
 * DS10 interrupts. A single-processor board: its two on-board network
 * controllers sit at devices 9 and 11 with one interrupt input each, and
 * its four slots at devices 14 to 17, whose pins count down from the
 * slot's own base. Device 7 is the ISA bridge.
 *
 * The console's own table (a byte per pin, devices 0 to 17, at 0x155e38 of
 * the decompressed DS10SRM.ROM V7.3-1) says the same, and also places the
 * ISA bridge's IDE function at device 13 (ISA IRQ 14 and 15) and its USB
 * function at device 1 (ISA IRQ 10): those interrupt through the bridge,
 * so they have no input here (docs/platforms/ds10.md).
 */
int ds10_pci_interrupt(int hose, int slot, int intx, int func) {
  if (slot == 9)
    return 29; // on-board network
  if (slot == 11)
    return 30; // second on-board network
  if (slot >= 14 && slot <= 17)
    return 32 + 4 * (slot - 14) + (3 - intx);
  return -1; // the ISA bridge and anything the board does not wire
}

/// DS10 slots: the board wires 9, 11 and 14 to 17, with the ISA bridge at 7.
const char *ds10_slot_refusal(int hose, int slot) {
  if (hose != 0)
    return "this machine has one PCI bus";
  if (ds10_pci_interrupt(hose, slot, 0, 0) < 0)
    return "this machine wires add-in devices to devices 9, 11 and 14 to 17 "
           "only";
  return nullptr;
}

/**
 * What the DS10 hangs on its I2C bus.
 *
 * The console addresses 0x38, 0x39, 0x4f, 0x51 and 0x60 to 0x67, and names
 * the last eight `iic_rcm_nvram0` to `iic_rcm_nvram7`: the non-volatile
 * memory of the machine's remote management controller. It reads byte 17 of
 * the first one to decide which machine of the family it is, and -- because
 * it does not check whether the file opened -- faults if the part is not
 * there at all (docs/platforms/ds10.md).
 *
 * These are erased parts: every byte 0xff, which is what an unprogrammed
 * serial ROM reads as. What a real machine's holds we do not know, and it
 * is not ours to invent: the console's own table treats a code of 6 or more
 * as no code, so an erased part reads as "nothing recorded" and the console
 * names itself from the first entry of its table. The other four addresses
 * are left unanswered, since neither the parts nor their contents are known.
 */
static void ds10_i2c_devices(I2CBus &bus) {
  for (uint8_t a = 0x60; a <= 0x67; a++)
    bus.attach(std::make_shared<Eeprom24C02>(a, std::vector<uint8_t>()));
}

/**
 * The DS10's own hardware: the ES40's TIG-bus parts (es40_board_devices),
 * and its I2C bus controller, a PCF8584 at PCI 0 memory 0xffff0000 -- where
 * the console's own iic_read_csr/iic_write_csr address it. The console
 * initialises it and then waits for the bus to go free before it reads the
 * machine's serial ROMs.
 */
void ds10_board_devices(CConfigurator *cfg, CSystem *sys) {
  es40_board_devices(cfg, sys);
  CPCF8584 *i2c = new CPCF8584(cfg, sys, U64(0x00000800ffff0000));
  ds10_i2c_devices(i2c->bus());
}
