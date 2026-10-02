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
 * The machines this emulator knows.
 *
 * A row per board; what a board contributes beyond its row lives in
 * platforms/<board>/ (Boards.hpp). docs/platforms.md describes how another
 * is added.
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "Platform.hpp"

static const platform_config platforms[] = {
    {"es40", "AlphaServer ES40", CHIPSET_TSUNAMI, "ev68cb", 4, 26, 35,
     "cl67srmrom.exe", FW_LFU_BUNDLE, 2, SECONDARIES_BY_CONSOLE,
     es40_board_devices, es40_pci_interrupt, es40_slot_refusal, U64(0x8000),
     es40_console_patches},
    // Under construction (docs/platforms/ds20e.md). The processor is the
    // EV68CB row because it is the only one there; the board took EV6,
    // EV67 and EV68AL, so the console will name the processor wrongly
    // until its row exists.
    {"ds20e", "AlphaServer DS20E", CHIPSET_TSUNAMI, "ev68cb", 2, 26, 32,
     "PC264SRM.ROM", FW_ROM_HEADER, 2, SECONDARIES_AFTER_ARBITRATION,
     ds20e_board_devices, ds20e_pci_interrupt, ds20e_slot_refusal, 0,
     es40_console_patches},
    // Under construction (docs/platforms/ds10.md): one processor, one PCI
    // bus. The processor row is the EV68CB for now, as on the DS20E.
    {"ds10", "AlphaServer DS10", CHIPSET_TSUNAMI, "ev68cb", 1, 26, 31,
     "DS10SRM.ROM", FW_ROM_HEADER, 1, SECONDARIES_AFTER_ARBITRATION,
     ds10_board_devices, ds10_pci_interrupt, ds10_slot_refusal, 0,
     es40_console_patches},
    // Under construction (docs/platforms/ds20l.md): its console image comes
    // as an update file, with no header in front of it.
    {"ds20l", "AlphaServer DS20L", CHIPSET_TSUNAMI, "ev68cb", 2, 26, 32,
     "DS20L_V6_6.EXE", FW_RAW_IMAGE, 2, SECONDARIES_AFTER_ARBITRATION,
     es40_board_devices, ds20l_pci_interrupt, ds20l_slot_refusal, 0,
     es40_console_patches},
    // Under construction (docs/platforms/marvel.md): two EV7s, each with its
    // own memory (memory.bits is per processor; ES47 per EV7: 512 MB to
    // 8 GB, GS1280 Technical Summary), the console in an update bundle like
    // the ES40's that decompresses to 0x440000. No IO7 yet, so no PCI.
    {"es47", "AlphaServer ES47", CHIPSET_MARVEL, "ev7", 2, 29, 33,
     "SRM_V7_3.EXE", FW_LFU_BUNDLE, 0, SECONDARIES_BY_CONSOLE, nullptr,
     es47_pci_interrupt, es47_slot_refusal, 0, nullptr, 0x480000},
};

const platform_config *find_platform(const char *name) {
  for (const platform_config &p : platforms)
    if (!strcmp(p.name, name))
      return &p;
  return nullptr;
}
