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
 * The floating-point test's decode over the current DO_ macros. The
 * routines themselves are src/cpu/AlphaCPU_ieeefloat.cpp and
 * AlphaCPU_vaxfloat.cpp, compiled into the test as they are.
 **/
#include "AlphaCPU.hpp"
#include "cpu_debug.hpp"

#include "cpu_fp_operate.hpp"

#include "fp_exec.inc"
