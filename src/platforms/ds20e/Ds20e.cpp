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
 * wiring, its slots, and its I2C bus; and the AlphaServer DS20
 * (docs/platforms/ds20.md), which differs from it by one part on that bus.
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
int ds20e_pci_interrupt(int hose, int slot, int intx, int func) {
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
const char *ds20e_slot_refusal(const platform_config &, int hose, int slot) {
  if (slot > 10)
    return "this machine's console does not look beyond PCI device 10";
  if (ds20e_pci_interrupt(hose, slot, 0, 0) < 0)
    return "this machine wires add-in devices to hose 0 devices 6 to 9 and "
           "hose 1 devices 7 to 10 only";
  return nullptr;
}

/**
 * The parts the DS20 and DS20E hang on their I2C bus.
 *
 * The console names each part in a table (0x15d618 in the decompressed
 * PC264SRM.ROM V7.3-1: name, kind, size, bus address):
 *
 *   0x60-0x67  iic_rcm_nvram0-7, 256-byte serial ROMs
 *   0x4f       iic_rcm_temp, 4 bytes, a thermometer (the console's
 *              commands to it, 0xa1 0xa2 0xac 0xee 0xaa, are a DS1621's
 *              [inferred from the values alone])
 *   0x27       iic_8574_ocp, the operator control panel's PCF8574
 *
 * and more that neither machine answers here (iic_ocp0/1, iic_smb0,
 * iic_cpu0/1, iic_system0/1). Erased serial ROMs -- every byte 0xff --
 * stand in for all of them, for the same reason as the DS10's: what a real
 * machine records there is not ours to invent. That makes the thermometer
 * read 255 degrees.
 *
 * Which parts answer is what tells the console which machine it is on
 * (0x7fd80, which computes the HWRPB's system variation): without
 * iic_rcm_temp it is an AlphaPC 264DP (member 1), with it but without
 * iic_8574_ocp an AlphaServer DS20 (member 6), with both a DS20E (member
 * 8). So the DS20 is this bus without the PCF8574 (docs/platforms/ds20.md).
 */
static void ds20_family_i2c_devices(I2CBus &bus, bool panel_8574) {
  for (uint8_t a = 0x60; a <= 0x67; a++)
    bus.attach(std::make_shared<Eeprom24C02>(a, std::vector<uint8_t>()));
  if (panel_8574)
    bus.attach(std::make_shared<Eeprom24C02>(0x27, std::vector<uint8_t>()));
  bus.attach(std::make_shared<Eeprom24C02>(0x4f, std::vector<uint8_t>()));
}

/**
 * The DS20E's own hardware: the ES40's TIG-bus parts (es40_board_devices),
 * and the I2C bus controller the DS10 has too -- a PCF8584 at PCI 0 memory
 * 0xfff80000, which the console initialises and then waits on for the bus
 * to go free before it reads the machine's serial ROMs.
 *
 * Which DS20E the console names (AlphaServer or AlphaStation) is bit 0 of
 * byte 17 of iic_rcm_nvram0 (0x7fa78): erased, it is 1, an AlphaStation.
 */
void ds20e_board_devices(CConfigurator *cfg, CSystem *sys) {
  es40_board_devices(cfg, sys);
  CPCF8584 *i2c = new CPCF8584(cfg, sys, U64(0x00000800fff80000));
  ds20_family_i2c_devices(i2c->bus(), true);
}

/**
 * The AlphaServer DS20's own hardware: the DS20E's without the operator
 * control panel's PCF8574, which is how its console tells the two apart
 * (above). Everything else -- the console, its interrupt table, the slots
 * -- is the DS20E's.
 */
void ds20_board_devices(CConfigurator *cfg, CSystem *sys) {
  es40_board_devices(cfg, sys);
  CPCF8584 *i2c = new CPCF8584(cfg, sys, U64(0x00000800fff80000));
  ds20_family_i2c_devices(i2c->bus(), false);
}
