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
int es40_pci_interrupt(int hose, int slot, int intx, int func) {
  return ((slot + 1) * 4 + (hose & 3) * 0x10 + (intx & 3)) & 0x3f;
}

/**
 * ES40 slots: ten, four on hose 0 (devices 1 to 4) and six on hose 1
 * (devices 1 to 6), as the sample es40.cfg has always said and the only
 * slots `alphabox configure` offers. Hose 0 device 0 is the bridge's own
 * place, and 7, 15, 17 and 19 hold the M1543C's functions. The wiring
 * above gives each hose 16 inputs, four slots' worth from device 1, so
 * hose 0 devices 5 and 6 would share hose 1 devices 1 and 2's lines. The
 * console writes (device + 1) * 4 + 16 * hose into any card it finds,
 * wherever it is (DE500s at hose 0 devices 1-14 and hose 1 devices 0-11
 * all got a line, lab/serial-slot), so it does not object -- but hose 0
 * device 5 gets 0x18, hose 1 device 1's. Windows 2000's HAL
 * routes only the real slots: it stops with 0xA5 (0x10003, PDO, 5, ...)
 * when a driver starts on hose 0 device 5, and the same driver on device 3
 * runs (lab/driver-release-test/slot5). Firmware that meets a device on the
 * M1543C's places fails in obscure ways (SCSI disks going missing, device
 * conflicts). So everything else is refused.
 */
const char *es40_slot_refusal(int hose, int slot) {
  if (hose == 0 && (slot == 7 || slot == 15 || slot == 17 || slot == 19))
    return "that PCI slot is reserved for a system-internal device. The "
           "ES40's PCI slots are pci0.1 to pci0.4 and pci1.1 to pci1.6";
  if ((hose == 0 && slot >= 1 && slot <= 4) ||
      (hose == 1 && slot >= 1 && slot <= 6))
    return nullptr;
  return "the ES40 has no such PCI slot. Its PCI slots are pci0.1 to "
         "pci0.4 and pci1.1 to pci1.6 (a bridge in one of them adds more)";
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
 * built with SRM_NO_SPEEDUPS). They are addresses in the ES40's console
 * and only the ES40 row applies them: in another console the same words are
 * other code. In the DS10's, 0x68320 is the first instruction of
 * mop_loop_requester (its stack-frame allocation, so the power-up network
 * test returned through a clobbered frame), and 0x14248..0x142c8 are its
 * PALcode's I/O addresses (docs/platforms/ds10.md).
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
