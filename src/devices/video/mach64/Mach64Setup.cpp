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
 * The Rage Pro's triangle setup engine.
 *
 * Three vertices go into block 1, eight registers each from 1_90 (vertex
 * 1), 1_98 and 1_A0: S, T, W, specular ARGB, Z, ARGB, X_Y. The same
 * registers have a second, packed set of addresses, 1_AB..1_BF (S, T, W of
 * all three; then the three speculars, Zs, ARGBs and X_Ys), which is what
 * atidrab.dll writes. Writing ONE_OVER_AREA (1_97 or 1_9F) or
 * ONE_OVER_AREA_UC (1_C0) draws the triangle, unless SETUP_CNTL (1_C1)
 * bit 0 says not to.
 *
 * The register pages of ATI's guides that define these (RRG-G03300
 * 6-31..6-43) are missing from the copies here; the formats are what
 * atidrab writes, checked against its own numbers:
 *   X_Y   X in bits 31..16, Y in 15..0, signed, 2 fraction bits
 *   S T W IEEE single; the texel address is S/W, T/W
 *   Z     unsigned 16.16
 *   ARGB  8888, specular's alpha the fog factor
 *   ONE_OVER_AREA  IEEE single, 1 / (twice the area in pixels): the
 *         rasteriser here works the area out from the vertices itself.
 * The pixel pipeline behind it is the GT's (SCALE_3D_CNTL, Z_CNTL,
 * ALPHA_TST_CNTL, the texture registers), in Mach64Pipe.hpp.
 *
 * Seen drawing right: dxdiag's Direct3D cube -- Gouraud shading, the
 * specular term, the scissor. Written but not yet seen: texturing (the
 * S/W, T/W scale to texels is a guess), the Z buffer, alpha blending and
 * test, fog. Dithering is not modelled; colours are truncated.
 **/

#include "Mach64.hpp"
#include "Mach64Pipe.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace mach64;

namespace {

// Block 1 dword indices. (The block-0 3D registers these triangles are
// drawn with are in regs_t::gt at their own offsets.)
constexpr u32 V1_BASE = 0x90; ///< vertex n at V1_BASE + 8 * n
constexpr u32 V_S = 0, V_T = 1, V_W = 2, V_SPEC = 3, V_Z = 4, V_ARGB = 5,
              V_XY = 6;
constexpr u32 ONE_OVER_AREA_A = 0x97, ONE_OVER_AREA_B = 0x9f;
constexpr u32 PACKED_FIRST = 0xab, PACKED_LAST = 0xbf;
constexpr u32 ONE_OVER_AREA_UC = 0xc0;
constexpr u32 SETUP_CNTL = 0xc1;
constexpr u32 DONT_START_TRI = 1u << 0;

/// Where a packed address (1_AB..1_BF) keeps its value.
u32 packed_to_vertex(u32 idx) {
  const u32 k = idx - PACKED_FIRST;
  if (k < 9) // S, T, W of vertex k / 3
    return V1_BASE + 8 * (k / 3) + (k % 3);
  static const u32 field[4] = {V_SPEC, V_Z, V_ARGB, V_XY};
  const u32 j = k - 9;
  return V1_BASE + 8 * (j % 3) + field[j / 3];
}

float as_float(u32 v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

s32 sext16(u32 v) { return s32(s16(u16(v))); }

} // namespace

/**
 * A block-1 dword was written: fold the packed addresses onto the
 * vertex registers and start the triangle on ONE_OVER_AREA.
 **/
void CMach64::setup_written(u32 idx) {
  if (idx >= PACKED_FIRST && idx <= PACKED_LAST)
    r.block1[packed_to_vertex(idx)] = r.block1[idx];
  if ((idx == ONE_OVER_AREA_UC || idx == ONE_OVER_AREA_A ||
       idx == ONE_OVER_AREA_B) &&
      !(r.block1[SETUP_CNTL] & DONT_START_TRI))
    setup_triangle();
}

void CMach64::setup_triangle() {
  struct vtx {
    s32 x, y; // quarter pixels
    float s, t, w;
    u32 spec, argb;
    double z;
  } v[3];
  for (int i = 0; i < 3; i++) {
    const u32 *b = &r.block1[V1_BASE + 8 * i];
    v[i].x = sext16(b[V_XY] >> 16);
    v[i].y = sext16(b[V_XY]);
    v[i].s = as_float(b[V_S]);
    v[i].t = as_float(b[V_T]);
    v[i].w = as_float(b[V_W]);
    v[i].spec = b[V_SPEC];
    v[i].argb = b[V_ARGB];
    v[i].z = double(b[V_Z]) / 65536.0;
  }

  // Twice the signed area, in quarter pixels squared.
  const s64 area = s64(v[1].x - v[0].x) * (v[2].y - v[0].y) -
                   s64(v[1].y - v[0].y) * (v[2].x - v[0].x);
  if (area == 0)
    return;
  if (area < 0)
    std::swap(v[1], v[2]);
  const double inv_area = 1.0 / double(area < 0 ? -area : area);

  // The destination, its scissor and the pipe's settings.
  const u32 dst_offset = (r.dst_off_pitch & 0xfffff) * 8;
  const u32 dst_pitch = ((r.dst_off_pitch >> 22) & 0x3ff) * 8;
  const int dst_fmt = int(r.dp_pix_width & 0xf);
  const int dst_bytes = dst_fmt == BPP_32 ? 4 : dst_fmt >= BPP_15 ? 2 : 1;
  const int sc_left = int(r.sc_left_right & 0x1fff);
  const int sc_right = int((r.sc_left_right >> 16) & 0x1fff);
  const int sc_top = int(r.sc_top_bottom & 0x7fff);
  const int sc_bottom = int((r.sc_top_bottom >> 16) & 0x7fff);

  const u32 s3d = r.gt[SCALE_3D_CNTL >> 2];
  const int fcn = int(s3d >> 6) & 3;
  const int alpha_fog = int(s3d >> 11) & 3;
  const int blend_src = int(s3d >> 16) & 7;
  const int blend_dst = int(s3d >> 19) & 7;
  const int light = int(s3d >> 22) & 3;
  const bool textured = (s3d >> 30) & 1;
  const u32 atst = r.gt[ALPHA_TST_CNTL >> 2];
  const bool specular = (atst >> 31) & 1;
  const bool alpha_test = atst & 1;
  const int alpha_fn = int(atst >> 4) & 7;
  const u32 alpha_ref = (atst >> 16) & 0xff;
  const u32 z_cntl = r.gt[Z_CNTL >> 2];
  const bool z_en = z_cntl & 1;
  const int z_test = int(z_cntl >> 4) & 7;
  const bool z_write = (z_cntl >> 8) & 1;
  const u32 z_op = r.gt[Z_OFF_PITCH >> 2];
  const u32 z_offset = (z_op & 0xfffff) * 8;
  const u32 z_pitch = ((z_op >> 22) & 0x3ff) * 8;
  const u32 tsp = r.gt[TEX_SIZE_PITCH >> 2];
  const int tex_pitch = 1 << (tsp & 0xf);
  const int tex_w = 1 << ((tsp >> 4) & 0xf);
  const int tex_h = 1 << ((tsp >> 8) & 0xf);
  const int tex_fmt = int(r.dp_pix_width >> 28) & 0xf;
  const u32 tex_off = r.gt[TEX_0_OFF >> 2];
  const u32 mask = vram_mask();
  u8 *mem = vga.memory;
  const u32 flat = (r.block1[SETUP_CNTL] >> 3) & 3; // 0: Gouraud

  // Pixels whose centres fall inside, with a top-left rule for the edges.
  const int x0 = std::max(
      sc_left, int(std::floor(std::min({v[0].x, v[1].x, v[2].x}) / 4.0)));
  const int x1 = std::min(
      sc_right, int(std::ceil(std::max({v[0].x, v[1].x, v[2].x}) / 4.0)));
  const int y0 = std::max(
      sc_top, int(std::floor(std::min({v[0].y, v[1].y, v[2].y}) / 4.0)));
  const int y1 = std::min(
      sc_bottom, int(std::ceil(std::max({v[0].y, v[1].y, v[2].y}) / 4.0)));
  auto edge = [](const vtx &a, const vtx &b, s64 px, s64 py) {
    return s64(b.x - a.x) * (py - a.y) - s64(b.y - a.y) * (px - a.x);
  };
  auto top_left = [](const vtx &a, const vtx &b) {
    return (b.y < a.y) || (b.y == a.y && b.x > a.x);
  };
  const bool tl0 = top_left(v[1], v[2]), tl1 = top_left(v[2], v[0]),
             tl2 = top_left(v[0], v[1]);

  for (int py = y0; py <= y1; py++) {
    for (int px = x0; px <= x1; px++) {
      const s64 cx = 4 * px + 2, cy = 4 * py + 2;
      const s64 e0 = edge(v[1], v[2], cx, cy);
      const s64 e1 = edge(v[2], v[0], cx, cy);
      const s64 e2 = edge(v[0], v[1], cx, cy);
      if (e0 < 0 || e1 < 0 || e2 < 0)
        continue;
      if ((e0 == 0 && !tl0) || (e1 == 0 && !tl1) || (e2 == 0 && !tl2))
        continue;
      const double l0 = double(e0) * inv_area, l1 = double(e1) * inv_area;
      const double l2 = 1.0 - l0 - l1;
      auto lerp8 = [&](u32 a, u32 b, u32 c, int sh) {
        return clamp8(
            s32(std::lround(l0 * ((a >> sh) & 0xff) + l1 * ((b >> sh) & 0xff) +
                            l2 * ((c >> sh) & 0xff))));
      };
      auto lerp_argb = [&](u32 a, u32 b, u32 c) {
        return (lerp8(a, b, c, 24) << 24) | (lerp8(a, b, c, 16) << 16) |
               (lerp8(a, b, c, 8) << 8) | lerp8(a, b, c, 0);
      };

      u32 color = flat ? v[flat - 1].argb
                       : (fcn == 3 ? lerp_argb(v[0].argb, v[1].argb, v[2].argb)
                                   : v[0].argb);
      if (textured) {
        const double w = l0 * v[0].w + l1 * v[1].w + l2 * v[2].w;
        const double s = l0 * v[0].s + l1 * v[1].s + l2 * v[2].s;
        const double t = l0 * v[0].t + l1 * v[1].t + l2 * v[2].t;
        int tu = w != 0.0 ? int(std::floor(s / w * tex_w)) : 0;
        int tv = w != 0.0 ? int(std::floor(t / w * tex_h)) : 0;
        tu &= tex_w - 1;
        tv &= tex_h - 1;
        const u32 addr =
            tex_off + u32((tv * tex_pitch + tu) * pipe_texel_bytes(tex_fmt));
        const u32 texel =
            pipe_texel(mem, mask, addr, tex_fmt, &vga.dac.color[0],
                       (r.dac_cntl & DAC_8BIT_EN) != 0);
        switch (light) {
        case 0: // the texel
          color = texel;
          break;
        case 1: { // modulate
          u32 out = 0;
          for (int i = 0; i < 4; i++)
            out |=
                ((((texel >> (8 * i)) & 0xff) * ((color >> (8 * i)) & 0xff)) /
                 255)
                << (8 * i);
          color = out;
          break;
        }
        default: { // the texel over the colour, by its alpha
          const u32 ta = texel >> 24;
          u32 out = color & 0xff000000u;
          for (int i = 0; i < 3; i++) {
            const u32 tc = (texel >> (8 * i)) & 0xff;
            const u32 cc = (color >> (8 * i)) & 0xff;
            out |= ((tc * ta + cc * (255 - ta)) / 255) << (8 * i);
          }
          color = out;
        }
        }
      }
      const u32 spec = lerp_argb(v[0].spec, v[1].spec, v[2].spec);
      if (specular)
        color = (color & 0xff000000u) | pipe_add_sat(color, spec);
      if (alpha_fog == 2) { // fog: the specular alpha is the fog factor
        const u32 f = spec >> 24;
        const u32 fog = r.dp_frgd_clr;
        u32 out = color & 0xff000000u;
        for (int i = 0; i < 3; i++) {
          const u32 cc = (color >> (8 * i)) & 0xff;
          const u32 fc = (fog >> (8 * i)) & 0xff;
          out |= ((cc * f + fc * (255 - f)) / 255) << (8 * i);
        }
        color = out;
      }
      if (alpha_test && !pipe_z_pass(alpha_fn, color >> 24, alpha_ref))
        continue;

      if (z_en) {
        const u32 za = z_offset + (u32(py) * z_pitch + u32(px)) * 2;
        const u32 zold = mem[za & mask] | (mem[(za + 1) & mask] << 8);
        const double zf = l0 * v[0].z + l1 * v[1].z + l2 * v[2].z;
        const u32 znew = u32(std::clamp(zf, 0.0, 65535.0));
        if (!pipe_z_pass(z_test, znew, zold))
          continue;
        if (z_write) {
          mem[za & mask] = u8(znew);
          mem[(za + 1) & mask] = u8(znew >> 8);
        }
      }

      const u32 da = dst_offset + (u32(py) * dst_pitch + u32(px)) * dst_bytes;
      u32 old = 0;
      for (int i = 0; i < dst_bytes; i++)
        old |= u32(mem[(da + i) & mask]) << (8 * i);
      if (alpha_fog == 1) { // blend with the destination
        const u32 d = pipe_unpack(old, dst_fmt);
        const u32 sa = color >> 24;
        color = (color & 0xff000000u) |
                pipe_add_sat(pipe_factor(blend_src, color, d, sa),
                             pipe_factor(blend_dst, d, color, sa));
      }
      const u32 out =
          (pipe_pack(color, dst_fmt) & r.write_mask) | (old & ~r.write_mask);
      for (int i = 0; i < dst_bytes; i++)
        mem[(da + i) & mask] = u8(out >> (8 * i));
    }
  }
  state.vga_mem_updated = 1;
}
