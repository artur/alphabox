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
#include "Es45Dpr.hpp"
#include "Flash.hpp"

namespace {
/// A slot's interrupt inputs: the DRIR bit of INTA, and how many pins are
/// wired (INTB.. follow INTA).
struct es45_slot {
  int hose, slot, inta, pins;
};

/**
 * The console's own table (ES45 V7.3-2, at 0x175220 in its memory: one word
 * per slot, a byte per pin, 0xff for none), and the lines it writes into
 * cards' configuration space, which agree with it (lab/platforms/es45,
 * slot probes). The interrupt line a console writes is the Titan's DRIR bit
 * (Linux sys_titan.c: irq = line + 16, enabled by DIM bit irq - 16). The
 * hot-plug controllers (hose 0 slot 12, hoses 1 and 3 slot 6) have one pin;
 * hose 2 slot 5, a slot of the Model 1B's backplane, two.
 **/
const es45_slot es45_slots[] = {
    {0, 8, 0x14, 4},  {0, 9, 0x18, 4}, {0, 10, 0x0c, 4}, {0, 11, 0x10, 4},
    {0, 12, 0x0b, 1}, {1, 1, 0x1c, 4}, {1, 2, 0x20, 4},  {1, 6, 0x0a, 1},
    {2, 1, 0x00, 4},  {2, 2, 0x04, 4}, {2, 5, 0x2c, 2},  {3, 1, 0x24, 4},
    {3, 2, 0x28, 4},  {3, 6, 0x09, 1},
};

const es45_slot *find_slot(int hose, int slot) {
  for (const es45_slot &s : es45_slots)
    if (s.hose == hose && s.slot == slot)
      return &s;
  return nullptr;
}
} // namespace

/// The input of the Titan's DRIR a device's pin reaches, or -1.
int es45_pci_interrupt(int hose, int slot, int intx) {
  const es45_slot *s = find_slot(hose, slot);
  if (!s || (intx & 3) >= s->pins)
    return -1;
  return s->inta + (intx & 3);
}

/// Add-in devices go where the console gives interrupts: the slots of the
/// table above, except the hot-plug controllers' places, which are the
/// board's own (and are not modelled).
const char *es45_slot_refusal(int hose, int slot) {
  const es45_slot *s = find_slot(hose, slot);
  if (!s || s->pins == 1)
    return "the ES45's slots are hose 0 devices 8 to 11, hoses 1 and 3 "
           "devices 1 and 2, and hose 2 devices 1, 2 and 5";
  return nullptr;
}

/// The ES45's RMC dual-port RAM and the flash the console lives in.
void es45_board_devices(CConfigurator *cfg, CSystem *sys) {
  new CEs45Dpr(cfg, sys);
  new CFlash(cfg, sys, 2); // two 2 MB parts (es45.md)
}
