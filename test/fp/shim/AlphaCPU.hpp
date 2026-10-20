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
 * Stand-in for src/cpu/AlphaCPU.hpp in the floating-point test (test/fp).
 *
 * The test compiles the interpreter's own AlphaCPU_ieeefloat.cpp and
 * AlphaCPU_vaxfloat.cpp, unchanged, against this header: it is first on the
 * test's include path, so their `#include "AlphaCPU.hpp"` lands here. It
 * declares only the part of CAlphaCPU the floating-point routines and the
 * DO_ macros of cpu_fp_operate.hpp touch.
 **/
#if !defined(ALPHABOX_FP_SHIM_HPP)
#define ALPHABOX_FP_SHIM_HPP

#include "StdAfx.hpp"
#include "cpu_defs.hpp"

#include <cstring>

/* What GO_PAL calls on the system. */
struct FpShimSystem {
  void cpu_clear_lock(int) {}
};

/* Integer register numbers without PALshadow (the test never runs in
   PALmode with shadow registers enabled). */
#define RREG(a) ((a) & 0x1f)

#include "fp_shim_class.inc"

#endif // !defined(ALPHABOX_FP_SHIM_HPP)
