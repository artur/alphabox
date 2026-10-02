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
 * The IO7 is packet M5, and until it exists nothing can sit on a PCI bus
 * here: the interrupt map and the slots are placeholders that refuse.
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"

/// No IO7 yet, so no interrupt routing (the IO7's LSI_CTL registers, M5).
int es47_pci_interrupt(int hose, int slot, int intx) {
  (void)hose;
  (void)slot;
  (void)intx;
  return -1;
}

const char *es47_slot_refusal(int hose, int slot) {
  (void)hose;
  (void)slot;
  return "the ES47 has no PCI yet: its IO7 is not emulated "
         "(docs/platforms/marvel.md, packet M5)";
}
