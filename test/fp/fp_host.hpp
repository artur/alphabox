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
 * The host FPU operations the floating-point test uses as its IEEE oracle
 * (fp_host.cpp). Rounding modes use the Alpha encoding: 0 chopped, 1 minus
 * infinity, 2 normal, 3 plus infinity.
 **/
#if !defined(ALPHABOX_FP_HOST_HPP)
#define ALPHABOX_FP_HOST_HPP

#include <cstdint>

enum class HostOp { Add, Sub, Mul, Div, Sqrt };

struct HostFlags {
  bool inexact = false, overflow = false, underflow = false, invalid = false,
       divzero = false;
};

struct HostT {
  uint64_t bits;
  HostFlags flags;
};

struct HostS {
  uint32_t bits;
  HostFlags flags;
};

HostT host_t(HostOp op, uint64_t a, uint64_t b, int alpha_rnd);
HostS host_s(HostOp op, uint32_t a, uint32_t b, int alpha_rnd);
HostS host_t_to_s(uint64_t a, int alpha_rnd);
HostT host_q_to_t(int64_t v, int alpha_rnd);
HostS host_q_to_s(int64_t v, int alpha_rnd);
/* -1, 0, 1, or 2 for unordered */
int host_cmp_t(uint64_t a, uint64_t b);

#endif // !defined(ALPHABOX_FP_HOST_HPP)
