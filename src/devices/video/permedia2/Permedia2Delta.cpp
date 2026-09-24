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
 * The Permedia 2's delta unit (Programmer's Reference 4.2): triangle and
 * line set-up from three stored vertices. It computes what a driver would
 * otherwise send -- the rasterizer's edges and every enabled parameter's
 * start value and its X and dominant-edge Y derivatives -- loads them, and
 * sends Render and, past a knee, ContinueNewSub.
 *
 * Vertices arrive at 0x200 + 0x10 * vertex (fixed point) or 0x230 + 0x10 *
 * vertex (IEEE single), fourteen parameters each (Table 4-1). Coverage is
 * Direct3D 7's: pixel (x, y) is sampled at integer coordinates (x, y),
 * and drawn when inside, the top and left edges in, the bottom and right
 * edges out. The triangle is moved half a pixel onto the rasterizer's
 * half-pixel centres; the rasterizer draws [floor(xdom), floor(xsub)) per
 * scanline, so each edge is handed over a further half pixel to the right,
 * less one bit, which makes that [ceil(xl - 0.5), ceil(xr - 0.5)).
 * Parameters start at that biased dominant edge and the rasterizer's
 * sub-pixel correction takes them to each span's first pixel centre; the
 * edges go over at 32.32 and texture coordinates as planes, so that
 * pixels exactly on an edge land where Direct3D's reference puts them.
 *
 * What perm2's Direct3D sends that the manual does not say: colours in
 * 1.30 where 255/256 is full intensity, and texture coordinates in a
 * 2048-texel space whatever the map's size (an 8-texel map's far edge is
 * s = 1/256).
 **/

#include "Permedia2.hpp"

#include <cmath>
#include <cstring>

using namespace permedia2;

enum VertexParam {
  VP_S = 0,
  VP_T = 1,
  VP_Q = 2,
  VP_KS = 3,
  VP_KD = 4,
  VP_R = 5,
  VP_G = 6,
  VP_B = 7,
  VP_A = 8,
  VP_F = 9,
  VP_X = 10,
  VP_Y = 11,
  VP_Z = 12,
  VP_PACKED = 14,
};

/**
 * A vertex register: its value kept as a float, as the chip keeps them
 * (reading one back returns the clamped float). Fixed-point parameters
 * are converted from Table 4-1's formats; colours are kept 256 to the
 * unit, so the drivers' 255/256 reads as 255.
 **/
void CPermedia2::delta_vertex(u32 tag, u32 data) {
  auto &g = r.g;
  const u32 group = (tag >> 4) - 0x20; // 0-2 fixed, 3-5 float
  const int v = int(group % 3);
  const int p = int(tag & 15);
  if (p == VP_PACKED) {
    // Packed 8888 colour, ABGR or, with DeltaMode's ColorOrder, ARGB.
    const bool argb = (G(T_DELTA_MODE) >> 18) & 1;
    const u32 rr = argb ? (data >> 16) & 0xff : data & 0xff;
    const u32 bb = argb ? data & 0xff : (data >> 16) & 0xff;
    g.vtx[v][VP_R] = float(rr);
    g.vtx[v][VP_G] = float((data >> 8) & 0xff);
    g.vtx[v][VP_B] = float(bb);
    g.vtx[v][VP_A] = float(data >> 24);
    return;
  }
  float f;
  if (group >= 3) {
    memcpy(&f, &data, 4);
    if (p >= VP_R && p <= VP_A)
      f *= 256.0f;
  } else {
    const s32 d = s32(data);
    switch (p) {
    case VP_S:
    case VP_T:
    case VP_Q:
      f = float(double(d) / 1073741824.0); // 2.30
      break;
    case VP_KS:
    case VP_KD:
      f = float(double(data) / 4194304.0); // 2.22 unsigned
      break;
    case VP_R:
    case VP_G:
    case VP_B:
    case VP_A:
      f = float(double(data) / 1073741824.0 * 256.0); // 1.30 unsigned
      break;
    case VP_F:
      f = float(double(d) / 4194304.0); // 10.22
      break;
    case VP_X:
    case VP_Y:
      f = float(double(d) / 65536.0); // 16.16
      break;
    case VP_Z:
      f = float(double(data) / 1073741824.0); // 1.30 unsigned
      break;
    default:
      f = 0;
      break;
    }
  }
  g.vtx[v][p] = f;
}

static inline u32 fix16(double v) { return u32(s32(std::floor(v * 65536.0))); }

/// A colour DDA register: 256 to the unit above 15 fraction bits.
static inline u32 color_reg(double v) {
  return u32(s32(std::llround(v * 32768.0)));
}

/**
 * DrawTriangle (and RepeatTriangle): set up and draw the triangle in
 * vertex store. `cmd` is the Render command's data; the draw is qualified
 * by DeltaMode as 4.2.1 describes.
 **/
void CPermedia2::delta_triangle(u32 cmd) {
  auto &g = r.g;
  const u32 dm = G(T_DELTA_MODE);
  g.last_draw = cmd;
  g.last_was_line = false;
  const float (*V)[16] = g.vtx;

  const double area =
      (double(V[1][VP_X]) - V[0][VP_X]) * (double(V[2][VP_Y]) - V[0][VP_Y]) -
      (double(V[2][VP_X]) - V[0][VP_X]) * (double(V[1][VP_Y]) - V[0][VP_Y]);
  if (area == 0)
    return;
  if (dm & (1u << 17)) { // back-face culling; the sign is the command's
    const bool reject_negative = (cmd >> 20) & 1;
    if (reject_negative ? area < 0 : area > 0)
      return;
  }

  // Top to bottom: a, b, c.
  int a = 0, b = 1, c = 2;
  if (V[b][VP_Y] < V[a][VP_Y])
    std::swap(a, b);
  if (V[c][VP_Y] < V[a][VP_Y])
    std::swap(a, c);
  if (V[c][VP_Y] < V[b][VP_Y])
    std::swap(b, c);
  // Direct3D samples at integer coordinates, the rasterizer at half-pixel
  // centres: move the triangle half a pixel over.
  const double xa = V[a][VP_X] + 0.5, ya = V[a][VP_Y] + 0.5;
  const double xb = V[b][VP_X] + 0.5, yb = V[b][VP_Y] + 0.5;
  const double xc = V[c][VP_X] + 0.5, yc = V[c][VP_Y] + 0.5;
  const s32 y0 = s32(std::ceil(ya - 0.5));
  const s32 yk = s32(std::ceil(yb - 0.5));
  const s32 y1 = s32(std::ceil(yc - 0.5));
  if (y1 <= y0)
    return;

  const double bias = 0.5 - 1.0 / 65536.0;
  const double ddom = (xc - xa) / (yc - ya);
  const double dsub1 = yb > ya ? (xb - xa) / (yb - ya) : 0;
  const double dsub2 = yc > yb ? (xc - xb) / (yc - yb) : 0;
  const double cy0 = y0 + 0.5;
  const double xdom0 = xa + ddom * (cy0 - ya) + bias;

  // Each parameter's plane: P = P(a) + dPdx (x - xa) + dPdy (y - ya).
  const double det = (xb - xa) * (yc - ya) - (xc - xa) * (yb - ya);
  auto plane = [&](double pa, double pb, double pc, double &dx, double &dy) {
    dx = ((pb - pa) * (yc - ya) - (pc - pa) * (yb - ya)) / det;
    dy = ((pc - pa) * (xb - xa) - (pb - pa) * (xc - xa)) / det;
  };
  // Load a parameter: its value at the first scanline's dominant edge, its
  // X derivative, and its derivative down the dominant edge.
  auto load = [&](int p, double scale, double &start, double &dx,
                  double &dydom) {
    double dy;
    plane(V[a][p] * scale, V[b][p] * scale, V[c][p] * scale, dx, dy);
    start = V[a][p] * scale + dx * (xdom0 - xa) + dy * (cy0 - ya);
    dydom = dy + dx * ddom;
  };
  double st, dx, dyd;

  if (dm & (1u << 6)) { // smooth shading: the colour DDA's values
    const u32 tags[4][3] = {{T_R_START, T_DR_DX, T_DR_DY_DOM},
                            {T_G_START, T_DG_DX, T_DG_DY_DOM},
                            {T_B_START, T_DB_DX, T_DB_DY_DOM},
                            {T_A_START, 0, 0}};
    for (int i = 0; i < 4; i++) {
      load(VP_R + i, 1.0, st, dx, dyd);
      G(tags[i][0]) = color_reg(st);
      if (tags[i][1]) {
        G(tags[i][1]) = color_reg(dx);
        G(tags[i][2]) = color_reg(dyd);
      }
    }
  }
  if (dm & (1u << 7)) { // depth, 17.11 in U and L
    const double zmax = ((dm >> 2) & 3) == 1 ? 65535.0 : 32767.0;
    load(VP_Z, zmax, st, dx, dyd);
    auto put = [&](u32 tu, u32 tl, double v) {
      const s64 z = s64(std::llround(v * 2048.0));
      G(tu) = u32(z >> 11);
      G(tl) = u32(z & 0x7ff) << 21;
    };
    put(T_Z_START_U, T_Z_START_L, st);
    put(T_DZ_DX_U, T_DZ_DX_L, dx);
    put(T_DZ_DY_DOM_U, T_DZ_DY_DOM_L, dyd);
  }
  if ((dm & (1u << 5)) && (cmd & RENDER_TEXTURE)) {
    // S and T to texels with 20 fraction bits. The texture space is 2048
    // texels whatever the map's size: perm2 sends an 8-texel map's far
    // edge as s = 1/256. Q is 1 << 27.
    const double sw = 2048.0 * 1048576.0, th = sw;
    double scale = 1.0;
    if (((dm >> 14) & 3) == 2) { // normalise s, t, q to at most 1
      double m = 0;
      for (int v = 0; v < 3; v++)
        for (int p = VP_S; p <= VP_Q; p++)
          m = std::max(m, double(std::fabs(V[v][p])));
      if (m > 0)
        scale = 1.0 / m;
    }
    // The planes themselves, in texels, for per-pixel evaluation.
    g.tex_plane_next = true;
    g.tp_x0 = xa;
    g.tp_y0 = ya;
    const double pscale[3] = {2048.0 * scale, 2048.0 * scale, scale};
    for (int i = 0; i < 3; i++) {
      double pdx, pdy;
      plane(V[a][VP_S + i] * pscale[i], V[b][VP_S + i] * pscale[i],
            V[c][VP_S + i] * pscale[i], pdx, pdy);
      g.tp[i][0] = V[a][VP_S + i] * pscale[i];
      g.tp[i][1] = pdx;
      g.tp[i][2] = pdy;
    }
    load(VP_S, sw * scale, st, dx, dyd);
    G(T_S_START) = u32(s32(std::llround(st)));
    G(T_DS_DX) = u32(s32(std::llround(dx)));
    G(T_DS_DY_DOM) = u32(s32(std::llround(dyd)));
    load(VP_T, th * scale, st, dx, dyd);
    G(T_T_START) = u32(s32(std::llround(st)));
    G(T_DT_DX) = u32(s32(std::llround(dx)));
    G(T_DT_DY_DOM) = u32(s32(std::llround(dyd)));
    load(VP_Q, 134217728.0 * scale, st, dx, dyd);
    G(T_Q_START) = u32(s32(std::llround(st)));
    G(T_DQ_DX) = u32(s32(std::llround(dx)));
    G(T_DQ_DY_DOM) = u32(s32(std::llround(dyd)));
  }
  if ((dm & (1u << 4)) && (cmd & RENDER_FOG)) { // fog: 2.19 above 4 bits
    load(VP_F, 8388608.0, st, dx, dyd);
    G(T_F_START) = u32(s32(std::llround(st)));
    G(T_DF_DX) = u32(s32(std::llround(dx)));
    G(T_DF_DY_DOM) = u32(s32(std::llround(dyd)));
  }

  // The edges.
  G(T_START_X_DOM) = fix16(xdom0);
  G(T_DX_DOM) = fix16(ddom);
  G(T_START_Y) = u32(y0) << 16;
  G(T_DY) = 1u << 16;
  if (dm & (1u << 12)) // NoDraw: the host finishes it
    return;
  u32 render = (cmd & ~(3u << RENDER_PRIMITIVE_SHIFT)) |
               (PRIM_TRAPEZOID << RENDER_PRIMITIVE_SHIFT);
  if (!(dm & (1u << 10)))
    render &= ~RENDER_SUBPIXEL_CORRECTION;
  // The registers get the 16.16 values; the rasterizer is handed the
  // same edges at 32.32, so that a pixel centre exactly on an edge falls
  // on the side Direct3D puts it.
  auto exact = [](double v) { return s64(std::llround(v * 4294967296.0)); };
  g.exact = true;
  g.exact_xdom = exact(xdom0);
  g.exact_dxdom = exact(ddom);
  auto sub = [&](double x, double d) {
    G(T_START_X_SUB) = fix16(x);
    G(T_DX_SUB) = fix16(d);
    g.exact_xsub = exact(x);
    g.exact_dxsub = exact(d);
  };
  if (yk > y0) {
    sub(xa + dsub1 * (cy0 - ya) + bias, dsub1);
    G(T_COUNT) = u32(yk - y0);
    raster_begin(render);
    if (y1 > yk) {
      sub(xb + dsub2 * (yk + 0.5 - yb) + bias, dsub2);
      raster_continue(T_CONTINUE_NEW_SUB, u32(y1 - yk));
    }
  } else {
    sub(xb + dsub2 * (cy0 - yb) + bias, dsub2);
    G(T_COUNT) = u32(y1 - y0);
    raster_begin(render);
  }
  g.exact = false;
}

/**
 * DrawLine01 / DrawLine10 (and RepeatLine): a one-pixel line from one
 * vertex to the other, stepped along its major axis a pixel a step, the
 * parameters' derivatives per step in the dominant-edge registers (4.12.7).
 **/
void CPermedia2::delta_line(u32 cmd, int from, int to) {
  auto &g = r.g;
  const u32 dm = G(T_DELTA_MODE);
  g.last_draw = cmd;
  g.last_was_line = true;
  const float *p0 = g.vtx[from], *p1 = g.vtx[to];
  if (((cmd >> RENDER_PRIMITIVE_SHIFT) & 3) == PRIM_POINT) {
    // A Draw command's primitive type may make it a point: the first
    // vertex alone, its colour and depth as they are.
    G(T_START_X_DOM) = fix16(p0[VP_X] + 0.5);
    G(T_START_Y) = fix16(p0[VP_Y] + 0.5);
    if (dm & (1u << 6)) {
      G(T_R_START) = color_reg(p0[VP_R]);
      G(T_G_START) = color_reg(p0[VP_G]);
      G(T_B_START) = color_reg(p0[VP_B]);
      G(T_A_START) = color_reg(p0[VP_A]);
    }
    if (dm & (1u << 7)) {
      const double zmax = ((dm >> 2) & 3) == 1 ? 65535.0 : 32767.0;
      const s64 z0 = s64(std::llround(double(p0[VP_Z]) * zmax * 2048.0));
      G(T_Z_START_U) = u32(z0 >> 11);
      G(T_Z_START_L) = u32(z0 & 0x7ff) << 21;
    }
    if (!(dm & (1u << 12)))
      raster_begin(cmd);
    return;
  }
  const double dx = double(p1[VP_X]) - p0[VP_X],
               dy = double(p1[VP_Y]) - p0[VP_Y];
  const double len = std::max(std::fabs(dx), std::fabs(dy));
  const s32 n = s32(std::floor(len + 0.5));
  if (n <= 0)
    return;
  const double sx = dx / len, sy = dy / len;
  G(T_START_X_DOM) = fix16(p0[VP_X] + 0.5);
  G(T_DX_DOM) = fix16(sx);
  G(T_START_Y) = fix16(p0[VP_Y] + 0.5);
  G(T_DY) = fix16(sy);
  G(T_COUNT) = u32(n);
  auto per_step = [&](int p, double scale, u32 ts, u32 td) {
    G(ts) = u32(s32(std::floor(p0[p] * scale)));
    G(td) = u32(s32(std::floor((double(p1[p]) - p0[p]) * scale / len)));
  };
  if (dm & (1u << 6)) {
    per_step(VP_R, 32768.0, T_R_START, T_DR_DY_DOM);
    per_step(VP_G, 32768.0, T_G_START, T_DG_DY_DOM);
    per_step(VP_B, 32768.0, T_B_START, T_DB_DY_DOM);
    G(T_A_START) = color_reg(p0[VP_A]);
  }
  if (dm & (1u << 7)) {
    const double zmax = ((dm >> 2) & 3) == 1 ? 65535.0 : 32767.0;
    const s64 z0 = s64(std::floor(double(p0[VP_Z]) * zmax * 2048.0));
    const s64 dz =
        s64(std::floor((double(p1[VP_Z]) - p0[VP_Z]) * zmax * 2048.0 / len));
    G(T_Z_START_U) = u32(z0 >> 11);
    G(T_Z_START_L) = u32(z0 & 0x7ff) << 21;
    G(T_DZ_DY_DOM_U) = u32(dz >> 11);
    G(T_DZ_DY_DOM_L) = u32(dz & 0x7ff) << 21;
  }
  if (dm & (1u << 12))
    return;
  raster_begin((cmd & ~(3u << RENDER_PRIMITIVE_SHIFT)) |
               (PRIM_LINE << RENDER_PRIMITIVE_SHIFT));
}
