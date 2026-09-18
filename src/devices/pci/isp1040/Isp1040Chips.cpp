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
 * The QLogic ISP parts.
 *
 * The 1020 and the 1040 are both PCI device 0x1020 and differ by revision:
 * the ES40 console matches on the device alone and calls either "QLogic
 * ISP10x0". The KZPBA the machine ships with is an ISP1040.
 *
 * The 1080 and the 1240 are the generation after, and each has a PCI
 * device number of its own. The 1080 is one Ultra2 bus, low-voltage
 * differential and twice the speed; the 1240 is two Ultra Wide buses on
 * one PCI function -- not two functions, the way the Symbios 53C896 is
 * built, but one RISC with a SCSI processor at each end of it, sharing the
 * queues and the mailboxes.
 *
 * The 1080 and the 1240 run one and the same firmware, 8.15, as the
 * microcode header in NetBSD's tree states it ("ISP1240/ISP1080/ISP1280
 * Initiator Firmware ... Firmware Version 8.15.00"). The PCI revisions of
 * the newer parts are first silicon; nothing in the drivers of this
 * generation reads the revision or the hardware-revision nibble of
 * BIU_CONF0 -- Linux qla1280 consults it only on the 1040, to work around
 * a broken FIFO in one stepping.
 **/
#include "Isp1040.hpp"

// In order: the configuration class, the part's name, its PCI device and
// revision, how many SCSI buses it has, wide, Ultra, Ultra2, whether it is
// of the 1080 generation, and the firmware version it reports.
static const isp_chip_config chips[] = {
    {"isp1020", "ISP1020 Fast Wide", 0x1020, 0x02, 1, true, false, false, false,
     2, 15, 0},
    {"isp1040", "ISP1040 Ultra Wide", 0x1020, 0x05, 1, true, true, false, false,
     4, 65, 0},
    {"isp1080", "ISP1080 Ultra2 Wide", 0x1080, 0x01, 1, true, true, true, true,
     8, 15, 0},
    {"isp1240", "ISP1240 Dual Ultra Wide", 0x1240, 0x01, 2, true, true, false,
     true, 8, 15, 0},
};

const isp_chip_config *CIsp1040::find_chip(const char *name) {
  for (const isp_chip_config &c : chips)
    if (!strcmp(c.name, name))
      return &c;
  return nullptr;
}
