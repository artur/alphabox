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
 * PCI windows, DMA translation, the interrupt controller: the row only
 * names which chipset, chipsets/), and anything
 * the processor decides (its identity: CpuModel.hpp). Adding a machine is
 * described in docs/platforms.md.
 **/
#if !defined(INCLUDED_PLATFORM_H_)
#define INCLUDED_PLATFORM_H_

#include "StdAfx.hpp"

#include "Chipset.hpp"

class CConfigurator;
class CSystem;
struct marvel_layout;
struct titan_layout;

/// How a console firmware image is packaged.
enum firmware_format {
  /// An update bundle ("LFU APU") holding several images, as the ES40's
  /// cl67srmrom.exe is: Alphabox finds the console inside it.
  FW_LFU_BUNDLE,
  /// A raw image behind the standard Alpha ROM header (c3c3 5a5a 3c3c
  /// a5a5), as the firmware CD's PC264SRM.ROM and DS10SRM.ROM are.
  FW_ROM_HEADER,
  /// The console image with nothing in front of it, as the firmware CD's
  /// update files (DS20L_V6_6.EXE and the other *_V7_3.EXE) are: they
  /// start directly with the console's self-decompressor.
  FW_RAW_IMAGE,
};

/// When the processors other than the first start running.
enum secondary_start {
  /// The console starts them itself: the ES40's does, through the
  /// management processor, so they wait until it says so.
  SECONDARIES_BY_CONSOLE,
  /// Every processor must already be running PALcode when the console
  /// looks: the DS20E has no management processor, and its console asserts
  /// a processor's halt line and expects an answer. The others are released
  /// at their PALcode reset entry once processor 0 has cleared the
  /// chipset's arbitration and so become the console's primary
  /// (CSystem::start_secondaries).
  SECONDARIES_AFTER_ARBITRATION,
};

/// A word the loader patches into the decompressed console (a table ends
/// with address 0).
struct rom_patch {
  u64 address;
  u32 insn;
};

/**
 * A device soldered to the board that is an ordinary device class: the
 * configuration may describe it at its place (a network controller's
 * backend), and when it says nothing about that place the device is added
 * with `defaults`. A table ends with a null name.
 */
struct onboard_device {
  const char *name;         ///< its place, e.g. "pci0.8"
  const char *device_class; ///< e.g. "i82559er"
  const char *defaults;     ///< its block's text, e.g. "type=\"null\";"
};

/**
 * One of the board's own PCI functions: the class a configuration names for
 * it and where it sits. What a tool that writes a configuration (alphabox
 * configure) needs to place them. A table ends with a null class.
 */
struct board_function {
  const char *device_class; ///< e.g. "ali_ide"
  const char *place;        ///< e.g. "pci0.15"
};

/**
 * The revisions of a Tsunami-family board's core-logic chips, as its console
 * reads them for `show config` (docs/platforms.md, "Chip revisions"). Each
 * field is the register the SRM console prints its revision from; the
 * printed text is the console's own (it halves a TIG or arbiter byte into
 * major.minor as byte >> 5 and byte & 31).
 */
struct tsunami_revisions {
  /// Cchip MISC<REV> (HRM Table 10-12): 1 is a Tsunami, 8 a Typhoon. The
  /// console prints REV + 1.
  u8 cchip_rev;
  /// Pchip PMONCTL (HRM Table 10-47) as read; the DS20 family's console
  /// prints "Rev 2.2" instead of "Rev 2" when it is not zero.
  u64 pchip_pmonctl;
  /// TIG bus byte at 0x30000000, which the ES40's console prints as its TIG
  /// revision (in hex).
  u8 tig_trr;
  /// TIG bus bytes at 0x38000180, 0x38000140 and 0x380001c0: the TIG
  /// revision and the arbiter's revision and identity byte, which the other
  /// boards' consoles print.
  u8 tig_rev;
  u8 arbiter_rev;
  u8 arbiter_id;
};

struct platform_config {
  const char *name;        ///< the `platform` value, e.g. "es40"
  const char *description; ///< for messages, e.g. "AlphaServer ES40"

  /// The system logic the board is built around (Chipset.hpp).
  chipset_kind chipset;

  const char *cpu_model; ///< the processor class this board takes
  int max_cpus;

  int min_memory_bits; ///< smallest and largest memory size the board holds
  int max_memory_bits;

  const char *firmware_file; ///< default console image
  firmware_format firmware;

  int pci_hoses; ///< PCI buses out of the chipset

  /// How the processors other than the first start.
  secondary_start secondaries;

  /**
   * The board's own hardware -- what is neither the chipset nor a device
   * the configuration names: the ES40's DPR and flash, the DS10's I2C
   * controller and what hangs on its bus. Built right after the system,
   * from platforms/<board>/.
   */
  void (*board_devices)(CConfigurator *cfg, CSystem *sys);

  /**
   * The interrupt input a device's pin reaches, or -1 when the slot has no
   * interrupt. `hose` and `slot` are the device's place on the machine's
   * own buses (behind a bridge, the outermost bridge's slot), and `intx`
   * is 0-3 for INTA-INTD. `func` is the device's PCI function, or -1 for a
   * device behind a bridge: on some boards an embedded slot wires its
   * functions to other lines than their pins say (the ES47, es47/Es47.cpp).
   */
  int (*pci_interrupt)(int hose, int slot, int intx, int func);

  /**
   * Whether an add-in device may sit at this slot, and why not when it may
   * not: slots that hold the board's own hardware are refused, because
   * firmware that finds something unexpected there misbehaves in ways that
   * are hard to recognise.
   */
  const char *(*slot_refusal)(const platform_config &board, int hose, int slot);

  /**
   * Where the PALcode lives that the interpreter may replace with its
   * native routines (cpu/AlphaCPU_vmspal.cpp), or 0 when it may not. They
   * were written against the ES40 console's OpenVMS PALcode, at PAL_BASE
   * 0x8000; the DS20E's and DS20L's consoles put their own PALcode builds
   * there too, and with the native routines OpenVMS 8.4 hangs after its
   * banner (DS20E, one processor) or bugchecks on the second processor
   * (DS20E, DS20L). Boards with 0 run the real PALcode, as JIT builds
   * always do (docs/platforms/ds20e.md).
   */
  u64 vmspal_pal_base;

  /// Words patched into the decompressed console for speed, or nullptr.
  const rom_patch *console_patches;

  /// How much of low memory the decompressed console occupies, which is
  /// what the decompressed-image cache (rom.decompressed) saves; 0 for the
  /// 2 MB every Tsunami console fits in. Marvel's console runs up to
  /// 0x440000.
  u32 console_bytes;

  /// A Marvel board's processor layout: their coordinates, PIDs and IO7s
  /// (chipsets/marvel/Topology.hpp); nullptr on every other board.
  const marvel_layout *marvel = nullptr;

  /// A Titan board's chipset facts: the Cchip's pass, how memory fills
  /// the arrays (chipsets/titan/Titan.hpp); nullptr elsewhere.
  const titan_layout *titan = nullptr;

  /// The board's own devices of ordinary classes, or nullptr.
  const onboard_device *onboard = nullptr;

  /// Which of the chipset's four memory arrays the board has slots for,
  /// a bit each (what `memory.arrays` may fill); 0 for all four. The DS15
  /// has arrays 0 and 2 (its owner's guide, examples 2-5 and 2-7); the
  /// DS10's and DS20L's consoles list arrays 0 and 1 and no others, with
  /// all four populated.
  u8 memory_arrays = 0;

  /// The largest memory array the board's console can read, in MB; 0 for
  /// 8192, the Typhoon's AARn<15:12>. The DS20E's console takes the size
  /// from AARn<14:12> (V7.3-1, 0x86648: `sra 12; and 7`), the 21272's
  /// field, so 1024 MB is its largest: it reads a 2 GB array as none and
  /// a 4 GB one as 16 MB. The DS10's and DS20L's consoles answer the same
  /// way [their code was not read]. More memory goes in more arrays.
  u16 max_array_mb = 0;

  /// The board's own PCI functions, where its console looks for them, or
  /// nullptr when it has none a configuration names.
  const board_function *functions = nullptr;

  /// A Tsunami-family board's chip revisions; nullptr means the defaults
  /// the ES40 reported before the rows existed (Typhoon, PMONCTL 0).
  const tsunami_revisions *tsunami = nullptr;
};

/// The board named `name`, or nullptr.
const platform_config *find_platform(const char *name);

/// The boards, in the order of their table: the `i`th, or nullptr past the
/// last.
const platform_config *platform_at(size_t i);

/// The board Alphabox emulates unless the configuration says otherwise.
#define DEFAULT_PLATFORM "es40"

#endif // !defined(INCLUDED_PLATFORM_H_)
