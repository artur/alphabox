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
 * What tells one Alpha processor from another of the same core family.
 *
 * The core (`CAlphaCPU`) implements the EV6 family: the 21264 instruction
 * set, its internal processor registers and its PALcode interface. Within
 * that family a part is a table row (CpuModels.cpp) -- the values software
 * reads to identify the processor. A part with a different core (EV4, EV5,
 * EV7) is not a row: it needs its own core.
 *
 * Every value here is read by the firmware or the operating system, so a
 * wrong one is visible: the console prints the processor's name from the
 * chip identification, and guests choose code paths from the architecture
 * mask. Add a row only with a source for its values -- see
 * docs/platforms.md.
 **/
#if !defined(INCLUDED_CPUMODEL_H_)
#define INCLUDED_CPUMODEL_H_

#include "StdAfx.hpp"

struct cpu_model {
  const char *name; ///< configuration class, e.g. "ev68cb"
  const char *part; ///< the part, for messages, e.g. "21264CB (EV68CB)"
  u8 chip_id;       ///< I_CTL<31:24>, which the console turns into a name
  u8 type_major;    ///< processor type reported to software [ARM D-1..3]
  u8 type_minor;    ///< revision within the type
  u8 implver;       ///< IMPLVER result: 2 = EV6 family
  u64 amask;        ///< architecture extensions this part implements
};

/// Architecture extension bits, as AMASK reports them [ARM D-4].
#define AMASK_BWX U64(0x0001)      ///< byte and word access
#define AMASK_FIX U64(0x0002)      ///< square root and FP conversions
#define AMASK_CIX U64(0x0004)      ///< count extension
#define AMASK_MVI U64(0x0100)      ///< multimedia instructions
#define AMASK_TRAP U64(0x0200)     ///< precise trap reporting
#define AMASK_PREFETCH U64(0x1000) ///< prefetch with modify intent

/// The part named `name`, or nullptr.
const cpu_model *find_cpu_model(const char *name);

#endif // !defined(INCLUDED_CPUMODEL_H_)
