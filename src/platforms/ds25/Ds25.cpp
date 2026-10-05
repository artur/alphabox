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
#include "Ds25Aic7899.hpp"
#include "Ds25Dpr.hpp"
#include "Flash.hpp"
#include "Titan.hpp"

namespace {
/// A slot's interrupt inputs: the DRIR bit of INTA, and how many pins are
/// wired (INTB.. follow INTA).
struct ds25_slot {
  int hose, slot, inta, pins;
};

/**
 * The console's own table (DS25 update utility V7.3, at 0x179f10 in its
 * memory: one word per slot, a byte per pin, 0xff for none; hose 0 from
 * slot 7, hoses 1-3 from slot 1, six slots each -- the layout of the
 * ES45's table, platforms/es45/Es45.cpp). The interrupt line a console
 * writes is the Titan's DRIR bit. The one-pin places (hose 0 slot 12,
 * hoses 1 and 3 slot 6) are where the ES45 has its hot-plug controllers.
 **/
const ds25_slot ds25_slots[] = {
    {0, 8, 0x14, 4},  {0, 9, 0x18, 4}, {0, 10, 0x0c, 4}, {0, 11, 0x10, 4},
    {0, 12, 0x0b, 1}, {1, 1, 0x1c, 4}, {1, 2, 0x20, 4},  {1, 6, 0x0a, 1},
    {2, 1, 0x00, 4},  {2, 5, 0x04, 4}, {3, 1, 0x24, 4},  {3, 2, 0x28, 4},
    {3, 6, 0x09, 1},
};

const ds25_slot *find_slot(int hose, int slot) {
  for (const ds25_slot &s : ds25_slots)
    if (s.hose == hose && s.slot == slot)
      return &s;
  return nullptr;
}
} // namespace

/// The input of the Titan's DRIR a device's pin reaches, or -1.
int ds25_pci_interrupt(int hose, int slot, int intx, int func) {
  const ds25_slot *s = find_slot(hose, slot);
  if (!s || (intx & 3) >= s->pins)
    return -1;
  return s->inta + (intx & 3);
}

/**
 * The six physical slots (owner's guide EK-DS250-UG, table 1-1: slots 1-6
 * are hose 1 devices 1 and 2, hose 3 devices 2 and 1, hose 0 devices 9 and
 * 10), and the places of the two on-board network controllers (hose 0
 * device 8, the Intel 82559ER of ds25_onboard; hose 2 device 5, a Broadcom
 * 5703c, which no model here is, so that a NIC may stand in there: the
 * guide's show config). Hose 0 device 11 has a line in the console's table
 * but no slot; the hot-plug controllers' places and the AIC-7899's are the
 * board's.
 **/
const char *ds25_slot_refusal(int hose, int slot) {
  const ds25_slot *s = find_slot(hose, slot);
  if (!s || s->pins == 1 || (hose == 2 && slot == 1) ||
      (hose == 0 && slot == 11))
    return "the DS25's slots are hose 0 devices 9 and 10 and hoses 1 and 3 "
           "devices 1 and 2; hose 0 device 8 and hose 2 device 5 are the "
           "on-board network controllers' places";
  return nullptr;
}

/// The RMC's dual-port RAM and the flash, as on the ES45 [assumed].
void ds25_board_devices(CConfigurator *cfg, CSystem *sys) {
  new CDs25Dpr(cfg, sys);
  new CFlash(cfg, sys, 2);
  new CDs25Aic7899(cfg, sys, 2, 1);
}

/**
 * The on-board Intel 82559ER at hose 0 device 8 (the owner's guide's show
 * config: "8 Intel 82559ER Ethern eia0.0.0.8.0"), unconnected unless the
 * configuration gives pci0.8 a backend. A configuration that names another
 * device at pci0.8 gets that one in its place [not a real DS25; kept for
 * the configurations that put a DE500 there].
 **/
const onboard_device ds25_onboard[] = {
    {"pci0.8", "i82559er", "type=\"null\";"},
    {nullptr, nullptr, nullptr},
};

/// Its Cchip is pass 18 (the owner's guide's show config).
const titan_layout ds25_titan = {18, false};
