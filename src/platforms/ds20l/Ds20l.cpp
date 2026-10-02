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
 * The AlphaServer DS20L board (docs/platforms/ds20l.md): its interrupt
 * wiring and its slots. Its own hardware is the ES40's TIG-bus parts
 * (es40_board_devices).
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"

/**
 * DS20L interrupts. Linux drives this board with the ES40's table ("Sharks
 * strongly resemble Clipper, at least as far as interrupt routing"), and
 * the console (V6.6-10) agrees for most of it: the interrupt lines it gives
 * a single-function card are the ES40's, (device + 1) * 4 + 16 * hose, on
 * hose 0 devices 3 to 5 and hose 1 devices 3 and 4. Two places differ: pin
 * A of hose 0 device 6 gets 0x1f and of hose 1 device 5 gets 0x2b -- the
 * fourth input of the device's group, not the first. Devices anywhere else
 * it finds (hose 0 8-12, hose 1 6-10) get no line at all
 * (docs/platforms/ds20l.md).
 *
 * Pins B to D of those two devices are not known: they are given the rest
 * of the group in turn (B the first input, C the second, D the third),
 * which is a guess.
 */
int ds20l_pci_interrupt(int hose, int slot, int intx) {
  const int h = hose & 1;
  if (slot < 3 || slot > (h == 0 ? 6 : 5))
    return -1;
  if (slot == (h == 0 ? 6 : 5))
    intx = (intx + 3) & 3; // pin A on the group's fourth input
  return es40_pci_interrupt(h, slot, intx);
}

/// DS20L slots: the places its console gives an interrupt line.
const char *ds20l_slot_refusal(int hose, int slot) {
  if (ds20l_pci_interrupt(hose, slot, 0) < 0)
    return "this machine wires add-in devices to hose 0 devices 3 to 6 and "
           "hose 1 devices 3 to 5 only";
  return nullptr;
}
