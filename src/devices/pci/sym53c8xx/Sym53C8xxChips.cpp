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
 * The 53C8xx parts.
 **/

#include "Sym53C8xx.hpp"

#include <cstring>

// Values from the Symbios data manuals: SYM53C825A v3.0, SYM53C875/875E
// v4.0, SYM53C895 v3 (register chapters and PCI configuration). The 53C810
// row keeps the values this emulation has always used.
//
// Columns: name, PCI device, PCI revision, MACNTL, RAM bytes, ID mask;
// writable bits of SCNTL2, SCNTL2 write-1-to-clear, SCNTL3, SCID, GPREG,
// CTEST5, SIEN1; SIST1 read-clear and fatal bits; writable bits of STIME1,
// STEST1, STEST2, STEST3; STEST4 read value.
// clang-format off
static const sym_chip_config sym_chips[] = {
  // 53C810: narrow Fast SCSI-2, no SCRIPTS RAM.
  {"53C810",  0x0001, 0x01, 0x40,    0, 0x07,
   0x80, 0x00, 0x77, 0x67, 0x03, 0x18, 0x07, 0x07, 0x04,
   0x0f, 0xc0, 0x9b, 0xf7, 0x00},

  // 53C825A: Fast-Wide SCSI-2, 4 KB RAM. The A part is told from the 825
  // by the revision's upper nibble; the manual's value is unreadable, so
  // any A-part revision (> 0x0f) is used.
  {"53C825",  0x0003, 0x14, 0x60, 4096, 0x0f,
   0xf2, 0x09, 0x7f, 0x6f, 0x1f, 0x3f, 0x07, 0x07, 0x04,
   0x7f, 0xc0, 0xbf, 0xff, 0x00},

  // 53C875: Ultra-Wide, 4 KB RAM, clock doubler (STEST1 DBLEN/DBLSEL),
  // SCNTL3 bit 7 is ULTRA.
  {"53C875",  0x000f, 0x04, 0x70, 4096, 0x0f,
   0xf2, 0x09, 0xff, 0x6f, 0x1f, 0x3f, 0x07, 0x07, 0x04,
   0x7f, 0xcc, 0xbf, 0xff, 0x00},

  // 53C895: Ultra2-Wide LVD, 4 KB RAM, clock quadrupler (STEST1 QEN/QSEL),
  // SBMC interrupt, STEST4 reporting an LVD bus with the quadrupler locked.
  {"53C895",  0x000c, 0x00, 0xd0, 4096, 0x0f,
   0xf2, 0x09, 0xff, 0x6f, 0x1f, 0x3f, 0x17, 0x17, 0x14,
   0x7f, 0xcc, 0xbf, 0xff, 0xe0},
};
// clang-format on

const sym_chip_config *CSym53C8xx::find_chip(const char *name) {
  for (const auto &c : sym_chips)
    if (!strcmp(c.name + 3, name)) // "53C810" matches "810"
      return &c;
  return nullptr;
}
