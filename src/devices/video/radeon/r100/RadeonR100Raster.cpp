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
 * The Radeon's 3D engine, back half: the rasteriser and the pixel
 * pipeline. Register meanings are radeon_reg.h's names as Mesa's r100
 * driver and X.org's composite use them (see RadeonR100_3D.hpp):
 *
 * Setup and rasteriser (SE_CNTL, RE_*):
 *   - vertices (of every primitive) snap to SE_CNTL's ROUND_PREC <31:30>
 *     (1/16..1/2 pixel) by ROUND_MODE <29:28> (truncate, round, round
 *     with ties to even or to odd); pixel centres are at +0.5
 *     with VTX_PIX_CENTER_OGL <27>, at the integer otherwise (Direct3D);
 *   - culling: front faces wind as FFACE_CULL_DIR <0> says (1
 *     counter-clockwise, as the screen shows it) -- GL's winding, which
 *     Mesa keeps by flipping Y in the viewport and swaps when it renders
 *     unflipped to a texture (radeonFrontFace); BFACE <2:1> and FFACE
 *     <4:3> draw a face solid
 *     (3), as points (1) or lines (2), or cull it (0) [the point and line
 *     codes are inferred from R200's];
 *   - coverage by the top-left rule; the clip rectangle RE_TOP_LEFT ..
 *     RE_WIDTH_HEIGHT (inclusive x2/y2, which Mesa loads with the
 *     scissor) always applies;
 *   - shading per attribute (DIFFUSE <9:8>, ALPHA <11:10>, SPECULAR
 *     <13:12>, FOG <15:14>): 0 solid (RE_SOLID_COLOR; no specular; fog
 *     factor 1) [inference], 1 flat (the provoking vertex), 2 Gouraud,
 *     linear in screen space; texture coordinates perspective-correct
 *     when the unit's TXFORMAT PERSPECTIVE_ENABLE <31> is set, then
 *     divided by Q;
 *   - lines one pixel wide through the snapped endpoints (the pixels
 *     whose centres the segment passes along its major axis, the nearest
 *     across it) without the last pixel, or with
 *     WIDELINE_ENABLE <20> SE_LINE_WIDTH (12.4 pixels) wide as a quad;
 *     the line pattern RE_LINE_PATTERN when PP_CNTL PATTERN_ENABLE <2>;
 *     points one pixel; the polygon stipple (32x32, RE_STIPPLE_DATA, RE_MISC
 *     offsets) when PP_CNTL STIPPLE_ENABLE <0>.
 * Textures (PP_TXFILTER_n, PP_TXFORMAT_n, PP_TXOFFSET_n; three units):
 *   formats I8, AI88, RGB332, ARGB1555, RGB565, ARGB4444, ARGB8888,
 *   RGBA8888, Y8, the two packed YUV 4:2:2 orders, DXT1, DXT2/3, DXT4/5;
 *   alpha forced to one without ALPHA_IN_MAP <6> (but I8's alpha is
 *   always the intensity: Mesa radeonUpdateTextureEnv); power-of-two sizes
 *   <11:8>, <15:12> with mip levels packed one after another, each row
 *   32-byte aligned (Mesa radeon_mipmap_tree.c), or with NON_POWER2 <7>
 *   PP_TEX_SIZE_n and PP_TEX_PITCH_n (+32 bytes, Linux r100.c); the
 *   coordinate set ST_ROUTE <25:24>; magnification and minification
 *   filters, nearest/linear mip selection, LOD bias <15:8> (Mesa's
 *   encoding: +4 at 127, -1 at -128), MAX_MIP_LEVEL <19:16>, the eight
 *   wrap modes per axis and PP_BORDER_COLOR_n; YUV_TO_RGB <20>.
 * Combiners (PP_TXCBLEND_n, PP_TXABLEND_n, PP_TFACTOR_n), enabled per
 *   stage by PP_CNTL TEX_BLEND_n_ENABLE <14:12>; the equations follow from
 *   Mesa's mapping of every GL env mode: ADD A*B+C, SUBTRACT A*B-C,
 *   ADDSIGNED A*B+C-0.5, BLEND A*(1-C)+B*C, DOT3 sum((A-.5)(B-.5))
 *   replicated (Mesa adds the x4 through the scale field); arguments
 *   complemented by COMP_ARG_A/B/C <15>,<16>,<17>, scale <22:21>, clamp
 *   <23>. "Current" starts as the diffuse colour.
 * Then the specular colour added (PP_CNTL SPECULAR_ENABLE <21>), fog
 *   (FOG_ENABLE <22>; PP_FOG_COLOR, the factor from the specular alpha,
 *   or the diffuse alpha with FOG_USE_DIFFUSE_ALPHA), the alpha test
 *   (ALPHA_TEST_ENABLE <23>, PP_MISC), stencil and Z (RB3D_CNTL <7>, <8>;
 *   RB3D_ZSTENCILCNTL: 16-, 24- (stencil in <31:24>) and 32-bit integer
 *   Z, the eight compare functions, the six stencil operations, the
 *   write enable <30>; RB3D_STENCILREFMASK), blending (RB3D_CNTL <0>,
 *   RB3D_BLENDCNTL: the GL factors 32..42, add or subtract, clamped or
 *   not), the logic op (ROP_ENABLE <6>, RB3D_ROPCNTL), the plane mask
 *   (<1>, RB3D_PLANEMASK) and the write in COLOR_FORMAT <13:10>: ARGB1555,
 *   RGB565, ARGB8888, RGB332, Y8, RGB8 (the red channel), ARGB4444.
 * Also: micro-tiled and endian-swapped surfaces, cube maps, table fog,
 *   the floating-point and W depth formats, dithering and rounding,
 *   polygon offset, anti-aliased lines and polygons (each at its code,
 *   with its source or marked as an inference).
 * Not modelled (each said once at runtime): macro tiling (no source gives
 *   the R100's layout), volume textures (Mesa's r100 driver refuses them;
 *   no layout source), the chroma key (no source names its key colour
 *   register), the hierarchical Z (it changes no pixel).
 **/

#include "Radeon.hpp"
#include "RadeonR100_3D.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace radeon::r100;

namespace {
inline float f32(u32 v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline u32 to_n(float v, int bits) {
  const float m = float((1 << bits) - 1);
  return u32(std::lround(clamp01(v) * m));
}
inline void argb_to_f(u32 v, float o[4]) {
  o[0] = float((v >> 16) & 0xff) / 255.0f;
  o[1] = float((v >> 8) & 0xff) / 255.0f;
  o[2] = float(v & 0xff) / 255.0f;
  o[3] = float(v >> 24) / 255.0f;
}

enum : u32 {
  TXF_I8 = 0,
  TXF_AI88 = 1,
  TXF_RGB332 = 2,
  TXF_ARGB1555 = 3,
  TXF_RGB565 = 4,
  TXF_ARGB4444 = 5,
  TXF_ARGB8888 = 6,
  TXF_RGBA8888 = 7,
  TXF_Y8 = 8,
  TXF_VYUY422 = 10,
  TXF_YVYU422 = 11,
  TXF_DXT1 = 12,
  TXF_DXT23 = 14,
  TXF_DXT45 = 15
};
enum : u32 {
  CF_ARGB1555 = 3,
  CF_RGB565 = 4,
  CF_ARGB8888 = 6,
  CF_RGB332 = 7,
  CF_Y8 = 8,
  CF_RGB8 = 9,
  CF_ARGB4444 = 15
};

int texfmt_bytes(u32 f) {
  switch (f) {
  case TXF_I8:
  case TXF_RGB332:
  case TXF_Y8:
    return 1;
  case TXF_ARGB8888:
  case TXF_RGBA8888:
    return 4;
  default:
    return 2;
  }
}

/// Unpack a pixel of a colour buffer format into 0..1 floats.
void unpack_cb(u32 fmt, u32 v, float o[4]) {
  switch (fmt) {
  case CF_ARGB1555:
    o[0] = float((v >> 10) & 31) / 31.0f;
    o[1] = float((v >> 5) & 31) / 31.0f;
    o[2] = float(v & 31) / 31.0f;
    o[3] = (v & 0x8000) ? 1.0f : 0.0f;
    return;
  case CF_RGB565:
    o[0] = float((v >> 11) & 31) / 31.0f;
    o[1] = float((v >> 5) & 63) / 63.0f;
    o[2] = float(v & 31) / 31.0f;
    o[3] = 1.0f;
    return;
  case CF_RGB332:
    o[0] = float((v >> 5) & 7) / 7.0f;
    o[1] = float((v >> 2) & 7) / 7.0f;
    o[2] = float(v & 3) / 3.0f;
    o[3] = 1.0f;
    return;
  case CF_Y8:
  case CF_RGB8:
    o[0] = o[1] = o[2] = o[3] = float(v & 0xff) / 255.0f;
    return;
  case CF_ARGB4444:
    o[0] = float((v >> 8) & 15) / 15.0f;
    o[1] = float((v >> 4) & 15) / 15.0f;
    o[2] = float(v & 15) / 15.0f;
    o[3] = float((v >> 12) & 15) / 15.0f;
    return;
  default:
    argb_to_f(v, o);
  }
}

u32 pack_cb(u32 fmt, const float c[4]) {
  switch (fmt) {
  case CF_ARGB1555:
    return (c[3] >= 0.5f ? 0x8000u : 0) | (to_n(c[0], 5) << 10) |
           (to_n(c[1], 5) << 5) | to_n(c[2], 5);
  case CF_RGB565:
    return (to_n(c[0], 5) << 11) | (to_n(c[1], 6) << 5) | to_n(c[2], 5);
  case CF_RGB332:
    return (to_n(c[0], 3) << 5) | (to_n(c[1], 3) << 2) | to_n(c[2], 2);
  case CF_Y8:
    return to_n(0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2], 8);
  case CF_RGB8:
    return to_n(c[0], 8);
  case CF_ARGB4444:
    return (to_n(c[3], 4) << 12) | (to_n(c[0], 4) << 8) | (to_n(c[1], 4) << 4) |
           to_n(c[2], 4);
  default:
    return (to_n(c[3], 8) << 24) | (to_n(c[0], 8) << 16) |
           (to_n(c[1], 8) << 8) | to_n(c[2], 8);
  }
}

int cb_bytes(u32 fmt) {
  switch (fmt) {
  case CF_RGB332:
  case CF_Y8:
  case CF_RGB8:
    return 1;
  case CF_ARGB8888:
    return 4;
  default:
    return 2;
  }
}

bool compare(u32 fn, u32 a, u32 b) { // "a FN b"
  switch (fn & 7) {
  case 0:
    return false;
  case 1:
    return a < b;
  case 2:
    return a <= b;
  case 3:
    return a == b;
  case 4:
    return a >= b;
  case 5:
    return a > b;
  case 6:
    return a != b;
  default:
    return true;
  }
}

u32 logic_op(u32 op, u32 s, u32 d) {
  switch (op & 15) {
  case 0:
    return 0;
  case 1:
    return ~(s | d);
  case 2:
    return ~s & d;
  case 3:
    return ~s;
  case 4:
    return s & ~d;
  case 5:
    return ~d;
  case 6:
    return s ^ d;
  case 7:
    return ~(s & d);
  case 8:
    return s & d;
  case 9:
    return ~(s ^ d);
  case 10:
    return d;
  case 11:
    return ~s | d;
  case 12:
    return s;
  case 13:
    return s | ~d;
  case 14:
    return s | d;
  default:
    return 0xffffffffu;
  }
}

/// A texel index under a wrap mode (PP_TXFILTER CLAMP_S/T); -1 for the
/// border colour.
int wrap(int i, int size, u32 mode) {
  switch (mode & 7) {
  case 0: // wrap
    i %= size;
    return i < 0 ? i + size : i;
  case 1: { // mirror
    int p = i % (2 * size);
    if (p < 0)
      p += 2 * size;
    return p < size ? p : 2 * size - 1 - p;
  }
  case 2: // clamp to the last texel
    return std::min(std::max(i, 0), size - 1);
  case 3: // mirror once, then clamp to the last texel
    if (i < 0)
      i = -i - 1;
    return std::min(i, size - 1);
  case 4: // clamp to the border
  case 6: // clamp, GL style (the coordinate clamped first)
    return (i < 0 || i >= size) ? -1 : i;
  default: // mirror once, then the border
    if (i < 0)
      i = -i - 1;
    return i >= size ? -1 : i;
  }
}

void yuv_to_rgb(float y, float u, float v, float o[3]) {
  y = (y - 16.0f / 255) * 1.164f;
  u -= 0.5f;
  v -= 0.5f;
  o[0] = clamp01(y + 1.596f * v);
  o[1] = clamp01(y - 0.813f * v - 0.391f * u);
  o[2] = clamp01(y + 2.018f * u);
}

void rgb565_f(u32 v, float o[3]) {
  o[0] = float((v >> 11) & 31) / 31.0f;
  o[1] = float((v >> 5) & 63) / 63.0f;
  o[2] = float(v & 31) / 31.0f;
}

/// A surface's endian swap of a dword as the chip reads or writes it:
/// 1 the bytes of each 16-bit half, 2 all four bytes, 3 the two halves
/// (radeon_reg.h TXO_ENDIAN_BYTE/WORD/HALFDW_SWAP, COLOR_ENDIAN_WORD/
/// DWORD_SWAP; the same codes as CP_RB_CNTL BUF_SWAP in the R5xx guide)
/// [inference: the texture codes' names read as "swap the bytes of each
/// word" (1), "of each dword" (2), "the half-dwords" (3)].
inline u32 surf_swap(u32 v, u32 mode) {
  switch (mode & 3) {
  case 1:
    return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
  case 2:
    return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
  case 3:
    return (v << 16) | (v >> 16);
  }
  return v;
}

/// An unsigned float of `e` exponent and `m` mantissa bits (bias `bias`)
/// for v >= 0, saturating: a stand-in for the undocumented floating-point
/// depth formats whose integer order is the value's order.
inline u32 ufloat_bits(double v, int e, int m, int bias) {
  if (!(v > 0))
    return 0;
  int ex;
  const double fr = std::frexp(v, &ex); // v = fr * 2^ex, fr in [0.5, 1)
  int be = ex - 1 + bias;               // v = (2 fr) * 2^(ex - 1)
  const u32 emax = (1u << e) - 1, mmax = (1u << m) - 1;
  if (be >= int(emax))
    return (emax << m) | mmax;
  if (be <= 0) { // denormal
    const double d = std::ldexp(v, m - 1 + bias - 1 + 1);
    return u32(std::min(double(mmax), std::floor(d)));
  }
  const u32 mant = u32(std::floor((2 * fr - 1) * double(1u << m)));
  return (u32(be) << m) | std::min(mant, mmax);
}
} // namespace

/// Snapshot the state a draw uses.
void CRadeonR100_3D::raster_setup() {
  rs.pp_cntl = reg(PP_CNTL);
  rs.rb3d_cntl = reg(RB3D_CNTL);
  rs.se_cntl = reg(SE_CNTL);
  rs.zcntl = reg(RB3D_ZSTENCILCNTL);
  rs.blend = reg(RB3D_BLENDCNTL);
  rs.misc = reg(PP_MISC);
  rs.color_base = c.mc_to_vram(reg(RB3D_COLOROFFSET) & ~15u);
  rs.color_pitch = reg(RB3D_COLORPITCH) & 0x1ff8;
  rs.color_fmt = (rs.rb3d_cntl >> 10) & 15;
  rs.color_bpp = u32(cb_bytes(rs.color_fmt));
  // RB3D_COLORPITCH COLOR_TILE_ENABLE <16>, COLOR_MICROTILE_ENABLE <17>,
  // COLOR_ENDIAN <19:18>; RB3D_DEPTHPITCH DEPTH_HYPERZ <17:16>,
  // DEPTH_ENDIAN <19:18> (radeon_reg.h)
  rs.color_micro = (reg(RB3D_COLORPITCH) >> 17) & 1;
  rs.color_swap = (reg(RB3D_COLORPITCH) >> 18) & 3;
  if (reg(RB3D_COLORPITCH) & (1u << 16))
    warn_once(20, "macro-tiled colour buffers (no source gives the R100's "
                  "macro tile layout; drawn linear)");
  rs.z_base = c.mc_to_vram(reg(RB3D_DEPTHOFFSET) & ~15u);
  rs.z_pitch = reg(RB3D_DEPTHPITCH) & 0x1ff8;
  rs.zfmt = rs.zcntl & 15;
  rs.z_micro = ((reg(RB3D_DEPTHPITCH) >> 16) & 3) != 0;
  rs.z_swap = (reg(RB3D_DEPTHPITCH) >> 18) & 3;
  rs.zbias_factor = regf(SE_ZBIAS_FACTOR);
  rs.zbias_const = regf(SE_ZBIAS_CONSTANT);
  rs.dither_y = -1;
  rs.dither_err[0] = rs.dither_err[1] = rs.dither_err[2] = 0;
  const u32 tl = reg(RE_TOP_LEFT), br = reg(RE_WIDTH_HEIGHT);
  rs.clip_l = int(tl & 0x7ff);
  rs.clip_t = int((tl >> 16) & 0x7ff);
  rs.clip_r = int(br & 0x7ff);
  rs.clip_b = int((br >> 16) & 0x7ff);
  const u32 srm = reg(RB3D_STENCILREFMASK);
  rs.stencil_ref = srm & 0xff;
  rs.stencil_mask = (srm >> 16) & 0xff;
  rs.stencil_wmask = (srm >> 24) & 0xff;
  rs.planemask = reg(RB3D_PLANEMASK);
  rs.rop = (reg(RB3D_ROPCNTL) >> 8) & 15;
  float fc[4];
  argb_to_f(reg(PP_FOG_COLOR), fc);
  memcpy(rs.fog_col, fc, sizeof(rs.fog_col));

  for (int u = 0; u < 3; u++) {
    texunit_t &t = rs.tex[u];
    t.enabled = (rs.pp_cntl >> (4 + u)) & 1;
    const u32 o = PP_UNIT_STRIDE * u32(u);
    t.filter = reg(PP_TXFILTER_0 + o);
    t.format = reg(PP_TXFORMAT_0 + o);
    t.offset = reg(PP_TXOFFSET_0 + o);
    t.cblend = reg(PP_TXCBLEND_0 + o);
    t.ablend = reg(PP_TXABLEND_0 + o);
    t.tfactor = reg(PP_TFACTOR_0 + o);
    if (!t.enabled)
      continue;
    t.fmt = int(t.format & 31);
    t.npot = (t.format >> 7) & 1;
    t.persp = (t.format >> 31) & 1;
    t.route = int((t.format >> 24) & 3);
    t.bpp = texfmt_bytes(u32(t.fmt));
    // PP_TXOFFSET ENDIAN <1:0> (or TXFORMAT ENDIAN <27:26>), MACRO_TILE
    // <2>, MICRO_TILE <4:3>; TXFORMAT CUBIC_MAP_ENABLE <30>
    t.swap = (t.offset & 3) ? (t.offset & 3) : ((t.format >> 26) & 3);
    t.micro = ((t.offset >> 3) & 3) != 0;
    t.cube = (t.format >> 30) & 1;
    if (t.offset & 4)
      warn_once(24, "macro-tiled textures (no source gives the R100's "
                    "macro tile layout; read linear)");
    if (t.format & (1u << 29))
      warn_once(23, "the texture chroma key (no source names the R100's "
                    "key colour register)");
    const u32 base = t.offset & ~31u;
    if (t.cube) {
      // A cube map (Mesa radeon_texstate.c, radeon_state_init.c
      // cube_emit_cs): one level (r100 cube maps have no mip levels,
      // radeon_tex.c), the faces +X, -X, +Y, -Y, +Z at
      // PP_CUBIC_OFFSET_Tn_0..4 and -Z at PP_TXOFFSET_n, all the size
      // TXFORMAT gives
      static const u32 cube_base[3] = {0x1dd0, 0x1e00, 0x1e14};
      for (int f = 0; f < 5; f++)
        t.face_off[f] = reg(cube_base[u] + 4 * u32(f)) & ~31u;
      t.face_off[5] = base;
      const int lw = int((t.format >> 8) & 15), lh = int((t.format >> 12) & 15);
      t.w = 1 << lw;
      t.h = 1 << lh;
      t.levels = 1;
      t.level_off[0] = base;
      t.level_w[0] = t.w;
      t.level_h[0] = t.h;
      t.level_pitch[0] = (t.w * t.bpp + 31) & ~31;
      continue;
    }
    if (t.npot) {
      const u32 sz = reg(PP_TEX_SIZE_0 + 8 * u32(u));
      t.w = int(sz & 0x7ff) + 1;
      t.h = int((sz >> 16) & 0x7ff) + 1;
      t.levels = 1;
      t.level_off[0] = base;
      t.level_w[0] = t.w;
      t.level_h[0] = t.h;
      t.level_pitch[0] = int((reg(PP_TEX_PITCH_0 + 8 * u32(u)) & 0x3fe0) + 32);
      continue;
    }
    const int lw = int((t.format >> 8) & 15), lh = int((t.format >> 12) & 15);
    t.w = 1 << lw;
    t.h = 1 << lh;
    const int maxlev = int((t.filter >> 16) & 15);
    t.levels = std::min(maxlev, std::max(lw, lh)) + 1;
    u32 off = base;
    for (int l = 0; l < t.levels && l < 12; l++) {
      const int w = std::max(1, t.w >> l), h = std::max(1, t.h >> l);
      int pitch, rows;
      if (t.fmt >= int(TXF_DXT1)) {
        const int bb = t.fmt == int(TXF_DXT1) ? 8 : 16;
        pitch = ((w + 3) / 4) * bb;
        rows = (h + 3) / 4;
      } else {
        pitch = w * t.bpp;
        rows = h;
      }
      pitch = (pitch + 31) & ~31;
      t.level_off[l] = off;
      t.level_w[l] = w;
      t.level_h[l] = h;
      t.level_pitch[l] = pitch;
      off += u32(pitch * rows);
    }
  }
}

/// One texel of a level, decoded to 0..1 RGBA.
void CRadeonR100_3D::fetch_texel(const texunit_t &tu, int level, int x, int y,
                                 float o[4]) {
  const u32 base = tu.cube ? tu.face_off[m_cube_face] : tu.level_off[level];
  const u32 pitch = u32(tu.level_pitch[level]);
  const u32 f = u32(tu.fmt);
  auto rd8 = [&](u32 off) {
    if (!tu.swap)
      return mem_read8(base + off);
    const u32 a = base + off;
    return u8(surf_swap(mem_read32(a & ~3u), tu.swap) >> (8 * (a & 3)));
  };
  auto rd16 = [&](u32 off) { return u32(rd8(off)) | (u32(rd8(off + 1)) << 8); };
  auto rd32 = [&](u32 off) { return rd16(off) | (rd16(off + 2) << 16); };
  bool has_alpha = (tu.format >> 6) & 1;
  if (f >= TXF_DXT1) {
    const u32 bb = f == TXF_DXT1 ? 8 : 16;
    const u32 boff = u32(y / 4) * pitch + u32(x / 4) * bb;
    const int px = x & 3, py = y & 3;
    const u32 cb = boff + (f == TXF_DXT1 ? 0 : 8);
    const u32 c0 = rd16(cb), c1 = rd16(cb + 2);
    const u32 bits = rd32(cb + 4);
    const u32 code = (bits >> (2 * (py * 4 + px))) & 3;
    float a[3], b[3];
    rgb565_f(c0, a);
    rgb565_f(c1, b);
    o[3] = 1.0f;
    const bool four = f != TXF_DXT1 || c0 > c1;
    for (int k = 0; k < 3; k++) {
      switch (code) {
      case 0:
        o[k] = a[k];
        break;
      case 1:
        o[k] = b[k];
        break;
      case 2:
        o[k] = four ? (2 * a[k] + b[k]) / 3 : (a[k] + b[k]) / 2;
        break;
      default:
        o[k] = four ? (a[k] + 2 * b[k]) / 3 : 0.0f;
        break;
      }
    }
    if (f == TXF_DXT1) {
      if (!four && code == 3)
        o[3] = 0.0f;
    } else if (f == TXF_DXT23) {
      const u32 nib = u32(py * 4 + px);
      const u32 ab = rd8(boff + nib / 2);
      o[3] = float((nib & 1) ? ab >> 4 : ab & 15) / 15.0f;
    } else {
      const u32 a0 = rd8(boff), a1 = rd8(boff + 1);
      uint64_t ab = 0;
      for (int k = 0; k < 6; k++)
        ab |= uint64_t(rd8(boff + 2 + u32(k))) << (8 * k);
      const u32 ac = u32(ab >> (3 * (py * 4 + px))) & 7;
      float av;
      if (ac == 0)
        av = float(a0);
      else if (ac == 1)
        av = float(a1);
      else if (a0 > a1)
        av = (float(8 - ac) * a0 + float(ac - 1) * a1) / 7.0f;
      else if (ac == 6)
        av = 0;
      else if (ac == 7)
        av = 255;
      else
        av = (float(6 - ac) * a0 + float(ac - 1) * a1) / 5.0f;
      o[3] = av / 255.0f;
    }
    if (!has_alpha)
      o[3] = 1.0f;
    return;
  }
  // micro-tiled: 32-byte tiles of 8x4, 8x2 or 4x2 texels by size (Mesa
  // radeon_tile.c) [inference: Mesa's r100 driver never tiles textures]
  const u32 off = tu.micro ? surface_addr(0, pitch / u32(tu.bpp), u32(tu.bpp),
                                          true, false, x, y)
                           : u32(y) * pitch + u32(x) * u32(tu.bpp);
  switch (f) {
  case TXF_I8: {
    const float i = float(rd8(off)) / 255.0f;
    o[0] = o[1] = o[2] = o[3] = i;
    return; // I8's alpha is the intensity, ALPHA_IN_MAP or not
  }
  case TXF_AI88: {
    const u32 v = rd16(off);
    o[0] = o[1] = o[2] = float(v & 0xff) / 255.0f;
    o[3] = float(v >> 8) / 255.0f;
    break;
  }
  case TXF_RGB332: {
    const u32 v = rd8(off);
    o[0] = float((v >> 5) & 7) / 7.0f;
    o[1] = float((v >> 2) & 7) / 7.0f;
    o[2] = float(v & 3) / 3.0f;
    o[3] = 1.0f;
    has_alpha = false;
    break;
  }
  case TXF_ARGB1555:
    unpack_cb(CF_ARGB1555, rd16(off), o);
    break;
  case TXF_RGB565:
    unpack_cb(CF_RGB565, rd16(off), o);
    has_alpha = false;
    break;
  case TXF_ARGB4444:
    unpack_cb(CF_ARGB4444, rd16(off), o);
    break;
  case TXF_ARGB8888:
    argb_to_f(rd32(off), o);
    break;
  case TXF_RGBA8888: {
    const u32 v = rd32(off);
    o[0] = float(v >> 24) / 255.0f;
    o[1] = float((v >> 16) & 0xff) / 255.0f;
    o[2] = float((v >> 8) & 0xff) / 255.0f;
    o[3] = float(v & 0xff) / 255.0f;
    break;
  }
  case TXF_Y8:
    o[0] = o[1] = o[2] = float(rd8(off)) / 255.0f;
    o[3] = 1.0f;
    has_alpha = false;
    break;
  case TXF_VYUY422:
  case TXF_YVYU422: {
    // a pair of pixels per dword: VYUY422 = Y0 U Y1 V in memory,
    // YVYU422 = U Y0 V Y1 (Mesa maps YCBCR_REV and YCBCR to them)
    // [inference: the byte orders follow Mesa's formats]
    const u32 poff = u32(y) * pitch + u32(x & ~1) * 2;
    const u32 v = rd32(poff);
    float yy, uu, vv;
    if (f == TXF_VYUY422) {
      yy = float((x & 1) ? (v >> 16) & 0xff : v & 0xff);
      uu = float((v >> 8) & 0xff);
      vv = float(v >> 24);
    } else {
      yy = float((x & 1) ? v >> 24 : (v >> 8) & 0xff);
      uu = float(v & 0xff);
      vv = float((v >> 16) & 0xff);
    }
    if (tu.filter & (1u << 20)) {
      yuv_to_rgb(yy / 255, uu / 255, vv / 255, o);
    } else {
      o[0] = vv / 255;
      o[1] = yy / 255;
      o[2] = uu / 255;
    }
    o[3] = 1.0f;
    has_alpha = false;
    break;
  }
  default:
    o[0] = o[1] = o[2] = 0;
    o[3] = 1;
    return;
  }
  if (!has_alpha)
    o[3] = 1.0f;
}

void CRadeonR100_3D::texel(const texunit_t &tu, int level, int x, int y,
                           float o[4]) {
  const int w = tu.level_w[level], h = tu.level_h[level];
  const int xi = wrap(x, w, tu.filter >> 23), yi = wrap(y, h, tu.filter >> 27);
  if (xi < 0 || yi < 0) {
    argb_to_f(reg(PP_BORDER_COLOR_0 + 4 * u32(&tu - rs.tex)), o);
    return;
  }
  fetch_texel(tu, level, xi, yi, o);
}

/**
 * A filtered sample at (s, t) -- texture space, 0..1 across the level-0
 * image -- and level of detail `lod` (log2 of texels per pixel).
 **/
void CRadeonR100_3D::sample(int unit, float s, float t, float lod, float o[4]) {
  const texunit_t &tu = rs.tex[unit];
  const u32 flt = tu.filter;
  // LOD bias: Mesa's encoding, -1.0 at -128 up to +4.0 at 127
  const int b = int(int8_t((flt >> 8) & 0xff));
  lod += b >= 0 ? float(b) * 4.0f / 127.0f : float(b) / 128.0f;
  const bool mag = lod <= 0.0f;
  const u32 minf = (flt >> 1) & 15;
  bool linear;
  int lev0 = 0, lev1 = 0;
  float frac = 0.0f;
  if (mag) {
    linear = (flt & 1) != 0;
  } else {
    // 0/1 nearest/linear, 8/9 the anisotropic ones (filtered as 0/1)
    linear = minf == 1 || minf == 9;
    const int maxl = tu.levels - 1;
    switch (minf) {
    case 2: // nearest mip nearest
    case 6: // linear mip nearest
    case 10:
      lev0 = std::min(maxl, int(std::floor(lod + 0.5f)));
      linear = minf == 6;
      break;
    case 3: // nearest mip linear
    case 7: // linear mip linear
    case 11:
      lev0 = std::min(maxl, int(std::floor(lod)));
      lev1 = std::min(maxl, lev0 + 1);
      frac = lod - float(lev0);
      linear = minf == 7;
      break;
    default: // no mip-mapping: level 0
      break;
    }
  }
  // GL's clamp (6) and mirror-once clamp (7): the coordinate clamped to
  // the image, so that only a linear filter reaches the border
  const u32 ms = (flt >> 23) & 7, mt = (flt >> 27) & 7;
  const bool gl_clamp_s = ms >= 6, gl_clamp_t = mt >= 6;
  auto at_level = [&](int l, float out[4]) {
    const float w = float(tu.level_w[l]), h = float(tu.level_h[l]);
    float u = s * w, v = t * h;
    if (ms == 7)
      u = std::fabs(u);
    if (mt == 7)
      v = std::fabs(v);
    if (gl_clamp_s)
      u = std::min(std::max(u, 0.0f), w);
    if (gl_clamp_t)
      v = std::min(std::max(v, 0.0f), h);
    if (!linear) {
      int iu = int(std::floor(u)), iv = int(std::floor(v));
      if (gl_clamp_s)
        iu = std::min(iu, tu.level_w[l] - 1);
      if (gl_clamp_t)
        iv = std::min(iv, tu.level_h[l] - 1);
      texel(tu, l, iu, iv, out);
      return;
    }
    u -= 0.5f;
    v -= 0.5f;
    const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    const float fx = u - float(x0), fy = v - float(y0);
    float t00[4], t10[4], t01[4], t11[4];
    texel(tu, l, x0, y0, t00);
    texel(tu, l, x0 + 1, y0, t10);
    texel(tu, l, x0, y0 + 1, t01);
    texel(tu, l, x0 + 1, y0 + 1, t11);
    for (int k = 0; k < 4; k++)
      out[k] = (t00[k] * (1 - fx) + t10[k] * fx) * (1 - fy) +
               (t01[k] * (1 - fx) + t11[k] * fx) * fy;
  };
  at_level(lev0, o);
  if (frac > 0.0f && lev1 != lev0) {
    float o1[4];
    at_level(lev1, o1);
    for (int k = 0; k < 4; k++)
      o[k] = o[k] * (1 - frac) + o1[k] * frac;
  }
}

/**
 * A fragment through the pixel pipeline, written to the colour buffer
 * unless a test drops it.
 **/
void CRadeonR100_3D::fragment(frag_t &f) {
  if (f.x < rs.clip_l || f.x > rs.clip_r || f.y < rs.clip_t ||
      f.y > rs.clip_b || f.x < 0 || f.y < 0)
    return;
  const u32 pp = rs.pp_cntl;
  if (pp & 1) { // polygon stipple
    const u32 m = reg(RE_MISC);
    const u32 row = m_stipple[u32(f.y + int((m >> 8) & 31)) & 31];
    const u32 bx = u32(f.x + int(m & 31)) & 31;
    if (!((row >> ((m & (1u << 16)) ? 31 - bx : bx)) & 1))
      return;
  }
  // Textures.
  float tc[3][4] = {};
  for (int u = 0; u < 3; u++) {
    if (!rs.tex[u].enabled)
      continue;
    if (rs.tex[u].cube) {
      float fs, ft;
      m_cube_face = cube_face(f.tex[u][0], f.tex[u][1], f.tex[u][3], &fs, &ft);
      sample(u, fs, ft, f.tex[u][2], tc[u]);
      m_cube_face = 0;
    } else {
      sample(u, f.tex[u][0], f.tex[u][1], f.tex[u][2], tc[u]);
    }
  }

  // Combiners.
  float cur[4] = {f.col[0], f.col[1], f.col[2], f.col[3]};
  for (int u = 0; u < 3; u++) {
    if (!((pp >> (12 + u)) & 1))
      continue;
    const texunit_t &t = rs.tex[u];
    float tf[4];
    argb_to_f(t.tfactor, tf);
    auto carg = [&](u32 sel, bool comp, float o[3]) {
      float v[4] = {0, 0, 0, 0};
      bool alpha = false;
      switch (sel) {
      case 2:
      case 3:
        memcpy(v, cur, sizeof(v));
        break;
      case 4:
      case 5:
        memcpy(v, f.col, sizeof(v));
        break;
      case 6:
      case 7:
        memcpy(v, f.spec, sizeof(v));
        break;
      case 8:
      case 9:
        memcpy(v, tf, sizeof(v));
        break;
      default:
        if (sel >= 10 && sel <= 15)
          memcpy(v, tc[(sel - 10) / 2], sizeof(v));
        break;
      }
      alpha = sel >= 2 && (sel & 1);
      for (int k = 0; k < 3; k++) {
        const float x = alpha ? v[3] : v[k];
        o[k] = comp ? 1.0f - x : x;
      }
    };
    auto aarg = [&](u32 sel, bool comp) {
      float x = 0;
      switch (sel) {
      case 1:
        x = cur[3];
        break;
      case 2:
        x = f.col[3];
        break;
      case 3:
        x = f.spec[3];
        break;
      case 4:
        x = tf[3];
        break;
      case 5:
      case 6:
      case 7:
        x = tc[sel - 5][3];
        break;
      default:
        break;
      }
      return comp ? 1.0f - x : x;
    };
    auto scale_of = [](u32 r) {
      const u32 s = (r >> 21) & 3;
      return s == 1 ? 2.0f : s == 2 ? 4.0f : 1.0f;
    };
    const u32 cb = t.cblend, ab = t.ablend;
    float A[3], B[3], C[3], out[4];
    carg(cb & 31, (cb >> 15) & 1, A);
    carg((cb >> 5) & 31, (cb >> 16) & 1, B);
    carg((cb >> 10) & 31, (cb >> 17) & 1, C);
    const u32 cop = (cb >> 18) & 7;
    float dot = 0;
    for (int k = 0; k < 3; k++) {
      switch (cop) {
      case 1:
        out[k] = A[k] * B[k] - C[k];
        break;
      case 2:
        out[k] = A[k] * B[k] + C[k] - 0.5f;
        break;
      case 3:
        out[k] = A[k] * (1 - C[k]) + B[k] * C[k];
        break;
      case 4:
        dot += (A[k] - 0.5f) * (B[k] - 0.5f);
        break;
      default:
        out[k] = A[k] * B[k] + C[k];
        break;
      }
    }
    if (cop == 4)
      out[0] = out[1] = out[2] = dot;
    const float csc = scale_of(cb);
    for (int k = 0; k < 3; k++) {
      out[k] *= csc;
      if (cb & (1u << 23))
        out[k] = clamp01(out[k]);
    }
    if (cop == 4 && !(ab & (1u << 9))) {
      out[3] = out[0];
    } else {
      const float a = aarg(ab & 15, (ab >> 15) & 1),
                  b = aarg((ab >> 4) & 15, (ab >> 16) & 1),
                  cc = aarg((ab >> 8) & 15, (ab >> 17) & 1);
      switch ((ab >> 18) & 7) {
      case 1:
        out[3] = a * b - cc;
        break;
      case 2:
        out[3] = a * b + cc - 0.5f;
        break;
      case 3:
        out[3] = a * (1 - cc) + b * cc;
        break;
      default:
        out[3] = a * b + cc;
        break;
      }
      out[3] *= scale_of(ab);
    }
    if (ab & (1u << 23))
      out[3] = clamp01(out[3]);
    memcpy(cur, out, sizeof(cur));
  }
  if (pp & (1u << 21)) // specular
    for (int k = 0; k < 3; k++)
      cur[k] = clamp01(clamp01(cur[k]) + f.spec[k]);
  for (float &k : cur)
    k = clamp01(k);
  if (pp & (1u << 22)) { // fog
    const u32 src = (reg(PP_FOG_COLOR) >> 25) & 3;
    float fv = clamp01(src == 2 ? f.col[3] : f.spec[3]);
    // Table fog (PP_FOG_COLOR FOG_TABLE <24>): the factor from the table
    // loaded through FOG_TABLE_INDEX/DATA, indexed by the depth (FOG_
    // USE_DEPTH), the diffuse alpha (2) or the specular alpha (3)
    // [inference: 256 entries of 8 bits, the index the source scaled to
    // 0..255; the table's form is not documented]
    if (reg(PP_FOG_COLOR) & (1u << 24)) {
      const float idx = src == 2   ? f.col[3]
                        : src == 3 ? f.spec[3]
                                   : std::min(std::max(f.z, 0.0f), 1.0f);
      fv = float(m_fog_table[to_n(idx, 8)]) / 255.0f;
    }
    for (int k = 0; k < 3; k++)
      cur[k] = cur[k] * fv + rs.fog_col[k] * (1 - fv);
  }
  // Anti-aliasing (PP_CNTL ANTI_ALIAS <25:24>): the alpha times the share
  // of the pixel the line or polygon covers, for blending to use
  // [inference: GL's smooth lines and polygons; the R100's coverage
  // computation is not documented]
  cur[3] *= f.cov;
  if (pp & (1u << 23)) { // alpha test
    const u32 a8 = to_n(cur[3], 8);
    if (!compare(rs.misc >> 8, a8, rs.misc & 0xff))
      return;
  }

  // Stencil and Z.
  const u32 rb = rs.rb3d_cntl;
  const bool zen = (rb >> 8) & 1, sen = (rb >> 7) & 1;
  if (zen || sen) {
    // RB3D_ZSTENCILCNTL DEPTH_FORMAT <3:0> (radeon_reg.h): 16-bit (0),
    // 24-bit with stencil in <31:24> (2) and 32-bit (4) integer Z; 24-
    // (3) and 32-bit (5) floating-point Z; 16-, 24- and 32-bit floating
    // point W (7, 9, 11). The floating-point layouts are not documented:
    // they are stored here as unsigned floats (Z: 4 exponent bits and 20
    // mantissa for 24 bits, IEEE single for 32; W: 5/11, 6/18 and IEEE
    // single) whose integer order is the value's, which is what the
    // depth test sees [inference]. The 24-bit forms keep stencil above.
    const u32 zf = rs.zfmt;
    const int zb = (zf == 0 || zf == 7) ? 2 : 4;
    const bool z24 = zf == 2 || zf == 3 || zf == 9;
    const u32 za = surface_addr(rs.z_base, rs.z_pitch, u32(zb), rs.z_micro,
                                true, f.x, f.y);
    const u32 old = surf_read(za, zb, rs.z_swap);
    u32 zold, sold = 0;
    if (z24) {
      zold = old & 0xffffff;
      sold = old >> 24;
    } else {
      zold = old;
    }
    const double zd = std::min(std::max(double(f.z), 0.0), 1.0);
    u32 znew;
    switch (zf) {
    case 0:
      znew = u32(std::llround(zd * 65535.0));
      break;
    case 2:
      znew = u32(std::llround(zd * double(0xffffff)));
      break;
    case 4:
      znew = u32(std::llround(zd * double(0xffffffffu)));
      break;
    case 3:
      znew = ufloat_bits(zd, 4, 20, 15);
      break;
    case 5: {
      const float zf32 = float(zd);
      memcpy(&znew, &zf32, 4);
      break;
    }
    case 7:
      znew = ufloat_bits(f.w, 5, 11, 15);
      break;
    case 9:
      znew = ufloat_bits(f.w, 6, 18, 31);
      break;
    case 11: {
      const float w32 = f.w;
      memcpy(&znew, &w32, 4);
      break;
    }
    default:
      warn_once(25, "an undefined depth format");
      znew = u32(std::llround(zd * 65535.0));
      break;
    }
    u32 snew = sold;
    bool pass = true;
    auto sop = [&](u32 op) {
      switch (op & 7) {
      case 1:
        return 0u;
      case 2:
        return rs.stencil_ref;
      case 3:
        return sold < 255 ? sold + 1 : 255u;
      case 4:
        return sold > 0 ? sold - 1 : 0u;
      case 5:
        return ~sold & 0xffu;
      default:
        return sold;
      }
    };
    const u32 zc = rs.zcntl;
    if (sen && z24) {
      if (!compare(zc >> 12, rs.stencil_ref & rs.stencil_mask,
                   sold & rs.stencil_mask)) {
        snew = sop(zc >> 16);
        pass = false;
      }
    }
    bool zpass = true;
    if (pass && zen)
      zpass = compare(zc >> 4, znew, zold);
    if (pass && sen && z24)
      snew = sop(zpass ? zc >> 20 : zc >> 24);
    u32 zw = zold;
    if (pass && zpass && zen && (zc & (1u << 30)))
      zw = znew;
    if (z24) {
      const u32 sw = (sold & ~rs.stencil_wmask) | (snew & rs.stencil_wmask);
      const u32 nv = (zw & 0xffffff) | (sw << 24);
      if (nv != old)
        surf_write(za, 4, nv, rs.z_swap);
    } else if (zw != zold) {
      surf_write(za, zb, zw, rs.z_swap);
    }
    if (!pass || !zpass)
      return;
  }

  // Blend, logic op, plane mask, write.
  const u32 bpp = rs.color_bpp;
  const u32 ca = surface_addr(rs.color_base, rs.color_pitch, bpp,
                              rs.color_micro, false, f.x, f.y);
  const u32 dv = surf_read(ca, int(bpp), rs.color_swap);
  if (rb & 1) {
    float d[4];
    unpack_cb(rs.color_fmt, dv, d);
    const u32 sf = (rs.blend >> 16) & 63, df = (rs.blend >> 24) & 63;
    auto factor = [&](u32 code, int k) -> float {
      switch (code) {
      case 32:
        return 0;
      case 33:
        return 1;
      case 34:
        return cur[k];
      case 35:
        return 1 - cur[k];
      case 36:
        return d[k];
      case 37:
        return 1 - d[k];
      case 38:
        return cur[3];
      case 39:
        return 1 - cur[3];
      case 40:
        return d[3];
      case 41:
        return 1 - d[3];
      case 42:
        return k == 3 ? 1.0f : std::min(cur[3], 1 - d[3]);
      default:
        warn_once(26,
                  "blend factors outside the GL set (32..42): not modelled");
        return code == 0 ? 0.0f : 1.0f;
      }
    };
    const u32 fn = (rs.blend >> 12) & 3;
    for (int k = 0; k < 4; k++) {
      const float s = cur[k] * factor(sf, k), dd = d[k] * factor(df, k);
      // COMB_FCN <13:12>: add or subtract, clamped or not -- the result
      // is stored in a fixed-point buffer, clamped either way
      cur[k] = clamp01((fn & 2) ? s - dd : s + dd);
    }
  }
  u32 out = pack_color(cur, f.x, f.y);
  if (rb & (1u << 6))
    out = logic_op(rs.rop, out, dv);
  if (rb & (1u << 1))
    out = (dv & ~rs.planemask) | (out & rs.planemask);
  const u32 mask = bpp == 4 ? 0xffffffffu : (1u << (8 * bpp)) - 1;
  out &= mask;
  if (out != (dv & mask))
    surf_write(ca, int(bpp), out, rs.color_swap);
  m_pixels++;
}

namespace {
/// An attribute plane across a triangle: value = a*x + b*y + c.
struct plane_t {
  double a, b, c;
};
plane_t make_plane(const double x[3], const double y[3], const double v[3],
                   double area) {
  plane_t p;
  p.a = ((v[1] - v[0]) * (y[2] - y[0]) - (v[2] - v[0]) * (y[1] - y[0])) / area;
  p.b = ((v[2] - v[0]) * (x[1] - x[0]) - (v[1] - v[0]) * (x[2] - x[0])) / area;
  p.c = v[0] - p.a * x[0] - p.b * y[0];
  return p;
}
inline double eval(const plane_t &p, double x, double y) {
  return p.a * x + p.b * y + p.c;
}
} // namespace

/**
 * A triangle in window coordinates. `prov` is the provoking vertex whose
 * colours flat shading uses (or nullptr).
 **/
void CRadeonR100_3D::raster_triangle(const RadeonVertex *vin[3],
                                     const RadeonVertex *prov) {
  const u32 se = rs.se_cntl;
  const double centre = (se & (1u << 27)) ? 0.0 : 0.5; // D3D: +0.5 shift
  double x[3], y[3];
  for (int i = 0; i < 3; i++) {
    x[i] = snap(vin[i]->x) + centre;
    y[i] = snap(vin[i]->y) + centre;
  }
  double area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
  if (area == 0.0 || !std::isfinite(area))
    return;
  // front or back: the winding as the screen shows it, so that a
  // counter-clockwise triangle has a negative area in these y-down
  // coordinates
  const bool ccw_front = se & 1;
  const bool front = ccw_front ? area < 0 : area > 0;
  const u32 mode = front ? (se >> 3) & 3 : (se >> 1) & 3;
  if (mode == 0)
    return;
  if (mode == 1 || mode == 2) {
    // the triangle's provoking vertex shades its points and edges flat
    if (mode == 1)
      for (int i = 0; i < 3; i++)
        raster_point(*vin[i], prov);
    else
      for (int i = 0; i < 3; i++)
        raster_line(*vin[i], *vin[(i + 1) % 3], prov);
    return;
  }
  // orient so that the inside is positive
  int o[3] = {0, 1, 2};
  if (area < 0) {
    std::swap(o[1], o[2]);
    area = -area;
  }
  double X[3], Y[3];
  const RadeonVertex *v[3];
  for (int i = 0; i < 3; i++) {
    X[i] = x[o[i]];
    Y[i] = y[o[i]];
    v[i] = vin[o[i]];
  }

  // shading: per attribute group, solid / flat / Gouraud
  float col[3][4], spec[3][4];
  float solid[4];
  argb_to_f(reg(RE_SOLID_COLOR), solid);
  const RadeonVertex *pv = prov ? prov : v[0];
  for (int i = 0; i < 3; i++) {
    const u32 dm = (se >> 8) & 3, am = (se >> 10) & 3, sm = (se >> 12) & 3,
              fm = (se >> 14) & 3;
    for (int k = 0; k < 3; k++) {
      col[i][k] = dm == 0 ? solid[k] : dm == 1 ? pv->col[k] : v[i]->col[k];
      spec[i][k] = sm == 0 ? 0.0f : sm == 1 ? pv->spec[k] : v[i]->spec[k];
    }
    col[i][3] = am == 0 ? solid[3] : am == 1 ? pv->col[3] : v[i]->col[3];
    spec[i][3] = fm == 0 ? 1.0f : fm == 1 ? pv->spec[3] : v[i]->spec[3];
  }

  // attribute planes
  plane_t pc[4], ps[4], pz, pw;
  double tmp[3];
  for (int k = 0; k < 4; k++) {
    for (int i = 0; i < 3; i++)
      tmp[i] = col[i][k];
    pc[k] = make_plane(X, Y, tmp, area);
    for (int i = 0; i < 3; i++)
      tmp[i] = spec[i][k];
    ps[k] = make_plane(X, Y, tmp, area);
  }
  for (int i = 0; i < 3; i++)
    tmp[i] = v[i]->z;
  pz = make_plane(X, Y, tmp, area);
  for (int i = 0; i < 3; i++)
    tmp[i] = v[i]->w;
  pw = make_plane(X, Y, tmp, area);
  // texture coordinates: s, t, q per unit (times 1/w when perspective)
  plane_t pt[3][3];
  bool persp[3];
  bool nonparam[3];
  const u32 cf = reg(SE_COORD_FMT);
  for (int u = 0; u < 3; u++) {
    if (!rs.tex[u].enabled)
      continue;
    const int r = rs.tex[u].route;
    persp[u] = rs.tex[u].persp;
    nonparam[u] = (cf >> (8 + r)) & 1;
    static const int comp[3] = {0, 1, 3};
    for (int k = 0; k < 3; k++) {
      for (int i = 0; i < 3; i++)
        tmp[i] =
            double(v[i]->tex[r][comp[k]]) * (persp[u] ? double(v[i]->w) : 1.0);
      pt[u][k] = make_plane(X, Y, tmp, area);
    }
  }

  // the bounding box, inside the clip rectangle
  int x0 = int(std::floor(std::min({X[0], X[1], X[2]}) - 0.5));
  int x1 = int(std::ceil(std::max({X[0], X[1], X[2]}) + 0.5));
  int y0 = int(std::floor(std::min({Y[0], Y[1], Y[2]}) - 0.5));
  int y1 = int(std::ceil(std::max({Y[0], Y[1], Y[2]}) + 0.5));
  x0 = std::max(x0, std::max(rs.clip_l, 0));
  y0 = std::max(y0, std::max(rs.clip_t, 0));
  x1 = std::min(x1, rs.clip_r);
  y1 = std::min(y1, rs.clip_b);

  // edges: E(p) = (b - a) x (p - a), inside positive; a zero counts on
  // a top edge (dy == 0, dx > 0) or a left edge (dy < 0)
  struct edge_t {
    double ax, ay, dx, dy;
    bool incl;
  } e[3];
  for (int i = 0; i < 3; i++) {
    const int j = (i + 1) % 3;
    e[i].ax = X[i];
    e[i].ay = Y[i];
    e[i].dx = X[j] - X[i];
    e[i].dy = Y[j] - Y[i];
    e[i].incl = e[i].dy < 0 || (e[i].dy == 0 && e[i].dx > 0);
  }
  auto texcoord = [&](int u, double px, double py, float &s, float &t,
                      float *r = nullptr) {
    double qv = eval(pt[u][2], px, py);
    double sv = eval(pt[u][0], px, py), tv = eval(pt[u][1], px, py);
    if (persp[u]) {
      const double w = eval(pw, px, py);
      if (w != 0) {
        sv /= w;
        tv /= w;
        qv /= w;
      }
    }
    if (rs.tex[u].cube) {
      // a cube map's third coordinate rides in the Q slot (Mesa
      // radeon_swtcl.c, radeon_maos_arrays.c): no divide
      s = float(sv);
      t = float(tv);
      if (r)
        *r = float(qv);
      return;
    }
    if (qv != 0 && qv != 1) {
      sv /= qv;
      tv /= qv;
    }
    if (nonparam[u]) {
      sv /= rs.tex[u].w;
      tv /= rs.tex[u].h;
    }
    s = float(sv);
    t = float(tv);
  };
  // Polygon offset (SE_CNTL ZBIAS_ENABLE_TRI <18>; SE_ZBIAS_FACTOR and
  // SE_ZBIAS_CONSTANT, floats): GL's, the factor times the depth slope
  // plus the constant, which Mesa (radeonPolygonOffset) loads as the
  // units times one depth step
  const double zbias =
      (se & (1u << 18)) ? double(rs.zbias_factor) *
                                  std::max(std::fabs(pz.a), std::fabs(pz.b)) +
                              double(rs.zbias_const)
                        : 0.0;
  const bool aa = (rs.pp_cntl >> 25) & 1; // ANTI_ALIAS_POLY
  for (int py = y0; py <= y1; py++) {
    const double cy = py + 0.5;
    for (int px = x0; px <= x1; px++) {
      const double cx = px + 0.5;
      bool in = true;
      for (int i = 0; i < 3 && in; i++) {
        const double ev = e[i].dx * (cy - e[i].ay) - e[i].dy * (cx - e[i].ax);
        in = ev > 0 || (ev == 0 && e[i].incl);
      }
      if (!in && !aa)
        continue;
      frag_t f;
      f.x = px;
      f.y = py;
      f.z = float(eval(pz, cx, cy)) + zbias;
      {
        const double rhw = eval(pw, cx, cy);
        f.w = rhw != 0 ? float(1.0 / rhw) : 0.0f;
      }
      f.cov = 1.0f;
      if (aa) {
        // the share of 4x4 sample points inside
        int n = 0;
        for (int sy = 0; sy < 4; sy++)
          for (int sx = 0; sx < 4; sx++) {
            const double qx = px + (sx + 0.5) / 4, qy = py + (sy + 0.5) / 4;
            bool inside = true;
            for (int i = 0; i < 3 && inside; i++) {
              const double ev =
                  e[i].dx * (qy - e[i].ay) - e[i].dy * (qx - e[i].ax);
              inside = ev > 0 || (ev == 0 && e[i].incl);
            }
            n += inside;
          }
        if (!n)
          continue;
        f.cov = float(n) / 16.0f;
      }
      for (int k = 0; k < 4; k++) {
        f.col[k] = clamp01(float(eval(pc[k], cx, cy)));
        f.spec[k] = clamp01(float(eval(ps[k], cx, cy)));
      }
      for (int u = 0; u < 3; u++) {
        if (!rs.tex[u].enabled)
          continue;
        float s, t, s1, t1, s2, t2, r = 0;
        texcoord(u, cx, cy, s, t, &r);
        f.tex[u][3] = r;
        if (rs.tex[u].cube) {
          f.tex[u][0] = s;
          f.tex[u][1] = t;
          f.tex[u][2] = -100.0f; // no mip levels
          continue;
        }
        texcoord(u, cx + 1, cy, s1, t1);
        texcoord(u, cx, cy + 1, s2, t2);
        const float w = float(rs.tex[u].w), h = float(rs.tex[u].h);
        const float dux = (s1 - s) * w, dvx = (t1 - t) * h;
        const float duy = (s2 - s) * w, dvy = (t2 - t) * h;
        const float rho = std::max(std::sqrt(dux * dux + dvx * dvx),
                                   std::sqrt(duy * duy + dvy * dvy));
        f.tex[u][0] = s;
        f.tex[u][1] = t;
        f.tex[u][2] = rho > 0 ? std::log2(rho) : -100.0f;
      }
      fragment(f);
    }
  }
}

/// A fragment at a vertex (points, line pixels): its own attributes.
static void frag_from(CRadeonR100_3D::frag_t &f, const RadeonVertex &v,
                      const int route[3]) {
  f.z = v.z;
  f.w = v.w != 0 ? 1.0f / v.w : 0.0f;
  f.cov = 1.0f;
  memcpy(f.col, v.col, sizeof(f.col));
  memcpy(f.spec, v.spec, sizeof(f.spec));
  for (int u = 0; u < 3; u++) {
    const float *t = v.tex[route[u]];
    const float qv = t[3] != 0 ? t[3] : 1.0f;
    f.tex[u][0] = t[0] / qv;
    f.tex[u][1] = t[1] / qv;
    f.tex[u][2] = 0.0f;
    f.tex[u][3] = t[3];
  }
}

/**
 * A window coordinate snapped to the setup engine's grid: SE_CNTL
 * ROUND_PREC <31:30> 1/16, 1/8, 1/4 or 1/2 pixel, ROUND_MODE <29:28>
 * truncating (towards minus infinity, as a two's complement fixed-point
 * value truncates), rounding, or rounding with a tie going to the even or
 * the odd step (radeon_reg.h ROUND_MODE_TRUNC, _ROUND, _ROUND_EVEN,
 * _ROUND_ODD; the modes are read from their names). Every vertex of every
 * primitive is snapped (Mesa: TRUNC and 1/8 for GL, ROUND and 1/4 for its
 * blits; nadarad: TRUNC and 1/16).
 **/
double CRadeonR100_3D::snap(double v) const {
  static const double prec[4] = {16, 8, 4, 2};
  const u32 se = rs.se_cntl;
  const double q = prec[(se >> 30) & 3];
  const double s = v * q;
  double r;
  switch ((se >> 28) & 3) {
  case 0:
    r = std::floor(s);
    break;
  case 1:
    r = std::floor(s + 0.5);
    break;
  default: {
    r = std::floor(s + 0.5);
    if (r - s == 0.5) { // a tie: r is the step above
      const bool odd = std::fmod(r, 2.0) != 0;
      if (odd == (((se >> 28) & 3) == 2)) // even mode with r odd, or odd
        r -= 1.0;                         // mode with r even: the other
    }
    break;
  }
  }
  return r / q;
}

/**
 * SE_CNTL's shading per attribute group for a fragment that is a vertex's
 * or a line's: DIFFUSE <9:8>, ALPHA <11:10>, SPECULAR <13:12>, FOG <15:14>
 * each solid (RE_SOLID_COLOR; no specular; no fog), flat (the provoking
 * vertex's) or Gouraud (the fragment's own), as for triangles.
 **/
void CRadeonR100_3D::shade_fragment(frag_t &f, const RadeonVertex *prov) const {
  const u32 se = rs.se_cntl;
  const u32 dm = (se >> 8) & 3, am = (se >> 10) & 3, sm = (se >> 12) & 3,
            fm = (se >> 14) & 3;
  float solid[4];
  argb_to_f(reg(RE_SOLID_COLOR), solid);
  for (int k = 0; k < 3; k++) {
    if (dm == 0)
      f.col[k] = solid[k];
    else if (dm == 1 && prov)
      f.col[k] = prov->col[k];
    if (sm == 0)
      f.spec[k] = 0.0f;
    else if (sm == 1 && prov)
      f.spec[k] = prov->spec[k];
  }
  if (am == 0)
    f.col[3] = solid[3];
  else if (am == 1 && prov)
    f.col[3] = prov->col[3];
  if (fm == 0)
    f.spec[3] = 1.0f;
  else if (fm == 1 && prov)
    f.spec[3] = prov->spec[3];
}

void CRadeonR100_3D::raster_point(const RadeonVertex &a,
                                  const RadeonVertex *prov) {
  frag_t f;
  const int route[3] = {rs.tex[0].route, rs.tex[1].route, rs.tex[2].route};
  frag_from(f, a, route);
  shade_fragment(f, prov ? prov : &a);
  const double centre = (rs.se_cntl & (1u << 27)) ? 0.0 : 0.5;
  f.x = int(std::floor(snap(a.x) + centre));
  f.y = int(std::floor(snap(a.y) + centre));
  if (rs.se_cntl & (1u << 16)) // ZBIAS_ENABLE_POINT: the constant alone
    f.z += rs.zbias_const;
  fragment(f);
}

/**
 * A line: one pixel wide through its snapped endpoints, the last pixel
 * left out; or SE_LINE_WIDTH wide as a quad.
 **/
void CRadeonR100_3D::raster_line(const RadeonVertex &a, const RadeonVertex &b,
                                 const RadeonVertex *prov) {
  if (!prov)
    prov = &b;
  const u32 se = rs.se_cntl;
  float width = float(reg(SE_LINE_WIDTH) & 0xffff) / 16.0f;
  // an anti-aliased line (PP_CNTL ANTI_ALIAS_LINE <24>) is drawn as the
  // quad it covers with the polygons' coverage [inference]
  const bool aa_line = (rs.pp_cntl >> 24) & 1;
  if (aa_line) {
    if (!(se & (1u << 20)) || width < 1.0f)
      width = 1.0f;
    const u32 save_pp = rs.pp_cntl, save_se = rs.se_cntl;
    rs.pp_cntl |= 2u << 24;
    rs.se_cntl |= 1u << 20;
    RadeonVertex a2 = a, b2 = b;
    if (se & (1u << 17)) { // ZBIAS_ENABLE_LINE
      a2.z += rs.zbias_const;
      b2.z += rs.zbias_const;
    }
    rs.se_cntl &= ~(1u << 18); // the line's own offset, not the polygons'
    const u32 save_w = c.R(SE_LINE_WIDTH);
    c.R(SE_LINE_WIDTH) = u32(width * 16.0f);
    rs.pp_cntl &= ~(1u << 24);
    raster_line(a2, b2, prov);
    c.R(SE_LINE_WIDTH) = save_w;
    rs.pp_cntl = save_pp;
    rs.se_cntl = save_se;
    return;
  }
  if ((se & (1u << 20)) && width >= 1.0f &&
      (width > 1.0f || (rs.pp_cntl & (2u << 24)))) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0)
      return;
    const float nx = -dy / len * width / 2, ny = dx / len * width / 2;
    RadeonVertex q[4] = {a, b, b, a};
    q[0].x += nx;
    q[0].y += ny;
    q[1].x += nx;
    q[1].y += ny;
    q[2].x -= nx;
    q[2].y -= ny;
    q[3].x -= nx;
    q[3].y -= ny;
    const u32 save = rs.se_cntl;
    rs.se_cntl |= (3u << 1) | (3u << 3); // no culling of a line's quad
    const RadeonVertex *t1[3] = {&q[0], &q[1], &q[2]};
    const RadeonVertex *t2[3] = {&q[0], &q[2], &q[3]};
    raster_triangle(t1, prov);
    raster_triangle(t2, prov);
    rs.se_cntl = save;
    return;
  }
  // One pixel wide: through the endpoints as the setup engine snapped
  // them (SE_CNTL ROUND_PREC keeps every vertex to a sixteenth of a pixel
  // at best, a line's too), not through the pixels holding them. Along
  // the major axis the pixels whose centres the segment passes, the
  // start's in and the end's out; across it the pixel whose centre is
  // nearest the line there (GL's diamond-exit rule for a segment between
  // diamonds; Direct3D's lines likewise). A tie -- the line exactly
  // between two centres -- goes to the higher coordinate [inference: no
  // R100 source describes its line rasteriser, only the snapped sub-pixel
  // vertices it starts from].
  const double centre = (se & (1u << 27)) ? 0.0 : 0.5;
  const double ax = snap(a.x) + centre, ay = snap(a.y) + centre;
  const double bx = snap(b.x) + centre, by = snap(b.y) + centre;
  const double ldx = bx - ax, ldy = by - ay;
  const double len2 = ldx * ldx + ldy * ldy;
  if (len2 == 0 || !std::isfinite(len2))
    return;
  const bool xmaj = std::fabs(ldx) >= std::fabs(ldy);
  const double s0 = xmaj ? ax : ay, s1 = xmaj ? bx : by;
  const double m0 = xmaj ? ay : ax, dm = xmaj ? ldy : ldx;
  const int dir = s1 > s0 ? 1 : -1;
  // centres c + 0.5 in [s0, s1) going up, in (s1, s0] going down
  const int c0 = dir > 0 ? int(std::ceil(s0 - 0.5)) : int(std::floor(s0 - 0.5));
  const int c1 = dir > 0 ? int(std::ceil(s1 - 0.5)) : int(std::floor(s1 - 0.5));
  const u32 pat = reg(RE_LINE_PATTERN);
  const u32 repeat = std::max(1u, (pat >> 16) & 0xff);
  if (pat & (1u << 29))
    line_stipple_count = 0;
  const int route[3] = {rs.tex[0].route, rs.tex[1].route, rs.tex[2].route};
  for (int cc = c0; cc != c1; cc += dir) {
    const double mc = double(cc) + 0.5;
    const int mi = int(std::floor(m0 + dm * (mc - s0) / (s1 - s0)));
    const int px = xmaj ? cc : mi, py = xmaj ? mi : cc;
    bool draw = true;
    if (rs.pp_cntl & 4) {
      const u32 bit = (line_stipple_count / repeat) & 15;
      draw = (pat >> ((pat & (1u << 28)) ? 15 - bit : bit)) & 1;
    }
    line_stipple_count++;
    if (draw) {
      // the attributes at the pixel centre's projection onto the line
      // (GL's t for a line fragment)
      const double tt = ((px + 0.5 - ax) * ldx + (py + 0.5 - ay) * ldy) / len2;
      const float t = float(std::min(1.0, std::max(0.0, tt)));
      RadeonVertex m = a;
      const float *pa = &a.x, *pb = &b.x;
      float *pm = &m.x;
      const size_t nf = offsetof(RadeonVertex, clip_space) / sizeof(float);
      for (size_t k = 0; k < nf; k++)
        pm[k] = pa[k] + (pb[k] - pa[k]) * t;
      frag_t f;
      frag_from(f, m, route);
      shade_fragment(f, prov);
      f.x = px;
      f.y = py;
      if (se & (1u << 17)) // ZBIAS_ENABLE_LINE: the constant alone
        f.z += rs.zbias_const;
      fragment(f);
    }
  }
}

/**
 * Surfaces. A pitch is in pixels. Micro tiling (RB3D_COLORPITCH
 * COLOR_MICROTILE_ENABLE, the Z buffer's DEPTH_HYPERZ tiling, PP_TXOFFSET
 * MICRO_TILE) packs 32-byte tiles: 8x4 pixels of a byte, 8x2 of two bytes
 * (4x4 for 16-bit depth), 4x2 of four, a tile row's tiles side by side,
 * each tile's rows one after the other (Mesa radeon_tile.c, the family's
 * software tiler) [inference for the R100: Mesa's r100 driver itself
 * never tiles; the depth buffer's HyperZ tiling is taken to be this one].
 **/
u32 CRadeonR100_3D::surface_addr(u32 base, u32 pitch_px, u32 bpp, bool micro,
                                 bool depth, int x, int y) const {
  if (!micro)
    return base + (u32(y) * pitch_px + u32(x)) * bpp;
  u32 tw, th;
  switch (bpp) {
  case 1:
    tw = 8;
    th = 4;
    break;
  case 2:
    tw = depth ? 4 : 8;
    th = depth ? 4 : 2;
    break;
  default:
    tw = 4;
    th = 2;
    break;
  }
  const u32 ux = u32(x), uy = u32(y);
  return base + (uy / th) * th * pitch_px * bpp + (ux / tw) * 32 +
         (uy % th) * tw * bpp + (ux % tw) * bpp;
}

u32 CRadeonR100_3D::surf_read(u32 addr, int bytes, u32 swap) const {
  if (!swap)
    return c.vram_read(addr, bytes);
  const u32 a = addr & ~3u;
  const u32 dw = surf_swap(c.vram_read(a, 4), swap);
  const u32 v = dw >> (8 * (addr & 3));
  return bytes == 4 ? v : v & ((1u << (8 * bytes)) - 1);
}

void CRadeonR100_3D::surf_write(u32 addr, int bytes, u32 data, u32 swap) {
  if (!swap) {
    c.vram_write(addr, bytes, data);
    return;
  }
  const u32 a = addr & ~3u;
  u32 dw = surf_swap(c.vram_read(a, 4), swap);
  const u32 sh = 8 * (addr & 3);
  const u32 m = (bytes == 4 ? 0xffffffffu : (1u << (8 * bytes)) - 1) << sh;
  dw = (dw & ~m) | ((data << sh) & m);
  c.vram_write(a, 4, surf_swap(dw, swap));
}

/**
 * A channel quantised for the colour buffer (RB3D_CNTL, radeon_reg.h):
 * DITHER_ENABLE <2> dithers, by default by carrying each pixel's
 * quantisation error to the next one along the line, with
 * SCALE_DITHER_ENABLE <4> by an ordered pattern; DITHER_INIT <5> starts
 * each line's error afresh. Without dithering ROUND_ENABLE <3> rounds, and
 * without that the value is truncated. Mesa's driconf options name the
 * modes (dither_mode: "horizontal error diffusion", "... reset error at
 * line start", "ordered 2D dithering"; round_mode: truncate or round;
 * radeon_state_init.c); the pattern and the error's arithmetic are not
 * documented: a 4x4 Bayer matrix and the error in the target's units
 * [inference].
 **/
u32 CRadeonR100_3D::quantise(float v, int bits, int ch, int x, int y) {
  const float mx = float((1 << bits) - 1);
  const float val = clamp01(v) * mx;
  const u32 cntl = rs.rb3d_cntl;
  float q;
  if (cntl & (1u << 2)) {
    if (cntl & (1u << 4)) {
      static const int bayer[4][4] = {
          {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
      q = std::floor(val + (float(bayer[y & 3][x & 3]) + 0.5f) / 16.0f);
    } else {
      if (ch < 3) {
        const float t = val + rs.dither_err[ch];
        q = std::floor(t + 0.5f);
        rs.dither_err[ch] = t - std::min(std::max(q, 0.0f), mx);
      } else {
        q = std::floor(val + 0.5f);
      }
    }
  } else if (cntl & (1u << 3)) {
    q = std::floor(val + 0.5f);
  } else {
    q = std::floor(val + 1e-4f);
  }
  return u32(std::min(std::max(q, 0.0f), mx));
}

u32 CRadeonR100_3D::pack_color(const float cc[4], int x, int y) {
  // DITHER_INIT: each line's error starts afresh
  if (y != rs.dither_y && (rs.rb3d_cntl & (1u << 5)))
    rs.dither_err[0] = rs.dither_err[1] = rs.dither_err[2] = 0;
  u32 v;
  switch (rs.color_fmt) {
  case CF_ARGB1555:
    v = (cc[3] >= 0.5f ? 0x8000u : 0) | (quantise(cc[0], 5, 0, x, y) << 10) |
        (quantise(cc[1], 5, 1, x, y) << 5) | quantise(cc[2], 5, 2, x, y);
    break;
  case CF_RGB565:
    v = (quantise(cc[0], 5, 0, x, y) << 11) |
        (quantise(cc[1], 6, 1, x, y) << 5) | quantise(cc[2], 5, 2, x, y);
    break;
  case CF_RGB332:
    v = (quantise(cc[0], 3, 0, x, y) << 5) |
        (quantise(cc[1], 3, 1, x, y) << 2) | quantise(cc[2], 2, 2, x, y);
    break;
  case CF_Y8:
    v = quantise(0.299f * cc[0] + 0.587f * cc[1] + 0.114f * cc[2], 8, 0, x, y);
    break;
  case CF_RGB8:
    v = quantise(cc[0], 8, 0, x, y);
    break;
  case CF_ARGB4444:
    v = (quantise(cc[3], 4, 3, x, y) << 12) |
        (quantise(cc[0], 4, 0, x, y) << 8) |
        (quantise(cc[1], 4, 1, x, y) << 4) | quantise(cc[2], 4, 2, x, y);
    break;
  default:
    v = (quantise(cc[3], 8, 3, x, y) << 24) |
        (quantise(cc[0], 8, 0, x, y) << 16) |
        (quantise(cc[1], 8, 1, x, y) << 8) | quantise(cc[2], 8, 2, x, y);
    break;
  }
  rs.dither_y = y;
  return v;
}

/**
 * The cube map face of direction (s, t, r) and the coordinates on it,
 * 0..1: OpenGL's table (the major axis picks the face; sc, tc and ma as
 * the GL specification's 3.8.6), faces numbered +X, -X, +Y, -Y, +Z, -Z.
 * PP_MISC RIGHT_HAND_CUBE_OGL <24> versus _D3D is not distinguished
 * [inference: Mesa leaves the D3D setting and draws GL cube maps right
 * with it].
 **/
int CRadeonR100_3D::cube_face(float s, float t, float r, float *fs, float *ft) {
  const float as = std::fabs(s), at = std::fabs(t), ar = std::fabs(r);
  int face;
  float sc, tc, ma;
  if (as >= at && as >= ar) {
    face = s >= 0 ? 0 : 1;
    ma = as;
    sc = s >= 0 ? -r : r;
    tc = -t;
  } else if (at >= ar) {
    face = t >= 0 ? 2 : 3;
    ma = at;
    sc = s;
    tc = t >= 0 ? r : -r;
  } else {
    face = r >= 0 ? 4 : 5;
    ma = ar;
    sc = r >= 0 ? s : -s;
    tc = -t;
  }
  if (ma == 0)
    ma = 1;
  *fs = (sc / ma + 1) * 0.5f;
  *ft = (tc / ma + 1) * 0.5f;
  return face;
}
