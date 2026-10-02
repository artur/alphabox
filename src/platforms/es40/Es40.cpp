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
 * The AlphaServer ES40 board: its interrupt wiring, its slots, and the
 * hardware on its TIG bus -- the dual-port RAM shared with the management
 * controller (DPR.cpp) and the flash that holds the consoles (Flash.cpp).
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "DPR.hpp"
#include "Flash.hpp"

/**
 * ES40 interrupts: the chipset's 64 interrupt inputs are wired to the PCI
 * slots by the board, in the pattern the console programs on its first
 * pass -- input (slot + 1) * 4 + hose * 16 + (pin - 1), sixty-four inputs
 * wide. Devices behind a bridge arrive here with the bridge's slot and
 * their pin already rotated (CPCIDevice::do_pci_interrupt), which is what
 * the console's own numbering does.
 */
int es40_pci_interrupt(int hose, int slot, int intx) {
  return ((slot + 1) * 4 + (hose & 3) * 0x10 + (intx & 3)) & 0x3f;
}

/**
 * ES40 slots: hose 0 device 0 is the bridge's own place, and 7, 15, 17 and
 * 19 hold the M1543C's functions. Firmware that meets an add-in device
 * there fails in obscure ways (SCSI disks going missing, device conflicts),
 * so the configuration is refused instead.
 */
const char *es40_slot_refusal(int hose, int slot) {
  if (hose != 0)
    return nullptr;
  if (slot == 0)
    return "PCI slot pci0.0 is reserved and cannot be used for add-in "
           "devices. Use pci0.1 through pci0.4";
  if (slot == 7 || slot == 15 || slot == 17 || slot == 19)
    return "that PCI slot is reserved for a system-internal device. Use "
           "pci0.1 through pci0.4 for add-in devices";
  return nullptr;
}

/**
 * The ES40's own hardware on the TIG bus: the DPR its management controller
 * shares with the console, and the flash. The DS20E, DS10 and DS20L are
 * given the same two parts (each board's function calls this one), which is
 * what the emulator has always done; whether those consoles want a DPR at
 * all has not been looked at.
 */
void es40_board_devices(CConfigurator *cfg, CSystem *sys) {
  new CDPR(cfg, sys);
  new CFlash(cfg, sys);
}

/**
 * Words the loader patches into the decompressed console for speed (unless
 * built with SRM_NO_SPEEDUPS). They are addresses in the ES40's console;
 * every Tsunami row applies them because the emulator always did, and
 * nobody has checked what they hit in the other consoles.
 */
const rom_patch es40_console_patches[] = {
    {U64(0x14248), 0xe7e00000}, // e7e00000 = BEQ r31, +0
    {U64(0x14288), 0xe7e00000},
    {U64(0x142c8), 0xe7e00000},
    {U64(0x68320), 0xe7e00000},
    {U64(0x8bb78), 0xe7e00000}, // memory test (aa)
    {U64(0x8bc0c), 0xe7e00000}, // memory test (bb)
    {U64(0x8bc94), 0xe7e00000}, // memory test (00)
    // {U64(0xb1158), 0xe7e00000}, // CPU sync?
    {0, 0},
};
