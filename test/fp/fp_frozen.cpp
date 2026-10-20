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
 * The floating-point routines as they were at the test's baseline commit
 * (ALPHABOX_FP_BASELINE in CMakeLists.txt), compiled inside namespace
 * frozen so they run beside the current ones. CMake extracts the baseline's
 * AlphaCPU_ieeefloat.cpp, AlphaCPU_vaxfloat.cpp and cpu_fp_operate.hpp with
 * `git show` into fp_frozen_src/ in the build directory. The headers they
 * share with the current code (cpu_defs.hpp, the shim) are the current ones:
 * their own #includes find the guards already set and add nothing.
 **/
#include "AlphaCPU.hpp"
#include "cpu_debug.hpp"

namespace frozen {
#include "fp_shim_class.inc"

#include "fp_frozen_src/AlphaCPU_ieeefloat.cpp"
#include "fp_frozen_src/AlphaCPU_vaxfloat.cpp"

#include "fp_frozen_src/cpu_fp_operate.hpp"

#include "fp_exec.inc"
} // namespace frozen
