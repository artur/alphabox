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
    // and 64 KB L1 caches. The chip ID is what the console's PALcode tests:
    // it reads I_CTL<29:24> and takes the EV7 path when it is 2 (0x3eaa0 in
    // the decompressed SRM V7.3-1). The HWRPB processor type is 15
    // (Linux hwrpb.h EV7_CPU); the console derives the type and revision it
    // reports itself, from the chip ID and an on-chip register, so the type
    // and minor here are for messages. The L2 is 1.75 MB, 7-way (Linux
    // setup.c), but console listings print 1.50 MB for revision 2 parts
    // (docs/platforms/marvel.md), and the console calls this row "EV7 rev
    // 2.1": six ways enabled, which the console reads back from BBOX_CTL.
    {"ev7", "21364 (EV7)", 2, 15, 2, 2,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP |
         AMASK_PREFETCH,
     CPU_FAMILY_EV7, 64, 64, 1536},
    // EV7z (Linux: EV79), the same core at 1.15-1.3 GHz: HWRPB type 16
    // (hwrpb.h EV79_CPU). Real listings call it "EV7 rev 3.0" with 1.75 MB
    // of cache. Its chip ID is not known to differ [guess: the same 2; the
    // console's PALcode adds bit 19 of an on-chip register at CSR 0x28020 to
    // it before indexing its type table, which is where a revision would
    // show].
    {"ev7z", "21364 (EV7z)", 2, 16, 3, 2,
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
