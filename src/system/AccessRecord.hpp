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

#if !defined(INCLUDED_ACCESSRECORD_H)
#define INCLUDED_ACCESSRECORD_H

#include "StdAfx.hpp"

/**
 * \brief The device access recorder: the accesses the guest makes to the
 * devices it names, one line each, in a form two builds can be diffed by
 * (test/tools/trace_diff.py) and a unit test can replay.
 *
 * ALPHABOX_RECORD=<name>[,<name>...] selects what is recorded:
 * - a device by its configuration node, as "pci0.7", "ali" or "pci0.7(ali)";
 *   every access to every range it registered;
 * - "dpr", "flash", "i2c": the board's dual-port RAM, flash and PCF8584 I2C
 *   controller (DS10, DS20, DS20E);
 * - "pcicfg": configuration space of every PCI device, as the bus sees it
 *   (not the devices' own reads of their registers);
 * - "csr": the Tsunami's Cchip, Dchip, Pchip and TIG registers;
 * - "raw": record every access; without it a run of accesses that repeats
 *   with a period of up to four records is written once and its repeats are
 *   dropped, so a polling loop reads the same however long it spins.
 *
 * The lines go to ALPHABOX_RECORD_FILE (default alphabox-record.txt in the
 * working directory), complete once the emulator exits:
 *
 *   <device> <who> <r|w> <space> <address> <bits> <value>
 *
 * who is cpuN, or dev for a device's own access; space is the device's range
 * index, "f<n>" for a PCI function's configuration space, or the chip for a
 * CSR; address is the offset into that range; value is what was written or
 * what the read returned, all in hex. Repeats are dropped per device and
 * processor, so the order between processors does not change what is kept.
 **/
namespace access_record {
/// Any recording selected; read on every device access, so a plain flag.
extern const bool g_on;

/// Whether the name is selected: a token equal to it, or to the part of a
/// "name(value)" before or inside the parentheses.
bool selected(const char *name);

/// Record one access.
void note(const char *device, const char *space, u64 address, int bits,
          bool write, u64 value);
/// The same with the range index as the space.
void note(const char *device, int index, u64 address, int bits, bool write,
          u64 value);
} // namespace access_record

#endif // !defined(INCLUDED_ACCESSRECORD_H)
