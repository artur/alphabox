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
 * The 21464's opcode 1C short-vector SIMD set (Ev8Simd.hpp). Every
 * operation is docs/platforms/ev8-isa.md 3.3's; the IDS names the
 * instructions and describes the multimedia unit's datapath [21464 IDS Rev
 * 1.1k, PDF pp. 70-72, 212-221] but states no result, so every result is
 * the sheet's proposal (D8), and what the sheet marks [inference] or
 * [decision] is marked here too. x.b[i], x.w[i], x.l[i] are byte, word and
 * longword i, i = 0 the least significant; SAT clamps to the element's
 * range.
 **/
#include "StdAfx.hpp"

#include "Ev8Simd.hpp"

namespace {

/// The element shapes of the "zz" blocks: zz = 00 UB8, 01 UW4, 10 SB8,
/// 11 SW4, bit 0 the wider element and bit 1 the signed form [inference,
/// agreed with nada, 2026-10-24]; the L2 codes are x4 (U) and x6 (S).
struct Shape {
  int w; ///< element width in bits
  bool signed_;
};

Shape zz(u32 f) { return {(f & 1) ? 16 : 8, (f & 2) != 0}; }
Shape l2(u32 f) { return {32, (f & 2) != 0}; }

inline u64 mask_of(int w) { return w == 64 ? ~U64(0) : (U64(1) << w) - 1; }

/// Element i of x, zero- or sign-extended.
inline s64 el(u64 x, int w, int i, bool sgn) {
  const u64 v = (x >> (w * i)) & mask_of(w);
  if (sgn && (v >> (w - 1)))
    return (s64)(v | ~mask_of(w));
  return (s64)v;
}

inline s64 sat(s64 v, int w, bool sgn) {
  const s64 lo = sgn ? -(((s64)1) << (w - 1)) : 0;
  const s64 hi = sgn ? (((s64)1) << (w - 1)) - 1 : (((s64)1) << w) - 1;
  return v < lo ? lo : (v > hi ? hi : v);
}

inline u64 put(u64 r, s64 v, int w, int i) {
  return r | (((u64)v & mask_of(w)) << (w * i));
}

/// VADD, VSUB: Rc.e[i] <- SAT(Rav.e[i] +/- Rbv.e[i]) ("Saturation is
/// required for the PADD and PSUB operations" [IDS p. 219]).
u64 vaddsub(u64 a, u64 b, Shape s, bool sub) {
  u64 r = 0;
  for (int i = 0; i < 64 / s.w; i++) {
    const s64 x = el(a, s.w, i, s.signed_), y = el(b, s.w, i, s.signed_);
    r = put(r, sat(sub ? x - y : x + y, s.w, s.signed_), s.w, i);
  }
  return r;
}

/// VMINMAX [inference, IDS pp. 215-216, 219]: the minimum and maximum of
/// Rb's running pair and every element of Ra. [decision]: Rb<31:0> holds the
/// running minimum and Rb<63:32> the running maximum, each an element-size
/// value in the low bits of its longword; the results sign-extended to 32
/// bits for the S forms, zero-extended for the U forms.
u64 vminmax(u64 a, u64 b, Shape s) {
  s64 mn = el(b, s.w, 0, s.signed_);
  s64 mx = el(b >> 32, s.w, 0, s.signed_);
  for (int i = 0; i < 64 / s.w; i++) {
    const s64 x = el(a, s.w, i, s.signed_);
    mn = x < mn ? x : mn;
    mx = x > mx ? x : mx;
  }
  return ((u64)mn & 0xffffffff) | ((u64)mx << 32);
}

/// The tree ops [inference, IDS pp. 213-218]: each element pair through the
/// ALU or the multipliers, the eight byte or four word results summed by the
/// tree adder; the sum is exact and written as a 64-bit two's-complement
/// integer [decision]. TMULUS: Ra's elements unsigned, Rb's signed
/// [decision, the name's order].
u64 tree(u32 f, u64 a, u64 b) {
  const Shape s = zz(f);
  const u32 op = (f >> 2) & 0x1f; // 0x40 >> 2 = 0x10 TADD ... 0x54 >> 2
  s64 sum = 0;
  for (int i = 0; i < 64 / s.w; i++) {
    bool sa = s.signed_, sb = s.signed_;
    if (op == 0x15) // TMULUSB8 (54), TMULUSW4 (55): zz's size bit only
      sa = false, sb = true;
    const s64 x = el(a, s.w, i, sa), y = el(b, s.w, i, sb);
    switch (op) {
    case 0x10: // TADD
      sum += x + y;
      break;
    case 0x11: // TSUB
      sum += x - y;
      break;
    case 0x12: // TABSERR: the positive one of a - b and b - a [IDS p. 215]
      sum += x > y ? x - y : y - x;
      break;
    case 0x13: // TSQERR: (a - b)^2 through the multipliers
      sum += (x - y) * (x - y);
      break;
    default: // TMUL, TMULUS
      sum += x * y;
      break;
    }
  }
  return (u64)sum;
}

/// The shifts [decision]: one count for all elements, from Rbv or the
/// literal, modulo the element width.
u64 shift(u32 f, u64 a, u64 b) {
  int w;
  switch (f) {
  case 0x60:
  case 0x62:
  case 0x68:
    w = 8;
    break;
  case 0x61:
  case 0x63:
  case 0x69:
    w = 16;
    break;
  default: // 0x64, 0x66, 0x6c
    w = 32;
    break;
  }
  const int n = (int)(b & (u64)(w - 1));
  const bool left = f >= 0x68;
  const bool arith = f == 0x62 || f == 0x63 || f == 0x66;
  u64 r = 0;
  for (int i = 0; i < 64 / w; i++) {
    const s64 x = el(a, w, i, arith);
    r = put(r, left ? (s64)((u64)x << n) : (x >> n), w, i);
  }
  return r;
}

/// PERMB8 [inference, IDS pp. 220-221; the selector numbering, the figure's
/// order, decision]: byte i of Rc is chosen by s = Rbv<4i+3:4i>: 0-7
/// Rav.b[s]; 8-11 Rbv.b[s-4], Rb's high longword; 12 every bit the sign of
/// Rc.b[i-1] (0 for byte 0); 13 reserved, 0 in Alphabox; 14 0xFF; 15 0x00.
u64 permb8(u64 a, u64 b) {
  u64 r = 0;
  u64 prev = 0;
  for (int i = 0; i < 8; i++) {
    const u32 s = (u32)(b >> (4 * i)) & 0xf;
    u64 v;
    if (s <= 7)
      v = (a >> (8 * s)) & 0xff;
    else if (s <= 11)
      v = (b >> (8 * (s - 4))) & 0xff;
    else if (s == 12)
      v = (i && (prev & 0x80)) ? 0xff : 0;
    else if (s == 14)
      v = 0xff;
    else // 13 (reserved), 15
      v = 0;
    r |= v << (8 * i);
    prev = v;
  }
  return r;
}

} // namespace

namespace ev8simd {

bool assigned(u32 f) {
  switch (f) {
  case 0x04: // CMPWGE
  case 0x05: // CMPLGE
  case 0x08: // PERMB8
  case 0x09: // GPKBLB4
  case 0x14: // VADDUL2
  case 0x16: // VADDSL2
  case 0x1c: // VSUBUL2
  case 0x1e: // VSUBSL2
  case 0x24: // VMINMAXUL2
  case 0x26: // VMINMAXSL2
  case 0x54: // TMULUSB8
  case 0x55: // TMULUSW4
  case 0x59: // VMULLUW4
  case 0x5b: // VMULHUW4
  case 0x60: // VSRB8
  case 0x61: // VSRW4
  case 0x62: // VSRAB8
  case 0x63: // VSRAW4
  case 0x64: // VSRL2
  case 0x66: // VSRAL2
  case 0x68: // VSLB8
  case 0x69: // VSLW4
  case 0x6c: // VSLL2
    return true;
  default:
    // The zz blocks: VADD 10-13, VSUB 18-1B, VMINMAX 20-23, the packs and
    // unpacks 28-2F, the tree ops 40-53.
    return (f >= 0x10 && f <= 0x13) || (f >= 0x18 && f <= 0x1b) ||
           (f >= 0x20 && f <= 0x23) || (f >= 0x28 && f <= 0x2f) ||
           (f >= 0x40 && f <= 0x53);
  }
}

u64 exec(u32 f, u64 a, u64 b) {
  switch (f) {
  case 0x04: { // CMPWGE: Rc<2i+1:2i> <- 11 if Rav.w[i] >= Rbv.w[i]
               // unsigned, else 00; Rc<63:8> <- 0 [IDS p. 219, inference;
               // two bits per word, a byte mask for ZAP, decision]
    u64 r = 0;
    for (int i = 0; i < 4; i++)
      if (el(a, 16, i, false) >= el(b, 16, i, false))
        r |= U64(3) << (2 * i);
    return r;
  }
  case 0x05: { // CMPLGE: Rc<4i+3:4i> <- 1111 or 0000 [decision, by analogy]
    u64 r = 0;
    for (int i = 0; i < 2; i++)
      if (el(a, 32, i, false) >= el(b, 32, i, false))
        r |= U64(0xf) << (4 * i);
    return r;
  }
  case 0x08:
    return permb8(a, b);
  case 0x09: { // GPKBLB4: Rav.l[0], Rav.l[1], Rbv.l[0], Rbv.l[1] as signed
               // longwords clamped to 0..255 in Rc.b[0..3]; Rc<63:32> <- 0
               // [decision: only the name, IDS p. 70]
    const s64 v[4] = {el(a, 32, 0, true), el(a, 32, 1, true),
                      el(b, 32, 0, true), el(b, 32, 1, true)};
    u64 r = 0;
    for (int i = 0; i < 4; i++)
      r = put(r, v[i] < 0 ? 0 : (v[i] > 255 ? 255 : v[i]), 8, i);
    return r;
  }
  case 0x28:   // PKUWB8
  case 0x29:   // PKULW4
  case 0x2a:   // PKSWB8
  case 0x2b: { // PKSLW4: Rc's low half from Ra, the high half from Rb
               // [IDS p. 220, inference; Ra low, decision]
    const int w = (f & 1) ? 32 : 16;
    const bool sgn = (f & 2) != 0;
    const int n = 64 / w;
    u64 r = 0;
    for (int i = 0; i < n; i++) {
      r = put(r, sat(el(a, w, i, sgn), w / 2, sgn), w / 2, i);
      r = put(r, sat(el(b, w, i, sgn), w / 2, sgn), w / 2, n + i);
    }
    return r;
  }
  case 0x2c:   // UPKUBW4
  case 0x2d:   // UPKUWL2
  case 0x2e:   // UPKSBW4
  case 0x2f: { // UPKSWL2: Rbv's low elements widened, zero- or sign-
               // extended; Ra is not read, as MVI's UNPKBW [decision]
    const int w = (f & 1) ? 16 : 8;
    const bool sgn = (f & 2) != 0;
    u64 r = 0;
    for (int i = 0; i < 32 / w; i++)
      r = put(r, el(b, w, i, sgn), 2 * w, i);
    return r;
  }
  case 0x59:   // VMULLUW4: the product's <15:0>
  case 0x5b: { // VMULHUW4: its <31:16>, unsigned [IDS p. 218, inference]
    const int sh = f == 0x5b ? 16 : 0;
    u64 r = 0;
    for (int i = 0; i < 4; i++)
      r = put(r, (s64)((u64)(el(a, 16, i, false) * el(b, 16, i, false)) >> sh),
              16, i);
    return r;
  }
  case 0x14:
  case 0x16:
    return vaddsub(a, b, l2(f), false);
  case 0x1c:
  case 0x1e:
    return vaddsub(a, b, l2(f), true);
  case 0x24:
  case 0x26:
    return vminmax(a, b, l2(f));
  default:
    break;
  }
  if (f >= 0x10 && f <= 0x13)
    return vaddsub(a, b, zz(f), false);
  if (f >= 0x18 && f <= 0x1b)
    return vaddsub(a, b, zz(f), true);
  if (f >= 0x20 && f <= 0x23)
    return vminmax(a, b, zz(f));
  if (f >= 0x40 && f <= 0x55)
    return tree(f, a, b);
  return shift(f, a, b); // 60-6C
}

} // namespace ev8simd
