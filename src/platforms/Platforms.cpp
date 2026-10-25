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
#include "CpuModel.hpp"
#include "Platform.hpp"

// The boards' own PCI functions (Platform.hpp board_function): the ALi
// M1543C's on the Tsunami and Titan boards, the I/O expander's on the ES47
// and on each ES80 drawer. The places are where each console looks for them
// (docs/platforms/*.md).
static const board_function es40_functions[] = {{"ali", "pci0.7"},
                                                {"ali_ide", "pci0.15"},
                                                {"ali_pmu", "pci0.17"},
                                                {"ali_usb", "pci0.19"},
                                                {nullptr, nullptr}};
// The ALi stands in for the DS20E's Cypress bridge where its console looks
// (devices 0-10).
static const board_function ds20e_functions[] = {
    {"ali", "pci0.5"}, {"ali_ide", "pci0.6"}, {nullptr, nullptr}};
static const board_function ds10_functions[] = {{"ali_usb", "pci0.1"},
                                                {"ali", "pci0.7"},
                                                {"ali_ide", "pci0.13"},
                                                {nullptr, nullptr}};
static const board_function ds20l_functions[] = {{"ali", "pci0.7"},
                                                 {"ali_ide", "pci0.15"},
                                                 {"ali_usb", "pci0.19"},
                                                 {nullptr, nullptr}};
// ES45 and DS25: the IDE at device 16, not the ES40's 15; no USB listed.
static const board_function es45_functions[] = {
    {"ali", "pci0.7"}, {"ali_ide", "pci0.16"}, {nullptr, nullptr}};
static const board_function ds15_functions[] = {
    {"ali", "pci0.7"}, {"ali_ide", "pci0.13"}, {nullptr, nullptr}};
// The I/O expander on PID 0's IO7, hose 2: its CMD 649 IDE and its USS-344
// USB, the EV7 machines' only keyboard.
static const board_function marvel_functions[] = {
    {"cmd649", "pci2.2"}, {"uss344", "pci2.3"}, {nullptr, nullptr}};

// The core-logic revisions each Tsunami-family console prints in `show
// config`, from real listings (docs/platforms/<board>.md has each source).
// The printed text is in the comment. TIG and arbiter bytes are major << 5 |
// minor: 4.22 is 0x96.
//
// ES40, V5.9-24, "Cchip Rev 9(C4)", "TIG Rev 10":
// https://ratatoskr.run/lkml/2001/05/383420 and the ES40 user interface
// guide (EK-ES240-UI.B01 on manx-docs.org).
static const tsunami_revisions es40_revisions = {8, 0, 0x10, 0xfe, 0, 0};
// DS20E, V6.1-2, "Cchip Rev 2.1", "Pchip Rev 2.2", "TIG Rev 4.22",
// "Arbiter Rev 2.16 (0x1)": https://pastebin.com/kC3Lirdj
static const tsunami_revisions ds20e_revisions = {1, 1, 0, 0x96, 0x50, 0x01};
// DS20, V7.2-1, "TIG Rev 4.14", "Arbiter Rev 2.10 (0x1)":
// https://www.zx.net.nz/computers/dec/ds20/
static const tsunami_revisions ds20_revisions = {1, 1, 0, 0x8e, 0x4a, 0x01};
// DS10, V7.3-1, "Cchip Rev 2", "TIG Rev 2.1", "Arbiter Rev 7.30 (0xfe)":
// https://metebalci.com/blog/alpha-and-tru64-in-2023/ (the Pchip prints
// "Rev 2" with PMONCTL 0, as on the ES40)
static const tsunami_revisions ds10_revisions = {1, 0, 0, 0x41, 0xfe, 0xfe};
// DS20L, V5.9-16, "Cchip Rev 2.1", "TIG Rev 4.15", "Arbiter Rev 2.8 (0x1)":
// https://www.manualsdir.com/manuals/99104/hp-ds20l.html?page=97
static const tsunami_revisions ds20l_revisions = {1, 1, 0, 0x8f, 0x48, 0x01};

static const platform_config platforms[] = {
    {"es40", "AlphaServer ES40", CHIPSET_TSUNAMI, "ev68cb", 4, 26, 35,
     "cl67srmrom.exe", FW_LFU_BUNDLE, 2, SECONDARIES_BY_CONSOLE,
     es40_board_devices, es40_pci_interrupt, es40_slot_refusal,
     es40_console_patches, 0, nullptr, nullptr, nullptr, 0, 0, es40_functions,
     &es40_revisions},
    // docs/platforms/ds20e.md. The processor is the
    // EV68CB row because it is the only one there; the board took EV6,
    // EV67 and EV68AL, so the console will name the processor wrongly
    // until its row exists.
    {"ds20e", "AlphaServer DS20E", CHIPSET_TSUNAMI, "ev68cb", 2, 26, 32,
     "PC264SRM.ROM", FW_ROM_HEADER, 2, SECONDARIES_AFTER_ARBITRATION,
     ds20e_board_devices, ds20e_pci_interrupt, ds20e_slot_refusal, nullptr, 0,
     nullptr, nullptr, nullptr, 0, 1024, ds20e_functions, &ds20e_revisions},
    // docs/platforms/ds20.md: the DS20E's console, interrupt table and
    // slots; the console knows it from the DS20E by its I2C bus
    // (ds20_board_devices). The board took EV6 processors, for which there
    // is no row: the EV68CB stands in, as on the DS20E.
    {"ds20", "AlphaServer DS20", CHIPSET_TSUNAMI, "ev68cb", 2, 26, 32,
     "PC264SRM.ROM", FW_ROM_HEADER, 2, SECONDARIES_AFTER_ARBITRATION,
     ds20_board_devices, ds20e_pci_interrupt, ds20e_slot_refusal, nullptr, 0,
     nullptr, nullptr, nullptr, 0, 1024, ds20e_functions, &ds20_revisions},
    // docs/platforms/ds10.md: one processor, one PCI
    // bus. The processor row is the EV68CB for now, as on the DS20E.
    {"ds10", "AlphaServer DS10", CHIPSET_TSUNAMI, "ev68cb", 1, 26, 31,
     "DS10SRM.ROM", FW_ROM_HEADER, 1, SECONDARIES_AFTER_ARBITRATION,
     ds10_board_devices, ds10_pci_interrupt, ds10_slot_refusal, nullptr, 0,
     nullptr, nullptr, nullptr, 0x3, 1024, ds10_functions, &ds10_revisions},
    // docs/platforms/ds20l.md: its console image comes
    // as an update file, with no header in front of it.
    // Memory: its console lists arrays 0 and 1 only, of up to 1 GB each
    // (Platform.hpp), so 2 GB is what it can report.
    {"ds20l", "AlphaServer DS20L", CHIPSET_TSUNAMI, "ev68cb", 2, 26, 31,
     "DS20L_V6_6.EXE", FW_RAW_IMAGE, 2, SECONDARIES_AFTER_ARBITRATION,
     es40_board_devices, ds20l_pci_interrupt, ds20l_slot_refusal, nullptr, 0,
     nullptr, nullptr, nullptr, 0x3, 1024, ds20l_functions, &ds20l_revisions},
    // docs/platforms/marvel.md: two EV7s, each with its
    // own memory (memory.bits is per processor; ES47 per EV7: 512 MB to
    // 8 GB, GS1280 Technical Summary), the console in an update bundle like
    // the ES40's that decompresses to 0x440000. One IO7, on PID 0: hoses
    // 0-3. The ES47, ES80 and GS1280 also take the 21464 on its own
    // interface (`ev8n`), which puts the machine in 21464 mode
    // (docs/platforms/ev8-native.md, section 2): no console runs there yet,
    // a boot image does (es47/Es47.cpp, marvel_board_devices).
    {"es47", "AlphaServer ES47", CHIPSET_MARVEL, "ev7", 2, 29, 33,
     "SRM_V7_3.EXE", FW_LFU_BUNDLE, 4, SECONDARIES_BY_CONSOLE,
     es47_board_devices, es47_pci_interrupt, es47_slot_refusal, nullptr,
     0x480000, &es47_layout, nullptr, nullptr, 0, 0, marvel_functions, nullptr,
     "ev8n"},
    // docs/platforms/marvel.md, M6c: up to four ES47
    // drawers in one N/S ring, eight EV7s, an IO7 on each drawer's first
    // processor (PIDs 0, 8, 16, 24: hoses 0-3, 32-35, 64-67, 96-99). The
    // same console image; memory.bits is per processor. Each IO7 is a 2P
    // drawer's embedded I/O: the console's interrupt rule (M7c).
    {"es80", "AlphaServer ES80", CHIPSET_MARVEL, "ev7", 8, 29, 33,
     "SRM_V7_3.EXE", FW_LFU_BUNDLE, 100, SECONDARIES_BY_CONSOLE,
     es80_board_devices, marvel_pci_interrupt, marvel_slot_refusal, nullptr,
     0x480000, &es80_layout, nullptr, nullptr, 0, 0, marvel_functions, nullptr,
     "ev8n"},
    // docs/platforms/marvel.md, M6c: 8P drawers in a
    // torus, here up to two (sixteen EV7s, PIDs 0-15), one I/O drawer on
    // PID 0: a standard I/O drawer ("3.3V PCI-X I/O"), no slot remap (M7c).
    {"gs1280", "AlphaServer GS1280", CHIPSET_MARVEL, "ev7", 16, 29, 34,
     "SRM_V7_3.EXE", FW_LFU_BUNDLE, 4, SECONDARIES_BY_CONSOLE,
     gs1280_board_devices, marvel_pci_interrupt, marvel_slot_refusal, nullptr,
     0x480000, &gs1280_layout, nullptr, nullptr, 0, 0, nullptr, nullptr, "ev8n"},
    // docs/platforms/es45.md: the first Titan board.
    // Its console comes as an update utility, which installs it into the
    // flash, and the utility runs up to 0x648000 (console_bytes). No
    // console patches: the ES40's are addresses in the ES40's
    // console.
    {"es45", "AlphaServer ES45", CHIPSET_TITAN, "ev68cb", 4, 26, 35,
     "ES45_V7_3.EXE", FW_RAW_IMAGE, 4, SECONDARIES_BY_CONSOLE,
     es45_board_devices, es45_pci_interrupt, es45_slot_refusal, nullptr,
     0x680000, nullptr, &es45_titan, nullptr, 0, 0, es45_functions},
    // The same machine on its Model 1 backplane (the owner's guide's "Model
    // 1B"): six PCI slots and an AGP slot, hose 2 device 5. The console
    // tells it from the others by the A-port's AGP_PRESENT alone.
    {"es45m1", "AlphaServer ES45 Model 1", CHIPSET_TITAN, "ev68cb", 4, 26, 35,
     "ES45_V7_3.EXE", FW_RAW_IMAGE, 4, SECONDARIES_BY_CONSOLE,
     es45_board_devices, es45_pci_interrupt, es45m1_slot_refusal, nullptr,
     0x680000, nullptr, &es45m1_titan, nullptr, 0, 0, es45_functions},
    // docs/platforms/ds25.md: the second Titan board,
    // its console from an update utility like the ES45's.
    {"ds25", "AlphaServer DS25", CHIPSET_TITAN, "ev68cb", 2, 26, 34,
     "DS25_V7_3.EXE", FW_RAW_IMAGE, 4, SECONDARIES_BY_CONSOLE,
     ds25_board_devices, ds25_pci_interrupt, ds25_slot_refusal, nullptr,
     0x680000, nullptr, &ds25_titan, ds25_onboard, 0, 0, es45_functions},
    // docs/platforms/ds15.md: the one-processor Titan
    // board, its console from an update utility like the DS25's, which runs
    // up to 0x702000 (console_bytes). One PA-chip: the slot table uses
    // hoses 0 and 2 only.
    {"ds15", "AlphaServer DS15", CHIPSET_TITAN, "ev68cb", 1, 26, 32,
     "DS15_V7_3.EXE", FW_RAW_IMAGE, 4, SECONDARIES_BY_CONSOLE,
     ds15_board_devices, ds15_pci_interrupt, ds15_slot_refusal, nullptr,
     0x710000, nullptr, &ds15_titan, nullptr, 0x5, 0, ds15_functions},
};

const platform_config *platform_at(size_t i) {
  return i < sizeof(platforms) / sizeof(platforms[0]) ? &platforms[i]
                                                      : nullptr;
}

const platform_config *find_platform(const char *name) {
  for (const platform_config &p : platforms)
    if (!strcmp(p.name, name))
      return &p;
  return nullptr;
}

bool platform_takes(const platform_config &board, const cpu_model &cpu) {
  for (const char *row : {board.cpu_model, board.other_cpu_model})
    if (const cpu_model *m = row ? find_cpu_model(row) : nullptr)
      if (m->family == cpu.family)
        return true;
  return false;
}
