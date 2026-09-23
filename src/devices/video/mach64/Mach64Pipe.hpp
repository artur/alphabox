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
 * The Mach64 3D pixel pipeline's pieces, shared by the GT's trapezoids and
 * the Rage Pro's setup engine: texel formats (DP_SCALE_PIX_WIDTH), blend
 * factors (ALPHA_BLND_SRC/DST), the Z test (Z_CNTL) and destination pixels
 * (DP_DST_PIX_WIDTH), all through ARGB 8888.
 **/

#ifndef MACH64PIPE_HPP
#define MACH64PIPE_HPP

#include "datatypes.hpp"

namespace mach64 {

/// Sign-extend the low `bits` of a register.
inline s32 sext(u32 v, int bits) {
  return s32(v << (32 - bits)) >> (32 - bits);
}

inline u32 clamp8(s32 v) { return v < 0 ? 0 : v > 255 ? 255 : u32(v); }

/// A texel as ARGB 8888 from VRAM, `fmt` one of DP_SCALE_PIX_WIDTH's.
inline u32 pipe_texel(const u8 *vram, u32 mask, u32 addr, int fmt,
                      const u8 *palette, bool pal8bit) {
  auto x5 = [](u32 v) { return (v << 3) | (v >> 2); };
  auto x6 = [](u32 v) { return (v << 2) | (v >> 4); };
  auto x4 = [](u32 v) { return (v << 4) | v; };
  switch (fmt) {
  case 2: { // 8 bpp through the palette
    const u8 *c = &palette[3 * vram[addr & mask]];
    const u32 rr = pal8bit ? c[0] : (c[0] << 2) | (c[0] >> 4);
    const u32 g = pal8bit ? c[1] : (c[1] << 2) | (c[1] >> 4);
    const u32 b = pal8bit ? c[2] : (c[2] << 2) | (c[2] >> 4);
    return 0xff000000u | (rr << 16) | (g << 8) | b;
  }
  case 3: { // aRGB 1555
    const u32 v = vram[addr & mask] | (vram[(addr + 1) & mask] << 8);
    return ((v & 0x8000) ? 0xff000000u : 0) | (x5((v >> 10) & 31) << 16) |
           (x5((v >> 5) & 31) << 8) | x5(v & 31);
  }
  case 4: { // RGB 565
    const u32 v = vram[addr & mask] | (vram[(addr + 1) & mask] << 8);
    return 0xff000000u | (x5(v >> 11) << 16) | (x6((v >> 5) & 63) << 8) |
           x5(v & 31);
  }
  case 6: // aRGB 8888
    return vram[addr & mask] | (vram[(addr + 1) & mask] << 8) |
           (vram[(addr + 2) & mask] << 16) |
           (u32(vram[(addr + 3) & mask]) << 24);
  case 7: { // RGB 332
    const u32 v = vram[addr & mask];
    const u32 rr = (v >> 5) & 7, g = (v >> 2) & 7, b = v & 3;
    return 0xff000000u | (((rr << 5) | (rr << 2) | (rr >> 1)) << 16) |
           (((g << 5) | (g << 2) | (g >> 1)) << 8) | (b * 0x55);
  }
  case 15: { // aRGB 4444
    const u32 v = vram[addr & mask] | (vram[(addr + 1) & mask] << 8);
    return (x4(v >> 12) << 24) | (x4((v >> 8) & 15) << 16) |
           (x4((v >> 4) & 15) << 8) | x4(v & 15);
  }
  }
  return 0xffff00ffu; // an unmodelled format shows as magenta
}

/// The bits of pipe_texel's ARGB that the texel itself carries (the rest
/// are its high bits repeated): what a texture colour key is compared on.
inline u32 pipe_texel_key_bits(int fmt) {
  switch (fmt) {
  case 3:
    return 0x00f8f8f8u; // 1555
  case 4:
    return 0x00f8fcf8u; // 565
  case 7:
    return 0x00e0e0c0u; // 332
  case 15:
    return 0x00f0f0f0u; // 4444
  }
  return 0x00ffffffu; // 8888, palette
}

inline int pipe_texel_bytes(int fmt) {
  switch (fmt) {
  case 2:
  case 7:
    return 1;
  case 6:
    return 4;
  default:
    return 2;
  }
}

/// Blend factor `f` (ALPHA_BLND_SRC/DST encoding) applied to `c`, with the
/// other operand `o` for factors 2 and 3 and the source alpha `as`.
inline u32 pipe_factor(int f, u32 c, u32 o, u32 as) {
  u32 k[3];
  switch (f) {
  case 0:
    return 0;
  case 1:
    return c & 0xffffff;
  case 2: // the other colour, channel by channel
  case 3:
    for (int i = 0; i < 3; i++) {
      const u32 ov = (o >> (8 * i)) & 0xff;
      k[i] = f == 2 ? ov : 255 - ov;
    }
    break;
  case 4:
  case 5:
    k[0] = k[1] = k[2] = f == 4 ? as : 255 - as;
    break;
  default:
    return c & 0xffffff;
  }
  u32 out = 0;
  for (int i = 0; i < 3; i++)
    out |= (((c >> (8 * i)) & 0xff) * k[i] / 255) << (8 * i);
  return out;
}

inline u32 pipe_add_sat(u32 a, u32 b) {
  u32 out = 0;
  for (int i = 0; i < 3; i++) {
    const u32 v = ((a >> (8 * i)) & 0xff) + ((b >> (8 * i)) & 0xff);
    out |= (v > 255 ? 255 : v) << (8 * i);
  }
  return out;
}

/// Z test: true when the new pixel passes against the stored one.
inline bool pipe_z_pass(int test, u32 znew, u32 zold) {
  switch (test) {
  case 0:
    return false;
  case 1:
    return znew < zold;
  case 2:
    return znew <= zold;
  case 3:
    return znew == zold;
  case 4:
    return znew >= zold;
  case 5:
    return znew > zold;
  case 6:
    return znew != zold;
  }
  return true;
}

/// ARGB 8888 into a destination pixel of DP_DST_PIX_WIDTH `fmt`.
inline u32 pipe_pack(u32 argb, int fmt) {
  const u32 rr = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
  switch (fmt) {
  case 3:
    return ((argb >> 31) << 15) | ((rr >> 3) << 10) | ((g >> 3) << 5) |
           (b >> 3);
  case 4:
    return ((rr >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  case 6:
    return argb;
  case 7:
    return (rr & 0xe0) | ((g >> 3) & 0x1c) | (b >> 6);
  default: // 8 bpp pseudocolour: nothing better than the blue channel's index
    return b;
  }
}

/// A destination pixel of `fmt` as ARGB 8888, for blending against.
inline u32 pipe_unpack(u32 v, int fmt) {
  auto x5 = [](u32 c) { return (c << 3) | (c >> 2); };
  auto x6 = [](u32 c) { return (c << 2) | (c >> 4); };
  switch (fmt) {
  case 3:
    return (x5((v >> 10) & 31) << 16) | (x5((v >> 5) & 31) << 8) | x5(v & 31);
  case 4:
    return (x5((v >> 11) & 31) << 16) | (x6((v >> 5) & 63) << 8) | x5(v & 31);
  case 6:
    return v & 0xffffff;
  case 7: {
    const u32 rr = (v >> 5) & 7, g = (v >> 2) & 7, b = v & 3;
    return (((rr << 5) | (rr << 2) | (rr >> 1)) << 16) |
           (((g << 5) | (g << 2) | (g >> 1)) << 8) | (b * 0x55);
  }
  }
  return v & 0xff;
}

} // namespace mach64

#endif
