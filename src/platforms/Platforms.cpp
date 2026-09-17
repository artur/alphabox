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
 * Only the ES40 so far; docs/platforms.md describes how another is added
 * and docs/platforms/ds20e.md is the first one queued.
 **/
#include "StdAfx.hpp"

#include "Platform.hpp"

/**
 * ES40 interrupts: the chipset's 64 interrupt inputs are wired to the PCI
 * slots by the board, in the pattern the console programs on its first
 * pass -- input (slot + 1) * 4 + hose * 16 + (pin - 1), sixty-four inputs
 * wide. Devices behind a bridge arrive here with the bridge's slot and
 * their pin already rotated (CPCIDevice::do_pci_interrupt), which is what
 * the console's own numbering does.
 */
static int es40_pci_interrupt(int hose, int slot, int intx) {
  return ((slot + 1) * 4 + (hose & 3) * 0x10 + (intx & 3)) & 0x3f;
}

/**
 * ES40 slots: hose 0 device 0 is the bridge's own place, and 7, 15, 17 and
 * 19 hold the M1543C's functions. Firmware that meets an add-in device
 * there fails in obscure ways (SCSI disks going missing, device conflicts),
 * so the configuration is refused instead.
 */
static const char *es40_slot_refusal(int hose, int slot) {
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
 * DS20E interrupts. The board's devices live at device numbers 5 to 10 --
 * the console scans no further -- and each one's pins reach a fixed set of
 * chipset interrupt inputs, counting down from 15 as the device number
 * rises. Device 5 is the ISA bridge, whose devices interrupt through it;
 * device 6 is the board's own SCSI.
 *
 * Confirmed against the console: it gives a controller at device 6 pin A
 * input 16+3 and at device 8 pin A input 16+11, which is what this returns.
 * (Linux carries the same table as `dp264_map_irq`.)
 */
static int ds20e_pci_interrupt(int hose, int slot, int intx) {
  int input;
  if (slot == 6)
    input = intx == 0 ? 3 : 2; // the board's own SCSI
  else if (slot >= 7 && slot <= 10)
    input = 15 - 4 * (slot - 7) - intx;
  else
    return -1; // the ISA bridge and anything the board does not wire

  return 16 + 16 * (hose & 1) + input;
}

/// DS20E slots: the console looks at device numbers 0 to 10 and the board
/// wires 5 to 10; the ISA bridge belongs at 5.
static const char *ds20e_slot_refusal(int hose, int slot) {
  if (slot > 10)
    return "this machine's console does not look beyond PCI device 10";
  return nullptr;
}

/**
 * DS10 interrupts. A single-processor board: its two on-board network
 * controllers sit at devices 9 and 11 with one interrupt input each, and
 * its four slots at devices 14 to 17, whose pins count down from the
 * slot's own base. Device 7 is the ISA bridge. (Linux carries the same
 * table as `webbrick_map_irq`.)
 */
static int ds10_pci_interrupt(int hose, int slot, int intx) {
  if (slot == 9)
    return 29; // on-board network
  if (slot == 11)
    return 30; // second on-board network
  if (slot >= 14 && slot <= 17)
    return 32 + 4 * (slot - 14) + (3 - intx);
  return -1; // the ISA bridge and anything the board does not wire
}

/// DS10 slots: the board wires 9, 11 and 14 to 17, with the ISA bridge at 7.
static const char *ds10_slot_refusal(int hose, int slot) {
  if (hose != 0)
    return "this machine has one PCI bus";
  return nullptr;
}

/**
 * DS20L interrupts: the ES40's wiring. Linux drives this board with the
 * same table it uses for the ES40 ("Sharks strongly resemble Clipper, at
 * least as far as interrupt routing"), so until this machine's own console
 * says otherwise, that is what it gets.
 */
static int ds20l_pci_interrupt(int hose, int slot, int intx) {
  return es40_pci_interrupt(hose, slot, intx);
}

/// DS20L slots: not yet known; nothing is refused.
static const char *ds20l_slot_refusal(int hose, int slot) { return nullptr; }

static const platform_config platforms[] = {
    {"es40", "AlphaServer ES40", "ev68cb", 4, 26, 35, "cl67srmrom.exe",
     FW_LFU_BUNDLE, 2, true, es40_pci_interrupt, es40_slot_refusal},
    // Under construction (docs/platforms/ds20e.md). The processor is the
    // EV68CB row because it is the only one there; the board took EV6,
    // EV67 and EV68AL, so the console will name the processor wrongly
    // until its row exists.
    {"ds20e", "AlphaServer DS20E", "ev68cb", 2, 26, 32, "PC264SRM.ROM",
     FW_ROM_HEADER, 2, false, ds20e_pci_interrupt, ds20e_slot_refusal},
    // Under construction (docs/platforms/ds10.md): one processor, one PCI
    // bus. The processor row is the EV68CB for now, as on the DS20E.
    {"ds10", "AlphaServer DS10", "ev68cb", 1, 26, 31, "DS10SRM.ROM",
     FW_ROM_HEADER, 1, false, ds10_pci_interrupt, ds10_slot_refusal},
    // Under construction: its console image comes as an update file, with
    // no header in front of it.
    {"ds20l", "AlphaServer DS20L", "ev68cb", 2, 26, 32, "DS20L_V6_6.EXE",
     FW_RAW_IMAGE, 2, false, ds20l_pci_interrupt, ds20l_slot_refusal},
};

const platform_config *find_platform(const char *name) {
  for (const platform_config &p : platforms)
    if (!strcmp(p.name, name))
      return &p;
  return nullptr;
}
