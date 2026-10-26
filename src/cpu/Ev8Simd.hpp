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
 * The 21464's (EV8's) short-vector SIMD set under opcode 1C, stage S5 of
 * docs/platforms/ev8.md, as docs/platforms/ev8-isa.md 3.3 defines it (the
 * contract with the nada toolchain). Integer operate format, Rb or a
 * literal; no instruction of the set traps or reads the FPCR. The rows with
 * CPU_EXT_EV8 (`ev8`, `ev8n`) decode it in execute() and compiled code calls
 * the same routine (CAlphaCPU::jit_simd); on every other row the words raise
 * OPCDEC as before.
 **/
#if !defined(INCLUDED_EV8SIMD_HPP)
#define INCLUDED_EV8SIMD_HPP

#include "datatypes.hpp"

namespace ev8simd {

/// True for a 1C function code the sheet assigns to the set (3.3's table);
/// the core's own codes (00, 01, 30-3F, 70, 78) and the free ones are not.
bool assigned(u32 function);

/// Rc of the instruction with 7-bit function `function` for Rav = a and
/// Rbv = b (the literal, zero-extended, in the literal form). Only for an
/// assigned() function.
u64 exec(u32 function, u64 a, u64 b);

} // namespace ev8simd

#endif // !defined(INCLUDED_EV8SIMD_HPP)
