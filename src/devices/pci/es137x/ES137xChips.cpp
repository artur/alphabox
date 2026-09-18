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
 * The AudioPCI parts: a row per member of the family, and what each one
 * answers in PCI configuration space.
 **/
#include "StdAfx.hpp"

#ifdef HAVE_SDL
#include "ES137x.hpp"

#include <string.h>

/* The ES1370 keeps the subsystem id it has always had here -- the initials
   of the author of the model ours grew out of, which no real card ever
   reported, but which is what our ES1370 has said since it arrived and
   which no guest looks at. Its register window is 256 bytes wide for the
   same reason: that is the size it has always claimed, and although the
   real part decodes only 64 nothing has ever depended on the difference.
 *
 * The ES1371 says what the specification says it says, because a guest
 * very much does look. Windows 2000 binds its driver by the subsystem id
 * first -- PCI\VEN_1274&DEV_1371&SUBSYS_13711274 in wdma_ens.inf, the
 * Concert AC97 board -- and only then by the bare device id. Revision 2
 * is the original ES1371: the higher revisions are the ES1373 and the
 * Creative CT5880 respins, and drivers tell those apart by this one byte
 * and turn on S/PDIF and a reset quirk for them.
 *
 * The codec is a TriTech TR28023, which Creative sold as the CT1297 and
 * which is what sat on the retail AudioPCI 97 boards; its AC'97 vendor id
 * is the three letters TRA and a part number, and a driver will look that
 * up in a table of codecs it knows. */
static const es137x_chip_config chips[] = {
    // name      part      device  rev   BAR0 mask   subsystem      AC'97
    //   codec id
    {"es1370", "ES1370", 0x5000, 0x00, 0xffffff00, 0x4942, 0x4c4c, false, 0},
    {"es1371", "ES1371", 0x1371, 0x02, 0xffffffc0, 0x1274, 0x1371, true,
     0x54524103},
};

/**
 * The configuration header the part answers with. Both parts present the
 * same single base register -- the whole control window in I/O space --
 * and ask for an interrupt on INTA, so what differs between them is who
 * they say they are and how much of that window they decode.
 **/
void es137x_config_space(const es137x_chip_config &chip, u32 *data, u32 *mask) {
  data[0x00 >> 2] = (u32)chip.device << 16 | 0x1274; // CFID: vendor Ensoniq
  data[0x04 >> 2] = 0x02000001; // CFCS: I/O enabled, DEVSEL medium
  data[0x08 >> 2] = 0x04010000 | chip.revision; // CFRV: audio, revision
  data[0x10 >> 2] = 0x00000001;                 // BAR0: the control window
  data[0x2c >> 2] = (u32)chip.subsys_device << 16 | chip.subsys_vendor;
  data[0x3c >> 2] = 0x401101ff; // CFIT: INTA, line not yet assigned

  mask[0x04 >> 2] = 0x00000157;    // CFCS: command + status
  mask[0x0c >> 2] = 0x0000ffff;    // CFLT: latency timer + cache line size
  mask[0x10 >> 2] = chip.bar_mask; // BAR0
  mask[0x3c >> 2] = 0x000000ff;    // CFIT: interrupt line
}

const es137x_chip_config *CES137x::find_chip(const char *name) {
  for (const es137x_chip_config &c : chips)
    if (!strcmp(c.name, name))
      return &c;
  return nullptr;
}
#endif /* HAVE_SDL */
