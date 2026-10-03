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
 * The Radeon parts (RadeonChip.hpp).
 *
 * RV200, the Radeon 7500 (1002:5157 "QW"):
 *   - subsystems 1002:013A (AGP) and 1002:013B (PCI): the ES40's and the
 *     Marvel's SRM device tables;
 *   - 64 command FIFO entries: Linux r100_gui_wait_for_idle waits for 64
 *     free entries, and the Rage 128 Pro guide's GUI_FIFOCNT resets to
 *     0x40 (RRG 3-244);
 *   - 256 micro-engine RAM entries: Linux loads the R100 microcode
 *     (radeon/R100_cp.bin, used for the R100, RV100, RV200, RS100 and
 *     RS200: r100_cp_init_microcode) as DATAH/DATAL pairs from ME RAM
 *     address 0, and OpenVMS's DECwindows server writes 256 pairs;
 *   - the primary PIO queue's depth is not documented for the R100
 *     (CP_CSQ_CNTL CSQ_CNT_PRIMARY <7:0> is the count): 64 dwords
 *     [inference];
 *   - the clocks: 27 MHz reference (the card's BIOS PLL block, fp_bios +
 *     0x30, + 0x0e: 2700 in 10 kHz units; radeonfb's default for the
 *     family), an 180 MHz engine clock (the same block, + 0x08) with two
 *     pixel pipelines [the rate the busy time is modelled at; the BIOS's
 *     values are read again from the ROM the card runs, when it has one].
 **/

#include "RadeonChip.hpp"

#include <cstring>

namespace radeon {

static const ChipInfo kChips[] = {
    {"rv200", "Radeon 7500 (RV200)", 0x5157, 0x013a, 0x013b, 64, 64, 256, 27000,
     180000, 2},
};

const ChipInfo *find_chip(const char *name) {
  for (const ChipInfo &c : kChips)
    if (strcmp(c.name, name) == 0)
      return &c;
  return nullptr;
}

const ChipInfo &default_chip() { return kChips[0]; }

} // namespace radeon
