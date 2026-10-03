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
 * What each board contributes to its row in Platforms.cpp: the functions in
 * platforms/<board>/<Board>.cpp, next to the board's own hardware.
 **/
#if !defined(INCLUDED_BOARDS_H_)
#define INCLUDED_BOARDS_H_

#include "Platform.hpp"

class CConfigurator;
class CSystem;

// AlphaServer ES40 (platforms/es40/Es40.cpp)
int es40_pci_interrupt(int hose, int slot, int intx, int func);
const char *es40_slot_refusal(int hose, int slot);
void es40_board_devices(CConfigurator *cfg, CSystem *sys);
extern const rom_patch es40_console_patches[];

// AlphaServer DS20E (platforms/ds20e/Ds20e.cpp)
int ds20e_pci_interrupt(int hose, int slot, int intx, int func);
const char *ds20e_slot_refusal(int hose, int slot);
void ds20e_board_devices(CConfigurator *cfg, CSystem *sys);

// AlphaServer DS10 (platforms/ds10/Ds10.cpp)
int ds10_pci_interrupt(int hose, int slot, int intx, int func);
const char *ds10_slot_refusal(int hose, int slot);
void ds10_board_devices(CConfigurator *cfg, CSystem *sys);

// AlphaServer DS20L (platforms/ds20l/Ds20l.cpp)
int ds20l_pci_interrupt(int hose, int slot, int intx, int func);
const char *ds20l_slot_refusal(int hose, int slot);

// AlphaServer ES47 (platforms/es47/Es47.cpp), and what every Marvel board
// shares: the CMMs, the IO7s the layout cables, the hoses' interrupts
int es47_pci_interrupt(int hose, int slot, int intx, int func);
const char *es47_slot_refusal(int hose, int slot);
void es47_board_devices(CConfigurator *cfg, CSystem *sys);
extern const marvel_layout es47_layout;
void marvel_board_devices(CConfigurator *cfg, CSystem *sys);
int marvel_pci_interrupt(int hose, int slot, int intx, int func);
const char *marvel_slot_refusal(int hose, int slot);

// AlphaServer ES80 (platforms/es80/Es80.cpp)
void es80_board_devices(CConfigurator *cfg, CSystem *sys);
extern const marvel_layout es80_layout;

// AlphaServer GS1280 (platforms/gs1280/Gs1280.cpp)
void gs1280_board_devices(CConfigurator *cfg, CSystem *sys);
extern const marvel_layout gs1280_layout;

// AlphaServer ES45 (platforms/es45/Es45.cpp)
int es45_pci_interrupt(int hose, int slot, int intx, int func);
const char *es45_slot_refusal(int hose, int slot);
void es45_board_devices(CConfigurator *cfg, CSystem *sys);

// AlphaServer DS25 (platforms/ds25/Ds25.cpp)
int ds25_pci_interrupt(int hose, int slot, int intx, int func);
const char *ds25_slot_refusal(int hose, int slot);
void ds25_board_devices(CConfigurator *cfg, CSystem *sys);

// AlphaServer DS15 (platforms/ds15/Ds15.cpp)
int ds15_pci_interrupt(int hose, int slot, int intx, int func);
const char *ds15_slot_refusal(int hose, int slot);
void ds15_board_devices(CConfigurator *cfg, CSystem *sys);

#endif // !defined(INCLUDED_BOARDS_H_)
