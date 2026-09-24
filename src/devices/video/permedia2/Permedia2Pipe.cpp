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
 * The Permedia 2's 3D units (Programmer's Reference 4.6-4.13): the
 * localbuffer with its depth and stencil tests, the texture address and
 * read units with the texel look-up table and the YUV unit's chroma test,
 * texture application, fog and alpha blending, and the parameter DDAs
 * with their sub-pixel correction.
 *
 * Internal colours are the chip's: red in bits 7..0, then green, blue,
 * alpha in 31..24. Formats are Table 3-1's: from the least significant
 * bit, red, green, blue, alpha in BGR order, blue, green, red, alpha in
 * RGB order, the "back" variants a byte or a halfword up.
 **/

#include "Permedia2.hpp"

#include <cmath>

using namespace permedia2;

static inline s32 sext(u32 v, int bits) {
  return s32(v << (32 - bits)) >> (32 - bits);
}

static inline u32 clamp255(s32 v) { return u32(v < 0 ? 0 : v > 255 ? 255 : v); }

/// Depth values: the U register's integer and the top 11 bits of L as the
/// fraction (17.11). U is read whole: a delta set-up's start, extrapolated
/// to the dominant edge, can lie past 65535, and must not wrap.
static inline s64 depth_value(u32 u, u32 l) {
  return (s64(s32(u)) << 11) | (l >> 21);
}

// --- the parameter DDAs ----------------------------------------------------

/**
 * Sub-pixel correction (4.4.7): step every interpolated parameter from
 * where the dominant edge crosses the scanline to the first pixel's centre.
 **/
void CPermedia2::dda_correct_span() {
  auto &g = r.g;
  if (!(g.render & RENDER_SUBPIXEL_CORRECTION) || g.prim != PRIM_TRAPEZOID)
    return;
  const s64 d = ((s64(g.x) << 32) + 0x80000000ll - g.span_xdom) >> 16; // +X
  // Rounded to nearest: a coordinate that lands exactly on a texel edge
  // must not fall a bit short of it.
  auto step = [d](s64 per_x) { return (per_x * d + 0x8000) >> 16; };
  g.cr += s32(step(s32(G(T_DR_DX))));
  g.cg += s32(step(s32(G(T_DG_DX))));
  g.cb += s32(step(s32(G(T_DB_DX))));
  g.z += step(depth_value(G(T_DZ_DX_U), G(T_DZ_DX_L)));
  g.s += step(s32(G(T_DS_DX)));
  g.t += step(s32(G(T_DT_DX)));
  g.q += step(s32(G(T_DQ_DX)));
  g.f += s32(step(s32(G(T_DF_DX))));
}

// --- the localbuffer: depth and stencil ------------------------------------

static bool compare(u32 func, u32 a, u32 b) {
  switch (func & 7) {
  case 0:
    return false;
  case 1:
    return a < b;
  case 2:
    return a == b;
  case 3:
    return a <= b;
  case 4:
    return a > b;
  case 5:
    return a != b;
  case 6:
    return a >= b;
  }
  return true;
}

/**
 * The stencil and depth tests (4.7) against the localbuffer, 16 bits a
 * pixel: depth from bit 0 (15 or 16 bits wide), a 1-bit stencil at bit 15
 * when LBReadFormat has one. The buffer is updated as the tests say when
 * LBWriteMode allows it. False when the fragment is rejected.
 **/
bool CPermedia2::depth_stencil(s32 x, s32 y) {
  auto &g = r.g;
  const u32 dm = G(T_DEPTH_MODE), sm = G(T_STENCIL_MODE);
  const u32 lbm = G(T_LB_READ_MODE);
  const s64 w = fb_width(lbm);
  const s64 base = G(T_LB_WINDOW_BASE) & 0xffffff;
  const s64 a = (lbm & (1u << 18)) ? base - y * w + x : base + y * w + x;
  const u32 addr = u32(a * 2) & vram_mask();
  const u32 lb = vram_read(addr, 2);
  const u32 fmt = G(T_LB_READ_FORMAT);
  const u32 dmask = (fmt & 3) == 3 ? 0x7fffu : 0xffffu;
  const bool has_stencil = ((fmt >> 2) & 3) == 3;

  // Depth.
  bool dpass = true;
  u32 newz = lb & dmask;
  if (dm & 1) {
    const s64 zi = g.z >> 11;
    u32 fz = u32(zi < 0 ? 0 : zi > s64(dmask) ? dmask : zi);
    switch ((dm >> 2) & 3) {
    case 2:
      fz = G(T_DEPTH) & dmask;
      break;
    case 1:
    case 3:
      gp_unimplemented("localbuffer copies");
      break;
    }
    dpass = compare(dm >> 4, fz, lb & dmask);
    if (dpass && (dm & 2))
      newz = fz;
  }

  // Stencil, one bit.
  bool spass = true;
  u32 news = has_stencil ? (lb >> 15) & 1 : 0;
  if ((sm & 1) && has_stencil) {
    const u32 sd = G(T_STENCIL_DATA);
    const u32 ref = sd & 1, cmask = (sd >> 8) & 1, wmask = (sd >> 16) & 1;
    const u32 src = (lb >> 15) & 1;
    spass = compare(sm >> 10, ref & cmask, src & cmask);
    const u32 op = !spass  ? (sm >> 7) & 7
                   : dpass ? (sm >> 1) & 7
                           : (sm >> 4) & 7;
    u32 v = src;
    switch (op) {
    case 1:
      v = 0;
      break;
    case 2:
      v = ref;
      break;
    case 3:
      v = 1;
      break;
    case 4:
      v = 0;
      break;
    case 5:
      v = src ^ 1;
      break;
    }
    news = (v & wmask) | (src & ~wmask & 1);
  }

  if (G(T_LB_WRITE_MODE) & 1) {
    u32 v = (lb & ~dmask & 0x7fff) | newz;
    if (has_stencil)
      v = (v & 0x7fff) | (news << 15);
    if (v != lb)
      vram_write(addr, 2, v);
  }
  return dpass && spass;
}

// --- texture ---------------------------------------------------------------

/**
 * A raw pixel of a Table 3-1 format to the internal colour. Components are
 * scaled to eight bits by repeating their bits, or shifted with zeros
 * below (AlphaBlendMode's conversion bits). A format without alpha reads
 * as opaque.
 **/
u32 CPermedia2::unpack_color(u32 raw, u32 fmt, bool rgb, bool no_alpha,
                             bool shift) const {
  // Widths of red, green, blue, alpha, and how far up the pixel sits.
  int rw, gw, bw, aw, up = 0;
  bool offset = false;
  switch (fmt) {
  case 0:
    rw = gw = bw = aw = 8;
    break;
  case 1:
    rw = gw = bw = 5, aw = 1;
    break;
  case 13:
    rw = gw = bw = 5, aw = 1, up = 16;
    break;
  case 2:
    rw = gw = bw = aw = 4;
    break;
  case 5:
    rw = gw = 3, bw = 2, aw = 0;
    break;
  case 6:
    rw = gw = 3, bw = 2, aw = 0, up = 8;
    break;
  case 9:
    rw = 2, gw = 3, bw = 2, aw = 1;
    break;
  case 10:
    rw = 2, gw = 3, bw = 2, aw = 1, up = 8;
    break;
  case 11:
    rw = 2, gw = 3, bw = 2, aw = 0, offset = true;
    break;
  case 12:
    rw = 2, gw = 3, bw = 2, aw = 0, up = 8, offset = true;
    break;
  case 16:
    rw = 5, gw = 6, bw = 5, aw = 0;
    break;
  case 17:
    rw = 5, gw = 6, bw = 5, aw = 0, up = 16;
    break;
  case 14: { // CI8: the index in every component
    const u32 i = raw & 0xff;
    return i | (i << 8) | (i << 16) | 0xff000000u;
  }
  case 15: {
    const u32 i = (raw & 0xf) * 0x11;
    return i | (i << 8) | (i << 16) | 0xff000000u;
  }
  default:
    return raw;
  }
  u32 v = raw >> up;
  if (offset)
    v = (v - 64) & 0x7f;
  auto field = [&](int width, int pos) -> u32 {
    if (!width)
      return 0;
    u32 c = (v >> pos) & ((1u << width) - 1);
    if (shift)
      return c << (8 - width);
    u32 out = c << (8 - width);
    for (int sh = width; sh < 8; sh += width)
      out |= out >> sh;
    return out & 0xff;
  };
  u32 rr, gg, bb;
  if (rgb) {
    bb = field(bw, 0);
    gg = field(gw, bw);
    rr = field(rw, bw + gw);
  } else {
    rr = field(rw, 0);
    gg = field(gw, rw);
    bb = field(bw, rw + gw);
  }
  const u32 aa = (no_alpha || !aw) ? 0xff : field(aw, rw + gw + bw);
  return rr | (gg << 8) | (bb << 16) | (aa << 24);
}

/// A coordinate wrapped as TextureReadMode says: clamp, repeat, mirror.
static s32 wrap(s32 c, s32 size, u32 mode) {
  switch (mode & 3) {
  case 1:
    return c & (size - 1);
  case 2: {
    const s32 m = c & (2 * size - 1);
    return m < size ? m : 2 * size - 1 - m;
  }
  }
  return c < 0 ? 0 : c >= size ? size - 1 : c;
}

/**
 * One texel as an internal colour: (u, v) wrapped to the map, addressed
 * by TextureMapFormat (base, width by partial products, origin, texel
 * size), converted by TextureDataFormat and, with TexelLUTMode on, looked
 * up in the texel LUT -- the index for colour-index maps, each component
 * for the others.
 **/
u32 CPermedia2::texel_fetch(s32 u, s32 v) {
  const u32 trm = G(T_TEXTURE_READ_MODE);
  const u32 tmf = G(T_TEXTURE_MAP_FORMAT);
  const u32 tdf = G(T_TEXTURE_DATA_FORMAT);
  u = wrap(u, 1 << ((trm >> 9) & 15), trm >> 1);
  v = wrap(v, 1 << ((trm >> 13) & 15), trm >> 3);
  if (tmf & (1u << 17))
    gp_unimplemented("sub-patched textures");
  const s64 w = fb_width(tmf);
  const s64 base = G(T_TEXTURE_BASE_ADDRESS) & 0xffffff;
  const s64 a = (tmf & (1u << 16)) ? base - v * w + u : base + v * w + u;
  u32 raw;
  switch ((tmf >> 19) & 7) {
  case 0:
    raw = vram_read(u32(a) & vram_mask(), 1);
    break;
  case 1:
    raw = vram_read(u32(a * 2) & vram_mask(), 2);
    break;
  case 3: // 4 bits: two texels a byte, the first in the low nibble
    raw = (vram_read(u32(a >> 1) & vram_mask(), 1) >> ((a & 1) * 4)) & 0xf;
    break;
  case 4:
    raw = vram_read(u32(a * 3) & vram_mask(), 3);
    break;
  default:
    raw = vram_read(u32(a * 4) & vram_mask(), 4);
    break;
  }
  const u32 fmt = (tdf & 15) | (((tdf >> 6) & 1) << 4);
  const bool lut = G(T_TEXEL_LUT_MODE) & 1;
  if (lut && (fmt == 14 || fmt == 15)) {
    const u32 idx =
        fmt == 15 ? ((G(T_TEXEL_LUT_INDEX) & 0xf0) | (raw & 15)) : raw & 0xff;
    return r.g.lut[idx];
  }
  u32 c = unpack_color(raw, fmt, (tdf >> 5) & 1, (tdf >> 4) & 1, false);
  if (lut) {
    c = (r.g.lut[c & 0xff] & 0xff) | (r.g.lut[(c >> 8) & 0xff] & 0xff00) |
        (r.g.lut[(c >> 16) & 0xff] & 0xff0000) | (c & 0xff000000u);
  }
  return c;
}

/// YUV (Y in bits 7..0, U, V) to RGB, the CCIR 601 equations.
static u32 yuv_to_rgb(u32 c) {
  const int y = int(c & 0xff) - 16, u = int((c >> 8) & 0xff) - 128,
            v = int((c >> 16) & 0xff) - 128;
  const int rr = (298 * y + 409 * v + 128) >> 8;
  const int gg = (298 * y - 100 * u - 208 * v + 128) >> 8;
  const int bb = (298 * y + 516 * u + 128) >> 8;
  return clamp255(rr) | (clamp255(gg) << 8) | (clamp255(bb) << 16) |
         (c & 0xff000000u);
}

/**
 * The texture colour for this fragment: S and T in texels with 20
 * fraction bits (the manual says 12.18, but perm2 steps a one-to-one copy
 * by 1 << 20), divided by Q (1.0 = 1 << 27) with perspective correction on,
 * nearest or bilinear, then the YUV unit -- conversion and the chroma test.
 * False when the chroma test rejects the fragment; `texel` is left as it was
 * (no texture applied) when it rejects only the texel.
 **/
bool CPermedia2::texture_color(u32 &texel, s32 x, s32 y) {
  auto &g = r.g;
  const u32 trm = G(T_TEXTURE_READ_MODE);
  u32 c;
  if (trm & 1) {
    double u, v, q;
    if (g.tex_plane) {
      const double dx = x + 0.5 - g.tp_x0, dy = y + 0.5 - g.tp_y0;
      u = g.tp[0][0] + g.tp[0][1] * dx + g.tp[0][2] * dy;
      v = g.tp[1][0] + g.tp[1][1] * dx + g.tp[1][2] * dy;
      q = g.tp[2][0] + g.tp[2][1] * dx + g.tp[2][2] * dy;
    } else {
      u = double(g.s) / (1 << 20);
      v = double(g.t) / (1 << 20);
      q = double(g.q) / (1 << 27);
    }
    if ((G(T_TEXTURE_ADDRESS_MODE) & 2) && q != 0) {
      u /= q;
      v /= q;
    }
    if (trm & (1u << 17)) { // bilinear
      u -= 0.5;
      v -= 0.5;
      const double fu = std::floor(u), fv = std::floor(v);
      const s32 iu = s32(fu), iv = s32(fv);
      const u32 wu = u32((u - fu) * 256), wv = u32((v - fv) * 256);
      const u32 t00 = texel_fetch(iu, iv), t10 = texel_fetch(iu + 1, iv);
      const u32 t01 = texel_fetch(iu, iv + 1),
                t11 = texel_fetch(iu + 1, iv + 1);
      c = 0;
      for (int sh = 0; sh < 32; sh += 8) {
        const u32 a0 = (t00 >> sh) & 0xff, a1 = (t10 >> sh) & 0xff;
        const u32 b0 = (t01 >> sh) & 0xff, b1 = (t11 >> sh) & 0xff;
        const u32 top = a0 * (256 - wu) + a1 * wu;
        const u32 bot = b0 * (256 - wu) + b1 * wu;
        c |= (((top * (256 - wv) + bot * wv) >> 16) & 0xff) << sh;
      }
    } else {
      // A coordinate exactly on a texel's edge: Direct3D's reference
      // rasterizer quantises to 16 fraction bits after scaling by
      // 1 - 2^-20, so the texel before the edge (and zero stays zero) --
      // the rule the Rage Pro matched too; DirectDraw's stretch takes the
      // texel after it. A triangle from the delta unit is Direct3D's, a
      // primitive the driver set up itself a blit.
      auto quant = [&](double c) {
        if (!g.tex_plane)
          return s32(std::floor(c + 1.0 / 4096));
        return s32(std::floor(std::floor(c * (1.0 - 1.0 / 1048576) * 65536.0) /
                              65536.0));
      };
      c = texel_fetch(quant(u), quant(v));
    }
  } else {
    c = G(T_TEXEL0);
  }

  const u32 ym = G(T_YUV_MODE);
  const u32 before = c;
  if (ym & 1)
    c = yuv_to_rgb(c);
  if ((ym >> 1) & 3) {
    const u32 t = (ym & 8) ? c : before;
    const u32 lo = G(T_CHROMA_LOWER), hi = G(T_CHROMA_UPPER);
    bool inside = true;
    for (int sh = 0; sh < 24; sh += 8) {
      const u32 comp = (t >> sh) & 0xff;
      if (comp < ((lo >> sh) & 0xff) || comp > ((hi >> sh) & 0xff))
        inside = false;
    }
    const bool pass = ((ym >> 1) & 3) == 1 ? inside : !inside;
    if (!pass)
      return (ym & (1u << 4)) != 0; // reject the texel, or the fragment
  }
  texel = c;
  return true;
}

/**
 * Texture application (4.13.1, TextureColorMode): RGB copy, modulate or
 * decal, or the ramp modes' decal and modulate by Kd.
 **/
u32 CPermedia2::apply_texture(u32 c, u32 t) const {
  const u32 tcm = r.gp[T_TEXTURE_COLOR_MODE];
  const u32 mode = (tcm >> 1) & 7;
  const u32 ta = t >> 24, ca = c >> 24;
  auto mix = [&](auto fn) {
    u32 out = 0;
    for (int sh = 0; sh < 24; sh += 8)
      out |= (fn((c >> sh) & 0xff, (t >> sh) & 0xff) & 0xff) << sh;
    return out;
  };
  if (tcm & (1u << 4)) { // ramp
    u32 out = (mode & 1) ? mix([&](u32 cf, u32 ct) {
                             return (ct * ta + cf * (255 - ta)) / 255;
                           }) | (ca << 24)
                         : (t & 0xffffff) | ((ta * ca / 255) << 24);
    return out;
  }
  switch (mode & 3) {
  case 0: // modulate
    return mix([](u32 cf, u32 ct) { return (cf * ct) / 255; }) |
           ((ca * ta / 255) << 24);
  case 1: // decal
    return mix([&](u32 cf, u32 ct) {
             return (ct * ta + cf * (255 - ta)) / 255;
           }) |
           (ca << 24);
  default: // copy
    return t;
  }
}

/// Fog (4.13.2): C = f Ci + (1 - f) Cf, the fog index clamped to 0..1.
u32 CPermedia2::apply_fog(u32 c) const {
  const s32 fi = r.g.f >> 4; // 2.19 above four unused bits
  const s32 f = fi<0 ? 0 : fi>(1 << 19) ? (1 << 19) : fi;
  const u32 fc = r.gp[T_FOG_COLOR];
  u32 out = c & 0xff000000u;
  for (int sh = 0; sh < 24; sh += 8) {
    const s64 ci = (c >> sh) & 0xff, cf = (fc >> sh) & 0xff;
    out |= u32((ci * f + cf * ((1 << 19) - f)) >> 19) << sh;
  }
  return out;
}

/**
 * Alpha blending (4.13.3, AlphaBlendMode): the destination converted from
 * the framebuffer's format, then source-alpha blend or pre-multiplied.
 **/
u32 CPermedia2::alpha_blend(u32 c, u32 dest_raw) const {
  const u32 abm = r.gp[T_ALPHA_BLEND_MODE];
  const u32 fmt = ((abm >> 8) & 15) | (((abm >> 16) & 1) << 4);
  const u32 d = unpack_color(dest_raw, fmt, (abm >> 13) & 1, (abm >> 12) & 1,
                             (abm >> 17) & 1);
  const u32 op = (abm >> 1) & 0x7f;
  if (op == 16 >> 1 || op == 16) // format only
    return c;
  const u32 as = c >> 24;
  const bool premult = (op & 0x7f) == (81 >> 1) || op == 81;
  u32 out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    const u32 cs = (c >> sh) & 0xff, cd = (d >> sh) & 0xff;
    const u32 v = premult ? cs + cd * (255 - as) / 255
                          : (cs * as + cd * (255 - as)) / 255;
    out |= (v > 255 ? 255 : v) << sh;
  }
  return out;
}

/**
 * Ordered dither (4.14.2) before the colour format drops low bits: a 4x4
 * Bayer threshold, scaled to what the narrowest component loses.
 **/
u32 CPermedia2::dither(u32 c, s32 x, s32 y) const {
  static const u8 bayer[4][4] = {
      {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  const u32 dm = r.gp[T_DITHER_MODE];
  if ((dm & 3) != 3)
    return c;
  const u32 fmt = ((dm >> 2) & 15) | (((dm >> 16) & 1) << 4);
  int lost; // bits a component loses
  switch (fmt) {
  case 0:
  case 14:
    return c;
  case 16:
  case 17:
  case 1:
  case 13:
    lost = 3;
    break;
  case 2:
    lost = 4;
    break;
  default:
    lost = 5;
    break;
  }
  const u32 xo = (dm >> 6) & 3, yo = (dm >> 8) & 3;
  const u32 t = bayer[(y + yo) & 3][(x + xo) & 3];
  const u32 add = (t << lost) >> 4;
  u32 out = c & 0xff000000u;
  for (int sh = 0; sh < 24; sh += 8) {
    const u32 v = ((c >> sh) & 0xff) + add;
    out |= (v > 255 ? 255 : v) << sh;
  }
  return out;
}
