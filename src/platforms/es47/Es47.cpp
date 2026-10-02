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
 * The AlphaServer ES47 board (docs/platforms/marvel.md): two EV7s on one
 * module, and one IO7 on PID 0's I/O port.
 *
 * The module's management processor, the CMM, is Cmm.hpp. The IO7
 * (chipsets/marvel/Io7.hpp) gives PID 0 hoses 0-3: on a real ES47 hose 0 is
 * a one-slot 66 MHz PCI-X bus, hoses 1 and 2 two-slot buses, hose 2 also
 * carries the I/O expander module's SCSI, IDE and USB controllers in slots
 * 1-3, and hose 3 is the AGP slot (slot 5). Sources: the User Information
 * (2P backplane, 2P I/O expander module) and a real ES47's show config
 * (test/platforms/es47/show-config.txt).
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "Cmm.hpp"
#include "Configurator.hpp"
#include "Io7.hpp"
#include "Marvel.hpp"
#include "System.hpp"

/// The module's CMM answers both processors' GIO ports (Cmm.hpp).
void es47_board_devices(CConfigurator *cfg, CSystem *sys) {
  CMarvel *marvel = dynamic_cast<CMarvel *>(sys->chipset());
  if (!marvel)
    FAILURE(Configuration, "the ES47 board needs the Marvel chipset");
  marvel->set_management(std::unique_ptr<GioManagement>(
      new CEs47Cmm(sys, cfg->get_text_value("rom.nvram", "cmm_nvram.bin"))));
  // The drawer's one IO7, on PID 0's I/O port: PID 1 has "No Local I/O".
  marvel->attach_io7(0);
}

/**
 * Every slot's INTx is an LSI of the IO7 the hose belongs to: port <7:5>,
 * slot <4:2>, INTx <1:0> (Linux core_marvel.h). The chipset's input number
 * is the IO7's PID << 8 | the LSI (CMarvel::interrupt).
 */
int es47_pci_interrupt(int hose, int slot, int intx) {
  if (hose < 0 || hose > 3 || slot < 0 || slot > 7)
    return -1;
  return ((hose / 4) << 8) | (int)io7::lsi(hose % 4, slot, intx);
}

/// An LSI names the slot in three bits, so a slot is 1 to 7 on hoses 0-3.
const char *es47_slot_refusal(int hose, int slot) {
  if (hose < 0 || hose > 3)
    return "the ES47's IO7 has hoses 0-3 (PCI-X 0-2, AGP 3)";
  if (slot < 1 || slot > 7)
    return "an IO7 slot is 1 to 7 (its interrupt number has three bits for "
           "the slot)";
  return nullptr;
}
