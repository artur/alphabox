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
 * The AlphaServer DS25 board ("Granite" in Linux sys_titan.c): a Titan with
 * up to two processors, an RMC and a TIG like the ES45's
 * (docs/platforms/ds25.md).
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "DPR.hpp"
#include "Flash.hpp"

/// The DRIR input of a slot's pin. [Not yet read from the console: every
/// slot is refused until it is.]
int ds25_pci_interrupt(int hose, int slot, int intx) {
  (void)hose;
  (void)slot;
  (void)intx;
  return -1;
}

const char *ds25_slot_refusal(int hose, int slot) {
  (void)hose;
  (void)slot;
  return "the DS25's slots are not established yet (docs/platforms/ds25.md)";
}

/// The RMC's dual-port RAM and the flash, as on the ES45 [assumed].
void ds25_board_devices(CConfigurator *cfg, CSystem *sys) {
  new CDPR(cfg, sys);
  new CFlash(cfg, sys, 2);
}
