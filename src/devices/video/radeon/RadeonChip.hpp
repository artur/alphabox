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

/* The facts that tell one Radeon part from another, one row per part
 * (RadeonChips.cpp). Everything the common code (the register aperture,
 * the command FIFO, the CP, the GART, the PLLs and the CRTC) needs to know
 * about the chip it is is here, so that a part of another generation is a
 * row plus its own 3D engine. Only the RV200 (Radeon 7500) exists today.
 */

#if !defined(INCLUDED_RADEON_CHIP_H)
#define INCLUDED_RADEON_CHIP_H

#include <cstdint>

#include "datatypes.hpp"

namespace radeon {

struct ChipInfo {
  const char *name;      ///< the config's name for the part
  const char *marketing; ///< what the banner prints
  u16 device_id;         ///< PCI device
  u16 subsys_agp;        ///< subsystem device of the AGP board (vendor ATI)
  u16 subsys_pci;        ///< ... of the PCI board
  /// Entries of the RBBM's command FIFO (RBBM_STATUS CMDFIFO_AVAIL <6:0>).
  u32 cmdfifo_entries;
  /// Dwords the CP's primary queue takes in PIO mode (CP_CSQ_CNTL
  /// CSQ_CNT_PRIMARY).
  u32 csq_primary_dwords;
  /// Entries of the CP's micro-engine RAM (two dwords each, CP_ME_RAM_*).
  u32 me_ram_entries;
  /// The PLLs' reference clock, kHz.
  u32 ref_clock_khz;
  /// The engine clock (SCLK) and the pixels the engine writes a clock: the
  /// rate the engine's busy time is modelled at.
  u32 engine_clock_khz;
  u32 pixels_per_clock;
};

/// The row for a part by name (nullptr if there is none).
const ChipInfo *find_chip(const char *name);
/// The part this class emulates when the configuration names none.
const ChipInfo &default_chip();

} // namespace radeon

#endif // !defined(INCLUDED_RADEON_CHIP_H)
