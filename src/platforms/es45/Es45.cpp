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
 * The AlphaServer ES45 board (docs/platforms/es45.md): a Titan with four
 * PCI hoses, up to four processors, and the ES40's arrangement of management
 * hardware -- an RMC behind a dual-port RAM and the console's flash, both on
 * the TIG bus (ES45 Service Guide, 1.12).
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "DPR.hpp"
#include "Flash.hpp"

/**
 * The input of the Titan's DRIR a device's pin reaches.
 * [Provisional: the ES40's formula on hoses 0 and 1, the same pattern moved
 * up by 32 on hoses 2 and 3; to be replaced by the console's own table.]
 **/
int es45_pci_interrupt(int hose, int slot, int intx) {
  return ((slot + 1) * 4 + (hose & 3) * 0x10 + (intx & 3)) & 0x3f;
}

const char *es45_slot_refusal(int hose, int slot) {
  (void)hose;
  (void)slot;
  return nullptr;
}

/// The ES45's RMC dual-port RAM and the flash the console lives in.
void es45_board_devices(CConfigurator *cfg, CSystem *sys) {
  new CDPR(cfg, sys);
  new CFlash(cfg, sys, 2); // two 2 MB parts (es45.md)
}
