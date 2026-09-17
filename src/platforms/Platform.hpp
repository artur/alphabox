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
 * The machine around the chipset: what a board contributes that the
 * chipset and the processor do not.
 *
 * A board is a row in the table in Platforms.cpp, chosen with
 * `platform = "<name>";` in the machine block (the default is the ES40,
 * the machine this emulator started as). It says which processors and how
 * many the board takes, how much memory it holds, which PCI slots exist
 * and where their interrupts are wired, and which console firmware image
 * it runs and in what format.
 *
 * What is NOT here: anything the chipset decides (address decoding, the
 * PCI windows, DMA translation, the interrupt controller), and anything
 * the processor decides (its identity: CpuModel.hpp). Adding a machine is
 * described in docs/platforms.md.
 **/
#if !defined(INCLUDED_PLATFORM_H_)
#define INCLUDED_PLATFORM_H_

#include "StdAfx.hpp"

/// How a console firmware image is packaged.
enum firmware_format {
  /// An update bundle ("LFU APU") holding several images, as the ES40's
  /// cl67srmrom.exe is: Alphabox finds the console inside it.
  FW_LFU_BUNDLE,
  /// A raw image behind the standard Alpha ROM header (c3c3 5a5a 3c3c
  /// a5a5), as the firmware CD's PC264SRM.ROM and DS10SRM.ROM are. Not
  /// implemented yet: the first work item of the DS20E packet.
  FW_ROM_HEADER,
};

struct platform_config {
  const char *name;        ///< the `platform` value, e.g. "es40"
  const char *description; ///< for messages, e.g. "AlphaServer ES40"

  const char *cpu_model; ///< the processor class this board takes
  int max_cpus;

  int min_memory_bits; ///< smallest and largest memory size the board holds
  int max_memory_bits;

  const char *firmware_file; ///< default console image
  firmware_format firmware;

  int pci_hoses; ///< PCI buses out of the chipset

  /**
   * The interrupt input a device's pin reaches, or -1 when the slot has no
   * interrupt. `hose` and `slot` are the device's place on the machine's
   * own buses (behind a bridge, the outermost bridge's slot), and `intx`
   * is 0-3 for INTA-INTD.
   */
  int (*pci_interrupt)(int hose, int slot, int intx);

  /**
   * Whether an add-in device may sit at this slot, and why not when it may
   * not: slots that hold the board's own hardware are refused, because
   * firmware that finds something unexpected there misbehaves in ways that
   * are hard to recognise.
   */
  const char *(*slot_refusal)(int hose, int slot);
};

/// The board named `name`, or nullptr.
const platform_config *find_platform(const char *name);

/// The board Alphabox emulates unless the configuration says otherwise.
#define DEFAULT_PLATFORM "es40"

#endif // !defined(INCLUDED_PLATFORM_H_)
