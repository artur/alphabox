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
 * The 21464's (EV8's) paired single precision under opcode 07, stage S6 of
 * docs/platforms/ev8.md, as docs/platforms/ev8-isa.md 3.4 defines it (the
 * contract with the nada toolchain). The rows with CPU_EXT_EV8 (`ev8`,
 * `ev8n`) decode it in execute(); compiled code calls the same routine
 * (CAlphaCPU::jit_paired). On every other row opcode 07 raises OPCDEC.
 *
 * A register holds two IEEE singles, the high one in <63:32> and the low in
 * <31:0>, each in memory format [IDS p. 280]. Each half is computed as the
 * scalar S-format instruction with the same qualifiers and the same FPCR
 * (D9): the IEEE routines of AlphaCPU_ieeefloat.cpp, given the half in
 * register format (as LDS maps it) and the paired word itself, whose Trp
 * <15:13> and Rnd <12:11> are where the scalar format has them [IDS p. 280].
 * Each half's exceptions set their FPCR status bits and trap as the scalar
 * instruction's would; a trap raised in one half is the instruction's, with
 * EXC_SUM the OR of both halves' reports, and both halves are always
 * computed and written [IDS p. 280; ev8-isa.md section 5]. Compiled into
 * alphabox_fptest too (test/fp), against its shim.
 **/
#include "AlphaCPU.hpp"
#include "cpu_debug.hpp"

#include <cmath>

namespace {

/// The qualifier classes of ev8-isa.md 3.4 [decision; P-EST agreed].
enum PClass { P_NONE, P_ARITH, P_CMP, P_EST, P_CVT, P_BIT };

PClass paired_class(u32 fnc) {
  switch (fnc) {
  case 0x00: // PMUL
  case 0x01: // PMULL
  case 0x02: // PMULH
  case 0x05: // PMULLN
  case 0x06: // PMULHN
  case 0x10: // PADD
  case 0x11: // PCADD
  case 0x12: // PHADD
  case 0x14: // PSUB
  case 0x15: // PSUBC
  case 0x16: // PHSUB
  case 0x1f: // PHSUBR
    return P_ARITH;
  case 0x08: // PARCPL: 08 (agreed with nada), the IDS's table gives 0A
  case 0x09: // PARCPLH
  case 0x0a: // PARCPLL
  case 0x0c: // PARSQRT
  case 0x0d: // PARSQRTH
  case 0x0e: // PARSQRTL
    return P_EST;
  case 0x28: // PCMPEQ
  case 0x29: // PCMPNEQ
  case 0x2a: // PCMPUN
  case 0x2c: // PCMPLT
  case 0x2d: // PCMPLE
  case 0x2e: // PFMAX
  case 0x2f: // PFMIN
    return P_CMP;
  case 0x39: // PCVTFI
    return P_CVT;
  case 0x18: // PMOVLL
  case 0x19: // PMOVLH
  case 0x1a: // PMOVHL
  case 0x1b: // PMOVHH
  case 0x20: // PCPYS
  case 0x21: // PCPYSN
  case 0x22: // PCPYSE
  case 0x30: // PCVTSP
  case 0x3a: // PEXTL
  case 0x3c: // PEXTH
    return P_BIT;
  default:
    return P_NONE;
  }
}

/// The 11-bit function <15:5> = Trp << 8 | Rnd << 6 | Fnc, the plain form
/// at Rnd = 10 (0x080 | Fnc) [inference]. Trp 000 none, 001 /U (/V), 101
/// /SU (/SV), 111 /SUI (/SVI). Every combination a class does not accept is
/// an unused code, OPCDEC (opcode 07 was reserved in Version 5, ARM p. 803)
/// [decision], but that the estimates take every Rnd alike.
bool paired_valid(u32 fn) {
  const u32 trp = (fn >> 8) & 7, rnd = (fn >> 6) & 3;
  switch (paired_class(fn & 0x3f)) {
  case P_ARITH: // ADDS's: /C, /M, none, /D with none, /U, /SU, /SUI
  case P_CVT:   // CVTTQ's: the same with /V, /SV, /SVI
    return trp == 0 || trp == 1 || trp == 5 || trp == 7;
  case P_EST: // none, /U, /SU; Rnd ignored (chopped, note 1)
    return trp == 0 || trp == 1 || trp == 5;
  case P_CMP: // CMPTEQ's: none, /SU
    return rnd == 2 && (trp == 0 || trp == 5);
  case P_BIT: // CPYS's: none
    return rnd == 2 && trp == 0;
  default:
    return false;
  }
}

inline u32 hi_of(u64 x) { return (u32)(x >> 32); }
inline u32 lo_of(u64 x) { return (u32)x; }
inline u64 pair(u32 h, u32 l) { return ((u64)h << 32) | l; }

const u32 S_ONE = 0x3f800000; // IEEE 1.0, the compares' true (note 4)
const u32 S_INF = 0x7f800000;
const u32 S_CQNAN = 0xffc00000; // the canonical quiet NaN, stored as STS
const u64 ONE_T = U64(0x3ff0000000000000);

} // namespace

/// One half as the scalar ADDS or SUBS.
u32 CAlphaCPU::paired_add(u32 a, u32 b, u32 ins, bool sub) {
  return ieee_sts(ieee_fadd(ieee_lds(a), ieee_lds(b), ins, DT_S, sub));
}

/// One half as the scalar MULS.
u32 CAlphaCPU::paired_mul(u32 a, u32 b, u32 ins) {
  return ieee_sts(ieee_fmul(ieee_lds(a), ieee_lds(b), ins, DT_S));
}

/// One half of a compare: exceptions as CMPTxx raises them, NEQ and UN as
/// EQ (a signalling NaN only), LT and LE any NaN [ev8-isa.md 3.4; HRM Table
/// A-11]; the half IEEE 1.0 or a true zero. op: 0 EQ, 1 NEQ, 2 UN, 3 LT,
/// 4 LE.
u32 CAlphaCPU::paired_cmp(u32 a, u32 b, u32 ins, int op) {
  const s32 c = ieee_fcmp(ieee_lds(a), ieee_lds(b), ins, op >= 3 ? 1 : 0);
  bool t;
  switch (op) {
  case 0:
    t = c == 0;
    break;
  case 1: // true when unordered
    t = c != 0;
    break;
  case 2:
    t = c == 2;
    break;
  case 3:
    t = c == -1;
    break;
  default:
    t = c == -1 || c == 0;
    break;
  }
  return t ? S_ONE : 0;
}

/// PFMAX, PFMIN [decision, ev8-isa.md 3.4]: (faX < fbX) ? fbX : faX and the
/// reverse, comparing as PCMPLT does. An unordered pair is an invalid
/// operation (the compare raised it), and the half gets what the scalar
/// rules give a NaN operand: Fb's quieted, else Fa's [inference: ADDS's,
/// ieee_fadd].
u32 CAlphaCPU::paired_maxmin(u32 a, u32 b, u32 ins, bool max) {
  const s32 c = ieee_fcmp(ieee_lds(a), ieee_lds(b), ins, 1);
  if (c == 2) {
    const bool b_nan = (b & 0x7f800000) == 0x7f800000 && (b & 0x007fffff);
    return (b_nan ? b : a) | 0x00400000;
  }
  if (max)
    return c == -1 ? b : a;
  return c == -1 ? a : b;
}

/// PCVTFI's half [decision, ev8-isa.md 3.4]: rounded by Rnd to a 32-bit
/// two's complement integer; out of range its low 32 bits, the integer
/// overflow reported as CVTTQ reports it at 64 bits.
u32 CAlphaCPU::paired_cvt(u32 b, u32 ins) {
  return (u32)ieee_cvtfi_n(ieee_lds(b), ins, true);
}

/// The estimates [decision, ev8-isa.md 3.4]: the exact reciprocal (root
/// false) or reciprocal square root, chopped to 14 fraction bits (fraction
/// <8:0> of the single zero), whatever Rnd is (note 1); the exceptions the
/// scalar DIVS of 1.0 by the operand raises (SQRTS for the root), except
/// INE, which an estimate never sets.
u32 CAlphaCPU::paired_est(u32 b, u32 ins, bool root) {
  const u32 ins_c = ins & ~(u32)I_FRND; // chopped
  const u64 op = ieee_lds(b);
  if (!root) {
    // DIVS/C of 1.0 by the operand, then the fraction chopped further:
    // truncating the truncated quotient is truncating the exact one.
    m_fp_no_ine = true;
    u64 r = ieee_fdiv(ONE_T, op, ins_c, DT_S);
    m_fp_no_ine = false;
    const u32 e = FPR_GETEXP(r);
    if (e != 0 && e != FPR_NAN)
      r &= ~(U64(0x1ff) << 29); // single fraction <8:0>, register <37:29>
    return ieee_sts(r);
  }
  // The root: SQRTS's operand checks and special cases (ieee_sqrt), then
  // the exact estimate by integers.
  UFP x;
  const int t = ieee_unpack(op, &x, ins_c);
  if (t == UFT_NAN)
    return b | 0x00400000; // quieted, as SQRTS returns it
  if (t == UFT_ZERO)
    // 1/sqrt(+-0): the infinity of the zero's sign, as 1.0 / SQRTS(+-0);
    // SQRTS raises nothing for a zero, so neither does the estimate
    // [inference: the sheet gives this case no value].
    return (x.sign ? 0x80000000 : 0) | S_INF;
  if (x.sign) {
    ieee_trap(TRAP_INV, 1, FPCR_INVD, ins_c);
    return S_CQNAN;
  }
  if (t == UFT_INF)
    return 0; // 1/sqrt(+inf) = +0, exact
  // A denormal operand has taken its unmaskable trap; the result is
  // UNPREDICTABLE, here the estimate of a zero. (In register format an S
  // denormal reads as a T denormal, far below the single's range.)
  if (t == UFT_DENORM)
    return S_INF;
  // x = m * 2^E: the 24 significant bits of a single (a denormal's, whose
  // trap ieee_unpack has taken, normalized) are frac<63:40>.
  const u64 m = x.frac >> 40;
  const s32 E = x.exp - T_BIAS - 23;
  // 1/sqrt(x) = q * 2^F with q in [2^14, 2^15) the chopped significand:
  // q^2 * m <= 2^G < (q + 1)^2 * m for G = -(2F + E), so q = floor(sqrt(
  // floor(2^G / m))). G + E must be even; G = 52 or 53 puts q in range, but
  // for m = 2^23 and G = 53, where q = 2^15 and G = 51 is the one.
  s32 G = (E & 1) ? 53 : 52;
  u64 q = 0;
  for (;;) {
    const u64 n = (U64(1) << G) / m;
    q = (u64)sqrt((double)n);
    while (q * q > n)
      q--;
    while ((q + 1) * (q + 1) <= n)
      q++;
    if (q < (U64(1) << 15))
      break;
    G -= 2;
  }
  const s32 F = -(G + E) / 2;
  const u32 bexp = (u32)(F + 14 + 127); // 1/sqrt(x) is in (2^-64, 2^64)
  return (bexp << 23) | (u32)((q - (U64(1) << 14)) << 9);
}

/**
 * Opcode 07 on a row with CPU_EXT_EV8, for both engines: false for a code
 * the sheet does not assign (the caller takes OPCDEC, before FEN, as for
 * the scalar operates); else the instruction, FEN first when the FPU is
 * disabled. Fc = F31 discards the result (the exceptions still happen).
 **/
bool CAlphaCPU::paired_operate(u32 ins) {
  const u32 fn = (ins >> 5) & 0x7ff;
  if (!paired_valid(fn))
    return false;
  if (state.fpen == 0) {
    GO_PAL(FEN);
    return true;
  }
  state.exc_sum = 0; // as FPSTART
  const u64 fa = state.f[FREG_1], fb = state.f[FREG_2];
  const u32 aH = hi_of(fa), aL = lo_of(fa), bH = hi_of(fb), bL = lo_of(fb);
  u64 r;
  switch (fn & 0x3f) {
  case 0x00: // PMUL
    r = pair(paired_mul(aH, bH, ins), paired_mul(aL, bL, ins));
    break;
  case 0x01: // PMULL
    r = pair(paired_mul(aH, bL, ins), paired_mul(aL, bL, ins));
    break;
  case 0x02: // PMULH
    r = pair(paired_mul(aH, bH, ins), paired_mul(aL, bH, ins));
    break;
  case 0x05: // PMULLN: fcL negated after rounding (note 8); a NaN's sign
             // too [inference]
    r = pair(paired_mul(aH, bL, ins), paired_mul(aL, bL, ins) ^ 0x80000000);
    break;
  case 0x06: // PMULHN
    r = pair(paired_mul(aH, bH, ins) ^ 0x80000000, paired_mul(aL, bH, ins));
    break;
  case 0x08: // PARCPL
    r = pair(paired_est(bH, ins, false), paired_est(bL, ins, false));
    break;
  case 0x09: // PARCPLH: the other half 0, neither checked nor raising
             // anything (note 1)
    r = pair(paired_est(bH, ins, false), 0);
    break;
  case 0x0a: // PARCPLL
    r = pair(0, paired_est(bL, ins, false));
    break;
  case 0x0c: // PARSQRT
    r = pair(paired_est(bH, ins, true), paired_est(bL, ins, true));
    break;
  case 0x0d: // PARSQRTH
    r = pair(paired_est(bH, ins, true), 0);
    break;
  case 0x0e: // PARSQRTL
    r = pair(0, paired_est(bL, ins, true));
    break;
  case 0x10: // PADD
    r = pair(paired_add(aH, bH, ins, false), paired_add(aL, bL, ins, false));
    break;
  case 0x11: // PCADD
    r = pair(paired_add(aH, bL, ins, false), paired_add(aL, bH, ins, false));
    break;
  case 0x12: // PHADD
    r = pair(paired_add(aH, aL, ins, false), paired_add(bH, bL, ins, false));
    break;
  case 0x14: // PSUB
    r = pair(paired_add(aH, bH, ins, true), paired_add(aL, bL, ins, true));
    break;
  case 0x15: // PSUBC
    r = pair(paired_add(aH, bL, ins, true), paired_add(aL, bH, ins, true));
    break;
  case 0x16: // PHSUB
    r = pair(paired_add(aH, aL, ins, true), paired_add(bH, bL, ins, true));
    break;
  case 0x1f: // PHSUBR
    r = pair(paired_add(aL, aH, ins, true), paired_add(bL, bH, ins, true));
    break;
  case 0x18: // PMOVLL
    r = pair(aL, bL);
    break;
  case 0x19: // PMOVLH
    r = pair(aL, bH);
    break;
  case 0x1a: // PMOVHL
    r = pair(aH, bL);
    break;
  case 0x1b: // PMOVHH
    r = pair(aH, bH);
    break;
  case 0x20: // PCPYS: each half Fa's sign, Fb's exponent and fraction
    r = (fa & U64(0x8000000080000000)) | (fb & U64(0x7fffffff7fffffff));
    break;
  case 0x21: // PCPYSN
    r = (~fa & U64(0x8000000080000000)) | (fb & U64(0x7fffffff7fffffff));
    break;
  case 0x22: // PCPYSE
    r = (fa & U64(0xff800000ff800000)) | (fb & U64(0x007fffff007fffff));
    break;
  case 0x28:   // PCMPEQ
  case 0x29:   // PCMPNEQ
  case 0x2a:   // PCMPUN
  case 0x2c:   // PCMPLT
  case 0x2d: { // PCMPLE
    const int op =
        (fn & 0x3f) <= 0x2a ? (int)(fn & 0x3f) - 0x28 : (int)(fn & 0x3f) - 0x29;
    r = pair(paired_cmp(aH, bH, ins, op), paired_cmp(aL, bL, ins, op));
    break;
  }
  case 0x2e: // PFMAX
    r = pair(paired_maxmin(aH, bH, ins, true),
             paired_maxmin(aL, bL, ins, true));
    break;
  case 0x2f: // PFMIN
    r = pair(paired_maxmin(aH, bH, ins, false),
             paired_maxmin(aL, bL, ins, false));
    break;
  case 0x30: // PCVTSP: two register-format singles packed as STS packs them,
             // no checking (note 5)
    r = pair(ieee_sts(fa), ieee_sts(fb));
    break;
  case 0x39: // PCVTFI
    r = pair(paired_cvt(bH, ins), paired_cvt(bL, ins));
    break;
  case 0x3a: // PEXTL: Fc <- fbL in register format, as LDS maps it (note 6)
    r = ieee_lds(bL);
    break;
  default: // 0x3c PEXTH
    r = ieee_lds(bH);
    break;
  }
  if (FREG_3 != 31)
    state.f[FREG_3] = r;
  return true;
}
