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
 * The Alpha processors of the EV6 family this emulator implements, and the
 * EV7 parts built on its core.
 *
 * The EV68CB is here because its values are established: the ES40 console
 * prints "Alpha EV68CB pass 4.0" from them, and the regression logs would
 * show any change. The other parts of the
 * family (EV6 21264, EV67 21264A, EV68AL, EV68DC) are rows waiting for
 * their values; a machine that ships with one of them -- the DS20E, for
 * instance -- adds its row with a source, and the console naming it
 * correctly is the check (docs/platforms/ds20e.md).
 **/
#include "StdAfx.hpp"

#include "CpuModel.hpp"

static const cpu_model models[] = {
    // 21264CB: chip identification 0x21, processor type 12 revision 6,
    // EV6-family IMPLVER, and every extension the part implements
    // [21264CB HRM 5-16, 2-38; ARM D-1..5].
    {"ev68cb", "21264CB (EV68CB)", 0x21, 12, 6, 2,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP |
         AMASK_PREFETCH,
     CPU_FAMILY_EV6, 64, 64, 0},
    // 21364 (EV7): the EV68 core, so the same IMPLVER and extensions
    // [guess: no document lists the 21364's AMASK; Linux has no EV7 case]
    // and 64 KB L1 caches. The chip ID is what the console's PALcode turns
    // into the processor type and revision it reports (0x3ea94 in the
    // decompressed SRM V7.3-1): I_CTL<29:24>, plus bit 19 of CSR 0x28020 when
    // the chip ID is 2, indexes a table of (minor, 15) pairs, minor = index
    // + 1, and the console names minor 1-5 "EV7 rev 1.0", "2.0", "2.1",
    // "2.2" and "3.0". Chip ID 2 is "EV7 rev 2.1", type 15 minor 3 (Linux
    // hwrpb.h EV7_CPU is 15). The L2 is 1.75 MB, 7-way (Linux setup.c), but
    // console listings print 1.50 MB for revision 2 parts
    // (docs/platforms/marvel.md): six ways enabled, which the console reads
    // back from BBOX_CTL.
    {"ev7", "21364 (EV7)", 2, 15, 3, 2,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP |
         AMASK_PREFETCH,
     CPU_FAMILY_EV7, 64, 64, 1536},
    // EV7z, the same core at 1.15-1.3 GHz. A real ES47 7/1300's show
    // config calls it "EV7 rev 3.0" with 1.75 MB of cache: minor 5, which
    // only chip ID 4 reaches in the console's table [inference: no document
    // gives the chip ID]. The console reports it as type 15 (show cpu:
    // "Type Major 15, Minor 5"): Linux's EV79_CPU, 16, is not what this
    // console hands on, and the rows's type is for messages.
    {"ev7z", "21364 (EV7z)", 4, 15, 5, 2,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP |
         AMASK_PREFETCH,
     CPU_FAMILY_EV7, 64, 64, 1792},
};

const cpu_model *find_cpu_model(const char *name) {
  for (const cpu_model &m : models)
    if (!strcmp(m.name, name))
      return &m;
  return nullptr;
}
