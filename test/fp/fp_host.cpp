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
 * The host FPU as the IEEE oracle of the floating-point test. Both hosts
 * Alphabox builds for (AArch64, x86-64 SSE) implement IEEE 754 binary32 and
 * binary64 with gradual underflow, in all four rounding modes. Every
 * operand goes through a volatile load and every result through a volatile
 * store, so the operation can't move across fesetround/fetestexcept or be
 * folded at compile time; CMake also builds this file with the compiler's
 * strict floating-point option where it has one.
 **/
#include "fp_host.hpp"

#include <cfenv>
#include <cmath>
#include <cstring>

#if defined(__clang__) || defined(_MSC_VER)
#pragma STDC FENV_ACCESS ON
#endif

static int host_mode(int alpha_rnd) {
  switch (alpha_rnd) {
  case 0:
    return FE_TOWARDZERO; // chopped
  case 1:
    return FE_DOWNWARD; // minus infinity
  case 3:
    return FE_UPWARD; // plus infinity
  default:
    return FE_TONEAREST; // normal
  }
}

static HostFlags host_flags() {
  HostFlags f;
  const int e = fetestexcept(FE_ALL_EXCEPT);
  f.inexact = (e & FE_INEXACT) != 0;
  f.overflow = (e & FE_OVERFLOW) != 0;
  f.underflow = (e & FE_UNDERFLOW) != 0;
  f.invalid = (e & FE_INVALID) != 0;
  f.divzero = (e & FE_DIVBYZERO) != 0;
  return f;
}

HostT host_t(HostOp op, uint64_t a, uint64_t b, int alpha_rnd) {
  volatile double va, vb, vr;
  double da, db;
  memcpy(&da, &a, 8);
  memcpy(&db, &b, 8);
  const int old = fegetround();
  fesetround(host_mode(alpha_rnd));
  feclearexcept(FE_ALL_EXCEPT);
  va = da;
  vb = db;
  switch (op) {
  case HostOp::Add:
    vr = va + vb;
    break;
  case HostOp::Sub:
    vr = va - vb;
    break;
  case HostOp::Mul:
    vr = va * vb;
    break;
  case HostOp::Div:
    vr = va / vb;
    break;
  case HostOp::Sqrt:
    vr = std::sqrt((double)vb);
    break;
  }
  HostT r;
  r.flags = host_flags();
  fesetround(old);
  const double d = vr;
  memcpy(&r.bits, &d, 8);
  return r;
}

HostS host_s(HostOp op, uint32_t a, uint32_t b, int alpha_rnd) {
  volatile float va, vb, vr;
  float fa, fb;
  memcpy(&fa, &a, 4);
  memcpy(&fb, &b, 4);
  const int old = fegetround();
  fesetround(host_mode(alpha_rnd));
  feclearexcept(FE_ALL_EXCEPT);
  va = fa;
  vb = fb;
  switch (op) {
  case HostOp::Add:
    vr = va + vb;
    break;
  case HostOp::Sub:
    vr = va - vb;
    break;
  case HostOp::Mul:
    vr = va * vb;
    break;
  case HostOp::Div:
    vr = va / vb;
    break;
  case HostOp::Sqrt:
    vr = std::sqrt((float)vb);
    break;
  }
  HostS r;
  r.flags = host_flags();
  fesetround(old);
  const float f = vr;
  memcpy(&r.bits, &f, 4);
  return r;
}

HostS host_t_to_s(uint64_t a, int alpha_rnd) {
  volatile double va;
  volatile float vr;
  double da;
  memcpy(&da, &a, 8);
  const int old = fegetround();
  fesetround(host_mode(alpha_rnd));
  feclearexcept(FE_ALL_EXCEPT);
  va = da;
  vr = (float)va;
  HostS r;
  r.flags = host_flags();
  fesetround(old);
  const float f = vr;
  memcpy(&r.bits, &f, 4);
  return r;
}

HostT host_q_to_t(int64_t v, int alpha_rnd) {
  volatile int64_t vv;
  volatile double vr;
  const int old = fegetround();
  fesetround(host_mode(alpha_rnd));
  feclearexcept(FE_ALL_EXCEPT);
  vv = v;
  vr = (double)vv;
  HostT r;
  r.flags = host_flags();
  fesetround(old);
  const double d = vr;
  memcpy(&r.bits, &d, 8);
  return r;
}

HostS host_q_to_s(int64_t v, int alpha_rnd) {
  volatile int64_t vv;
  volatile float vr;
  const int old = fegetround();
  fesetround(host_mode(alpha_rnd));
  feclearexcept(FE_ALL_EXCEPT);
  vv = v;
  vr = (float)vv;
  HostS r;
  r.flags = host_flags();
  fesetround(old);
  const float f = vr;
  memcpy(&r.bits, &f, 4);
  return r;
}

int host_cmp_t(uint64_t a, uint64_t b) {
  volatile double va, vb;
  double da, db;
  memcpy(&da, &a, 8);
  memcpy(&db, &b, 8);
  va = da;
  vb = db;
  if (std::isnan((double)va) || std::isnan((double)vb))
    return 2;
  return va < vb ? -1 : (va > vb ? 1 : 0);
}
