/* AXPbox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/axpbox
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

// clang-format off
static const sym_chip_config sym_chips[] = {
  // 53C810: narrow Fast SCSI-2, no on-chip RAM.
  {
    "53C810", 0x0001, 0x01, 0x40, 0,
    0x07,                   // id_mask
    0x80, 0x00, 0x77, 0x67, // scntl2, scntl2_w1c, scntl3, scid
    0x03, 0x18, 0x07,       // gpreg, ctest5, sien1
    0x07, 0x04,             // sist1 rc, fatal
    0x0f, 0xc0, 0x9b, 0xf7, // stime1, stest1, stest2, stest3
  },
};
// clang-format on

const sym_chip_config *CSym53C8xx::find_chip(const char *name) {
  for (const auto &c : sym_chips)
    if (!strcmp(c.name + 3, name)) // "53C810" matches "810"
      return &c;
  return nullptr;
}
