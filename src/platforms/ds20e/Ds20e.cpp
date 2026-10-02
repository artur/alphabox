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
 * The AlphaServer DS20E board (docs/platforms/ds20e.md): its interrupt
 * wiring, its slots, and its I2C bus.
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "PCF8584.hpp"
#include "i2c_spd.hpp"

#include <memory>
#include <vector>

/**
 * DS20E interrupts, as the console's own table has them (a byte per pin,
 * devices 5 to 10 of each hose, at 0x156be0 in the decompressed
 * PC264SRM.ROM V7.3-1; docs/platforms/ds20e.md):
 *
 *   hose 0: 5 --, 6 13 12 -- --, 7 1f-1c, 8 1b-18, 9 17-14, 10 --
 *   hose 1: 5 --, 6 --,          7 2f-2c, 8 2b-28, 9 27-24, 10 23-20
 *
 * Device 5 is the ISA bridge, whose devices interrupt through it, and
 * device 6 of hose 0 is the board's own SCSI with two lines. In the slots
 * each pin reaches an input counting down from 15 as the device number
 * rises. Linux's `dp264_map_irq` wires device 10 on hose 0 too (to the
 * same inputs as device 6); this console does not, and a card there gets
 * no interrupt line from it -- OpenVMS then never sees its interrupts.
 */
int ds20e_pci_interrupt(int hose, int slot, int intx) {
  const int h = hose & 1;
  int input;
  if (h == 0 && slot == 6 && intx <= 1)
    input = 3 - intx; // the board's own SCSI: pins A and B only
  else if (slot >= 7 && slot <= (h == 0 ? 9 : 10))
    input = 15 - 4 * (slot - 7) - intx;
  else
    return -1; // the ISA bridge and anything the board does not wire

  return 16 + 16 * h + input;
}

/**
 * DS20E slots: the console looks at device numbers 0 to 10, and its table
 * wires hose 0 devices 6 to 9 and hose 1 devices 7 to 10 (the ISA bridge
 * belongs at hose 0 device 5). A card anywhere else would get no interrupt.
 */
const char *ds20e_slot_refusal(int hose, int slot) {
  if (slot > 10)
    return "this machine's console does not look beyond PCI device 10";
  if (ds20e_pci_interrupt(hose, slot, 0) < 0)
    return "this machine wires add-in devices to hose 0 devices 6 to 9 and "
           "hose 1 devices 7 to 10 only";
  return nullptr;
}

/**
 * The parts the DS20E hangs on its I2C bus.
 *
 * Its console addresses the same block of serial ROMs as the DS10's (0x60
 * to 0x67), and two more parts at 0x27 and 0x4f whose identity is not
 * known. The ROMs are erased -- every byte 0xff -- for the same reason as
 * the DS10's: what a real machine records there is not ours to invent.
 */
static void ds20e_i2c_devices(I2CBus &bus) {
  for (uint8_t a = 0x60; a <= 0x67; a++)
    bus.attach(std::make_shared<Eeprom24C02>(a, std::vector<uint8_t>()));
  // Two more parts its console addresses, whose identity is NOT known:
  // erased serial ROMs stand in for them so the bus answers. That is a
  // modelling choice, not a fact -- with them present the console reads a
  // machine code and names itself a DS20E variant instead of falling back
  // to the first entry of its table, but which variant follows from the
  // erased data (docs/platforms/ds20e.md).
  bus.attach(std::make_shared<Eeprom24C02>(0x27, std::vector<uint8_t>()));
  bus.attach(std::make_shared<Eeprom24C02>(0x4f, std::vector<uint8_t>()));
}

/**
 * The DS20E's own hardware: the ES40's TIG-bus parts (es40_board_devices),
 * and the I2C bus controller the DS10 has too -- a PCF8584 at PCI 0 memory
 * 0xfff80000, which the console initialises and then waits on for the bus
 * to go free before it reads the machine's serial ROMs.
 */
void ds20e_board_devices(CConfigurator *cfg, CSystem *sys) {
  es40_board_devices(cfg, sys);
  CPCF8584 *i2c = new CPCF8584(cfg, sys, U64(0x00000800fff80000));
  ds20e_i2c_devices(i2c->bus());
}
