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
    // EXPERIMENTAL, NOT A MACHINE (docs/platforms/marvel.md): the ES47/ES80/
    // GS1280 console image, loaded onto the ES40's hardware and an EV68 core
    // only to see what it does first -- the L1 probe of the Marvel packet.
    // Nothing here is a Marvel fact except the firmware file and its form
    // (the same LFU wrapper as the ES40's, "CPQ MARVEL ALPH SRM"); there is
    // no EV7 core and no IO7, so the console cannot get far.
    {"marvel-probe", "Marvel console probe (experimental)", CHIPSET_TSUNAMI,
     "ev68cb", 1, 26, 35, "SRM_V7_3.EXE", FW_LFU_BUNDLE, 2,
     SECONDARIES_AFTER_ARBITRATION, es40_board_devices, es40_pci_interrupt,
     ds20l_slot_refusal, 0, es40_console_patches},
};

const platform_config *find_platform(const char *name) {
  for (const platform_config &p : platforms)
    if (!strcmp(p.name, name))
      return &p;
  return nullptr;
}
