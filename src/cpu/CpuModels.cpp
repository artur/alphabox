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
 * The Alpha processors of the EV6 family this emulator implements.
 *
 * Only the EV68CB is here, because it is the only one whose values are
 * established: the ES40 console prints "Alpha EV68CB pass 4.0" from them,
 * and the regression logs would show any change. The other parts of the
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
         AMASK_PREFETCH},
};

const cpu_model *find_cpu_model(const char *name) {
  for (const cpu_model &m : models)
    if (!strcmp(m.name, name))
      return &m;
  return nullptr;
}
