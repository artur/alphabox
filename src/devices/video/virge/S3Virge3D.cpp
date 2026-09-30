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
 * S3 ViRGE S3d engine, 3D commands: triangles and 3D lines.
 *
 * A triangle arrives set up (S3 ViRGE data book, 15.4.5 and 19.4): its
 * vertices numbered bottom to top, it is rendered scanline by scanline
 * from Y START upwards, Y01 lines along sides 02 and 01, then Y12 lines
 * along sides 02 and 12. X is 12.20 fixed point, each edge stepping by its
 * dX/dY a scanline; a scanline covers the pixels from the 02 side's X
 * (rounded up) to the other side's (rounded up, exclusive), left to right
 * or right to left as bit 31 of the count register says. Every attribute
 * -- colour (S8.7), Z (S16.15), U and V, W (S12.19), the mipmap level D
 * (S4.27) -- has a start value at the 02 side of the first scanline, a
 * step a scanline along that side and a step a pixel in the drawing
 * direction, so a scanline's first pixel is corrected by how far its
 * centre lies from where the 02 side crosses.
 *
 * Each pixel then goes through the pipeline of the data book's figure
 * 15-7: Z compare (before any colouring), texture filter (nearest,
 * bilinear, between mipmap levels), Blend4 colour generation, lighting
 * (complex reflection, modulate, decal), fog towards FOG_CLR by the source
 * alpha, alpha blending with the destination, and the store -- 8-bit
 * palette index, ZRGB1555 or RGB888 -- with the Z update after it.
 *
 * Texture coordinates are fractions of the texture: U and V hold the
 * texel at mipmap level L in their bits 27-L and up, with the bilinear
 * weight in the 8 bits below (86Box's reading of the S(4+s).(27-s)
 * format, which the S12.8.11 of the non-perspective case agrees with for
 * a 256-texel texture). Perspective-corrected triangles hold U/W, V/W and
 * 1/W, and the ViRGE/DX divides them back per pixel.
 *
 * The data book leaves open where exactly a pixel's sample point is and
 * how U and V scale; those follow 86Box's vid_s3_virge.c, and were then
 * checked against Direct3D's own software rasteriser with the Windows 2000
 * driver's register traffic (test/tools/d3d_check.sh).
 **/

#include "S3Virge.hpp"

#include <cmath>

using namespace virge;

namespace {

inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/// Bytes a texel takes, by the texel colour format. The two Blend4 formats
/// take a byte each and use one nibble of it, so that two textures can
/// share the bytes.
const int texel_bytes[8] = {4, 2, 2, 1, 1, 1, 1, 2};

} // namespace

/// Everything a 3D command draws with, decoded once from its registers.
struct CS3Virge::S3dCtx {
  u32 cmd;
  int op;
  u32 dest_base, dest_stride, z_base, z_stride;
  s32 clip_l, clip_r, clip_t, clip_b;
  bool clip;
  int dest_fmt; ///< 0 8 bpp, 1 ZRGB1555, 2 RGB888
  bool use_z;   ///< normal Z buffering with a compare that can pass
  int z_comp;
  bool z_update;
  bool textured, lit, perspective;
  int tex_fmt, filter, blend_mode;
  int max_level;
  u32 level_base[10];
  bool wrap;
  u32 border;
  s32 tbu, tbv;
  u32 tex_stride; ///< a flat texture's row pitch, 0 for a mipmap
  bool fog;
  u8 fog_r, fog_g, fog_b;
  int abc; ///< bits 19..18
  u32 color0, color1;
};

void CS3Virge::s3d_start(u32 cmd) {
  const int op = int((cmd >> CMD_COMMAND_SHIFT) & 15);
  if (m_trace_cmd) {
    printf("%s: 3D %d cmd %08x", devid_string, op, cmd);
    const u32 first = op == 8 ? 0xb0d4 : 0xb4d4;
    const u32 last = op == 8 ? 0xb17c : 0xb57c;
    for (u32 a = first; a <= last; a += 4) {
      if (a >= 0xb100 && a < 0xb144)
        continue;
      if (a >= 0xb500 && a < 0xb504)
        continue;
      u32 canon = a;
      if (a >= 0xb0d4 && a <= 0xb0fc)
        canon = a + 0x400;
      printf(" %04x=%08x", a, M(canon));
    }
    printf("\n");
  }
  switch (op) {
  case 0: // Gouraud shaded triangle
  case 1: // lit texture triangle
  case 2: // unlit texture triangle
  case 5: // lit texture triangle, perspective
  case 6: // unlit texture triangle, perspective
    s3d_triangle();
    break;
  case 8:
    s3d_line();
    break;
  case 15:
    break;
  default: {
    char what[64];
    snprintf(what, sizeof(what), "3D command %d", op);
    unimplemented(what);
    return;
  }
  }
  state.vga_mem_updated = 1;
  std::lock_guard<std::mutex> lock(m_int_lock);
  r.subsys_status |= INT_3D_DONE;
  update_int_line();
}

void CS3Virge::s3d_setup(S3dCtx &c) const {
  const u32 cmd = M(S3D_CMD);
  c.cmd = cmd;
  c.op = int((cmd >> CMD_COMMAND_SHIFT) & 15);
  const u32 mask = vram_mask() & ~7u;
  c.dest_base = M(S3D_DEST_BASE) & mask;
  c.dest_stride = (M(S3D_DEST_STRIDE) >> 16) & 0xff8;
  c.z_base = M(S3D_Z_BASE) & mask;
  c.z_stride = M(S3D_Z_STRIDE) & 0xff8;
  c.clip_l = (M(S3D_CLIP_LR) >> 16) & 0x7ff;
  c.clip_r = M(S3D_CLIP_LR) & 0x7ff;
  c.clip_t = (M(S3D_CLIP_TB) >> 16) & 0x7ff;
  c.clip_b = M(S3D_CLIP_TB) & 0x7ff;
  c.clip = (cmd & CMD_CLIP) != 0;
  c.dest_fmt = int((cmd >> 2) & 7);
  const int zmode = int((cmd >> 24) & 3);
  c.z_comp = int((cmd >> 20) & 7);
  c.use_z = zmode == 0;
  c.z_update = (cmd & (1u << 23)) != 0;
  c.textured = c.op == 1 || c.op == 2 || c.op == 5 || c.op == 6;
  c.lit = c.op == 1 || c.op == 5;
  c.perspective = c.op == 5 || c.op == 6;
  c.tex_fmt = int((cmd >> 5) & 7);
  c.filter = int((cmd >> 12) & 7);
  c.blend_mode = int((cmd >> 15) & 3);
  c.max_level = int((cmd >> 8) & 15);
  if (c.max_level > 9)
    c.max_level = 9;
  c.wrap = (cmd & (1u << 26)) != 0;
  c.tex_stride = c.filter >= 4 ? (M(S3D_DEST_STRIDE) & 0xff8) : 0;
  c.border = M(S3D_TEX_BORDER);
  c.fog = (cmd & (1u << 17)) != 0;
  c.fog_b = u8(M(S3D_FOG_COLOR));
  c.fog_g = u8(M(S3D_FOG_COLOR) >> 8);
  c.fog_r = u8(M(S3D_FOG_COLOR) >> 16);
  c.abc = int((cmd >> 18) & 3);
  c.color0 = M(S3D_COLOR0);
  c.color1 = M(S3D_COLOR1);
  // Mipmap levels are stored largest first from TEX_BASE: level L (2^L
  // texels a side) follows every larger one.
  u32 base = M(S3D_TEX_BASE) & mask;
  for (int lv = 9; lv >= 0; lv--) {
    c.level_base[lv] = base;
    if (lv <= c.max_level)
      base += (1u << (2 * lv)) * u32(texel_bytes[c.tex_fmt]);
  }
  // TBU/TBV are (4+s).(16-s): shifted to the coordinates' 27-s fraction.
  c.tbu = s32(M(0xb508) & 0xfffff) << 11;
  c.tbv = s32(M(0xb504) & 0xfffff) << 11;
}

/// One texel of level `lv` as ARGB, wrapping or taking the border colour.
CS3Virge::S3dRgba CS3Virge::s3d_texel(const S3dCtx &c, int lv, s32 iu,
                                      s32 iv) const {
  const s32 size = 1 << lv;
  u32 raw;
  bool border = false;
  if (c.wrap) {
    iu &= size - 1;
    iv &= size - 1;
  } else if (iu < 0 || iv < 0 || iu >= size || iv >= size) {
    border = true;
  }
  // A flat (not mipmapped) texture's rows are the source stride apart
  // (DEST_SRC_STR bits 11..0); a mipmap level's are its width.
  const u32 bytes = u32(texel_bytes[c.tex_fmt]);
  const u32 row = c.tex_stride ? c.tex_stride : u32(size) * bytes;
  const u32 at = c.level_base[lv] + u32(iv) * row + u32(iu) * bytes;
  const u8 *vram = vga.memory;
  const u32 mask = vram_mask();
  auto rd = [&](u32 a, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++)
      v |= u32(vram[(a + i) & mask]) << (8 * i);
    return v;
  };
  switch (c.tex_fmt) {
  case 0:
    raw = border ? c.border : rd(at, 4);
    break;
  case 1:
  case 2:
  case 7:
    raw = border ? c.border : rd(at, 2);
    break;
  case 4:
  case 5:
    raw = border ? c.border : vram[at & mask];
    raw = (c.tex_fmt == 5 ? raw >> 4 : raw) & 0x0f;
    break;
  default:
    raw = border ? c.border : vram[at & mask];
    break;
  }
  S3dRgba t;
  auto blend4 = [&](int f) {
    const int w = f * 255 / 15;
    auto mix = [&](int sh) {
      const int a = int((c.color0 >> sh) & 0xff),
                b = int((c.color1 >> sh) & 0xff);
      return (a * (255 - w) + b * w) / 255;
    };
    t.r = mix(16);
    t.g = mix(8);
    t.b = mix(0);
  };
  switch (c.tex_fmt) {
  case 0: // ARGB8888
    t.a = int(raw >> 24);
    t.r = int((raw >> 16) & 0xff);
    t.g = int((raw >> 8) & 0xff);
    t.b = int(raw & 0xff);
    break;
  case 1: // ARGB4444
    t.a = int((raw >> 12) & 15) * 17;
    t.r = int((raw >> 8) & 15) * 17;
    t.g = int((raw >> 4) & 15) * 17;
    t.b = int(raw & 15) * 17;
    break;
  case 2: { // ARGB1555
    auto w5 = [](u32 v) { return int(((v & 31) << 3) | ((v & 31) >> 2)); };
    t.a = (raw & 0x8000) ? 255 : 0;
    t.r = w5(raw >> 10);
    t.g = w5(raw >> 5);
    t.b = w5(raw);
    break;
  }
  case 3: // Alpha4, Blend4
    t.a = int((raw >> 4) & 15) * 17;
    blend4(int(raw & 15));
    break;
  case 4:
  case 5: // Blend4
    t.a = 255;
    blend4(int(raw & 15));
    break;
  case 6: { // palettized: the DAC's colour (only decal makes sense)
    const u8 *p = &vga.dac.color[3 * (raw & 0xff)];
    t.a = 255;
    t.r = (p[0] & 0x3f) << 2 | (p[0] & 0x3f) >> 4;
    t.g = (p[1] & 0x3f) << 2 | (p[1] & 0x3f) >> 4;
    t.b = (p[2] & 0x3f) << 2 | (p[2] & 0x3f) >> 4;
    break;
  }
  default: // YUV: not modelled
    t.a = 255;
    t.r = t.g = t.b = int(raw & 0xff);
    break;
  }
  return t;
}

/// The texture colour at (u, v) of level `lv`, nearest or bilinear.
CS3Virge::S3dRgba CS3Virge::s3d_sample_level(const S3dCtx &c, int lv, s32 u,
                                             s32 v, bool bilinear) const {
  const int shift = 27 - lv;
  // The ViRGE's and VX's texel grid sits half a texel from the DX's: they
  // take "the texel nearest to the programmed texture location" (ViRGE
  // data book, 15.4.8.1), texel centres on whole coordinates, where the
  // DX truncates. The Windows driver agrees: for the same triangle it
  // starts U and V half a texel lower on the ViRGE and VX than on the DX.
  if (m_chip.half_texel) {
    u += s32(1) << (shift - 1);
    v += s32(1) << (shift - 1);
  }
  if (!bilinear)
    return s3d_texel(c, lv, u >> shift, v >> shift);
  // The four texels nearest the sample (data book figure 15-8): weights
  // are measured from texel centres, half a texel in. Direct3D's
  // reference rasteriser agrees; the Windows driver adds no half-texel
  // offset of its own (TBU/TBV stay at its 1/256-texel rounding bias).
  u -= s32(1) << (shift - 1);
  v -= s32(1) << (shift - 1);
  const s32 iu = u >> shift, iv = v >> shift;
  const int fu = int((u >> (shift - 8)) & 0xff);
  const int fv = int((v >> (shift - 8)) & 0xff);
  const S3dRgba t00 = s3d_texel(c, lv, iu, iv);
  const S3dRgba t10 = s3d_texel(c, lv, iu + 1, iv);
  const S3dRgba t01 = s3d_texel(c, lv, iu, iv + 1);
  const S3dRgba t11 = s3d_texel(c, lv, iu + 1, iv + 1);
  const int w00 = (256 - fu) * (256 - fv), w10 = fu * (256 - fv);
  const int w01 = (256 - fu) * fv, w11 = fu * fv;
  S3dRgba t;
  t.r = (t00.r * w00 + t10.r * w10 + t01.r * w01 + t11.r * w11) >> 16;
  t.g = (t00.g * w00 + t10.g * w10 + t01.g * w01 + t11.g * w11) >> 16;
  t.b = (t00.b * w00 + t10.b * w10 + t01.b * w01 + t11.b * w11) >> 16;
  t.a = (t00.a * w00 + t10.a * w10 + t01.a * w01 + t11.a * w11) >> 16;
  return t;
}

/**
 * The texture colour for one pixel: the filter mode picks the levels and
 * how many texels; D (S4.27) counts levels down from the largest.
 **/
CS3Virge::S3dRgba CS3Virge::s3d_sample(const S3dCtx &c, s32 u, s32 v,
                                       s32 d) const {
  u += c.tbu;
  v += c.tbv;
  const bool mip = c.filter < 4;
  if (!mip)
    return s3d_sample_level(c, c.max_level, u, v, c.filter == 6);
  int lv = c.max_level, frac = 0;
  if (d > 0) {
    lv = c.max_level - int(d >> 27);
    frac = int((d >> 19) & 0xff);
  }
  if (lv < 0)
    lv = 0, frac = 0;
  const bool bilinear = c.filter == 2 || c.filter == 3;
  const S3dRgba a = s3d_sample_level(c, lv, u, v, bilinear);
  if ((c.filter != 1 && c.filter != 3) || lv == 0 || frac == 0)
    return a;
  const S3dRgba b = s3d_sample_level(c, lv - 1, u, v, bilinear);
  S3dRgba t;
  t.r = (a.r * (256 - frac) + b.r * frac) >> 8;
  t.g = (a.g * (256 - frac) + b.g * frac) >> 8;
  t.b = (a.b * (256 - frac) + b.b * frac) >> 8;
  t.a = (a.a * (256 - frac) + b.a * frac) >> 8;
  return t;
}

/**
 * One pixel after its Z compare passed: colour it, fog it, blend it,
 * store it. Colours are S8.7; the interpolators may overshoot, so they
 * are clamped here.
 **/
void CS3Virge::s3d_pixel(const S3dCtx &c, u32 dest, const s32 *attr) {
  S3dRgba src = {clamp8(attr[0] >> 7), clamp8(attr[1] >> 7),
                 clamp8(attr[2] >> 7), clamp8(attr[3] >> 7)};
  S3dRgba out = src;
  if (c.textured) {
    s32 u = attr[4], v = attr[5];
    if (c.perspective) {
      // U/W and V/W over 1/W (S12.19), back to texture fractions. The
      // ViRGE and ViRGE/VX hold U/W and V/W with four more fraction bits
      // than the DX and its successors.
      const s64 w = attr[6];
      if (w != 0) {
        const s64 inv = (s64(1) << 46) / w;
        const int shift = 8 + m_chip.persp_extra_bits + c.max_level;
        u = s32((s64(u) * inv) >> shift);
        v = s32((s64(v) * inv) >> shift);
      }
    }
    S3dRgba t = s3d_sample(c, u, v, attr[7]);
    if (c.lit) {
      switch (c.blend_mode) {
      case 0: // complex reflection: add
        t.r = clamp8(t.r + src.r);
        t.g = clamp8(t.g + src.g);
        t.b = clamp8(t.b + src.b);
        break;
      case 1: // modulate
        t.r = t.r * src.r / 255;
        t.g = t.g * src.g / 255;
        t.b = t.b * src.b / 255;
        break;
      default: // decal
        break;
      }
    }
    out = t;
  }
  // Fog: towards FOG_CLR by the source (Gouraud) alpha.
  if (c.fog) {
    const int a = src.a;
    out.r = (out.r * a + c.fog_r * (255 - a)) / 255;
    out.g = (out.g * a + c.fog_g * (255 - a)) / 255;
    out.b = (out.b * a + c.fog_b * (255 - a)) / 255;
  }
  const u8 *vram = vga.memory;
  const u32 mask = vram_mask();
  if (c.abc >= 2) {
    // 10: the alpha the pixel has at this stage; 11: the source alpha.
    const int a = c.abc == 3 ? src.a : out.a;
    int dr = 0, dg = 0, db = 0;
    if (c.dest_fmt == 1) {
      const u32 p = vram[dest & mask] | (u32(vram[(dest + 1) & mask]) << 8);
      dr = int(((p >> 10) & 31) << 3 | ((p >> 10) & 31) >> 2);
      dg = int(((p >> 5) & 31) << 3 | ((p >> 5) & 31) >> 2);
      db = int((p & 31) << 3 | (p & 31) >> 2);
    } else if (c.dest_fmt == 2) {
      db = vram[dest & mask];
      dg = vram[(dest + 1) & mask];
      dr = vram[(dest + 2) & mask];
    }
    out.r = (out.r * a + dr * (255 - a)) / 255;
    out.g = (out.g * a + dg * (255 - a)) / 255;
    out.b = (out.b * a + db * (255 - a)) / 255;
  }
  switch (c.dest_fmt) {
  case 0: // palettized: no colour arithmetic; the red channel as the index
    vga.memory[dest & mask] = u8(out.r);
    break;
  case 1: {
    const u32 p =
        (u32(out.r >> 3) << 10) | (u32(out.g >> 3) << 5) | u32(out.b >> 3);
    vga.memory[dest & mask] = u8(p);
    vga.memory[(dest + 1) & mask] = u8(p >> 8);
    break;
  }
  default:
    vga.memory[dest & mask] = u8(out.b);
    vga.memory[(dest + 1) & mask] = u8(out.g);
    vga.memory[(dest + 2) & mask] = u8(out.r);
    break;
  }
}

/// The Z compare: pass when Zs <op> Zzb (data book 15.4.6).
static bool z_pass(int comp, u32 zs, u32 zb) {
  switch (comp) {
  case 0:
    return false;
  case 1:
    return zs > zb;
  case 2:
    return zs == zb;
  case 3:
    return zs >= zb;
  case 4:
    return zs < zb;
  case 5:
    return zs != zb;
  case 6:
    return zs <= zb;
  default:
    return true;
  }
}

/**
 * One scanline of a triangle or a 3D line: pixels [x, xe) in the drawing
 * direction, `attr` the attributes at the first pixel (colour r, g, b, a;
 * u, v, w, d; z), `dx` their steps a pixel.
 **/
void CS3Virge::s3d_span(const S3dCtx &c, s32 y, s32 x, s32 xe, int step,
                        s32 *attr, const s32 *dx) {
  const int bytes = c.dest_fmt == 0 ? 1 : c.dest_fmt == 1 ? 2 : 3;
  const u8 *zb = vga.memory;
  const u32 mask = vram_mask();
  for (; x != xe; x += step) {
    bool draw = !c.clip || (x >= c.clip_l && x <= c.clip_r);
    if (x < 0 || x > 2047)
      draw = false;
    if (draw) {
      const u32 za = c.z_base + u32(y) * c.z_stride + u32(x) * 2;
      s64 zv = s64(attr[8]) >> 15;
      const u32 zs = zv < 0 ? 0 : zv > 0xffff ? 0xffff : u32(zv);
      bool pass = true;
      if (c.use_z)
        pass = z_pass(c.z_comp, zs,
                      zb[za & mask] | (u32(zb[(za + 1) & mask]) << 8));
      if (pass) {
        s3d_pixel(c, c.dest_base + u32(y) * c.dest_stride + u32(x) * bytes,
                  attr);
        if (c.use_z && c.z_update) {
          vga.memory[za & mask] = u8(zs);
          vga.memory[(za + 1) & mask] = u8(zs >> 8);
        }
      }
    }
    for (int i = 0; i < 9; i++)
      attr[i] += dx[i];
  }
}

void CS3Virge::s3d_triangle() {
  S3dCtx c;
  s3d_setup(c);
  // Attributes: r, g, b, a (S8.7), u, v, w, d, z. Colour registers pack
  // two 16-bit values; the deltas are signed.
  auto lo = [](u32 v) { return s32(s16(v & 0xffff)); };
  auto hi = [](u32 v) { return s32(s16(v >> 16)); };
  s32 base[9] = {
      s32(M(0xb550) & 0xffff), s32(M(0xb54c) >> 16), s32(M(0xb54c) & 0xffff),
      s32(M(0xb550) >> 16),    s32(M(0xb538)),       s32(M(0xb534)),
      s32(M(0xb514)),          s32(M(0xb530)),       s32(M(0xb55c))};
  const s32 dx[9] = {lo(M(0xb540)),  hi(M(0xb53c)),  lo(M(0xb53c)),
                     hi(M(0xb540)),  s32(M(0xb520)), s32(M(0xb51c)),
                     s32(M(0xb50c)), s32(M(0xb518)), s32(M(0xb554))};
  const s32 dy[9] = {lo(M(0xb548)),  hi(M(0xb544)),  lo(M(0xb544)),
                     hi(M(0xb548)),  s32(M(0xb52c)), s32(M(0xb528)),
                     s32(M(0xb510)), s32(M(0xb524)), s32(M(0xb558))};
  const u32 counts = M(0xb57c);
  const int step = (counts & 0x80000000u) ? 1 : -1;
  const s32 y01 = s32((counts >> 16) & 0x7ff), y12 = s32(counts & 0x7ff);
  s32 y = s32(M(0xb578) & 0x7ff);
  s64 x1 = s32(M(0xb574));
  const s64 dx02 = s32(M(0xb570));

  for (int part = 0; part < 2; part++) {
    s64 x2 = s32(M(part == 0 ? 0xb56c : 0xb564));
    const s64 dx2 = s32(M(part == 0 ? 0xb568 : 0xb560));
    const s32 lines = part == 0 ? y01 : y12;
    for (s32 n = 0; n < lines; n++, y--) {
      const bool row_in =
          y >= 0 && (!c.clip || (y >= c.clip_t && y <= c.clip_b));
      if (row_in) {
        // The first pixel at or past the 02 side in the drawing direction
        // (pixel x covers [x, x+1) in 12.20 terms, sampled at x).
        const s64 one = s64(1) << 20;
        s64 xs = (x1 + one - 1) >> 20, xe = (x2 + one - 1) >> 20;
        if (step < 0)
          xs--, xe--;
        if ((step > 0 && xs < xe) || (step < 0 && xs > xe)) {
          // How far the first pixel is from the edge, in the drawing
          // direction, as a fraction of a pixel.
          const s64 dist = step > 0 ? (xs << 20) - x1 : x1 - (xs << 20);
          s32 attr[9];
          for (int i = 0; i < 9; i++)
            attr[i] = s32(base[i] + ((s64(dx[i]) * dist) >> 20));
          s3d_span(c, y, s32(xs), s32(xe), step, attr, dx);
        }
      }
      x1 += dx02;
      x2 += dx2;
      for (int i = 0; i < 9; i++)
        base[i] += dy[i];
    }
  }
}

/**
 * A 3D line: a 2D line's scanline walk (bottom up, XSTART stepping by DX
 * a scanline, END0/END1 bounding the first and last scanlines) with
 * Gouraud colour and Z stepping a pixel along it.
 **/
void CS3Virge::s3d_line() {
  S3dCtx c;
  s3d_setup(c);
  c.textured = false;
  auto lo = [](u32 v) { return s32(s16(v & 0xffff)); };
  auto hi = [](u32 v) { return s32(s16(v >> 16)); };
  s32 attr[9] = {s32(M(0xb150) & 0xffff),
                 s32(M(0xb14c) >> 16),
                 s32(M(0xb14c) & 0xffff),
                 s32(M(0xb150) >> 16),
                 0,
                 0,
                 0,
                 0,
                 s32(M(0xb15c))};
  const s32 d[9] = {
      lo(M(0xb148)), hi(M(0xb144)), lo(M(0xb144)), hi(M(0xb148)), 0, 0, 0, 0,
      s32(M(0xb158))};
  const s32 e0 = s32((M(0xb16c) >> 16) & 0x7ff), e1 = s32(M(0xb16c) & 0x7ff);
  const s32 dxdy = s32(M(0xb170));
  s32 xs = s32(M(0xb174));
  s32 y = s32(M(0xb178) & 0x7ff);
  const s32 count = s32(M(0xb17c) & 0x7ff);
  const bool right = (M(0xb17c) & 0x80000000u) != 0;
  const int step = right ? 1 : -1;
  for (s32 n = count; n > 0; n--, y--) {
    const bool first = n == count, last = n == 1;
    s32 x = xs >> 20;
    const s32 next = last ? e1 + step : (xs + dxdy) >> 20;
    xs += dxdy;
    if (first && (right ? x < e0 : x > e0))
      x = e0;
    if (right ? x > next : x < next)
      continue;
    const bool row_in = y >= 0 && (!c.clip || (y >= c.clip_t && y <= c.clip_b));
    do {
      const bool before = right ? x < e0 : x > e0;
      const bool after = right ? x > e1 : x < e1;
      if (!before && !after && row_in)
        s3d_span(c, y, x, x + step, step, attr, d);
      else
        for (int i = 0; i < 9; i++)
          attr[i] += d[i];
      if (x != next)
        x += step;
    } while (x != next);
  }
}
