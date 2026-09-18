/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2021 Dietmar M. Zettl
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 *
 * Parts of this file based upon GXemul, which is Copyright (C) 2004-2007
 * Anders Gavare.  All rights reserved.
 */

/**
 * \file
 * The Tulip parts: a row per member of the family, and what each one
 * answers in PCI configuration space.
 **/
#include "StdAfx.hpp"

#if defined(HAVE_PCAP) || defined(__linux__)
#include "Tulip.hpp"
#include "System.hpp"
#include <string.h>

/* Each row claims a pass-2 revision. Drivers never look for an exact pass;
   they compare against a threshold -- a 21140 of revision 0x20 or later is
   a 21140A, a 21041 of revision 0x20 or later is the pass whose
   autonegotiation works -- so claiming pass 2 is claiming the part those
   drivers were written for.

   CSR13 to CSR15 hold the SIA on the parts that have one, and the 21040's
   and 21041's manuals are not to hand: what the rows below say is the
   simplest thing that is true of a part fresh out of reset, that its SIA is
   held in reset (CSR13 clear, since SRL is active high) and that the port
   therefore reports link fail. Nothing else in these registers is state the
   emulation reads back. The 21140 has no SIA at all, and its CSR12 is
   eight general purpose pins which come out of reset as inputs. */
static const tulip_chip_config chips[] = {
    // name        part    device  rev   subsystem   station address
    //   serial ROM         media                    CSR12/13/14/15 at reset
    {"dec21040", "21040", 0x0002, 0x23, 0, 0, TULIP_ID_ADDRESS_ROM,
     TULIP_SROM_NONE, TULIP_MEDIA_SIA_21040, SIASTAT_NCR | SIASTAT_LKF, 0, 0,
     0},
    {"dec21041", "21041", 0x0014, 0x21, 0, 0, TULIP_ID_SERIAL_ROM,
     TULIP_SROM_21041, TULIP_MEDIA_SIA_21041, SIASTAT_NCR | SIASTAT_LKF, 0, 0,
     0},
    {"dec21140", "21140A", 0x0009, 0x22, 0, 0, TULIP_ID_SERIAL_ROM,
     TULIP_SROM_21140, TULIP_MEDIA_GPR_21140, 0, 0, 0, 0},
    {"dec21143", "21143", 0x0019, 0x30, 0x1011, 0x500b, TULIP_ID_SERIAL_ROM,
     TULIP_SROM_21143, TULIP_MEDIA_SIA_21143, 0x000000C6, 0xFFFF0000,
     0xFFFFFFFF, 0x8FF00000},
};

const tulip_chip_config *CTulip::find_chip(const char *name) {
  for (const tulip_chip_config &c : chips)
    if (!strcmp(c.name, name))
      return &c;
  return nullptr;
}

/**
 * The configuration header the part answers with. Every Tulip presents the
 * same pair of base registers -- the CSRs in I/O space, and the same CSRs
 * again in memory space -- so what differs between the parts is only who
 * they say they are.
 **/
void tulip_config_space(const tulip_chip_config &chip, u32 *data, u32 *mask) {
  data[0x00 >> 2] = (u32)chip.device_id << 16 | 0x1011; // CFID: vendor DEC
  data[0x04 >> 2] = 0x02800000;                 // CFCS: command + status
  data[0x08 >> 2] = 0x02000000 | chip.revision; // CFRV: Ethernet, revision
  data[0x10 >> 2] = 0x00000001;                 // BAR0: CBIO
  data[0x14 >> 2] = 0x00000000;                 // BAR1: CBMA
  /* The subsystem registers arrived with PCI 2.1. The parts older than the
     21143 answer zero there, which is how a driver that wants to know the
     board asks their serial ROM instead. */
  data[0x2c >> 2] = (u32)chip.subsys_id << 16 | chip.subsys_vendor;
  /* CFIT: interrupt on INTA, line not yet assigned. The 21143 also states
     how much bus time it wants; for the older parts we have no such figure,
     and zero in those fields is the honest "no requirement". */
  data[0x3c >> 2] =
      chip.media == TULIP_MEDIA_SIA_21143 ? 0x281401ff : 0x000001ff;

  mask[0x04 >> 2] = 0x0000ffff; // CFCS: command + status
  mask[0x0c >> 2] = 0x0000ffff; // CFLT: latency timer + cache line size
  mask[0x10 >> 2] = 0xffffff00; // BAR0: CBIO
  mask[0x14 >> 2] = 0xffffff00; // BAR1: CBMA
  mask[0x3c >> 2] = 0x000000ff; // CFIT: interrupt line
}

#endif // defined(HAVE_PCAP) || defined(__linux__)
