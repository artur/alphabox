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
 * The QLogic ISP10x0 parts.
 *
 * Both are PCI device 0x1020 and differ by revision: the ES40 console
 * matches on the device alone and calls either "QLogic ISP10x0". The
 * KZPBA the machine ships with is an ISP1040.
 **/
#include "Isp1040.hpp"

static const isp_chip_config chips[] = {
    // name      part                 device rev  wide  ultra  firmware
    {"isp1020", "ISP1020 Fast Wide", 0x1020, 0x02, true, false, 2, 15, 0},
    {"isp1040", "ISP1040 Ultra Wide", 0x1020, 0x05, true, true, 4, 65, 0},
};

const isp_chip_config *CIsp1040::find_chip(const char *name) {
  for (const isp_chip_config &c : chips)
    if (!strcmp(c.name, name))
      return &c;
  return nullptr;
}
