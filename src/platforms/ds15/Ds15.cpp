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
 * The AlphaServer DS15: a Titan with one processor (docs/platforms/ds15.md).
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "Ds15Dpr.hpp"
#include "Ds15Tig.hpp"
#include "Flash.hpp"
#include "Titan.hpp"

namespace {
/// A place's interrupt lines, one per pin (INTA-INTD), 0xff for none.
struct ds15_slot {
  int hose, slot;
  u8 line[4];
};

/**
 * The console's own table (`pci_irq_table`, 0x1647c8 in the DS15 update
 * utility V7.3, read by its `map_irq`): four bytes per device, a byte per
 * pin, from device 7 on each hose, 16 bytes per hose. The DS15 has one
 * PA-chip, so hoses 0 and 2. `map_irq` gives hose 0 device 13 (the ALi IDE)
 * line 0xee, routed through ISA, before it looks at the table. Hose 0
 * device 8 is where the update utility's `aicrom_init` looks for the
 * AIC-7899's flash, and has two pins; devices 9 and 10 have one each; hose
 * 2 devices 7-10 have four [taken to be the four add-in slots: the console
 * table gives no names].
 **/
const ds15_slot ds15_slots[] = {
    {0, 8, {0x0d, 0x0c, 0xff, 0xff}},  {0, 9, {0x1c, 0xff, 0xff, 0xff}},
    {0, 10, {0x04, 0xff, 0xff, 0xff}}, {2, 7, {0x28, 0x29, 0x2a, 0x2b}},
    {2, 8, {0x24, 0x25, 0x26, 0x27}},  {2, 9, {0x18, 0x19, 0x1a, 0x1b}},
    {2, 10, {0x14, 0x15, 0x16, 0x17}},
};

const ds15_slot *find_slot(int hose, int slot) {
  for (const ds15_slot &s : ds15_slots)
    if (s.hose == hose && s.slot == slot)
      return &s;
  return nullptr;
}
} // namespace

/// The input of the Titan's DRIR a device's pin reaches, or -1.
int ds15_pci_interrupt(int hose, int slot, int intx, int func) {
  const ds15_slot *s = find_slot(hose, slot);
  if (!s || s->line[intx & 3] == 0xff)
    return -1;
  return s->line[intx & 3];
}

/// Add-in devices go where the console gives interrupts.
const char *ds15_slot_refusal(int hose, int slot) {
  if (!find_slot(hose, slot))
    return "the DS15's places are hose 0 devices 8 to 10 (on-board devices) "
           "and hose 2 devices 7 to 10";
  return nullptr;
}

/// The RMC's dual-port RAM and the flash, as on the ES45 and DS25
/// [assumed].
void ds15_board_devices(CConfigurator *cfg, CSystem *sys) {
  new CDs15Dpr(cfg, sys);
  new CDs15Tig(cfg, sys);
  new CFlash(cfg, sys, 2);
}

/// Its Cchip is pass 18 (the owner's guide's show config, example 2-5). Its
/// memory is two arrays of two DIMMs, 0 and 2: 1 GB in one array (example
/// 2-7), 2 GB as two alike (example 2-5).
const titan_layout ds15_titan = {18, true, false};
