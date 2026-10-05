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
 * The self-test's R100 scenes (ALPHABOX_RADEON_SELFTEST): the 3D engine
 * and TCL driven through the CP ring as a driver drives them, each frame
 * compared with a reference written here independently of the engine's
 * code (a double-precision barycentric rasteriser, its own texture
 * decoders, combiner equations, transform and lighting). The common
 * self-test (RadeonSelfTest.cpp) runs them between its 2D/CP checks and
 * its last ones; the frames are numbered 01-23 (docs/radeon.md).
 **/

#include "Radeon.hpp" // CRadeonEngineBus's definitions
#include "RadeonR100_3D.hpp"
#include "RadeonSelfTest.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

using namespace radeon;
using namespace radeon::r100;
using namespace radeon::selftest;

void CRadeonR100_3D::selftest_scenes(SelfTest &t) {
  // what the common part drives the card with
  auto report = [&](const std::string &what, bool ok,
                    const std::string &detail = "") {
    t.report(what, ok, detail);
  };
  auto &wr = t.wr;
  auto &sync = t.sync;
  auto &vrd = t.vrd;
  auto &vwr = t.vwr;
  auto &vr32 = t.vr32;
  auto &vw32 = t.vw32;
  auto &cp = t.cp;
  const std::string &dir = t.dir;

  const int W = 128, H = 128;
  // RB3D_CNTL ROUND_ENABLE: the references round to the nearest step (Mesa
  // sets it for round_mode "round"; without it the chip truncates)
  const u32 ROUNDING = 1u << 3;
  const u32 CB = 0x1000000, ZB = 0x1100000, TEX = 0x1200000;
  int scene_no = 0;
  auto clear_cb = [&](u32 v) {
    for (u32 i = 0; i < u32(W * H); i++)
      vw32(CB + i * 4, v);
  };
  auto clear_z = [&](u32 v) {
    for (u32 i = 0; i < u32(W * H); i++)
      vw32(ZB + i * 4, v);
  };
  // the state every scene starts from: an ARGB8888 colour buffer, no
  // tests, Gouraud shading with OpenGL pixel centres, no viewport
  // transform, TCL bypassed, the first stage passing the diffuse colour
  auto base_state = [&]() {
    cp({pkt0(RB3D_COLOROFFSET, 1),
        CB,
        pkt0(RB3D_COLORPITCH, 1),
        u32(W),
        pkt0(RB3D_DEPTHOFFSET, 1),
        ZB,
        pkt0(RB3D_DEPTHPITCH, 1),
        u32(W),
        pkt0(RB3D_CNTL, 1),
        ROUNDING | 6u << 10,
        pkt0(RB3D_ZSTENCILCNTL, 1),
        2u,
        pkt0(RB3D_BLENDCNTL, 1),
        (33u << 16) | (32u << 24),
        pkt0(RB3D_PLANEMASK, 1),
        0xffffffffu,
        pkt0(PP_CNTL, 1),
        0,
        pkt0(PP_MISC, 1),
        7u << 8,
        pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (3u << 6) | (2u << 8) | (2u << 10) |
            (2u << 12) | (2u << 14) | (1u << 27),
        pkt0(SE_COORD_FMT, 1),
        0,
        pkt0(SE_CNTL_STATUS, 1),
        1u << 8,
        pkt0(RE_TOP_LEFT, 1),
        0,
        pkt0(RE_WIDTH_HEIGHT, 1),
        (u32(H - 1) << 16) | u32(W - 1)});
  };
  // a frame and its reference as PNGs; the comparison with a tolerance
  // per channel; returns the number of differing pixels
  auto compare = [&](const std::string &name, const std::vector<u32> &ref,
                     int tol, int *worst = nullptr) {
    std::vector<u32> got(size_t(W * H));
    for (int i = 0; i < W * H; i++)
      got[size_t(i)] = vr32(CB + u32(i) * 4);
    int diff = 0, mx = 0;
    for (int i = 0; i < W * H; i++) {
      int d = 0;
      for (int s = 0; s < 32; s += 8)
        d = std::max(d, std::abs(int((got[size_t(i)] >> s) & 0xff) -
                                 int((ref[size_t(i)] >> s) & 0xff)));
      mx = std::max(mx, d);
      if (d > tol)
        diff++;
    }
    char nm[32];
    snprintf(nm, sizeof(nm), "%02d-", ++scene_no);
    write_png(dir + "/" + nm + name + ".png", W, H, got);
    // the frame, the reference and the differences (white), side by side
    // at twice the size
    std::vector<u32> cmpimg(size_t(W * 2 * 3 * H * 2));
    for (int y = 0; y < H * 2; y++)
      for (int x = 0; x < W * 2 * 3; x++) {
        const int panel = x / (W * 2), sx = (x % (W * 2)) / 2, sy = y / 2;
        const size_t i = size_t(sy * W + sx);
        u32 v = panel == 0 ? got[i] : panel == 1 ? ref[i] : 0;
        if (panel == 2) {
          int dd = 0;
          for (int sh = 0; sh < 32; sh += 8)
            dd = std::max(dd, std::abs(int((got[i] >> sh) & 0xff) -
                                       int((ref[i] >> sh) & 0xff)));
          v = dd > tol ? 0xffffffu : (got[i] >> 2) & 0x3f3f3fu;
        }
        cmpimg[size_t(y * W * 2 * 3 + x)] = v;
      }
    write_png(dir + "/" + nm + name + "-cmp.png", W * 2 * 3, H * 2, cmpimg);
    if (worst)
      *worst = mx;
    return diff;
  };
  // `allowed`: pixels a scene may differ by where a value sits exactly on
  // a rounding or coverage boundary (each scene says why)
  auto scene_report = [&](const std::string &name, int diff, int maxdiff,
                          int allowed = 0) {
    report("3D: " + name, diff <= allowed,
           diff ? "(" + std::to_string(diff) + " pixels beyond the tolerance" +
                      (allowed ? ", " + std::to_string(allowed) + " allowed"
                               : std::string()) +
                      ", largest difference " + std::to_string(maxdiff) + ")"
                : "");
  };
  // a vertex for 3D_DRAW_IMMD: X, Y, Z, packed RGBA colour
  struct V {
    float x, y, z, r, g, b, a;
  };
  auto pk_rgba = [](float r, float g, float b, float a) {
    return to8(r) | (to8(g) << 8) | (to8(b) << 16) | (to8(a) << 24);
  };
  const u32 VF_RGBA = 1u << 6;
  auto immd_xyzc = [&](u32 prim, const std::vector<V> &vs) {
    std::vector<u32> p = {
        pkt3(0x29, 2 + u32(vs.size()) * 4), VTX_Z | VTX_PKCOLOR,
        prim | (WALK_DATA << 4) | VF_RGBA | (u32(vs.size()) << 16)};
    for (const V &v : vs) {
      p.push_back(fbits(v.x));
      p.push_back(fbits(v.y));
      p.push_back(fbits(v.z));
      p.push_back(pk_rgba(v.r, v.g, v.b, v.a));
    }
    cp(p);
  };
  // the reference: Gouraud triangles over a background
  auto ref_gouraud = [&](std::vector<u32> &img, const V &a, const V &b,
                         const V &c) {
    // barycentrics of the vertices as passed (ref_triangle may swap two)
    const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    ref_triangle(
        {a.x, a.y}, {b.x, b.y}, {c.x, c.y}, W, H,
        [&](int x, int y, double, double, double) {
          const double px = x + 0.5, py = y + 0.5;
          const double la =
              ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
          const double lb =
              ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
          const double lc = 1 - la - lb;
          float col[4] = {float(la * a.r + lb * b.r + lc * c.r),
                          float(la * a.g + lb * b.g + lc * c.g),
                          float(la * a.b + lb * b.b + lc * c.b),
                          float(la * a.a + lb * b.a + lc * c.a)};
          img[size_t(y * W + x)] = argbf(col);
        });
  };

  // -- scene: Gouraud triangles as a list, a fan and a strip ---------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    const std::vector<V> tri = {{5.5f, 4.25f, 0, 1, 0, 0, 1},
                                {60.0625f, 20.5f, 0, 0, 1, 0, 1},
                                {12.75f, 58.375f, 0, 0, 0, 1, 1}};
    immd_xyzc(PRIM_TRI_LIST, tri);
    ref_gouraud(ref, tri[0], tri[1], tri[2]);
    const std::vector<V> fan = {{96, 32, 0, 1, 1, 1, 1},
                                {96, 4, 0, 1, 0, 0, 1},
                                {124, 32, 0, 0, 1, 0, 1},
                                {96, 60, 0, 0, 0, 1, 1},
                                {68, 32, 0, 1, 1, 0, 1}};
    immd_xyzc(PRIM_TRI_FAN, fan);
    for (int i = 1; i + 1 < 5; i++)
      ref_gouraud(ref, fan[0], fan[size_t(i)], fan[size_t(i + 1)]);
    std::vector<V> strip;
    for (int i = 0; i < 8; i++)
      strip.push_back({8.0f + float(i) * 15.5f, (i & 1) ? 120.0f : 72.5f, 0,
                       float(i) / 7, float(7 - i) / 7, (i & 1) ? 1.f : 0.f, 1});
    immd_xyzc(PRIM_TRI_STRIP, strip);
    for (int i = 0; i + 2 < 8; i++)
      ref_gouraud(ref, strip[size_t(i)], strip[size_t(i + 1)],
                  strip[size_t(i + 2)]);
    int mx;
    const int d = compare("gouraud", ref, 2, &mx);
    scene_report("Gouraud list, fan and strip", d, mx);
  }

  // -- scene: flat shading, RE_SOLID_COLOR, culling --------------------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // counter-clockwise front faces (as the screen shows them); back
    // faces culled
    cp({pkt0(SE_CNTL, 1), 1u | (0u << 1) | (3u << 3) | (3u << 6) | (1u << 8) |
                              (1u << 10) | (1u << 27)});
    const V fr[3] = {{60, 10, 0, 1, 0, 0, 1},
                     {10, 10, 0, 0, 1, 0, 1},
                     {10, 60, 0, 0.25f, 0.5f, 0.75f, 1}};
    const V bk[3] = {{70, 10, 0, 1, 1, 1, 1},
                     {120, 10, 0, 1, 1, 1, 1},
                     {70, 60, 0, 1, 1, 1, 1}};
    immd_xyzc(PRIM_TRI_LIST, {fr[0], fr[1], fr[2], bk[0], bk[1], bk[2]});
    // flat: the last vertex's colour
    ref_triangle({10, 10}, {60, 10}, {10, 60}, W, H,
                 [&](int x, int y, double, double, double) {
                   float c[4] = {0.25f, 0.5f, 0.75f, 1};
                   ref[size_t(y * W + x)] = argbf(c);
                 });
    // solid diffuse: RE_SOLID_COLOR
    cp({pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (0u << 8) | (0u << 10) | (1u << 27),
        pkt0(RE_SOLID_COLOR, 1), 0x80c08040u});
    immd_xyzc(PRIM_TRI_LIST, {{20, 70, 0, 1, 0, 0, 1},
                              {100, 80, 0, 0, 1, 0, 1},
                              {40, 120, 0, 0, 0, 1, 1}});
    ref_triangle({20, 70}, {100, 80}, {40, 120}, W, H,
                 [&](int x, int y, double, double, double) {
                   ref[size_t(y * W + x)] = 0x80c08040u;
                 });
    int mx;
    const int d = compare("flat-solid-cull", ref, 1, &mx);
    scene_report("flat and solid shading, back-face culling", d, mx);
  }

  // Textures in VRAM; a helper to point a unit at one.
  auto set_tex = [&](int unit, u32 off, u32 fmt, int lw, int lh, u32 filter,
                     u32 cblend, u32 ablend) {
    const u32 o = PP_UNIT_STRIDE * u32(unit);
    cp({pkt0(PP_TXFILTER_0 + o, 1), filter, pkt0(PP_TXFORMAT_0 + o, 1),
        fmt | (u32(lw) << 8) | (u32(lh) << 12) | (u32(unit) << 24),
        pkt0(PP_TXOFFSET_0 + o, 1), off, pkt0(PP_TXCBLEND_0 + o, 1), cblend,
        pkt0(PP_TXABLEND_0 + o, 1), ablend});
  };
  // combiner shorthands (radeon_reg.h argument codes)
  const u32 C_REPLACE_T0 = (0u) | (0u << 5) | (10u << 10) | (1u << 23);
  const u32 A_REPLACE_T0 = (0u) | (0u << 4) | (5u << 8) | (1u << 23);
  // a textured quad as a rectangle list: X, Y, S0, T0
  auto rect_st = [&](float x0, float y0, float x1, float y1, float s0, float t0,
                     float s1, float t1) {
    cp({pkt3(0x29, 2 + 12), VTX_ST0,
        PRIM_RECT_LIST | (WALK_DATA << 4) | (3u << 16), fbits(x0), fbits(y1),
        fbits(s0), fbits(t1), fbits(x1), fbits(y1), fbits(s1), fbits(t1),
        fbits(x1), fbits(y0), fbits(s1), fbits(t0)});
  };
  // the reference texel decoders and wrap
  auto ref_wrap = [](int i, int n, int mode) {
    switch (mode) {
    case 0:
      return ((i % n) + n) % n;
    case 1: {
      int p = ((i % (2 * n)) + 2 * n) % (2 * n);
      return p < n ? p : 2 * n - 1 - p;
    }
    case 2:
      return i < 0 ? 0 : i >= n ? n - 1 : i;
    default:
      return (i < 0 || i >= n) ? -1 : i;
    }
  };

  // -- scene: nearest and bilinear, wrap modes ------------------------------
  {
    base_state();
    clear_cb(0xff000000u);
    // a 16x16 ARGB8888 texture of distinct colours
    const int TW = 16;
    std::vector<u32> tex(size_t(TW * TW));
    for (int y = 0; y < TW; y++)
      for (int x = 0; x < TW; x++)
        tex[size_t(y * TW + x)] = 0xff000000u | (u32(x * 16) << 16) |
                                  (u32(y * 16) << 8) | u32(((x ^ y) & 1) * 255);
    for (int i = 0; i < TW * TW; i++)
      vw32(TEX + u32(i) * 4, tex[size_t(i)]);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    const u32 border = 0xff336699u;
    struct Q {
      float x0, y0, x1, y1, s0, t0, s1, t1;
      int filter;
      int wrap;
    };
    const Q qs[] = {{0, 0, 64, 64, 0, 0, 1, 1, 0, 0},
                    {64, 0, 128, 64, 0, 0, 1, 1, 1, 0},
                    {0, 64, 64, 128, -0.5f, -0.5f, 1.5f, 1.5f, 0, 0},
                    {64, 64, 96, 96, -0.5f, -0.5f, 1.5f, 1.5f, 0, 1},
                    {96, 64, 128, 96, -0.5f, -0.5f, 1.5f, 1.5f, 0, 2},
                    {64, 96, 96, 128, -0.5f, -0.5f, 1.5f, 1.5f, 0, 4},
                    {96, 96, 128, 128, -0.25f, 0.1f, 1.25f, 0.9f, 1, 1}};
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12), pkt0(PP_BORDER_COLOR_0, 1),
        border});
    for (const Q &q : qs) {
      const u32 filter = u32(q.filter) | (u32(q.filter) << 1) |
                         (u32(q.wrap) << 23) | (u32(q.wrap) << 27);
      set_tex(0, TEX, 6 | (1u << 6), 4, 4, filter, C_REPLACE_T0, A_REPLACE_T0);
      rect_st(q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1);
      const int mode = q.wrap == 4 ? 3 : q.wrap;
      for (int y = int(q.y0); y < int(q.y1); y++)
        for (int x = int(q.x0); x < int(q.x1); x++) {
          const double s =
              q.s0 + (q.s1 - q.s0) * (x + 0.5 - q.x0) / (q.x1 - q.x0);
          const double t =
              q.t0 + (q.t1 - q.t0) * (y + 0.5 - q.y0) / (q.y1 - q.y0);
          auto tx = [&](int i, int j) {
            const int a = ref_wrap(i, TW, mode), b = ref_wrap(j, TW, mode);
            return (a < 0 || b < 0) ? border : tex[size_t(b * TW + a)];
          };
          u32 out;
          if (!q.filter) {
            out = tx(int(std::floor(s * TW)), int(std::floor(t * TW)));
          } else {
            const double u = s * TW - 0.5, v = t * TW - 0.5;
            const int i0 = int(std::floor(u)), j0 = int(std::floor(v));
            const double fu = u - i0, fv = v - j0;
            float c[4] = {0, 0, 0, 0};
            const u32 t4[4] = {tx(i0, j0), tx(i0 + 1, j0), tx(i0, j0 + 1),
                               tx(i0 + 1, j0 + 1)};
            const double wgt[4] = {(1 - fu) * (1 - fv), fu * (1 - fv),
                                   (1 - fu) * fv, fu * fv};
            for (int k = 0; k < 4; k++) {
              float cc[4];
              unargb(t4[k], cc);
              for (int ch = 0; ch < 4; ch++)
                c[ch] += float(wgt[k]) * cc[ch];
            }
            out = argbf(c);
          }
          ref[size_t(y * W + x)] = out;
        }
    }
    int mx;
    const int d = compare("texture-filter-wrap", ref, 2, &mx);
    scene_report("textures: nearest, bilinear, wrap/mirror/clamp/border", d,
                 mx);
  }

  // -- scene: perspective-correct texturing ---------------------------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // a checkerboard 8x8 of 2 colours, wrapped
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++)
        vw32(TEX + u32(y * 8 + x) * 4,
             ((x ^ y) & 1) ? 0xffffffffu : 0xff2040c0u);
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12)});
    set_tex(0, TEX, 6 | (1u << 6) | (1u << 31), 3, 3, 0, C_REPLACE_T0,
            A_REPLACE_T0);
    // a floor: X, Y, Z, W0 (= 1/w), S, T; the far edge at w = 4
    struct PV {
      float x, y, rhw, s, t;
    };
    const PV pv[4] = {{8, 120, 1.0f, 0, 4},
                      {120, 120, 1.0f, 4, 4},
                      {80, 30, 0.25f, 4, 0},
                      {48, 30, 0.25f, 0, 0}};
    std::vector<u32> p = {pkt3(0x29, 2 + 4 * 5), VTX_W0 | VTX_ST0,
                          PRIM_TRI_FAN | (WALK_DATA << 4) | (4u << 16)};
    for (const PV &v : pv) {
      p.push_back(fbits(v.x));
      p.push_back(fbits(v.y));
      p.push_back(fbits(v.rhw));
      p.push_back(fbits(v.s));
      p.push_back(fbits(v.t));
    }
    cp(p);
    auto shade = [&](const PV &a, const PV &b, const PV &c) {
      const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
      ref_triangle(
          {a.x, a.y}, {b.x, b.y}, {c.x, c.y}, W, H,
          [&](int x, int y, double, double, double) {
            const double px = x + 0.5, py = y + 0.5;
            const double la =
                ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
            const double lb =
                ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
            const double lc = 1 - la - lb;
            const double w = la * a.rhw + lb * b.rhw + lc * c.rhw;
            const double s =
                (la * a.s * a.rhw + lb * b.s * b.rhw + lc * c.s * c.rhw) / w;
            const double t =
                (la * a.t * a.rhw + lb * b.t * b.rhw + lc * c.t * c.rhw) / w;
            const int i = ((int(std::floor(s * 8)) % 8) + 8) % 8;
            const int j = ((int(std::floor(t * 8)) % 8) + 8) % 8;
            ref[size_t(y * W + x)] = ((i ^ j) & 1) ? 0xffffffffu : 0xff2040c0u;
          });
    };
    shade(pv[0], pv[1], pv[2]);
    shade(pv[0], pv[2], pv[3]);
    int mx;
    const int d = compare("texture-perspective", ref, 0, &mx);
    // texels exactly on a boundary may round either way
    scene_report("textures: perspective-correct", d, mx, 8);
  }

  // -- scene: mip-mapping, nearest and linear between levels ----------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // 32x32 ARGB8888, six levels each a flat colour, packed with 32-byte
    // aligned rows (Mesa's layout)
    const u32 lc[6] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu,
                       0xffffff00u, 0xff00ffffu, 0xffff00ffu};
    u32 off = TEX;
    for (int l = 0; l < 6; l++) {
      const int s = 32 >> l, pitch = std::max(32, s * 4);
      for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++)
          vw32(off + u32(y * pitch + x * 4), lc[l]);
      off += u32(pitch * s);
    }
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12)});
    struct M {
      float x0, y0, size;
      u32 minf;
      int expect_lo;
      double frac;
    };
    // 32 texels across `size` pixels: lod = log2(32 / size)
    const M ms[] = {{0, 0, 32, 2, 0, 0},  {32, 0, 16, 2, 1, 0},
                    {48, 0, 8, 2, 2, 0},  {56, 0, 4, 2, 3, 0},
                    {0, 40, 24, 7, 0, 0}, {32, 40, 12, 7, 1, 0},
                    {64, 40, 6, 7, 2, 0}};
    for (const M &m : ms) {
      set_tex(0, TEX, 6 | (1u << 6), 5, 5, (m.minf << 1) | (5u << 16),
              C_REPLACE_T0, A_REPLACE_T0);
      rect_st(m.x0, m.y0, m.x0 + m.size, m.y0 + m.size, 0, 0, 1, 1);
      const double lod = std::log2(32.0 / m.size);
      float col[4];
      if (m.minf == 2) {
        unargb(lc[std::min(5, int(std::floor(lod + 0.5)))], col);
      } else {
        const int l0 = int(std::floor(lod));
        const double fr = lod - l0;
        float a[4], b[4];
        unargb(lc[l0], a);
        unargb(lc[std::min(5, l0 + 1)], b);
        for (int k = 0; k < 4; k++)
          col[k] = float(a[k] * (1 - fr) + b[k] * fr);
      }
      for (int y = int(m.y0); y < int(m.y0 + m.size); y++)
        for (int x = int(m.x0); x < int(m.x0 + m.size); x++)
          ref[size_t(y * W + x)] = argbf(col);
    }
    int mx;
    const int d = compare("texture-mipmap", ref, 2, &mx);
    scene_report("textures: mip levels, nearest and linear", d, mx);
  }

  // -- scene: texture formats -------------------------------------------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12)});
    // each format: an 8x8 texture of known texels, drawn 1:1 at 16x16
    // (each texel 2x2 pixels), the expected colours decoded here
    struct F {
      u32 fmt;
      int bytes;
    };
    const F fs[] = {{0, 1},  {1, 2},  {2, 1},  {3, 2}, {4, 2},
                    {5, 2},  {6, 4},  {7, 4},  {8, 1}, {12, 0},
                    {14, 0}, {15, 0}, {10, 2}, {11, 2}};
    int idx = 0;
    for (const F &f : fs) {
      const u32 base = TEX + 0x10000 * u32(idx);
      std::vector<u32> want(64);
      if (f.bytes) {
        const u32 pitch = 32; // 8 texels, rows 32-byte aligned
        for (int y = 0; y < 8; y++)
          for (int x = 0; x < 8; x++) {
            const u32 v = rnd();
            const u32 a = base + u32(y) * pitch + u32(x * f.bytes);
            for (int k = 0; k < f.bytes; k++)
              vwr(a + u32(k), 1, (v >> (8 * k)) & 0xff);
            float c[4] = {0, 0, 0, 1};
            const u32 b0 = v & 0xff, w16 = v & 0xffff;
            switch (f.fmt) {
            case 0: // I8: intensity everywhere, alpha too
              c[0] = c[1] = c[2] = c[3] = float(b0) / 255;
              break;
            case 1: // AI88
              c[0] = c[1] = c[2] = float(b0) / 255;
              c[3] = float((v >> 8) & 0xff) / 255;
              break;
            case 2: // RGB332
              c[0] = float(b0 >> 5) / 7;
              c[1] = float((b0 >> 2) & 7) / 7;
              c[2] = float(b0 & 3) / 3;
              break;
            case 3: // ARGB1555
              c[0] = float((w16 >> 10) & 31) / 31;
              c[1] = float((w16 >> 5) & 31) / 31;
              c[2] = float(w16 & 31) / 31;
              c[3] = float(w16 >> 15);
              break;
            case 4: // RGB565
              c[0] = float(w16 >> 11) / 31;
              c[1] = float((w16 >> 5) & 63) / 63;
              c[2] = float(w16 & 31) / 31;
              break;
            case 5: // ARGB4444
              c[3] = float(w16 >> 12) / 15;
              c[0] = float((w16 >> 8) & 15) / 15;
              c[1] = float((w16 >> 4) & 15) / 15;
              c[2] = float(w16 & 15) / 15;
              break;
            case 6:
              unargb(v, c);
              break;
            case 7: // RGBA8888
              c[0] = float(v >> 24) / 255;
              c[1] = float((v >> 16) & 0xff) / 255;
              c[2] = float((v >> 8) & 0xff) / 255;
              c[3] = float(v & 0xff) / 255;
              break;
            case 8: // Y8
              c[0] = c[1] = c[2] = float(b0) / 255;
              break;
            default:
              break;
            }
            want[size_t(y * 8 + x)] = argbf(c);
          }
        if (f.fmt == 10 || f.fmt == 11) {
          // the YUV formats, raw (YUV_TO_RGB off): R = V, G = Y, B = U
          for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
              const u32 a = base + u32(y) * 32 + u32(x & ~1) * 2;
              const u32 v = vr32(a);
              u32 yy, uu, vv;
              if (f.fmt == 10) { // Y0 U Y1 V
                yy = (x & 1) ? (v >> 16) & 0xff : v & 0xff;
                uu = (v >> 8) & 0xff;
                vv = v >> 24;
              } else { // U Y0 V Y1
                yy = (x & 1) ? v >> 24 : (v >> 8) & 0xff;
                uu = v & 0xff;
                vv = (v >> 16) & 0xff;
              }
              want[size_t(y * 8 + x)] =
                  0xff000000u | (vv << 16) | (yy << 8) | uu;
            }
        }
      } else {
        // DXT: 2x2 blocks of 4x4; each block's two 565 colours and codes
        const int bb = f.fmt == 12 ? 8 : 16;
        for (int by = 0; by < 2; by++)
          for (int bx = 0; bx < 2; bx++) {
            const u32 a = base + u32(by) * 32 + u32(bx * bb);
            u32 c0 = rnd() & 0xffff, c1 = rnd() & 0xffff;
            if (f.fmt == 12 && bx == 1)
              std::swap(c0, c1); // one 3-colour block too
            const u32 codes = rnd();
            const u32 cb = a + (f.fmt == 12 ? 0 : 8);
            vwr(cb, 2, c0);
            vwr(cb + 2, 2, c1);
            vw32(cb + 4, codes);
            u32 alo = rnd(), ahi = rnd();
            if (f.fmt != 12) {
              vw32(a, alo);
              vw32(a + 4, ahi);
            }
            auto c565 = [](u32 v, double o[3]) {
              o[0] = double(v >> 11) / 31;
              o[1] = double((v >> 5) & 63) / 63;
              o[2] = double(v & 31) / 31;
            };
            double A[3], B[3];
            c565(c0, A);
            c565(c1, B);
            for (int py = 0; py < 4; py++)
              for (int px = 0; px < 4; px++) {
                const u32 code = (codes >> (2 * (py * 4 + px))) & 3;
                const bool four = f.fmt != 12 || c0 > c1;
                float c[4] = {0, 0, 0, 1};
                for (int k = 0; k < 3; k++) {
                  double r = code == 0   ? A[k]
                             : code == 1 ? B[k]
                             : code == 2 ? (four ? (2 * A[k] + B[k]) / 3
                                                 : (A[k] + B[k]) / 2)
                                         : (four ? (A[k] + 2 * B[k]) / 3 : 0);
                  c[k] = float(r);
                }
                if (f.fmt == 12 && !four && code == 3)
                  c[3] = 0;
                const int ti = py * 4 + px;
                if (f.fmt == 14) {
                  const u64 all = u64(alo) | (u64(ahi) << 32);
                  c[3] = float((all >> (4 * ti)) & 15) / 15;
                } else if (f.fmt == 15) {
                  const u32 a0 = alo & 0xff, a1 = (alo >> 8) & 0xff;
                  const u64 bits = (u64(alo) >> 16) | (u64(ahi) << 16);
                  const u32 ac = u32(bits >> (3 * ti)) & 7;
                  double av =
                      ac == 0   ? a0
                      : ac == 1 ? a1
                      : a0 > a1 ? ((8.0 - ac) * a0 + (ac - 1.0) * a1) / 7
                      : ac == 6 ? 0
                      : ac == 7 ? 255
                                : ((6.0 - ac) * a0 + (ac - 1.0) * a1) / 5;
                  c[3] = float(av / 255);
                }
                want[size_t((by * 4 + py) * 8 + bx * 4 + px)] = argbf(c);
              }
          }
      }
      set_tex(0, base, f.fmt | (1u << 6), 3, 3, 0, C_REPLACE_T0, A_REPLACE_T0);
      const float x0 = float((idx % 7) * 18), y0 = float((idx / 7) * 18);
      rect_st(x0, y0, x0 + 16, y0 + 16, 0, 0, 1, 1);
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
          ref[size_t((int(y0) + y) * W + int(x0) + x)] =
              want[size_t((y / 2) * 8 + x / 2)];
      idx++;
    }
    int mx;
    const int d = compare("texture-formats", ref, 1, &mx);
    scene_report("textures: 14 formats (incl. DXT1/3/5, YUV)", d, mx);
  }

  // -- scene: combiners --------------------------------------------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // T0: a gradient with alpha; T1: a second one; diffuse from vertices
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++) {
        vw32(TEX + u32(y * 16 + x) * 4, (u32(y * 17) << 24) |
                                            (u32(x * 17) << 16) | (0x80u << 8) |
                                            u32(255 - x * 17));
        vw32(TEX + 0x1000 + u32(y * 16 + x) * 4, (u32(x * 17) << 24) |
                                                     (u32(y * 17) << 16) |
                                                     (u32(x * 8) << 8) | 0x40u);
      }
    const u32 TF = 0xc0306090u; // tfactor
    struct Cmb {
      u32 cb, ab;
      int stages;
    };
    // the arguments: 2 current, 4 diffuse, 8 tfactor, 10 T0, 11 T0 alpha,
    // 12 T1, 13 T1 alpha; ops <20:18>: 0 add, 1 sub, 2 add signed,
    // 3 blend, 4 dot3; COMP <17:15>; scale <22:21>; clamp <23>
    const Cmb cm[] = {
        {10u | (4u << 5) | (0u << 10) | (1u << 23), 5u | (2u << 4) | (1u << 23),
         1}, // modulate
        {10u | (0u << 5) | (1u << 16) | (4u << 10) | (1u << 23),
         5u | (0u << 4) | (1u << 16) | (2u << 8) | (1u << 23), 1}, // add
        {10u | (0u << 5) | (1u << 16) | (4u << 10) | (2u << 18) | (1u << 23),
         5u | (1u << 16) | (2u << 8) | (2u << 18) | (1u << 23), 1}, // addsigned
        {10u | (1u << 16) | (4u << 10) | (1u << 18) | (1u << 23),
         5u | (1u << 16) | (2u << 8) | (1u << 18) | (1u << 23), 1}, // subtract
        {4u | (10u << 5) | (13u << 10) | (3u << 18) | (1u << 23),
         2u | (5u << 4) | (6u << 8) | (3u << 18) | (1u << 23), 1}, // blend
        {10u | (8u << 5) | (4u << 18) | (2u << 21) | (1u << 23),
         4u | (1u << 23), 1}, // dot3
        {10u | (8u << 5) | (1u << 21) | (1u << 23),
         5u | (4u << 4) | (1u << 21) | (1u << 23), 1}, // modulate tf, 2x
        {2u | (12u << 5) | (1u << 23), 1u | (6u << 4) | (1u << 23),
         2}, // stage 2: current x T1
    };
    int k = 0;
    for (const Cmb &m : cm) {
      const float x0 = float((k % 4) * 32), y0 = float((k / 4) * 32);
      const u32 pp =
          (1u << 4) | (1u << 5) | (1u << 12) | (m.stages == 2 ? (1u << 13) : 0);
      cp({pkt0(PP_CNTL, 1), pp, pkt0(PP_TFACTOR_0, 1), TF,
          pkt0(PP_TFACTOR_0 + PP_UNIT_STRIDE, 1), TF});
      if (k == 7) {
        set_tex(0, TEX, 6 | (1u << 6), 4, 4, 0, C_REPLACE_T0, A_REPLACE_T0);
        set_tex(1, TEX + 0x1000, 6 | (1u << 6), 4, 4, 0, m.cb, m.ab);
      } else {
        set_tex(0, TEX, 6 | (1u << 6), 4, 4, 0, m.cb, m.ab);
        set_tex(1, TEX + 0x1000, 6 | (1u << 6), 4, 4, 0, 0, 0);
      }
      // the second unit's coordinates are the first's (ST_ROUTE 0)
      cp({pkt0(PP_TXFORMAT_0 + PP_UNIT_STRIDE, 1),
          6u | (1u << 6) | (4u << 8) | (4u << 12)});
      // a quad, diffuse a fixed colour
      const float dc[4] = {0.8f, 0.4f, 0.2f, 0.6f};
      std::vector<u32> p = {pkt3(0x29, 2 + 3 * 5), VTX_PKCOLOR | VTX_ST0,
                            PRIM_RECT_LIST | (WALK_DATA << 4) | VF_RGBA |
                                (3u << 16)};
      const float xs[3] = {x0, x0 + 32, x0 + 32},
                  ys[3] = {y0 + 32, y0 + 32, y0};
      for (int i = 0; i < 3; i++) {
        p.push_back(fbits(xs[i]));
        p.push_back(fbits(ys[i]));
        p.push_back(pk_rgba(dc[0], dc[1], dc[2], dc[3]));
        p.push_back(fbits((xs[i] - x0) / 32));
        p.push_back(fbits((ys[i] - y0) / 32));
      }
      cp(p);
      float dq[4]; // the diffuse colour as the vertex carries it
      for (int c = 0; c < 4; c++)
        dq[c] = float(to8(dc[c])) / 255;
      for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
          float t0[4], t1[4], tf[4];
          unargb(vr32(TEX + u32((y / 2) * 16 + x / 2) * 4), t0);
          unargb(vr32(TEX + 0x1000 + u32((y / 2) * 16 + x / 2) * 4), t1);
          unargb(TF, tf);
          float o[4];
          switch (k) {
          case 0:
            for (int c = 0; c < 4; c++)
              o[c] = t0[c] * dq[c];
            break;
          case 1:
            for (int c = 0; c < 4; c++)
              o[c] = clampf(t0[c] + dq[c]);
            break;
          case 2:
            for (int c = 0; c < 4; c++)
              o[c] = clampf(t0[c] + dq[c] - 0.5f);
            break;
          case 3:
            for (int c = 0; c < 4; c++)
              o[c] = clampf(t0[c] - dq[c]);
            break;
          case 4: // INTERPOLATE(T0, diffuse, T1 alpha): A=diffuse, B=T0
            for (int c = 0; c < 3; c++)
              o[c] = dq[c] * (1 - t1[3]) + t0[c] * t1[3];
            o[3] = dq[3] * (1 - t1[3]) + t0[3] * t1[3];
            break;
          case 5: {
            float dt = 0;
            for (int c = 0; c < 3; c++)
              dt += (t0[c] - 0.5f) * (tf[c] - 0.5f);
            o[0] = o[1] = o[2] = o[3] = clampf(4 * dt);
            break;
          }
          case 6:
            for (int c = 0; c < 3; c++)
              o[c] = clampf(2 * t0[c] * tf[c]);
            o[3] = clampf(2 * t0[3] * tf[3]);
            break;
          default:
            for (int c = 0; c < 4; c++)
              o[c] = t0[c] * t1[c];
            break;
          }
          ref[size_t((int(y0) + y) * W + int(x0) + x)] = argbf(o);
        }
      k++;
    }
    int mx;
    const int d = compare("combiners", ref, 2, &mx);
    scene_report("combiners: modulate/add/signed/sub/blend/dot3/2x/2 stages", d,
                 mx);
  }

  // -- scene: blending, alpha test, fog, specular ----------------------------
  {
    base_state();
    // a background of vertical stripes
    std::vector<u32> bg(size_t(W * H));
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        bg[size_t(y * W + x)] = (x / 8) & 1 ? 0xff20a040u : 0x40c02080u;
    for (int i = 0; i < W * H; i++)
      vw32(CB + u32(i) * 4, bg[size_t(i)]);
    std::vector<u32> ref = bg;
    struct B {
      u32 blend;
      float a;
    };
    // SRC_ALPHA / ONE_MINUS_SRC_ALPHA; ONE / ONE; DST_COLOR / ZERO;
    // subtract; ONE_MINUS_DST_ALPHA / DST_ALPHA
    const B bs[] = {{(38u << 16) | (39u << 24), 0.4f},
                    {(33u << 16) | (33u << 24), 0.5f},
                    {(36u << 16) | (32u << 24), 1.0f},
                    {(33u << 16) | (33u << 24) | (2u << 12), 0.5f},
                    {(41u << 16) | (40u << 24), 0.7f}};
    const float sc[3] = {0.3f, 0.6f, 0.9f};
    for (int i = 0; i < 5; i++) {
      cp({pkt0(RB3D_CNTL, 1), ROUNDING | (6u << 10) | 1u,
          pkt0(RB3D_BLENDCNTL, 1), bs[i].blend});
      const float x0 = float(i * 24), x1 = x0 + 24;
      immd_xyzc(PRIM_RECT_LIST, {{x0, 64, 0, sc[0], sc[1], sc[2], bs[i].a},
                                 {x1, 64, 0, sc[0], sc[1], sc[2], bs[i].a},
                                 {x1, 0, 0, sc[0], sc[1], sc[2], bs[i].a}});
      for (int y = 0; y < 64; y++)
        for (int x = int(x0); x < int(x1); x++) {
          float d[4], s[4] = {sc[0], sc[1], sc[2], bs[i].a}, o[4];
          // what the engine reads back: the 8-bit source colour
          for (float &v : s)
            v = float(to8(v)) / 255;
          unargb(bg[size_t(y * W + x)], d);
          for (int c = 0; c < 4; c++) {
            switch (i) {
            case 0:
              o[c] = s[c] * s[3] + d[c] * (1 - s[3]);
              break;
            case 1:
              o[c] = clampf(s[c] + d[c]);
              break;
            case 2:
              o[c] = s[c] * d[c];
              break;
            case 3:
              o[c] = clampf(s[c] - d[c]);
              break;
            default:
              o[c] = s[c] * (1 - d[3]) + d[c] * d[3];
              break;
            }
          }
          ref[size_t(y * W + x)] = argbf(o);
        }
    }
    // alpha test GREATER 0x80 over an alpha gradient (no blending)
    cp({pkt0(RB3D_CNTL, 1), ROUNDING | 6u << 10, pkt0(PP_CNTL, 1), 1u << 23,
        pkt0(PP_MISC, 1), 0x80u | (5u << 8)});
    immd_xyzc(PRIM_TRI_LIST, {{0, 64, 0, 1, 1, 1, 0},
                              {64, 64, 0, 1, 1, 1, 1},
                              {0, 128, 0, 1, 1, 1, 0}});
    ref_triangle({0, 64}, {64, 64}, {0, 128}, W, H,
                 [&](int x, int y, double, double, double) {
                   const double a = (x + 0.5) / 64.0; // alpha along x
                   if (to8(float(a)) > 0x80)
                     ref[size_t(y * W + x)] = (to8(float(a)) << 24) | 0xffffff;
                 });
    // fog: the factor in the packed specular's alpha, to a fog colour;
    // with the specular colour added (SPECULAR_ENABLE)
    cp({pkt0(PP_CNTL, 1), (1u << 22) | (1u << 21), pkt0(PP_FOG_COLOR, 1),
        0x00204080u});
    {
      std::vector<u32> p = {pkt3(0x29, 2 + 3 * 4), VTX_PKCOLOR | VTX_PKSPEC,
                            PRIM_TRI_LIST | (WALK_DATA << 4) | VF_RGBA |
                                (3u << 16)};
      const float fx[3] = {64, 128, 64}, fy[3] = {64, 64, 128};
      const float fog[3] = {1.0f, 0.0f, 0.5f};
      for (int i = 0; i < 3; i++) {
        p.push_back(fbits(fx[i]));
        p.push_back(fbits(fy[i]));
        p.push_back(pk_rgba(0.5f, 0.5f, 0.5f, 1));
        p.push_back(pk_rgba(0.1f, 0.0f, 0.2f, fog[i]));
      }
      cp(p);
      ref_triangle(
          {64, 64}, {128, 64}, {64, 128}, W, H,
          [&](int x, int y, double, double, double) {
            const double px = x + 0.5, py = y + 0.5;
            // barycentrics of (64,64),(128,64),(64,128)
            const double l1 = (px - 64) / 64, l2 = (py - 64) / 64;
            const double l0 = 1 - l1 - l2;
            const double f =
                l0 * (to8(1.0f) / 255.0) + l1 * 0 + l2 * (to8(0.5f) / 255.0);
            const double s0 = to8(0.5f) / 255.0;
            const double spec[3] = {to8(0.1f) / 255.0, 0, to8(0.2f) / 255.0};
            const double fc[3] = {0x20 / 255.0, 0x40 / 255.0, 0x80 / 255.0};
            float o[4] = {0, 0, 0, 1};
            for (int c = 0; c < 3; c++)
              o[c] = float(std::min(1.0, s0 + spec[c]) * f + fc[c] * (1 - f));
            ref[size_t(y * W + x)] = argbf(o);
          });
    }
    int mx;
    const int d = compare("blend-alphatest-fog", ref, 2, &mx);
    scene_report("blending (5 modes), alpha test, fog, specular", d, mx);
  }

  // -- scene: Z buffer (16 and 24 bit) and stencil --------------------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    std::vector<double> zref(size_t(W * H), 65535.0);
    // two triangles crossing in depth, Z LESS with writes, 16-bit Z
    for (int zf = 0; zf < 2; zf++) {
      const u32 zfmt = zf ? 2u : 0u;
      clear_z(zf ? 0x00ffffffu : 0xffffffffu);
      cp({pkt0(RB3D_CNTL, 1), ROUNDING | (6u << 10) | (1u << 8),
          pkt0(RB3D_ZSTENCILCNTL, 1), zfmt | (1u << 4) | (1u << 30)});
      const float y0 = float(zf * 64);
      const V t1[3] = {{4, y0 + 4, 0.2f, 1, 0, 0, 1},
                       {124, y0 + 30, 0.8f, 1, 0, 0, 1},
                       {4, y0 + 60, 0.2f, 1, 0, 0, 1}};
      const V t2[3] = {{124, y0 + 4, 0.1f, 0, 0, 1, 1},
                       {4, y0 + 30, 0.9f, 0, 0, 1, 1},
                       {124, y0 + 60, 0.1f, 0, 0, 1, 1}};
      immd_xyzc(PRIM_TRI_LIST, {t1[0], t1[1], t1[2], t2[0], t2[1], t2[2]});
      for (const V *t : {t1, t2}) {
        const V &a = t[0], &b = t[1], &c = t[2];
        const double area =
            (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        const double zmax = zf ? 16777215.0 : 65535.0;
        ref_triangle(
            {a.x, a.y}, {b.x, b.y}, {c.x, c.y}, W, H,
            [&](int x, int y, double, double, double) {
              const double px = x + 0.5, py = y + 0.5;
              const double la =
                  ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
              const double lb =
                  ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
              const double z = la * a.z + lb * b.z + (1 - la - lb) * c.z;
              const double zq = std::llround(z * zmax);
              double &zo = zref[size_t(y * W + x)];
              if (zq < zo) {
                zo = zq;
                float col[4] = {a.r, a.g, a.b, 1};
                ref[size_t(y * W + x)] = argbf(col);
              }
            });
      }
      // the next pass starts with the 24-bit buffer full
      for (int y = 64; y < H; y++)
        for (int x = 0; x < W; x++)
          zref[size_t(y * W + x)] = 16777215.0;
    }
    int mx;
    int d = compare("zbuffer", ref, 0, &mx);
    // depths exactly equal along the crossing may resolve either way
    scene_report("Z buffer, 16- and 24-bit, LESS with writes", d, mx, 6);

    // stencil: write 0x5 where a triangle covers, then draw a full quad
    // where stencil == 5
    base_state();
    clear_cb(0xff000000u);
    clear_z(0x00ffffffu);
    std::vector<u32> sref(size_t(W * H), 0xff000000u);
    cp({pkt0(RB3D_CNTL, 1), ROUNDING | (6u << 10) | (1u << 7) | (1u << 8),
        pkt0(RB3D_STENCILREFMASK, 1), 5u | (0xffu << 16) | (0xffu << 24),
        pkt0(RB3D_ZSTENCILCNTL, 1),
        2u | (7u << 4) | (7u << 12) | (2u << 20) | (2u << 24),
        pkt0(RB3D_PLANEMASK, 1), 0});
    immd_xyzc(PRIM_TRI_LIST, {{10, 10, 0.5f, 1, 1, 1, 1},
                              {118, 30, 0.5f, 1, 1, 1, 1},
                              {40, 118, 0.5f, 1, 1, 1, 1}});
    cp({pkt0(RB3D_PLANEMASK, 1), 0xffffffffu, pkt0(RB3D_ZSTENCILCNTL, 1),
        2u | (7u << 4) | (3u << 12)});
    immd_xyzc(PRIM_RECT_LIST, {{0, 128, 0.5f, 0, 1, 0, 1},
                               {128, 128, 0.5f, 0, 1, 0, 1},
                               {128, 0, 0.5f, 0, 1, 0, 1}});
    ref_triangle({10, 10}, {118, 30}, {40, 118}, W, H,
                 [&](int x, int y, double, double, double) {
                   sref[size_t(y * W + x)] = 0xff00ff00u;
                 });
    d = compare("stencil", sref, 0, &mx);
    scene_report("stencil: REPLACE then EQUAL, plane mask 0", d, mx);
  }

  // -- scene: 16-bit and other colour buffer formats --------------------------
  {
    int total = 0, mx = 0;
    const u32 fmts[5] = {4, 3, 15, 7, 9};
    std::vector<u32> got_all(size_t(W * H), 0), ref_all(size_t(W * H), 0);
    for (int fi = 0; fi < 5; fi++) {
      base_state();
      const u32 f = fmts[fi];
      const int bpp = (f == 7 || f == 9) ? 1 : 2;
      cp({pkt0(RB3D_CNTL, 1), ROUNDING | f << 10});
      for (u32 i = 0; i < u32(W * H * bpp); i += 4)
        vw32(CB + i, 0);
      const V tri[3] = {{0, 0, 0, 1, 0, 0, 1},
                        {128, 0, 0, 0, 1, 0, 0},
                        {0, 128, 0, 0, 0, 1, 1}};
      immd_xyzc(PRIM_TRI_LIST, {tri[0], tri[1], tri[2]});
      int bad = 0;
      ref_triangle(
          {0, 0}, {128, 0}, {0, 128}, W, H,
          [&](int x, int y, double, double, double) {
            const double l1 = (x + 0.5) / 128, l2 = (y + 0.5) / 128;
            const double l0 = 1 - l1 - l2;
            const double c[4] = {l0, l1, l2, l0 + l2};
            auto q = [&](double v, int bits) {
              return u32(std::lround(std::min(1.0, std::max(0.0, v)) *
                                     ((1 << bits) - 1)));
            };
            u32 want;
            switch (f) {
            case 4:
              want = (q(c[0], 5) << 11) | (q(c[1], 6) << 5) | q(c[2], 5);
              break;
            case 3:
              want = (c[3] >= 0.5 ? 0x8000u : 0) | (q(c[0], 5) << 10) |
                     (q(c[1], 5) << 5) | q(c[2], 5);
              break;
            case 15:
              want = (q(c[3], 4) << 12) | (q(c[0], 4) << 8) |
                     (q(c[1], 4) << 4) | q(c[2], 4);
              break;
            case 7:
              want = (q(c[0], 3) << 5) | (q(c[1], 3) << 2) | q(c[2], 2);
              break;
            default:
              want = q(c[0], 8);
              break;
            }
            const u32 got = vrd(CB + u32(y * W + x) * u32(bpp), bpp);
            // a channel's least significant bit may round either
            // way where the 8-bit colour sits halfway
            u32 dif = got ^ want;
            if (dif && (std::abs(int(got) - int(want)) > 1 &&
                        __builtin_popcount(dif) > 2))
              bad++;
          });
      total += bad;
      mx = std::max(mx, bad);
    }
    report("3D: colour formats 565, 1555, 4444, 332, RGB8", total == 0,
           total ? std::to_string(total) + " pixels" : "");
  }

  // -- scene: primitive types and the vertex walks ------------------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    auto fill_ref = [&](float x0, float y0, float x1, float y1, u32 c) {
      for (int y = int(y0); y < int(y1); y++)
        for (int x = int(x0); x < int(x1); x++)
          ref[size_t(y * W + x)] = c;
    };
    // flat colours (solid) so the walks can be compared exactly
    cp({pkt0(SE_CNTL, 1), (3u << 1) | (3u << 3) | (1u << 27),
        pkt0(RE_SOLID_COLOR, 1), 0});
    auto solid = [&](u32 c) { cp({pkt0(RE_SOLID_COLOR, 1), c}); };
    // vertex arrays in VRAM: positions (X, Y) and a second array (Z)
    const u32 VB = TEX + 0x40000;
    auto put_xy = [&](u32 base, const std::vector<float> &xy) {
      for (size_t i = 0; i < xy.size(); i++)
        vw32(base + u32(i) * 4, fbits(xy[i]));
    };
    // 1: quad list, 2 quads
    solid(0xffff0000u);
    {
      std::vector<u32> p = {pkt3(0x29, 2 + 16), 0,
                            PRIM_QUAD_LIST | (WALK_DATA << 4) | (8u << 16)};
      const float q[16] = {0,  0, 16, 0, 16, 16, 0,  16,
                           16, 0, 32, 0, 32, 16, 16, 16};
      for (float v : q)
        p.push_back(fbits(v));
      cp(p);
      fill_ref(0, 0, 32, 16, 0xffff0000u);
    }
    // 2: quad strip
    solid(0xff00ff00u);
    {
      std::vector<u32> p = {pkt3(0x29, 2 + 12), 0,
                            PRIM_QUAD_STRIP | (WALK_DATA << 4) | (6u << 16)};
      const float q[12] = {40, 0, 40, 16, 56, 0, 56, 16, 72, 0, 72, 16};
      for (float v : q)
        p.push_back(fbits(v));
      cp(p);
      fill_ref(40, 0, 72, 16, 0xff00ff00u);
    }
    // 3: polygon (an octagon's bounding square here: a square of 4)
    solid(0xff0000ffu);
    {
      std::vector<u32> p = {pkt3(0x29, 2 + 8), 0,
                            PRIM_POLYGON | (WALK_DATA << 4) | (4u << 16)};
      const float q[8] = {80, 0, 96, 0, 96, 16, 80, 16};
      for (float v : q)
        p.push_back(fbits(v));
      cp(p);
      fill_ref(80, 0, 96, 16, 0xff0000ffu);
    }
    // 4: 3D_LOAD_VBPNTR (two arrays: XY and Z) + 3D_DRAW_VBUF, tri list
    solid(0xffffff00u);
    put_xy(VB, {0, 24, 16, 24, 16, 40, 0, 24, 16, 40, 0, 40});
    for (u32 i = 0; i < 6; i++)
      vw32(VB + 0x1000 + i * 4, fbits(0.5f));
    cp({pkt3(0x2f, 4), 2, 2 | (2u << 8) | (1u << 16) | (1u << 24), VB,
        VB + 0x1000});
    cp({pkt3(0x28, 2), VTX_Z, PRIM_TRI_LIST | (WALK_LIST << 4) | (6u << 16)});
    fill_ref(0, 24, 16, 40, 0xffffff00u);
    // 5: 3D_DRAW_INDX: indices into the same arrays, a strip 0,1,3?  the
    // quad again shifted by drawing indices 0 1 2 / 3 4 5 reversed
    solid(0xff00ffffu);
    put_xy(VB + 0x2000, {24, 24, 40, 24, 40, 40, 24, 40});
    for (u32 i = 0; i < 4; i++)
      vw32(VB + 0x3000 + i * 4, fbits(0.5f));
    cp({pkt3(0x2f, 4), 2, 2 | (2u << 8) | (1u << 16) | (1u << 24), VB + 0x2000,
        VB + 0x3000});
    cp({pkt3(0x2a, 2 + 3), VTX_Z,
        PRIM_TRI_LIST | (WALK_INDEX << 4) | (6u << 16), (1u << 16) | 0u,
        (0u << 16) | 2u, (2u << 16) | 3u});
    fill_ref(24, 24, 40, 40, 0xff00ffffu);
    // 6: 3D_RNDR_GEN_INDX_PRIM, a fan from one vertex buffer (XY|Z)
    solid(0xffff00ffu);
    {
      const float vb[12] = {48, 24, 0.5f, 64, 24, 0.5f,
                            64, 40, 0.5f, 48, 40, 0.5f};
      for (u32 i = 0; i < 12; i++)
        vw32(VB + 0x4000 + i * 4, fbits(vb[i]));
      cp({pkt3(0x23, 4), VB + 0x4000, 4, VTX_Z,
          PRIM_TRI_FAN | (WALK_LIST << 4) | (4u << 16)});
      fill_ref(48, 24, 64, 40, 0xffff00ffu);
    }
    // 7: 3D_DRAW_IMMD_2 with SE_VTX_FMT: an R200 microcode packet, which
    // the R100 microcode the RV200 loads does not have -- it draws nothing
    solid(0xff808080u);
    {
      cp({pkt0(SE_VTX_FMT, 1), 0});
      std::vector<u32> p = {pkt3(0x35, 1 + 6),
                            PRIM_RECT_LIST | (WALK_DATA << 4) | (3u << 16)};
      const float q[6] = {72, 40, 88, 40, 88, 24};
      for (float v : q)
        p.push_back(fbits(v));
      cp(p);
    }
    // 8: through the registers: SE_VF_CNTL then SE_PORT_DATA
    solid(0xff408020u);
    {
      wr(SE_VTX_FMT, 0);
      wr(SE_VF_CNTL, PRIM_RECT_LIST | (WALK_DATA << 4) | (3u << 16));
      const float q[6] = {96, 40, 112, 40, 112, 24};
      for (int i = 0; i < 6; i++)
        wr(SE_PORT_DATA0 + u32(i % 16) * 4, fbits(q[i]));
      fill_ref(96, 24, 112, 40, 0xff408020u);
    }
    // 9: lines (strip and loop) and points
    solid(0xffffffffu);
    {
      std::vector<u32> p = {pkt3(0x29, 2 + 6), 0,
                            PRIM_LINE_STRIP | (WALK_DATA << 4) | (3u << 16)};
      const float q[6] = {10.5f, 60.5f, 50.5f, 60.5f, 50.5f, 90.5f};
      for (float v : q)
        p.push_back(fbits(v));
      cp(p);
      for (int x = 10; x < 50; x++)
        ref[size_t(60 * W + x)] = 0xffffffffu;
      for (int y = 60; y < 90; y++)
        ref[size_t(y * W + 50)] = 0xffffffffu;
      std::vector<u32> pt = {pkt3(0x29, 2 + 6), 0,
                             PRIM_POINT_LIST | (WALK_DATA << 4) | (3u << 16)};
      const float pp[6] = {70.5f, 70.5f, 80.25f, 75.75f, 90.0f, 80.0f};
      for (float v : pp)
        pt.push_back(fbits(v));
      cp(pt);
      ref[size_t(70 * W + 70)] = ref[size_t(75 * W + 80)] =
          ref[size_t(80 * W + 90)] = 0xffffffffu;
    }
    // 10: a wide line, 4 pixels (12.4), as a quad
    solid(0xffc08040u);
    {
      cp({pkt0(SE_CNTL, 1), (3u << 1) | (3u << 3) | (1u << 20) | (1u << 27),
          pkt0(SE_LINE_WIDTH, 1), 4u * 16});
      std::vector<u32> p = {pkt3(0x29, 2 + 4), 0,
                            PRIM_LINE_LIST | (WALK_DATA << 4) | (2u << 16)};
      const float q[4] = {10, 110, 110, 110};
      for (float v : q)
        p.push_back(fbits(v));
      cp(p);
      fill_ref(10, 108, 110, 112, 0xffc08040u);
    }
    int mx;
    const int d = compare("primitives", ref, 0, &mx);
    scene_report("primitives: quads, strips, polygon, lines, points, walks", d,
                 mx);
  }

  // -- scene: polygon stipple and line pattern -------------------------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    cp({pkt0(SE_CNTL, 1), (3u << 1) | (3u << 3) | (1u << 27),
        pkt0(RE_SOLID_COLOR, 1), 0xffffffffu, pkt0(RE_STIPPLE_ADDR, 1), 0});
    u32 stip[32];
    for (int i = 0; i < 32; i++) {
      stip[i] = rnd();
      cp({pkt0(RE_STIPPLE_DATA, 1), stip[i]});
    }
    cp({pkt0(RE_MISC, 1), 3u | (5u << 8), pkt0(PP_CNTL, 1), 1u});
    immd_xyzc(PRIM_RECT_LIST, {{0, 64, 0, 1, 1, 1, 1},
                               {128, 64, 0, 1, 1, 1, 1},
                               {128, 0, 0, 1, 1, 1, 1}});
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 128; x++)
        if ((stip[(y + 5) & 31] >> ((x + 3) & 31)) & 1)
          ref[size_t(y * W + x)] = 0xffffffffu;
    // a dashed line: pattern 0xf0f0 repeated twice per bit
    cp({pkt0(PP_CNTL, 1), 4u, pkt0(RE_LINE_PATTERN, 1),
        0xf0f0u | (2u << 16) | (1u << 29)});
    std::vector<u32> p = {pkt3(0x29, 2 + 4), 0,
                          PRIM_LINE_LIST | (WALK_DATA << 4) | (2u << 16)};
    const float q[4] = {0.5f, 100.5f, 120.5f, 100.5f};
    for (float v : q)
      p.push_back(fbits(v));
    cp(p);
    for (int x = 0; x < 120; x++)
      if ((0xf0f0u >> ((x / 2) & 15)) & 1)
        ref[size_t(100 * W + x)] = 0xffffffffu;
    int mx;
    const int d = compare("stipple-pattern", ref, 0, &mx);
    scene_report("polygon stipple and line pattern", d, mx);
  }

  // -- scene: TCL: a lit, fogged, perspective cube ---------------------------
  {
    base_state();
    clear_cb(0xff101010u);
    clear_z(0x00ffffffu);
    std::vector<u32> ref(size_t(W * H), 0xff101010u);
    std::vector<double> zref(size_t(W * H), 16777215.0);
    // matrices (row-major, one row per vector): model-view = rotate about
    // Y by 30 deg and X by 20, then translate z -4; projection = a 60
    // degree frustum, near 1 far 10, depth to [0, 1] (Direct3D's)
    auto mul = [](const double a[16], const double b[16], double o[16]) {
      for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
          double s = 0;
          for (int k = 0; k < 4; k++)
            s += a[r * 4 + k] * b[k * 4 + c];
          o[r * 4 + c] = s;
        }
    };
    const double ay = 30 * M_PI / 180, ax = 20 * M_PI / 180;
    const double ry[16] = {std::cos(ay),  0, std::sin(ay), 0, 0, 1, 0, 0,
                           -std::sin(ay), 0, std::cos(ay), 0, 0, 0, 0, 1};
    const double rx[16] = {1,
                           0,
                           0,
                           0,
                           0,
                           std::cos(ax),
                           -std::sin(ax),
                           0,
                           0,
                           std::sin(ax),
                           std::cos(ax),
                           0,
                           0,
                           0,
                           0,
                           1};
    const double tr[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -4, 0, 0, 0, 1};
    double rxy[16], mv[16], mvp[16];
    mul(rx, ry, rxy);
    mul(tr, rxy, mv);
    const double f = 1 / std::tan(30 * M_PI / 180), n = 1, fa = 10;
    const double pr[16] = {
        f, 0, 0,  0, 0, f, 0, 0, 0, 0, fa / (n - fa), n * fa / (n - fa),
        0, 0, -1, 0};
    mul(pr, mv, mvp);
    // the inverse transpose of the rotation part = the rotation itself
    // (orthonormal), translation irrelevant for normals
    auto vec_upload = [&](u32 index, const double *v4, int count) {
      std::vector<u32> p = {pkt0(SE_TCL_STATE_FLUSH, 1), 0,
                            pkt0(SE_TCL_VECTOR_INDX_REG, 1), index | (1u << 16),
                            pkt0_one(SE_TCL_VECTOR_DATA_REG, u32(count * 4))};
      for (int i = 0; i < count * 4; i++)
        p.push_back(fbits(float(v4[i])));
      cp(p);
    };
    vec_upload(0, mvp, 4); // matrix 0: model-view-projection
    vec_upload(4, mv, 4);  // matrix 1: model-view
    vec_upload(8, mv, 4);  // matrix 2: its inverse transpose (rotation)
    // light 0: directional from the upper left front (eye space), white;
    // light 1: a local orange light with linear attenuation and a specular
    // colour: with the viewer at infinity its half vector needs the eye
    // vector, loaded as Mesa's update_light loads it for GL's eye space,
    // (0, 0, -1): the direction the viewer looks in
    const double L0[3] = {-0.4, 0.5, 0.768};
    const double l0n = std::sqrt(L0[0] * L0[0] + L0[1] * L0[1] + L0[2] * L0[2]);
    const double L0n[4] = {L0[0] / l0n, L0[1] / l0n, L0[2] / l0n, 0};
    double H0[4] = {L0n[0], L0n[1], L0n[2] + 1, 0};
    const double h0n = std::sqrt(H0[0] * H0[0] + H0[1] * H0[1] + H0[2] * H0[2]);
    for (int k = 0; k < 3; k++)
      H0[k] /= h0n;
    const double amb0[4] = {0.1, 0.1, 0.1, 1}, dif0[4] = {0.8, 0.8, 0.8, 1},
                 spc0[4] = {0.6, 0.6, 0.6, 1};
    vec_upload(64, amb0, 1);
    vec_upload(72, dif0, 1);
    vec_upload(80, spc0, 1);
    vec_upload(88, L0n, 1);
    vec_upload(96, H0, 1);
    const double P1[4] = {1.5, -1.0, -2.0, 1};
    const double amb1[4] = {0, 0, 0, 1}, dif1[4] = {0.9, 0.5, 0.1, 1},
                 spc1[4] = {0.5, 0.4, 0.3, 1}, att1[4] = {0, 0.3, 1.0, 0},
                 dir1[4] = {0, 0, 0, 0};
    vec_upload(65, amb1, 1);
    vec_upload(73, dif1, 1);
    vec_upload(81, spc1, 1);
    vec_upload(89, P1, 1);
    vec_upload(97, dir1, 1);
    vec_upload(105, att1, 1);
    const double glob[4] = {0.05, 0.05, 0.1, 1}, eye[4] = {0, 0, -1, 1};
    vec_upload(122, glob, 1);
    vec_upload(124, eye, 1);
    // fog: linear from 3.5 to 8 (C = end / (end - start), D = -1 / (e - s))
    const double fogp[4] = {0, 8.0 / 4.5, -1.0 / 4.5, 0};
    vec_upload(123, fogp, 1);
    // range cutoff for light 1: large
    cp({pkt0(SE_TCL_SCALAR_INDX_REG, 1), 33u | (1u << 16),
        pkt0(SE_TCL_SCALAR_DATA_REG, 1), fbits(1e30f)});
    const float mat_e[4] = {0, 0, 0, 0}, mat_a[4] = {0.3f, 0.3f, 0.3f, 1},
                mat_d[4] = {0.2f, 0.6f, 0.9f, 1}, mat_s[4] = {1, 1, 1, 1};
    std::vector<u32> mp = {pkt0(SE_TCL_MATERIAL_EMISSIVE, 17)};
    for (const float *m : {mat_e, mat_a, mat_d, mat_s})
      for (int k = 0; k < 4; k++)
        mp.push_back(fbits(m[k]));
    mp.push_back(fbits(16.0f));
    cp(mp);
    // TCL state: matrices 0 (MVP), 1 (MV), 2 (IT MV); lighting with
    // separate specular; the lights' flags; fog linear; the viewport
    cp({pkt0(SE_TCL_MATRIX_SELECT_0, 2),
        1u | (2u << 16),
        0u,
        pkt0(SE_TCL_OUTPUT_VTX_SEL, 1),
        1u | 2u | 4u,
        pkt0(SE_TCL_LIGHT_MODEL_CTL, 1),
        1u | (1u << 3) | (1u << 5) | (1u << 16) | (1u << 18) | (1u << 20) |
            (1u << 22),
        pkt0(SE_TCL_PER_LIGHT_CTL_0, 1),
        (1u | 2u | 4u) | ((1u | 2u | 4u | 8u | 64u) << 16),
        pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        3u << 8,
        pkt0(SE_CNTL_STATUS, 1),
        0,
        pkt0(SE_VPORT_XSCALE, 6),
        fbits(64.0f),
        fbits(64.0f),
        fbits(-64.0f),
        fbits(64.0f),
        fbits(1.0f),
        fbits(0.0f),
        pkt0(SE_CNTL, 1),
        1u | (0u << 1) | (3u << 3) | (2u << 8) | (2u << 10) | (2u << 12) |
            (2u << 14) | (3u << 24) | (1u << 27),
        pkt0(RB3D_CNTL, 1),
        ROUNDING | (6u << 10) | (1u << 8),
        pkt0(RB3D_ZSTENCILCNTL, 1),
        2u | (1u << 4) | (1u << 30),
        pkt0(PP_CNTL, 1),
        (1u << 21) | (1u << 22),
        pkt0(PP_FOG_COLOR, 1),
        0x00808080u});
    // the cube: 6 faces x 2 triangles; arrays: position (XYZ), normal
    const double cube_n[6][3] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                 {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
    std::vector<double> pos, nor;
    for (const auto &nn : cube_n) {
      // two axes spanning the face, counter-clockwise seen from outside
      double u[3] = {nn[1], nn[2], nn[0]}, v[3];
      v[0] = nn[1] * u[2] - nn[2] * u[1];
      v[1] = nn[2] * u[0] - nn[0] * u[2];
      v[2] = nn[0] * u[1] - nn[1] * u[0];
      const double cs[6][2] = {{-1, -1}, {1, -1}, {1, 1},
                               {-1, -1}, {1, 1},  {-1, 1}};
      for (const auto &c2 : cs)
        for (int k = 0; k < 3; k++) {
          pos.push_back(nn[k] + c2[0] * u[k] + c2[1] * v[k]);
          if (k == 2)
            for (int m = 0; m < 3; m++)
              nor.push_back(nn[m]);
        }
    }
    const u32 nv = u32(pos.size() / 3);
    const u32 PA = TEX + 0x50000, NA = TEX + 0x60000;
    for (u32 i = 0; i < nv * 3; i++) {
      vw32(PA + i * 4, fbits(float(pos[i])));
      vw32(NA + i * 4, fbits(float(nor[i])));
    }
    cp({pkt3(0x2f, 4), 2, 3 | (3u << 8) | (3u << 16) | (3u << 24), PA, NA});
    cp({pkt3(0x28, 2), VTX_Z | VTX_N0,
        PRIM_TRI_LIST | (WALK_LIST << 4) | (1u << 9) | (nv << 16)});

    // The reference: the same transform, lighting and fog in double, then
    // the reference rasteriser on vertices snapped as the setup engine
    // snaps them (1/16 pixel, truncated).
    struct TV {
      double x, y, z, col[3], spec[3], fog;
    };
    auto tcl_ref = [&](const double *p3, const double *n3) {
      const double p4[4] = {p3[0], p3[1], p3[2], 1};
      double clip[4] = {0, 0, 0, 0}, e[4] = {0, 0, 0, 0}, nn[3] = {0, 0, 0};
      for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
          clip[r] += mvp[r * 4 + k] * p4[k];
          e[r] += mv[r * 4 + k] * p4[k];
        }
      for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
          nn[r] += mv[r * 4 + k] * n3[k];
      TV t;
      t.x = clip[0] / clip[3] * 64 + 64;
      t.y = clip[1] / clip[3] * -64 + 64;
      t.z = clip[2] / clip[3];
      double dif[3], spc[3] = {0, 0, 0};
      for (int k = 0; k < 3; k++)
        dif[k] = mat_e[k] + glob[k] * mat_a[k];
      // light 0, directional
      const double ndl0 = nn[0] * L0n[0] + nn[1] * L0n[1] + nn[2] * L0n[2];
      for (int k = 0; k < 3; k++)
        dif[k] += amb0[k] * mat_a[k] + std::max(0.0, ndl0) * dif0[k] * mat_d[k];
      if (ndl0 > 0) {
        const double ndh = nn[0] * H0[0] + nn[1] * H0[1] + nn[2] * H0[2];
        if (ndh > 0)
          for (int k = 0; k < 3; k++)
            spc[k] += std::pow(ndh, 16.0) * spc0[k] * mat_s[k];
      }
      // light 1, local, 1 / (1 + 0.3 d)
      double L[3] = {P1[0] - e[0], P1[1] - e[1], P1[2] - e[2]};
      const double d = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
      for (double &k : L)
        k /= d;
      const double at = 1 / (1 + 0.3 * d);
      const double ndl1 = nn[0] * L[0] + nn[1] * L[1] + nn[2] * L[2];
      for (int k = 0; k < 3; k++)
        dif[k] += at * std::max(0.0, ndl1) * dif1[k] * mat_d[k];
      if (ndl1 > 0) {
        // the viewer at infinity, towards +z in GL's eye space
        double Hv[3] = {L[0], L[1], L[2] + 1};
        const double hn =
            std::sqrt(Hv[0] * Hv[0] + Hv[1] * Hv[1] + Hv[2] * Hv[2]);
        const double ndh = (nn[0] * Hv[0] + nn[1] * Hv[1] + nn[2] * Hv[2]) / hn;
        if (ndh > 0)
          for (int k = 0; k < 3; k++)
            spc[k] += at * std::pow(ndh, 16.0) * spc1[k] * mat_s[k];
      }
      for (int k = 0; k < 3; k++) {
        t.col[k] = std::min(1.0, std::max(0.0, dif[k]));
        t.spec[k] = std::min(1.0, std::max(0.0, spc[k]));
      }
      t.fog = std::min(1.0, std::max(0.0, 8.0 / 4.5 - std::fabs(e[2]) / 4.5));
      return t;
    };
    for (u32 tri = 0; tri < nv / 3; tri++) {
      TV v[3];
      for (int i = 0; i < 3; i++)
        v[i] =
            tcl_ref(&pos[(tri * 3 + u32(i)) * 3], &nor[(tri * 3 + u32(i)) * 3]);
      for (TV &t : v) {
        t.x = std::floor(t.x * 16) / 16;
        t.y = std::floor(t.y * 16) / 16;
      }
      const double area = (v[1].x - v[0].x) * (v[2].y - v[0].y) -
                          (v[2].x - v[0].x) * (v[1].y - v[0].y);
      if (area >= 0)
        continue; // back face: counter-clockwise on screen is front
      ref_triangle(
          {v[0].x, v[0].y}, {v[1].x, v[1].y}, {v[2].x, v[2].y}, W, H,
          [&](int x, int y, double, double, double) {
            const double px = x + 0.5, py = y + 0.5;
            const double la = ((v[1].x - px) * (v[2].y - py) -
                               (v[2].x - px) * (v[1].y - py)) /
                              area;
            const double lb = ((v[2].x - px) * (v[0].y - py) -
                               (v[0].x - px) * (v[2].y - py)) /
                              area;
            const double lc = 1 - la - lb;
            const double z = la * v[0].z + lb * v[1].z + lc * v[2].z;
            const double zq = double(
                std::llround(std::min(1.0, std::max(0.0, z)) * 16777215.0));
            double &zo = zref[size_t(y * W + x)];
            if (!(zq < zo))
              return;
            zo = zq;
            const double fg = la * v[0].fog + lb * v[1].fog + lc * v[2].fog;
            const double fc = 128.0 / 255;
            float o[4] = {0, 0, 0, 1};
            for (int k = 0; k < 3; k++) {
              const double c =
                  la * v[0].col[k] + lb * v[1].col[k] + lc * v[2].col[k];
              const double sp =
                  la * v[0].spec[k] + lb * v[1].spec[k] + lc * v[2].spec[k];
              o[k] = float(std::min(1.0, c + sp) * fg + fc * (1 - fg));
            }
            ref[size_t(y * W + x)] = argbf(o);
          });
    }
    int mx;
    const int d = compare("tcl-lit-cube", ref, 3, &mx);
    // a vertex's float rounding can move a snapped edge by a sixteenth:
    // a handful of edge pixels may differ
    scene_report("TCL: transform, 2 lights, specular, fog, Z, culling", d, mx,
                 12);
  }

  // Mesa's radeonUploadTexMatrix (radeon_state.c), transcribed: a GL
  // texture matrix (column-major `src`) as the four vectors the driver
  // loads. For a 2D target the third and fourth rows change places (the
  // unit's third output is Q), and for a two-coordinate set without
  // texture generation (`swapcols`) the third and fourth columns too (the
  // unit's third input is the set's Q slot). Mesa's first two elements of
  // the last two vectors in the swapcols case are as it has them.
  auto mesa_texmat = [](const double *src, bool target2d, bool swapcols,
                        double *dest) {
    if (target2d && swapcols) {
      const int order[16] = {0, 4, 12, 8,  1, 5, 13, 9,
                             2, 6, 15, 11, 3, 7, 14, 10};
      for (int i = 0; i < 16; i++)
        dest[i] = src[order[i]];
    } else if (target2d) {
      int k = 0;
      for (int i : {0, 1, 3, 2})
        for (int c = 0; c < 4; c++)
          dest[k++] = src[i + 4 * c];
    } else {
      int k = 0;
      for (int i = 0; i < 4; i++)
        for (int c = 0; c < 4; c++)
          dest[k++] = src[i + 4 * c];
    }
  };

  // -- scene: TCL texture matrix, user clip plane, near-plane clipping ------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    auto vec_upload = [&](u32 index, const double *v4, int count) {
      std::vector<u32> p = {pkt0(SE_TCL_VECTOR_INDX_REG, 1), index | (1u << 16),
                            pkt0_one(SE_TCL_VECTOR_DATA_REG, u32(count * 4))};
      for (int i = 0; i < count * 4; i++)
        p.push_back(fbits(float(v4[i])));
      cp(p);
    };
    const double ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // matrix 0: identity MVP and MV; matrix 3: the texture matrix
    // s' = 2s + 0.25, t' = 3t -- GL's (column-major, the translation in
    // the fourth column) uploaded as Mesa uploads it for a two-coordinate
    // set on a 2D texture (mesa_texmat with the column swap): the
    // translation then meets the unit's third input, which is 1
    const double tm_gl[16] = {2, 0, 0, 0, 0,    3, 0, 0,
                              0, 0, 1, 0, 0.25, 0, 0, 1};
    double tm[16];
    mesa_texmat(tm_gl, true, true, tm);
    vec_upload(0, ident, 4);
    vec_upload(12, tm, 4);
    // the user clip plane 0: keep clip y <= 0.5 w  (0, -1, 0, 0.5)
    const double ucp0[4] = {0, -1, 0, 0.5};
    vec_upload(116, ucp0, 1);
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++)
        vw32(TEX + u32(y * 8 + x) * 4,
             0xff000000u | (u32(x * 32) << 16) | (u32(y * 32) << 8) | 0x80);
    set_tex(0, TEX, 6 | (1u << 6) | (1u << 31), 3, 3, 0, C_REPLACE_T0,
            A_REPLACE_T0);
    cp({pkt0(PP_CNTL, 1),
        (1u << 4) | (1u << 12),
        pkt0(SE_TCL_MATRIX_SELECT_0, 2),
        0u,
        0u | (3u << 16),
        pkt0(SE_TCL_OUTPUT_VTX_FMT, 2),
        VTX_Z | VTX_W0 | VTX_PKCOLOR | VTX_ST0,
        1u | (8u << 16),
        pkt0(SE_TCL_TEXTURE_PROC_CTL, 1),
        (1u << 0) | (1u << 4) | (0u << 16),
        pkt0(SE_TCL_LIGHT_MODEL_CTL, 1),
        0,
        pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        1u | (1u << 2),
        pkt0(SE_CNTL_STATUS, 1),
        0,
        pkt0(SE_VPORT_XSCALE, 6),
        fbits(64.0f),
        fbits(64.0f),
        fbits(-64.0f),
        fbits(64.0f),
        fbits(1.0f),
        fbits(0.0f),
        pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (2u << 8) | (2u << 10) | (3u << 24) |
            (1u << 27)});
    // a full-viewport quad (two triangles), XYZ + ST0, w = 1
    const float q[4][5] = {{-1, -1, 0.5f, 0, 0},
                           {1, -1, 0.5f, 1, 0},
                           {1, 1, 0.5f, 1, 1},
                           {-1, 1, 0.5f, 0, 1}};
    std::vector<u32> p = {pkt3(0x29, 2 + 4 * 5), VTX_Z | VTX_ST0,
                          PRIM_TRI_FAN | (WALK_DATA << 4) | (1u << 9) |
                              (4u << 16)};
    for (const auto &v : q)
      for (float c : v)
        p.push_back(fbits(c));
    cp(p);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const double ny = -(y + 0.5 - 64) / 64; // ndc y
        if (ny > 0.5)
          continue; // the user clip plane
        const double s = (x + 0.5) / 128, t = (ny + 1) / 2;
        const double s2 = 2 * s + 0.25, t2 = 3 * t;
        const int i = ((int(std::floor(s2 * 8)) % 8) + 8) % 8;
        const int j = ((int(std::floor(t2 * 8)) % 8) + 8) % 8;
        ref[size_t(y * W + x)] =
            0xff000000u | (u32(i * 32) << 16) | (u32(j * 32) << 8) | 0x80;
      }
    int mx;
    int d = compare("tcl-texmatrix-ucp", ref, 0, &mx);
    scene_report("TCL: texture matrix, user clip plane", d, mx, 8);

    // near-plane clipping: a flat triangle with a vertex behind the eye,
    // its coverage solved per pixel in clip space. Programmed as Mesa
    // programs the chip: OpenGL's projection (z/w from -1 at the near
    // plane to 1 at the far one) and a viewport that maps that to 0..1
    // (radeonUpdateWindow), so the volume is -w <= z <= w
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> nref(size_t(W * H), 0xff000000u);
    const double f = 1.0, n = 1, fa = 10;
    const double pr[16] = {f,
                           0,
                           0,
                           0,
                           0,
                           f,
                           0,
                           0,
                           0,
                           0,
                           (fa + n) / (n - fa),
                           2 * n * fa / (n - fa),
                           0,
                           0,
                           -1,
                           0};
    vec_upload(0, pr, 4);
    cp({pkt0(SE_TCL_MATRIX_SELECT_0, 2),
        0u,
        0u,
        pkt0(SE_TCL_OUTPUT_VTX_SEL, 1),
        1u,
        pkt0(SE_TCL_TEXTURE_PROC_CTL, 1),
        0,
        pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        1u,
        pkt0(SE_CNTL_STATUS, 1),
        0,
        pkt0(SE_VPORT_XSCALE, 6),
        fbits(64.0f),
        fbits(64.0f),
        fbits(-64.0f),
        fbits(64.0f),
        fbits(0.5f),
        fbits(0.5f),
        pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (1u << 8) | (1u << 10) | (3u << 6) |
            (3u << 24) | (1u << 27)});
    const double tri[3][3] = {{-2, -1, -3}, {2, -1, -3}, {0, -0.5, 2}};
    std::vector<u32> np = {pkt3(0x29, 2 + 3 * 4), VTX_Z | VTX_PKCOLOR,
                           PRIM_TRI_LIST | (WALK_DATA << 4) | (1u << 9) |
                               VF_RGBA | (3u << 16)};
    for (const auto &v : tri) {
      for (double c : v)
        np.push_back(fbits(float(c)));
      np.push_back(0xff40c0ffu);
    }
    cp(np);
    // clip coordinates of the vertices
    double cv[3][4];
    for (int i = 0; i < 3; i++)
      for (int r = 0; r < 4; r++)
        cv[i][r] = pr[r * 4] * tri[i][0] + pr[r * 4 + 1] * tri[i][1] +
                   pr[r * 4 + 2] * tri[i][2] + pr[r * 4 + 3];
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const double X = (x + 0.5 - 64) / 64, Y = -(y + 0.5 - 64) / 64;
        // solve a*P0 + b*P1 + c*P2 with p.x = X p.w, p.y = Y p.w, a+b+c=1
        double m[3][4];
        for (int i = 0; i < 3; i++) {
          m[0][i] = cv[i][0] - X * cv[i][3];
          m[1][i] = cv[i][1] - Y * cv[i][3];
          m[2][i] = 1;
        }
        m[0][3] = m[1][3] = 0;
        m[2][3] = 1;
        for (int c = 0; c < 3; c++) { // Gauss-Jordan
          int piv = c;
          for (int r = c + 1; r < 3; r++)
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c]))
              piv = r;
          for (int k = 0; k < 4; k++)
            std::swap(m[c][k], m[piv][k]);
          if (std::fabs(m[c][c]) < 1e-12)
            goto next;
          for (int r = 0; r < 3; r++)
            if (r != c) {
              const double fct = m[r][c] / m[c][c];
              for (int k = 0; k < 4; k++)
                m[r][k] -= fct * m[c][k];
            }
        }
        {
          const double a = m[0][3] / m[0][0], b = m[1][3] / m[1][1],
                       c = m[2][3] / m[2][2];
          if (a < 0 || b < 0 || c < 0)
            continue;
          const double w = a * cv[0][3] + b * cv[1][3] + c * cv[2][3];
          const double z = a * cv[0][2] + b * cv[1][2] + c * cv[2][2];
          if (w > 0 && z >= -w && z <= w)
            nref[size_t(y * W + x)] = 0xffffc040u;
        }
      next:;
      }
    d = compare("tcl-near-clip", nref, 0, &mx);
    // pixels on the clipped edges may fall either way: allow a thin line
    scene_report("TCL: near-plane clipping (coverage)", d, mx, 40);
  }

  // -- scene: Render composite as X.org programs it --------------------------
  // (xf86-video-ati radeon_exa_render.c R100TextureSetup and
  // R100PrepareComposite: OVER with a mask, IN through the combiner;
  // non-power-of-two textures with PP_TEX_SIZE / PP_TEX_PITCH - 32;
  // coordinates normalised by the picture size; a RECT_LIST)
  {
    base_state();
    std::vector<u32> bg(size_t(W * H));
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        bg[size_t(y * W + x)] =
            0xff000000u | (u32(x * 2) << 16) | 0x4000u | u32(y * 2);
    for (int i = 0; i < W * H; i++)
      vw32(CB + u32(i) * 4, bg[size_t(i)]);
    std::vector<u32> ref = bg;
    // the source: 50x30 ARGB8888 (premultiplied), pitch 256 bytes; the
    // mask: 50x30 A8, pitch 64 bytes
    const int SW = 50, SH = 30;
    const u32 SRC = TEX, MSK = TEX + 0x10000;
    for (int y = 0; y < SH; y++)
      for (int x = 0; x < SW; x++) {
        const u32 a = u32(128 + (x * 127) / SW);
        const u32 r = (a * u32(x * 5)) / 255, g = (a * u32(y * 8)) / 255,
                  b = (a * 200) / 255;
        vw32(SRC + u32(y * 256 + x * 4), (a << 24) | (r << 16) | (g << 8) | b);
        vwr(MSK + u32(y * 64 + x), 1, u32(y * 255 / (SH - 1)));
      }
    const u32 txf_src = 6 | (1u << 6) | (1u << 7) | (0u << 24);
    const u32 txf_msk = 0 | (1u << 6) | (1u << 7) | (1u << 24);
    cp({pkt0(PP_TXFILTER_0, 1), (2u << 23) | (2u << 27), pkt0(PP_TXFORMAT_0, 1),
        txf_src, pkt0(PP_TEX_SIZE_0, 1), u32(SW - 1) | (u32(SH - 1) << 16),
        pkt0(PP_TEX_PITCH_0, 1), 256 - 32, pkt0(PP_TXOFFSET_0, 1), SRC,
        pkt0(PP_TXFILTER_0 + PP_UNIT_STRIDE, 1), (2u << 23) | (2u << 27),
        pkt0(PP_TXFORMAT_0 + PP_UNIT_STRIDE, 1), txf_msk,
        pkt0(PP_TEX_SIZE_0 + 8, 1), u32(SW - 1) | (u32(SH - 1) << 16),
        pkt0(PP_TEX_PITCH_0 + 8, 1), 64 - 32,
        pkt0(PP_TXOFFSET_0 + PP_UNIT_STRIDE, 1), MSK, pkt0(PP_CNTL, 1),
        (1u << 4) | (1u << 5) | (1u << 12), pkt0(RB3D_CNTL, 1),
        ROUNDING | (6u << 10) | 1u,
        // IN: T0 colour x T1 alpha (A * B + C, C = 0)
        pkt0(PP_TXCBLEND_0, 1), 10u | (13u << 5) | (0u << 10) | (1u << 23),
        pkt0(PP_TXABLEND_0, 1), 5u | (6u << 4) | (0u << 8) | (1u << 23),
        pkt0(SE_VTX_FMT, 1), VTX_ST0 | VTX_ST1,
        // OVER: ONE, ONE_MINUS_SRC_ALPHA
        pkt0(RB3D_BLENDCNTL, 1), (33u << 16) | (39u << 24)});
    const float dx = 20, dy = 40;
    std::vector<u32> p = {pkt3(0x29, 2 + 3 * 6), VTX_ST0 | VTX_ST1,
                          PRIM_RECT_LIST | (WALK_DATA << 4) | (3u << 16)};
    const float vx[3] = {dx, dx + SW, dx + SW}, vy[3] = {dy + SH, dy + SH, dy};
    for (int i = 0; i < 3; i++) {
      const float s = (vx[i] - dx) / SW, t = (vy[i] - dy) / SH;
      for (float v : {vx[i], vy[i], s, t, s, t})
        p.push_back(fbits(v));
    }
    cp(p);
    for (int y = 0; y < SH; y++)
      for (int x = 0; x < SW; x++) {
        float s[4], d[4], o[4];
        unargb(vr32(SRC + u32(y * 256 + x * 4)), s);
        const float m = float(vrd(MSK + u32(y * 64 + x), 1)) / 255;
        const size_t i = size_t((int(dy) + y) * W + int(dx) + x);
        unargb(bg[i], d);
        for (int c = 0; c < 4; c++) {
          o[c] = clampf(s[c] * m + d[c] * (1 - s[3] * m));
        }
        ref[i] = argbf(o);
      }
    int mx;
    const int d = compare("render-composite", ref, 2, &mx);
    scene_report("X.org Render composite (OVER, IN mask, NPOT)", d, mx);
  }

  // -- scene: LOD bias, the other wrap modes, logic op, plane mask,
  // float colours, 32-bit indices, RNDR_GEN_INDX_PRIM with indices ----------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // a 32x32 mip chain of flat colours again
    const u32 lc[6] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu,
                       0xffffff00u, 0xff00ffffu, 0xffff00ffu};
    u32 off = TEX;
    for (int l = 0; l < 6; l++) {
      const int s = 32 >> l, pitch = std::max(32, s * 4);
      for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++)
          vw32(off + u32(y * pitch + x * 4), lc[l]);
      off += u32(pitch * s);
    }
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12)});
    // 1:1 (lod 0) with a bias of +1 (Mesa's encoding: 127 = +4.0, so
    // 32 = +1.008) and nearest-mip: level 1
    set_tex(0, TEX, 6 | (1u << 6), 5, 5, (2u << 1) | (32u << 8) | (5u << 16),
            C_REPLACE_T0, A_REPLACE_T0);
    rect_st(0, 0, 32, 32, 0, 0, 1, 1);
    for (int y = 0; y < 32; y++)
      for (int x = 0; x < 32; x++)
        ref[size_t(y * W + x)] = lc[1];
    // mirror once then clamp (3), and GL's clamp (6): a 4x4 texture
    // over -1..2
    const u32 border = 0xff102030u;
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
        vw32(TEX + 0x8000 + u32(y * 32 + x * 4),
             0xff000000u | (u32(x * 60) << 16) | (u32(y * 60) << 8) | 0x55);
    cp({pkt0(PP_BORDER_COLOR_0, 1), border});
    for (int m = 0; m < 2; m++) {
      const u32 mode = m ? 6u : 3u;
      set_tex(0, TEX + 0x8000, 6 | (1u << 6), 2, 2, (mode << 23) | (mode << 27),
              C_REPLACE_T0, A_REPLACE_T0);
      const float x0 = 40 + 40 * float(m);
      rect_st(x0, 0, x0 + 36, 36, -1, -1, 2, 2);
      for (int y = 0; y < 36; y++)
        for (int x = 0; x < 36; x++) {
          const double s = -1 + 3 * (x + 0.5) / 36, t = -1 + 3 * (y + 0.5) / 36;
          int i = int(std::floor(s * 4)), j = int(std::floor(t * 4));
          u32 v;
          if (!m) {
            auto mc = [](int k) {
              if (k < 0)
                k = -k - 1;
              return std::min(k, 3);
            };
            i = mc(i);
            j = mc(j);
            v = 0xff000000u | (u32(i * 60) << 16) | (u32(j * 60) << 8) | 0x55;
          } else {
            // GL_CLAMP with a nearest filter never shows the border: the
            // coordinate is clamped to the edge texels
            i = std::min(std::max(i, 0), 3);
            j = std::min(std::max(j, 0), 3);
            v = 0xff000000u | (u32(i * 60) << 16) | (u32(j * 60) << 8) | 0x55;
          }
          ref[size_t(y * W + int(x0) + x)] = v;
        }
    }
    // float colours (FPCOLOR | FPALPHA), XOR logic op, plane mask 0x00ffff00
    cp({pkt0(PP_CNTL, 1), 0, pkt0(RB3D_CNTL, 1),
        ROUNDING | (6u << 10) | (1u << 6) | (1u << 1), pkt0(RB3D_ROPCNTL, 1),
        6u << 8, pkt0(RB3D_PLANEMASK, 1), 0x00ffff00u});
    for (int y = 40; y < 72; y++)
      for (int x = 0; x < 32; x++) {
        vw32(CB + u32(y * W + x) * 4, 0xff336699u);
        ref[size_t(y * W + x)] = 0xff336699u ^ (0xffa0b0c0u & 0x00ffff00u);
      }
    {
      std::vector<u32> p = {pkt3(0x29, 2 + 3 * 6), VTX_FPCOLOR | VTX_FPALPHA,
                            PRIM_RECT_LIST | (WALK_DATA << 4) | (3u << 16)};
      const float q[3][2] = {{0, 72}, {32, 72}, {32, 40}};
      for (const auto &v : q)
        for (float c : {v[0], v[1], float(0xa0) / 255, float(0xb0) / 255,
                        float(0xc0) / 255, 1.0f})
          p.push_back(fbits(c));
      cp(p);
    }
    // 32-bit indices through 3D_DRAW_INDX, and RNDR_GEN_INDX_PRIM with a
    // 16-bit index walk, solid colours
    cp({pkt0(RB3D_CNTL, 1), ROUNDING | 6u << 10, pkt0(RB3D_PLANEMASK, 1),
        0xffffffffu, pkt0(SE_CNTL, 1), (3u << 1) | (3u << 3) | (1u << 27),
        pkt0(RE_SOLID_COLOR, 1), 0xff80ff80u});
    const u32 VB = TEX + 0x20000;
    const float xy[8] = {40, 40, 72, 40, 72, 72, 40, 72};
    for (int i = 0; i < 8; i++)
      vw32(VB + u32(i) * 4, fbits(xy[i]));
    cp({pkt3(0x2f, 3), 1, 2 | (2u << 8), VB});
    cp({pkt3(0x2a, 2 + 6), 0,
        PRIM_TRI_LIST | (WALK_INDEX << 4) | VF_INDEX_32 | (6u << 16), 0, 1, 2,
        0, 2, 3});
    for (int y = 40; y < 72; y++)
      for (int x = 40; x < 72; x++)
        ref[size_t(y * W + x)] = 0xff80ff80u;
    cp({pkt0(RE_SOLID_COLOR, 1), 0xffff8080u});
    const float xy2[8] = {80, 40, 112, 40, 112, 72, 80, 72};
    for (int i = 0; i < 8; i++)
      vw32(VB + 0x100 + u32(i) * 4, fbits(xy2[i]));
    cp({pkt3(0x23, 4 + 2), VB + 0x100, 4, 0,
        PRIM_TRI_FAN | (WALK_INDEX << 4) | (4u << 16), (1u << 16) | 0u,
        (3u << 16) | 2u});
    for (int y = 40; y < 72; y++)
      for (int x = 80; x < 112; x++)
        ref[size_t(y * W + x)] = 0xffff8080u;
    int mx;
    const int d = compare("misc-state", ref, 0, &mx);
    scene_report("LOD bias, mirror-once/GL-clamp, ROP, plane mask, "
                 "float colours, 32-bit and RNDR_GEN indices",
                 d, mx);
  }

  // ======================================================================
  // The features added to close the model's shortcuts (docs/radeon.md)
  // ======================================================================
  // a solid-colour rectangle list: X, Y and a packed colour
  auto rect_c = [&](float x0, float y0, float x1, float y1, u32 argb) {
    cp({pkt3(0x29, 2 + 9), VTX_PKCOLOR,
        PRIM_RECT_LIST | (WALK_DATA << 4) | (3u << 16), fbits(x0), fbits(y1),
        argb, fbits(x1), fbits(y1), argb, fbits(x1), fbits(y0), argb});
  };
  auto fill_rect = [&](std::vector<u32> &img, int x0, int y0, int x1, int y1,
                       u32 v) {
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++)
        img[size_t(y * W + x)] = v;
  };
  // the reference's own reading of a micro-tiled surface (Mesa
  // radeon_tile.c: 32-byte tiles, 4x2 at 32 bpp, 8x2 at 16)
  auto ref_tiled = [](u32 pitch_px, u32 bpp, int x, int y) {
    const u32 tw = bpp == 4 ? 4 : 8, th = 2;
    return (u32(y) / th) * th * pitch_px * bpp + (u32(x) / tw) * 32 +
           (u32(y) % th) * tw * bpp + (u32(x) % tw) * bpp;
  };
  auto bswap = [](u32 v) {
    return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
  };

  // -- scene: tiled and endian-swapped colour buffer and texture ---------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // (a) four rectangles into a micro-tiled, dword-swapped colour buffer
    // elsewhere, read back through the reference's own de-tiling
    const u32 CB2 = TEX + 0x80000;
    for (u32 i = 0; i < u32(W * H); i++)
      vw32(CB2 + i * 4, 0);
    cp({pkt0(RB3D_COLOROFFSET, 1), CB2, pkt0(RB3D_COLORPITCH, 1),
        u32(W) | (1u << 17) | (2u << 18)});
    rect_c(0, 0, 64, 32, 0xffff0000u);
    rect_c(64, 0, 128, 32, 0xff00ff00u);
    rect_c(0, 32, 64, 64, 0xff0000ffu);
    rect_c(64, 32, 128, 64, 0xff123456u);
    fill_rect(ref, 0, 0, 64, 32, 0xffff0000u);
    fill_rect(ref, 64, 0, 128, 32, 0xff00ff00u);
    fill_rect(ref, 0, 32, 64, 64, 0xff0000ffu);
    fill_rect(ref, 64, 32, 128, 64, 0xff123456u);
    sync();
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < W; x++)
        vw32(CB + u32(y * W + x) * 4,
             bswap(vr32(CB2 + ref_tiled(u32(W), 4, x, y))));
    // (b) a micro-tiled, byte-swapped (16-bit) RGB565 texture drawn 1:1
    // with nearest sampling into the lower half
    const int TW = 64;
    cp({pkt0(RB3D_COLOROFFSET, 1), CB, pkt0(RB3D_COLORPITCH, 1), u32(W)});
    std::vector<u16> tx(size_t(TW * TW));
    for (int y = 0; y < TW; y++)
      for (int x = 0; x < TW; x++)
        tx[size_t(y * TW + x)] =
            u16(((x * 31 / 63) << 11) | ((y * 63 / 63) << 5) | ((x ^ y) & 31));
    for (int y = 0; y < TW; y++)
      for (int x = 0; x < TW; x++) {
        const u32 a = TEX + ref_tiled(u32(TW), 2, x, y);
        const u16 v = tx[size_t(y * TW + x)];
        // byte swap within each 16-bit half (TXO_ENDIAN_BYTE_SWAP)
        vwr(a, 2, u32(((v & 0xff) << 8) | (v >> 8)));
      }
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12)});
    set_tex(0, TEX | 1u | (1u << 3), 4, 6, 6, 0, C_REPLACE_T0, A_REPLACE_T0);
    rect_st(32, 64, 96, 128, 0, 0, 1, 1);
    for (int y = 0; y < TW; y++)
      for (int x = 0; x < TW; x++) {
        const u16 v = tx[size_t(y * TW + x)];
        const u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        ref[size_t((64 + y) * W + 32 + x)] =
            0xff000000u | (((r << 3) | (r >> 2)) << 16) |
            (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
      }
    int mx;
    const int d = compare("tiling-endian", ref, 1, &mx);
    scene_report("micro tiling and endian swaps (colour, texture)", d, mx);
  }

  // -- scene: polygon offset, dithering and rounding --------------------
  {
    base_state();
    clear_cb(0xff000000u);
    clear_z(0x00ffffffu);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // a red quad at z 0.5, then the same quad in green with LESS: without
    // an offset it fails everywhere (equal depth); with SE_ZBIAS_CONSTANT
    // -4 steps (ZBIAS_ENABLE_TRI) it passes; then a sloped blue triangle
    // offset by a positive factor that pushes it behind the green
    cp({pkt0(RB3D_CNTL, 1), ROUNDING | (6u << 10) | (1u << 8),
        pkt0(RB3D_ZSTENCILCNTL, 1), 2u | (1u << 4) | (1u << 30)});
    immd_xyzc(PRIM_RECT_LIST, {{0, 32, 0.5f, 1, 0, 0, 1},
                               {64, 32, 0.5f, 1, 0, 0, 1},
                               {64, 0, 0.5f, 1, 0, 0, 1}});
    immd_xyzc(PRIM_RECT_LIST, {{0, 32, 0.5f, 0, 1, 0, 1},
                               {32, 32, 0.5f, 0, 1, 0, 1},
                               {32, 0, 0.5f, 0, 1, 0, 1}});
    const float step = 1.0f / 16777215.0f;
    cp({pkt0(SE_ZBIAS_FACTOR, 2), fbits(0.0f), fbits(-4 * step),
        pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (3u << 6) | (2u << 8) | (2u << 10) |
            (2u << 12) | (2u << 14) | (1u << 27) | (1u << 18)});
    immd_xyzc(PRIM_RECT_LIST, {{32, 32, 0.5f, 0, 1, 0, 1},
                               {64, 32, 0.5f, 0, 1, 0, 1},
                               {64, 0, 0.5f, 0, 1, 0, 1}});
    fill_rect(ref, 0, 0, 64, 32, 0xffff0000u);
    fill_rect(ref, 32, 0, 64, 32, 0xff00ff00u);
    // the sloped triangle: dz/dx = 0.004 a pixel; offset factor 1 and
    // constant 0 move it back by its slope, behind a quad at its own depth
    cp({pkt0(SE_ZBIAS_FACTOR, 2), fbits(0.0f), fbits(0.0f)});
    immd_xyzc(PRIM_RECT_LIST, {{0, 64, 0.3f, 1, 1, 1, 1},
                               {64, 64, 0.556f, 1, 1, 1, 1},
                               {64, 40, 0.556f, 1, 1, 1, 1}});
    cp({pkt0(SE_ZBIAS_FACTOR, 2), fbits(1.0f), fbits(0.0f)});
    immd_xyzc(PRIM_RECT_LIST, {{0, 64, 0.3f, 0, 0, 1, 1},
                               {64, 64, 0.556f, 0, 0, 1, 1},
                               {64, 40, 0.556f, 0, 0, 1, 1}});
    fill_rect(ref, 0, 40, 64, 64, 0xffffffffu);
    // rounding and dithering into an RGB565 buffer: a horizontal ramp of
    // red 0..1 in three bands: truncated, rounded, ordered dither
    const u32 CB16 = TEX + 0x80000;
    cp({pkt0(RB3D_ZSTENCILCNTL, 1), 2u | (7u << 4), pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (3u << 6) | (2u << 8) | (2u << 10) |
            (2u << 12) | (2u << 14) | (1u << 27),
        pkt0(RB3D_COLOROFFSET, 1), CB16, pkt0(RB3D_COLORPITCH, 1), u32(W)});
    std::vector<u16> got16(size_t(W * 3), 0), want16(size_t(W * 3), 0);
    const u32 modes[3] = {0, 1u << 3, (1u << 2) | (1u << 4)};
    for (int b = 0; b < 3; b++) {
      cp({pkt0(RB3D_CNTL, 1), (4u << 10) | modes[b]});
      // one line, x = 0..127: red at the pixel centre is (x + 0.5) / 128
      std::vector<u32> p = {pkt3(0x29, 2 + 2 * 4), VTX_FPCOLOR,
                            PRIM_LINE_LIST | (WALK_DATA << 4) | (2u << 16)};
      for (float x : {0.0f, 128.0f})
        for (float v : {x, float(80 + b), x / 128.0f, 0.0f, 0.0f})
          p.push_back(fbits(v));
      p[0] = pkt3(0x29, u32(p.size()) - 1);
      cp(p);
      for (int x = 0; x < W; x++) {
        // the line interpolates from x = 0 (0.0) to 128 (1.0): the pixel
        // at x takes the value at its centre, t = (x + 0.5) / 128
        const double v = (double(x) + 0.5) / 128.0 * 31.0;
        int q;
        if (b == 0)
          q = int(std::floor(v + 1e-4));
        else if (b == 1)
          q = int(std::floor(v + 0.5));
        else {
          static const int bayer[4][4] = {
              {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
          q = int(std::floor(v + (bayer[(80 + b) & 3][x & 3] + 0.5) / 16.0));
        }
        q = std::min(31, std::max(0, q));
        want16[size_t(b * W + x)] = u16(q << 11);
      }
    }
    for (int b = 0; b < 3; b++)
      for (int x = 0; x < W; x++)
        got16[size_t(b * W + x)] =
            u16(vrd(CB16 + u32((80 + b) * W + x) * 2, 2));
    int bad16 = 0;
    for (size_t i = 0; i < got16.size(); i++)
      bad16 += got16[i] != want16[i];
    int mx;
    const int d = compare("zbias-dither", ref, 0, &mx);
    scene_report("polygon offset (constant, slope factor)", d, mx);
    report("3D: truncation, rounding, ordered dither (RGB565)", bad16 == 0,
           bad16 ? std::to_string(bad16) + " pixels" : "");
    // horizontal error diffusion along a line: the reference carries the
    // error pixel to pixel
    cp({pkt0(RB3D_CNTL, 1), (4u << 10) | (1u << 2) | (1u << 5)});
    {
      std::vector<u32> p = {pkt3(0x29, 2 + 2 * 5), VTX_FPCOLOR,
                            PRIM_LINE_LIST | (WALK_DATA << 4) | (2u << 16)};
      for (float x : {0.0f, 128.0f})
        for (float v : {x, 90.0f, 1.0f / 3, 0.0f, 0.0f})
          p.push_back(fbits(v));
      cp(p);
    }
    int badd = 0;
    double err = 0;
    for (int x = 0; x < W; x++) {
      const double t = 31.0 / 3 + err;
      const int q = std::min(31, std::max(0, int(std::floor(t + 0.5))));
      err = t - q;
      badd += vrd(CB16 + u32(90 * W + x) * 2, 2) != u32(q << 11);
    }
    report("3D: horizontal error-diffusion dither", badd == 0,
           badd ? std::to_string(badd) + " pixels" : "");
  }

  // -- scene: cube map, table fog, anti-aliased polygon ------------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // six 8x8 faces, one colour each, +X -X +Y -Y +Z at the cube offsets,
    // -Z at TXOFFSET (Mesa's cube_emit_cs)
    const u32 face_col[6] = {0xffff0000u, 0xff00ffffu, 0xff00ff00u,
                             0xffff00ffu, 0xff0000ffu, 0xffffff00u};
    const u32 FB0 = TEX + 0xa0000;
    for (int f = 0; f < 6; f++)
      for (int i = 0; i < 64; i++)
        vw32(FB0 + u32(f) * 0x1000 + u32(i) * 4, face_col[f]);
    std::vector<u32> cub = {pkt0(0x1dd0, 5)};
    for (int f = 0; f < 5; f++)
      cub.push_back(FB0 + u32(f) * 0x1000);
    cp(cub);
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12)});
    set_tex(0, FB0 + 5 * 0x1000, 6 | (1u << 6) | (1u << 30), 3, 3, 0,
            C_REPLACE_T0, A_REPLACE_T0);
    // one rectangle per face, the direction (s, t, r) in ST0 and Q0
    const float dirs[6][3] = {{1, 0.1f, 0.2f}, {-1, 0.2f, 0.1f},
                              {0.1f, 1, 0.2f}, {0.2f, -1, 0.1f},
                              {0.1f, 0.2f, 1}, {0.2f, 0.1f, -1}};
    for (int f = 0; f < 6; f++) {
      const float x0 = float(f % 3) * 40, y0 = float(f / 3) * 40;
      std::vector<u32> p = {pkt3(0x29, 2 + 3 * 5), VTX_ST0 | VTX_Q0,
                            PRIM_RECT_LIST | (WALK_DATA << 4) | (3u << 16)};
      const float c3[3][2] = {{x0, y0 + 32}, {x0 + 32, y0 + 32}, {x0 + 32, y0}};
      for (const auto &c2 : c3)
        for (float v : {c2[0], c2[1], dirs[f][0], dirs[f][1], dirs[f][2]})
          p.push_back(fbits(v));
      cp(p);
      fill_rect(ref, int(x0), int(y0), int(x0) + 32, int(y0) + 32, face_col[f]);
    }
    // table fog: a triangle whose depth runs 0..1 left to right, fogged
    // to blue by a table of 256 entries (entry i = 255 - i)
    cp({pkt0(PP_CNTL, 1), 1u << 22, pkt0(PP_FOG_COLOR, 1),
        0x000000ffu | (1u << 24), pkt0(0x1a14, 1), 0});
    for (u32 i = 0; i < 256; i += 4) {
      u32 dw = 0;
      for (u32 k = 0; k < 4; k++)
        dw |= (255 - (i + k)) << (8 * k);
      cp({pkt0(0x1a18, 1), dw});
    }
    immd_xyzc(PRIM_RECT_LIST, {{0, 112, 0.0f, 1, 1, 1, 1},
                               {128, 112, 1.0f, 1, 1, 1, 1},
                               {128, 88, 1.0f, 1, 1, 1, 1}});
    for (int y = 88; y < 112; y++)
      for (int x = 0; x < W; x++) {
        const double z = (x + 0.5) / 128.0;
        const int idx = int(std::lround(z * 255));
        const double fv = (255 - idx) / 255.0;
        const float c[4] = {float(fv), float(fv), float(fv + (1 - fv)), 1};
        ref[size_t(y * W + x)] = argbf(c);
      }
    // an anti-aliased triangle blended over black: alpha times its 4x4
    // coverage
    cp({pkt0(PP_CNTL, 1), 2u << 24, pkt0(RB3D_CNTL, 1),
        ROUNDING | (6u << 10) | 1u, pkt0(RB3D_BLENDCNTL, 1),
        (38u << 16) | (39u << 24)});
    // on the setup engine's 1/16-pixel grid, so that snapping moves nothing
    const RefVtx ta = {80.25, 90.3125}, tb = {126.75, 100.125},
                 tc = {96.375, 127.625};
    immd_xyzc(PRIM_TRI_LIST, {{float(ta.x), float(ta.y), 0, 1, 1, 1, 1},
                              {float(tb.x), float(tb.y), 0, 1, 1, 1, 1},
                              {float(tc.x), float(tc.y), 0, 1, 1, 1, 1}});
    for (int y = 86; y < H; y++)
      for (int x = 76; x < W; x++) {
        int n = 0;
        for (int sy = 0; sy < 4; sy++)
          for (int sx = 0; sx < 4; sx++) {
            const double px = x + (sx + 0.5) / 4, py = y + (sy + 0.5) / 4;
            const RefVtx v3[3] = {ta, tb, tc};
            double area =
                (tb.x - ta.x) * (tc.y - ta.y) - (tc.x - ta.x) * (tb.y - ta.y);
            bool in = true;
            for (int i = 0; i < 3 && in; i++) {
              const RefVtx &p = v3[i], &q = v3[(i + 1) % 3];
              const double e =
                  (q.x - p.x) * (py - p.y) - (q.y - p.y) * (px - p.x);
              in = area > 0 ? e > 0 : e < 0;
            }
            n += in;
          }
        if (n) {
          const float a = float(n) / 16.0f;
          // over the fog band's colour where they overlap
          float dst[4];
          unargb(ref[size_t(y * W + x)], dst);
          // (the alpha blends too: a * a + 1 * (1 - a))
          const float c[4] = {a + dst[0] * (1 - a), a + dst[1] * (1 - a),
                              a + dst[2] * (1 - a), a * a + dst[3] * (1 - a)};
          ref[size_t(y * W + x)] = argbf(c);
        }
      }
    int mx;
    const int d = compare("cube-fogtable-aa", ref, 2, &mx);
    scene_report("cube map faces, table fog, anti-aliased polygon", d, mx);
  }

  // -- scene: floating-point and W depth, the HyperZ fast clear ----------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    // two triangles crossing in depth, in three formats: 24-bit float Z
    // (3), 32-bit float Z (5), 24-bit float W (9, from W0 = 1/w); the
    // nearer wins in each
    const u32 fmts[3] = {3, 5, 9};
    for (int k = 0; k < 3; k++) {
      clear_z(0xffffffffu);
      cp({pkt0(RB3D_CNTL, 1), ROUNDING | (6u << 10) | (1u << 8),
          pkt0(RB3D_ZSTENCILCNTL, 1), fmts[k] | (1u << 4) | (1u << 30)});
      const float y0 = float(k * 40);
      // X, Y, Z, W0 (1/w: nearer is larger), packed colour
      struct VW {
        float x, y, z, rhw;
        u32 c;
      };
      const VW t1[3] = {{4, y0 + 2, 0.2f, 1 / 2.0f, 0xffff0000u},
                        {124, y0 + 18, 0.8f, 1 / 8.0f, 0xffff0000u},
                        {4, y0 + 36, 0.2f, 1 / 2.0f, 0xffff0000u}};
      const VW t2[3] = {{124, y0 + 2, 0.1f, 1 / 1.5f, 0xff0000ffu},
                        {4, y0 + 18, 0.9f, 1 / 9.0f, 0xff0000ffu},
                        {124, y0 + 36, 0.1f, 1 / 1.5f, 0xff0000ffu}};
      std::vector<u32> p = {pkt3(0x29, 2 + 6 * 5), VTX_Z | VTX_W0 | VTX_PKCOLOR,
                            PRIM_TRI_LIST | (WALK_DATA << 4) | (6u << 16)};
      for (const VW *t : {t1, t2})
        for (int i = 0; i < 3; i++)
          for (u32 v : {fbits(t[i].x), fbits(t[i].y), fbits(t[i].z),
                        fbits(t[i].rhw), t[i].c})
            p.push_back(v);
      cp(p);
      // the reference: which triangle is nearer at each covered pixel (by
      // z, or by w for the W format), drawn in order with LESS
      std::vector<double> dref(size_t(W * H), 1e30);
      for (const VW *t : {t1, t2}) {
        const VW &a = t[0], &b = t[1], &c = t[2];
        const double area =
            (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        ref_triangle(
            {a.x, a.y}, {b.x, b.y}, {c.x, c.y}, W, H,
            [&](int x, int y, double, double, double) {
              const double px = x + 0.5, py = y + 0.5;
              const double la =
                  ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
              const double lb =
                  ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
              const double lc = 1 - la - lb;
              double dep;
              if (fmts[k] == 9)
                dep = 1 / (la * a.rhw + lb * b.rhw + lc * c.rhw);
              else
                dep = la * a.z + lb * b.z + lc * c.z;
              double &o = dref[size_t(y * W + x)];
              if (dep < o) {
                o = dep;
                ref[size_t(y * W + x)] = a.c;
              }
            });
      }
    }
    // the fast clear: Z zero everywhere, then 3D_CLEAR_ZMASK of the 8-line
    // bands 15 (y 120..127) from x 0 to 63 with clear value 1.0 (24-bit),
    // then a quad at z 0.5 with LESS: it shows only where the clear went
    clear_z(0);
    cp({pkt0(RB3D_ZSTENCILCNTL, 1), 2u | (1u << 4) | (1u << 30),
        pkt0(0x3230, 1), 0x00ffffffu});
    {
      // radeon_state.c: tileoffset = ((y1 >> 3) * pitch + x1) >> 6, START =
      // tileoffset * 8, COUNT = (((x2 & ~63) - (x1 & ~63)) >> 4) + 4
      const u32 y1 = 120, x1 = 0, x2 = 63;
      const u32 tileoffset = ((y1 >> 3) * u32(W) + x1) >> 6;
      cp({pkt3(0x32, 3), tileoffset * 8, (((x2 & ~63u) - (x1 & ~63u)) >> 4) + 4,
          0});
    }
    immd_xyzc(PRIM_RECT_LIST, {{0, 128, 0.5f, 0, 1, 0, 1},
                               {128, 128, 0.5f, 0, 1, 0, 1},
                               {128, 120, 0.5f, 0, 1, 0, 1}});
    fill_rect(ref, 0, 120, 64, 128, 0xff00ff00u);
    int mx;
    const int d = compare("floatz-wbuffer-fastclear", ref, 0, &mx);
    // depths equal on the crossing may resolve either way
    scene_report("float Z, W buffer, HyperZ fast clear", d, mx, 12);
  }

  // -- scene: TCL two-sided lighting, TCL culling, vertex blending -------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    auto vec4 = [&](u32 index, const float *v, int count) {
      std::vector<u32> p = {pkt0(SE_TCL_STATE_FLUSH, 1), 0,
                            pkt0(SE_TCL_VECTOR_INDX_REG, 1), index | (1u << 16),
                            pkt0_one(SE_TCL_VECTOR_DATA_REG, u32(count * 4))};
      for (int i = 0; i < count * 4; i++)
        p.push_back(fbits(v[i]));
      cp(p);
    };
    const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float shift[16] = {1, 0, 0, 0.5f, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    vec4(0, ident, 4); // matrix 0: identity
    vec4(4, shift, 4); // matrix 1: x + 0.5 (clip units)
    const float L0[4] = {0, 0, 1, 0}, H0[4] = {0, 0, 1, 0},
                dif[4] = {1, 1, 1, 1}, zero[4] = {0, 0, 0, 0},
                glob[4] = {0.2f, 0.2f, 0.2f, 1}, eye[4] = {0, 0, 1, 1};
    vec4(64, zero, 1);
    vec4(72, dif, 1);
    vec4(80, zero, 1);
    vec4(88, L0, 1);
    vec4(96, H0, 1);
    vec4(122, glob, 1);
    vec4(124, eye, 1);
    std::vector<u32> mp = {pkt0(SE_TCL_MATERIAL_EMISSIVE, 17)};
    const float me[4] = {0, 0, 0, 1}, ma[4] = {0, 0, 0, 1},
                md[4] = {0.6f, 0.4f, 0.2f, 1}, ms[4] = {0, 0, 0, 1};
    for (const float *m : {me, ma, md, ms})
      for (int k = 0; k < 4; k++)
        mp.push_back(fbits(m[k]));
    mp.push_back(fbits(1.0f));
    cp(mp);
    // viewport: clip [-1, 1] onto 0..128, y down
    cp({pkt0(SE_TCL_MATRIX_SELECT_0, 2),
        0u,
        0u,
        pkt0(SE_TCL_OUTPUT_VTX_SEL, 1),
        1u | 2u,
        pkt0(SE_TCL_LIGHT_MODEL_CTL, 1),
        1u,
        pkt0(SE_TCL_PER_LIGHT_CTL_0, 1),
        1u,
        pkt0(SE_CNTL_STATUS, 1),
        0,
        pkt0(SE_VPORT_XSCALE, 6),
        fbits(64.0f),
        fbits(64.0f),
        fbits(-64.0f),
        fbits(64.0f),
        fbits(1.0f),
        fbits(0.0f),
        pkt0(SE_CNTL, 1),
        1u | (3u << 1) | (3u << 3) | (3u << 6) | (2u << 8) | (2u << 10) |
            (2u << 12) | (2u << 14) | (3u << 24) | (1u << 27)});
    // a quad as a rectangle-list-free pair of triangles in clip space,
    // with a normal (0, 0, 1): position XYZ, normal
    auto tcl_quad = [&](float x0, float y0, float x1, float y1, bool ccw) {
      std::vector<u32> p = {pkt3(0x29, 2 + 6 * 6), VTX_Z | VTX_N0,
                            PRIM_TRI_LIST | (WALK_DATA << 4) | (1u << 9) |
                                (6u << 16)};
      // (x0, y0) is the clip-space lower left, the screen's too
      float c4[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
      const int ccw_i[6] = {0, 1, 2, 0, 2, 3}, cw_i[6] = {0, 2, 1, 0, 3, 2};
      for (int k = 0; k < 6; k++) {
        const int i = ccw ? ccw_i[k] : cw_i[k];
        for (float v : {c4[i][0], c4[i][1], 0.5f, 0.0f, 0.0f, 1.0f})
          p.push_back(fbits(v));
      }
      cp(p);
    };
    auto clip_rect = [&](float x0, float y0, float x1, float y1, u32 v) {
      fill_rect(
          ref, int(std::lround((x0 + 1) * 64)), int(std::lround((1 - y1) * 64)),
          int(std::lround((x1 + 1) * 64)), int(std::lround((1 - y0) * 64)), v);
    };
    const float lit[4] = {0.2f + 0.6f, 0.2f + 0.4f, 0.2f + 0.2f, 1};
    const float back[4] = {0.2f, 0.2f, 0.2f, 1};
    // the viewport's Y flip keeps the winding as the screen shows it:
    // counter-clockwise in clip space is counter-clockwise on the screen,
    // the front for SE_CNTL<0> and the TCL's CULL_FRONT_IS_CCW
    const u32 base_ucp = 1u << 28;
    // 1: without two-sided lighting both windings are lit the same
    cp({pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1), base_ucp});
    tcl_quad(-1.0f, 0.5f, -0.5f, 1.0f, true); // CCW on screen: front
    tcl_quad(-0.5f, 0.5f, 0.0f, 1.0f, false); // CW on screen: back
    clip_rect(-1.0f, 0.5f, -0.5f, 1.0f, argbf(lit));
    clip_rect(-0.5f, 0.5f, 0.0f, 1.0f, argbf(lit));
    // 2: with LIGHT_TWOSIDE the back face is lit with the normal reversed
    cp({pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1), base_ucp | (1u << 11)});
    tcl_quad(0.0f, 0.5f, 0.5f, 1.0f, true);
    tcl_quad(0.5f, 0.5f, 1.0f, 1.0f, false);
    clip_rect(0.0f, 0.5f, 0.5f, 1.0f, argbf(lit));
    clip_rect(0.5f, 0.5f, 1.0f, 1.0f, argbf(back));
    // 3: CULL_BACK in the TCL unit drops the back face
    cp({pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1), base_ucp | (1u << 30)});
    tcl_quad(-1.0f, 0.0f, -0.5f, 0.5f, true);
    tcl_quad(-0.5f, 0.0f, 0.0f, 0.5f, false);
    clip_rect(-1.0f, 0.0f, -0.5f, 0.5f, argbf(lit));
    // 4: vertex blending of matrix 0 (identity) and 1 (x + 0.5) with one
    // weight per vertex and VERTEX_BLEND_WGT_MINUS_ONE: weight 0.5 moves
    // the quad by 0.25; unlit vertex colours
    cp({pkt0(SE_TCL_LIGHT_MODEL_CTL, 1), 0u, pkt0(SE_TCL_OUTPUT_VTX_SEL, 1), 0u,
        pkt0(SE_TCL_MATRIX_SELECT_1, 1), 0u | (1u << 4),
        pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        base_ucp | (1u << 12) | (1u << 16) | (1u << 22)});
    {
      std::vector<u32> p = {
          pkt3(0x29, 2 + 6 * 5), VTX_Z | (1u << VTX_WEIGHT_SHIFT) | VTX_PKCOLOR,
          PRIM_TRI_LIST | (WALK_DATA << 4) | (1u << 9) | (6u << 16)};
      const float c4[4][2] = {
          {-1.0f, -0.5f}, {-0.5f, -0.5f}, {-0.5f, 0.0f}, {-1.0f, 0.0f}};
      const int ix[6] = {0, 2, 1, 0, 3, 2};
      for (int i : ix)
        for (u32 v : {fbits(c4[i][0]), fbits(c4[i][1]), fbits(0.5f),
                      fbits(0.5f), 0xff40c080u})
          p.push_back(v);
      cp(p);
      clip_rect(-0.75f, -0.5f, -0.25f, 0.0f, 0xff40c080u);
    }
    cp({pkt0(SE_TCL_MATRIX_SELECT_1, 1), 0u, pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        0u, pkt0(SE_CNTL_STATUS, 1), 1u << 8});
    int mx;
    const int d = compare("tcl-twoside-blend", ref, 1, &mx);
    scene_report("TCL two-sided lighting, TCL culling, vertex blending", d, mx);
  }

  // -- scene: TRI_TYPE_2, the 3-vertex lists, INDX_BUFFER -----------------
  {
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    cp({pkt0(RE_SOLID_COLOR, 1), 0xffffffffu, pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (1u << 27)});
    // TRI_TYPE_2 as a triangle list
    auto xy_draw = [&](u32 prim, const std::vector<float> &xy) {
      std::vector<u32> p = {pkt3(0x29, 2 + u32(xy.size())), 0,
                            prim | (WALK_DATA << 4) |
                                (u32(xy.size() / 2) << 16)};
      for (float v : xy)
        p.push_back(fbits(v));
      cp(p);
    };
    xy_draw(PRIM_TRI_FLAG, {8, 8, 40, 8, 8, 40});
    ref_triangle({8, 8}, {40, 8}, {8, 40}, W, H,
                 [&](int x, int y, double, double, double) {
                   ref[size_t(y * W + x)] = 0xffffffffu;
                 });
    // 3VRT_POINT_LIST: each vertex a point (pixel centres at +0.5, OpenGL)
    xy_draw(PRIM_POINT_LIST_3, {60, 10, 70, 10, 80, 10});
    for (int x : {60, 70, 80})
      ref[size_t(10 * W + x)] = 0xffffffffu;
    // 3VRT_LINE_LIST: the triple's three edges
    xy_draw(PRIM_LINE_LIST_3, {60.5f, 30.5f, 100.5f, 30.5f, 100.5f, 50.5f});
    // INDX_BUFFER with the R200 microcode's packets switched on: a
    // DRAW_INDX without indices, then the indices from a buffer (one
    // dword skipped, 16-bit indices)
    m_r200_packets = true;
    const u32 VB = TEX + 0xc0000, IXB = TEX + 0xc1000;
    const float vxy[8] = {8, 60, 40, 60, 40, 92, 8, 92};
    for (int i = 0; i < 8; i++)
      vw32(VB + u32(i) * 4, fbits(vxy[i]));
    vw32(IXB, 0xdeadbeefu);          // skipped
    vw32(IXB + 4, (1u << 16) | 0u);  // 0, 1
    vw32(IXB + 8, (0u << 16) | 2u);  // 2, 0
    vw32(IXB + 12, (3u << 16) | 2u); // 2, 3
    cp({pkt3(0x2f, 3), 1, 2 | (2u << 8), VB});
    cp({pkt3(0x2a, 2), 0, PRIM_TRI_LIST | (WALK_INDEX << 4) | (6u << 16)});
    cp({pkt3(0x33, 3), (1u << 16) | 0x810, IXB, 4});
    sync(); // the switch is the self-test's, not the FIFO's
    m_r200_packets = false;
    for (int y = 60; y < 92; y++)
      for (int x = 8; x < 40; x++)
        ref[size_t(y * W + x)] = 0xffffffffu;
    // and without the switch the same pair draws nothing (R100 microcode)
    cp({pkt3(0x2a, 2), 0, PRIM_TRI_LIST | (WALK_INDEX << 4) | (6u << 16)});
    cp({pkt3(0x33, 3), (1u << 16) | 0x810, IXB, 4});
    // the edges, OpenGL pixel centres (a vertex at 60.5 is pixel 60's
    // centre), the last pixel out: along x from 60, along y from 30, and
    // back from x = 100 to 61 with y = 30.5 + (x - 60) / 2 there, a
    // centre exactly between two rows going to the lower one on screen
    for (int x = 60; x < 100; x++)
      ref[size_t(30 * W + x)] = 0xffffffffu;
    for (int y = 30; y < 50; y++)
      ref[size_t(y * W + 100)] = 0xffffffffu;
    for (int x = 61; x <= 100; x++)
      ref[size_t((30 + (x - 60 + 1) / 2) * W + x)] = 0xffffffffu;
    int mx;
    const int d = compare("prims-r200-packets", ref, 0, &mx);
    scene_report("TRI_TYPE_2, 3-vertex lists, INDX_BUFFER (R200 packets)", d,
                 mx);
  }

  // -- scene: the provoking vertex in strips, quads and lines -------------
  {
    // SE_CNTL FLAT_SHADE_VTX <7:6> counts the vertices of a primitive in
    // the order they were sent: strip triangle i is v[i], v[i+1], v[i+2]
    // whichever winding the odd ones are drawn in (Direct3D flat shades
    // it with v[i], Mesa's GL strips with VTX_LAST take v[i+2]); a quad is
    // one colour, its last vertex's with VTX_LAST; a line takes its first
    // vertex with VTX_0, its second otherwise
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    auto flat_cntl = [&](u32 vtx) {
      cp({pkt0(SE_CNTL, 1), (3u << 1) | (3u << 3) | (vtx << 6) | (1u << 8) |
                                (1u << 10) | (1u << 27)});
    };
    auto colour = [](int i) {
      return V{0, 0, 0, float(i) / 5, float(5 - i) / 5, (i & 1) ? 1.f : 0.25f,
               1};
    };
    auto fill_tri = [&](const V &a, const V &b, const V &c3, const V &col) {
      ref_triangle({a.x, a.y}, {b.x, b.y}, {c3.x, c3.y}, W, H,
                   [&](int x, int y, double, double, double) {
                     float cc[4] = {col.r, col.g, col.b, col.a};
                     ref[size_t(y * W + x)] = argbf(cc);
                   });
    };
    for (u32 vtx : {0u, 3u}) {
      flat_cntl(vtx);
      std::vector<V> strip;
      const float y0 = vtx == 0 ? 4.0f : 34.0f;
      for (int i = 0; i < 6; i++) {
        V v = colour(i);
        v.x = 4.0f + 12.0f * float(i);
        v.y = (i & 1) ? y0 + 26.0f : y0;
        strip.push_back(v);
      }
      immd_xyzc(PRIM_TRI_STRIP, strip);
      for (size_t i = 0; i + 2 < strip.size(); i++)
        fill_tri(strip[i], strip[i + 1], strip[i + 2],
                 strip[vtx == 0 ? i : i + 2]);
    }
    // quads: VTX_LAST, then VTX_1
    for (u32 vtx : {3u, 1u}) {
      flat_cntl(vtx);
      const float y0 = vtx == 3 ? 4.0f : 34.0f;
      std::vector<V> q;
      const float xy[4][2] = {
          {80, y0}, {120, y0}, {120, y0 + 26}, {80, y0 + 26}};
      for (int i = 0; i < 4; i++) {
        V v = colour(i + 1);
        v.x = xy[i][0];
        v.y = xy[i][1];
        q.push_back(v);
      }
      immd_xyzc(PRIM_QUAD_LIST, q);
      const V &pv = q[vtx];
      fill_tri(q[0], q[1], q[2], pv);
      fill_tri(q[0], q[2], q[3], pv);
    }
    // flat lines: VTX_0 the first vertex's colour, VTX_LAST the second's
    for (u32 vtx : {0u, 3u}) {
      flat_cntl(vtx);
      const float y = vtx == 0 ? 80.5f : 90.5f;
      V a = colour(1), b = colour(4);
      a.x = 4.5f;
      b.x = 100.5f;
      a.y = b.y = y;
      immd_xyzc(PRIM_LINE_LIST, {a, b});
      const V &pv = vtx == 0 ? a : b;
      float cc[4] = {pv.r, pv.g, pv.b, pv.a};
      for (int x = 4; x < 100; x++)
        ref[size_t(int(y) * W + x)] = argbf(cc);
    }
    int mx;
    const int d = compare("flat-provoking", ref, 1, &mx);
    scene_report("flat shading: provoking vertex of strips, quads, lines", d,
                 mx);
  }

  // -- scene: vertex snapping, the four ROUND_MODEs ---------------------------
  {
    // SE_CNTL ROUND_PREC_HALF_PIX: every vertex to a half pixel, ties as
    // ROUND_MODE <29:28> says. A bottom edge at y = 20.25 is a tie between
    // 20.0 and 20.5: truncated or rounded to even 20.0, rounded or to odd
    // 20.5; with Direct3D pixel centres (+0.5) only 20.5 takes in row 20.
    // A bottom edge at y = 40.75 ties 40.5 and 41.0: truncated or to odd
    // 40.5, rounded or to even 41.0; with OpenGL centres only 41.0 takes in
    // row 40. So the four modes draw four different pairs
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    auto rect = [&](float x0, float y0, float x1, float y1) {
      immd_xyzc(PRIM_TRI_LIST, {{x0, y0, 0, 1, 1, 1, 1},
                                {x1, y0, 0, 1, 1, 1, 1},
                                {x1, y1, 0, 1, 1, 1, 1},
                                {x0, y0, 0, 1, 1, 1, 1},
                                {x1, y1, 0, 1, 1, 1, 1},
                                {x0, y1, 0, 1, 1, 1, 1}});
    };
    auto set_ref = [&](int x0, int x1, int y0, int y1) { // exclusive ends
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
          ref[size_t(y * W + x)] = 0xffffffffu;
    };
    for (u32 m = 0; m < 4; m++) {
      const u32 se = (3u << 1) | (3u << 3) | (2u << 8) | (2u << 10) |
                     (m << 28) | (3u << 30);
      const int x0 = 8 + 24 * int(m), x1 = x0 + 16;
      cp({pkt0(SE_CNTL, 1), se}); // Direct3D centres
      rect(float(x0), 10.0f, float(x1), 20.25f);
      set_ref(x0, x1, 10, (m == 1 || m == 3) ? 21 : 20);
      cp({pkt0(SE_CNTL, 1), se | (1u << 27)}); // OpenGL centres
      rect(float(x0), 30.0f, float(x1), 40.75f);
      set_ref(x0, x1, 30, (m == 1 || m == 2) ? 41 : 40);
    }
    // points and lines snap too: ROUND, half a pixel, OpenGL centres
    cp({pkt0(SE_CNTL, 1), (3u << 1) | (3u << 3) | (2u << 8) | (2u << 10) |
                              (1u << 27) | (1u << 28) | (3u << 30)});
    immd_xyzc(PRIM_POINT_LIST, {{8.8f, 60.2f, 0, 1, 1, 1, 1}}); // (9, 60)
    ref[size_t(60 * W + 9)] = 0xffffffffu;
    immd_xyzc(PRIM_LINE_LIST, {{10.8f, 80.2f, 0, 1, 1, 1, 1},
                               {40.8f, 80.2f, 0, 1, 1, 1, 1}}); // 11..40
    set_ref(11, 41, 80, 81);
    int mx;
    const int d = compare("round-modes", ref, 0, &mx);
    scene_report("vertex snapping: ROUND_MODE trunc/round/even/odd", d, mx);
  }

  // -- scene: lines through sub-pixel endpoints ------------------------------
  {
    // A line runs between its snapped endpoints, not between the pixels
    // holding them: along the major axis the pixels whose centres lie
    // from the start (in) to the end (out), across it the row (column)
    // whose centre is nearest the line at that centre. Endpoints on
    // sixteenths, so the default snapping (1/16, truncate) keeps them
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    cp({pkt0(RE_SOLID_COLOR, 1), 0xffffffffu, pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (1u << 27)});
    struct L {
      float x0, y0, x1, y1;
    };
    const L lines[] = {{10.25f, 10.25f, 70.75f, 30.75f},
                       {120.8125f, 40.0625f, 20.1875f, 60.9375f},
                       {100.6875f, 70.3125f, 110.0625f, 124.5f},
                       {30.5f, 120.875f, 40.125f, 72.25f}};
    for (const L &l : lines) {
      immd_xyzc(PRIM_LINE_LIST,
                {{l.x0, l.y0, 0, 1, 1, 1, 1}, {l.x1, l.y1, 0, 1, 1, 1, 1}});
      const bool xmaj = std::fabs(l.x1 - l.x0) >= std::fabs(l.y1 - l.y0);
      const double a0 = xmaj ? l.x0 : l.y0, a1 = xmaj ? l.x1 : l.y1;
      const double b0 = xmaj ? l.y0 : l.x0, b1 = xmaj ? l.y1 : l.x1;
      for (int c = 0; c < W; c++) {
        const double m = c + 0.5;
        const bool in = a1 > a0 ? (m >= a0 && m < a1) : (m <= a0 && m > a1);
        if (!in)
          continue;
        const int k = int(std::floor(b0 + (b1 - b0) * (m - a0) / (a1 - a0)));
        ref[size_t((xmaj ? k : c) * W + (xmaj ? c : k))] = 0xffffffffu;
      }
    }
    int mx;
    const int d = compare("line-subpixel", ref, 0, &mx);
    scene_report("lines through sub-pixel endpoints", d, mx);
  }

  // -- scene: PP_TXFILTER BORDER_MODE with the GL clamp -----------------------
  {
    // CLAMP_GL (6) on both axes, bilinear, coordinates 1.25..1.5 (all
    // beyond the image). BORDER_MODE_OGL: the coordinate clamps to the
    // edge, so the filter takes the last texel and the border half and
    // half on each axis -- a quarter red, three quarters border (GL_CLAMP).
    // BORDER_MODE_D3D: the coordinate runs on, every texel there is the
    // border (GL_CLAMP_TO_BORDER, as Mesa programs it)
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    for (u32 i = 0; i < 32; i++) // 4x4, rows 32-byte aligned
      vw32(TEX + i * 4, 0xffff0000u);
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12), pkt0(PP_BORDER_COLOR_0, 1),
        0xff0000ffu});
    for (u32 d3d : {0u, 1u}) {
      const u32 filter = 1u | (1u << 1) | (6u << 23) | (6u << 27) | (d3d << 31);
      set_tex(0, TEX, 6 | (1u << 6), 2, 2, filter, C_REPLACE_T0, A_REPLACE_T0);
      const float x0 = d3d ? 64.0f : 0.0f;
      rect_st(x0, 0, x0 + 64, 64, 1.25f, 1.25f, 1.5f, 1.5f);
      float c[4] = {d3d ? 0.0f : 0.25f, 0, d3d ? 1.0f : 0.75f, 1};
      for (int y = 0; y < 64; y++)
        for (int x = int(x0); x < int(x0) + 64; x++)
          ref[size_t(y * W + x)] = argbf(c);
    }
    int mx;
    const int d = compare("border-mode", ref, 2, &mx);
    scene_report("textures: BORDER_MODE_OGL/D3D with the GL clamp", d, mx);
  }
  // ==== the TCL unit as Mesa's r100 driver programs it ======================
  // What the scenes below share: vectors and scalars, OpenGL's viewport
  // (clip -1..1 onto the 128 pixels, y down; z/w -1..1 onto 0..1, Mesa's
  // radeonUpdateWindow) and OpenGL's projection (near 1, far 10).
  auto tvec = [&](u32 index, const double *v4, int count) {
    std::vector<u32> p = {pkt0(SE_TCL_STATE_FLUSH, 1), 0,
                          pkt0(SE_TCL_VECTOR_INDX_REG, 1), index | (1u << 16),
                          pkt0_one(SE_TCL_VECTOR_DATA_REG, u32(count * 4))};
    for (int i = 0; i < count * 4; i++)
      p.push_back(fbits(float(v4[i])));
    cp(p);
  };
  auto tscl = [&](u32 index, double v) {
    cp({pkt0(SE_TCL_SCALAR_INDX_REG, 1), index | (1u << 16),
        pkt0(SE_TCL_SCALAR_DATA_REG, 1), fbits(float(v))});
  };
  auto gl_viewport = [&]() {
    cp({pkt0(SE_VPORT_XSCALE, 6), fbits(64.0f), fbits(64.0f), fbits(-64.0f),
        fbits(64.0f), fbits(0.5f), fbits(0.5f)});
  };
  const double identd[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const double gl_near = 1, gl_far = 10;
  const double gl_proj[16] = {1,
                              0,
                              0,
                              0,
                              0,
                              1,
                              0,
                              0,
                              0,
                              0,
                              (gl_far + gl_near) / (gl_near - gl_far),
                              2 * gl_near * gl_far / (gl_near - gl_far),
                              0,
                              0,
                              -1,
                              0};
  // the guard band as Mesa loads it: none
  for (u32 i = 48; i < 52; i++)
    tscl(i, 1.0);

  // -- scene: the third texture coordinate (Q) through the texture matrix ----
  {
    // A 2D texture's divisor is the unit's third coordinate, in and out.
    // Left: three coordinates submitted, (s, t, r), and a GL texture matrix
    // that moves r to q -- Mesa's upload for that (no column swap, the q
    // row third) makes the vertex's third coordinate the divisor. Right:
    // eye-linear texture generation with a q plane (Mesa's
    // set_texgen_matrix: the four planes as the matrix, uploaded the same
    // way), the unit's EYE input having all four coordinates.
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++)
        vw32(TEX + u32(y * 8 + x) * 4,
             0xff000000u | (u32(x * 32) << 16) | (u32(y * 32) << 8) | 0x80);
    set_tex(0, TEX, 6 | (1u << 6) | (1u << 31), 3, 3, 0, C_REPLACE_T0,
            A_REPLACE_T0);
    tvec(0, identd, 4);
    tvec(4, identd, 4);
    cp({pkt0(PP_CNTL, 1), (1u << 4) | (1u << 12),
        pkt0(SE_TCL_MATRIX_SELECT_0, 2), 1u | (2u << 16), 0u | (3u << 16),
        pkt0(SE_TCL_LIGHT_MODEL_CTL, 1), 0, pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        1u, pkt0(SE_CNTL_STATUS, 1), 0, pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (2u << 8) | (2u << 10) | (3u << 24) |
            (1u << 27)});
    gl_viewport();
    auto texel_of = [](double u, double v) {
      const int i = ((int(std::floor(u * 8)) % 8) + 8) % 8;
      const int j = ((int(std::floor(v * 8)) % 8) + 8) % 8;
      return 0xff000000u | (u32(i * 32) << 16) | (u32(j * 32) << 8) | 0x80;
    };
    // left: (s, t, r) and the matrix (s, t, r, q) -> (s, t, 0, r)
    double gl[16] = {0}, tm[16];
    gl[0] = gl[5] = 1;
    gl[11] = 1; // the q row takes r
    mesa_texmat(gl, true, false, tm);
    tvec(12, tm, 4);
    cp({pkt0(SE_TCL_OUTPUT_VTX_FMT, 2),
        VTX_Z | VTX_W0 | VTX_PKCOLOR | VTX_ST0 | VTX_Q0, 1u | (8u << 16),
        pkt0(SE_TCL_TEXTURE_PROC_CTL, 1), (1u << 0) | (1u << 4) | (0u << 16)});
    {
      const float q[4][6] = {{-1, -1, 0, 0, 0, 1},
                             {0, -1, 0, 2, 0, 2},
                             {0, 1, 0, 2, 1, 2},
                             {-1, 1, 0, 0, 1, 1}};
      std::vector<u32> p = {pkt3(0x29, 2 + 4 * 6), VTX_Z | VTX_ST0 | VTX_Q0,
                            PRIM_TRI_FAN | (WALK_DATA << 4) | (1u << 9) |
                                (4u << 16)};
      for (const auto &v : q)
        for (float c : v)
          p.push_back(fbits(c));
      cp(p);
    }
    for (int y = 0; y < H; y++)
      for (int x = 0; x < 64; x++) {
        const double X = (x + 0.5) / 64, Y = 1 - (y + 0.5) / 128;
        const double r = 1 + X;
        ref[size_t(y * W + x)] = texel_of(2 * X / r, Y / r);
      }
    // right: generated from the eye position, s = x, t = y / 2 + 1 / 2,
    // q = 1 + x / 2
    double tg[16] = {0};
    tg[0] = 1;            // s plane (1, 0, 0, 0)
    tg[5] = tg[13] = 0.5; // t plane (0, 0.5, 0, 0.5)
    tg[3] = 0.5;          // q plane (0.5, 0, 0, 1)
    tg[15] = 1;
    mesa_texmat(tg, true, false, tm);
    tvec(12, tm, 4);
    cp({pkt0(SE_TCL_TEXTURE_PROC_CTL, 1), (1u << 0) | (1u << 4) | (5u << 16)});
    {
      const float q[4][3] = {{0, -1, 0}, {1, -1, 0}, {1, 1, 0}, {0, 1, 0}};
      std::vector<u32> p = {pkt3(0x29, 2 + 4 * 3), VTX_Z,
                            PRIM_TRI_FAN | (WALK_DATA << 4) | (1u << 9) |
                                (4u << 16)};
      for (const auto &v : q)
        for (float c : v)
          p.push_back(fbits(c));
      cp(p);
    }
    for (int y = 0; y < H; y++)
      for (int x = 64; x < W; x++) {
        const double X = (x + 0.5 - 64) / 64, Y = -(y + 0.5 - 64) / 64;
        const double q = 1 + 0.5 * X;
        ref[size_t(y * W + x)] = texel_of(X / q, (0.5 * Y + 0.5) / q);
      }
    cp({pkt0(SE_TCL_OUTPUT_VTX_FMT, 1), 0u, pkt0(SE_TCL_TEXTURE_PROC_CTL, 1),
        0u});
    int mx;
    const int d = compare("tcl-texmatrix-q", ref, 0, &mx);
    // a texel boundary may pass within a rounding of a pixel centre
    scene_report("TCL: Q through the texture matrix (3 coords, texgen)", d, mx,
                 24);
  }

  // -- scene: OpenGL's view volume and depth range -------------------------
  {
    // Quads at eye depths 1.5, 5 and 1.2 (z/w -0.26, 0.78, -0.63: two of
    // them in the near half of the volume, where z is negative) with a
    // Z test, one before the near plane and one beyond the far plane
    base_state();
    clear_cb(0xff000000u);
    clear_z(0x00ffffffu);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    std::vector<double> zref(size_t(W * H), 2.0);
    tvec(0, gl_proj, 4);
    cp({pkt0(SE_TCL_MATRIX_SELECT_0, 2),
        0u,
        0u,
        pkt0(SE_TCL_OUTPUT_VTX_FMT, 2),
        0u,
        1u,
        pkt0(SE_TCL_TEXTURE_PROC_CTL, 1),
        0,
        pkt0(SE_TCL_LIGHT_MODEL_CTL, 1),
        0,
        pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        1u,
        pkt0(SE_CNTL_STATUS, 1),
        0,
        pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (1u << 8) | (1u << 10) | (3u << 6) |
            (3u << 24) | (1u << 27),
        pkt0(RB3D_CNTL, 1),
        ROUNDING | (6u << 10) | (1u << 8),
        pkt0(RB3D_ZSTENCILCNTL, 1),
        2u | (1u << 4) | (1u << 30)});
    gl_viewport();
    struct Q {
      double d, x0, y0, x1, y1; // depth; the rectangle in z/w units
      u32 rgba, argb;
      bool visible;
    };
    const Q quads[] = {
        {1.5, -0.5, -0.5, 0.25, 0.5, 0xff4040ffu, 0xffff4040u, true},
        {5.0, -0.25, -0.25, 0.75, 0.75, 0xff40ff40u, 0xff40ff40u, true},
        {1.2, 0.0, -0.75, 0.5, 0.0, 0xffff4040u, 0xff4040ffu, true},
        {0.9, -0.875, 0.5, -0.625, 0.875, 0xffffffffu, 0xffffffffu, false},
        {12.0, -0.875, -0.875, -0.625, -0.625, 0xffffffffu, 0xffffffffu,
         false}};
    for (const Q &q : quads) {
      const double c4[4][2] = {
          {q.x0, q.y0}, {q.x1, q.y0}, {q.x1, q.y1}, {q.x0, q.y1}};
      std::vector<u32> p = {pkt3(0x29, 2 + 4 * 4), VTX_Z | VTX_PKCOLOR,
                            PRIM_TRI_FAN | (WALK_DATA << 4) | (1u << 9) |
                                VF_RGBA | (4u << 16)};
      for (const auto &c : c4) {
        p.push_back(fbits(float(c[0] * q.d)));
        p.push_back(fbits(float(c[1] * q.d)));
        p.push_back(fbits(float(-q.d)));
        p.push_back(q.rgba);
      }
      cp(p);
      if (!q.visible)
        continue;
      const double zn = (gl_far + gl_near) / (gl_far - gl_near) -
                        2 * gl_far * gl_near / ((gl_far - gl_near) * q.d);
      const double zw = 0.5 * zn + 0.5;
      for (int y = int(64 - 64 * q.y1); y < int(64 - 64 * q.y0); y++)
        for (int x = int(64 + 64 * q.x0); x < int(64 + 64 * q.x1); x++)
          if (zw < zref[size_t(y * W + x)]) {
            zref[size_t(y * W + x)] = zw;
            ref[size_t(y * W + x)] = q.argb;
          }
    }
    cp({pkt0(RB3D_CNTL, 1), ROUNDING | (6u << 10)});
    int mx;
    const int d = compare("tcl-gl-volume", ref, 0, &mx);
    scene_report("TCL: OpenGL's view volume (-w <= z <= w) and depth range", d,
                 mx);
  }

  // -- scene: lines and points clipped by the TCL unit -----------------------
  {
    // A line with an end behind the eye stops at the near plane; a line
    // across a user clip plane stops there; a line behind the eye is not
    // drawn; points before the near plane, outside the user plane or the
    // side of the volume are not drawn, one in the near half is
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    tvec(0, gl_proj, 4);
    const double ucp0[4] = {0, -1, 0, 0.25}; // keep y <= w / 4
    tvec(116, ucp0, 1);
    cp({pkt0(SE_TCL_MATRIX_SELECT_0, 2), 0u, 0u, pkt0(SE_TCL_OUTPUT_VTX_FMT, 2),
        0u, 1u, pkt0(SE_TCL_TEXTURE_PROC_CTL, 1), 0,
        pkt0(SE_TCL_LIGHT_MODEL_CTL, 1), 0, pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        1u | (1u << 2), pkt0(SE_CNTL_STATUS, 1), 0, pkt0(SE_CNTL, 1),
        (3u << 1) | (3u << 3) | (2u << 8) | (2u << 10) | (3u << 24) |
            (1u << 27)});
    gl_viewport();
    struct P3 {
      double x, y, z;
    };
    auto tcl_prims = [&](u32 prim, const std::vector<P3> &vs) {
      std::vector<u32> p = {pkt3(0x29, 2 + u32(vs.size()) * 4),
                            VTX_Z | VTX_PKCOLOR,
                            prim | (WALK_DATA << 4) | (1u << 9) | VF_RGBA |
                                (u32(vs.size()) << 16)};
      for (const P3 &v : vs) {
        p.push_back(fbits(float(v.x)));
        p.push_back(fbits(float(v.y)));
        p.push_back(fbits(float(v.z)));
        p.push_back(0xffffffffu);
      }
      cp(p);
    };
    tcl_prims(PRIM_LINE_LIST, {{0, 0, -2},       // the centre
                               {2, -1, 2},       // behind the eye
                               {-1.5, -1.5, -2}, // z/w (-0.75, -0.75)
                               {0, 1.5, -2},     // (0, 0.75): past the plane
                               {0.5, 0.5, 1},    // both ends behind the eye
                               {1, 1, 2},
                               {0.5, -1, -2}, // inside throughout
                               {1.5, -1.25, -2}});
    // the visible parts, in pixels (start in, end out): to the near plane
    // at (96, 80); to the user plane at (48, 48); nothing; whole
    struct L {
      double x0, y0, x1, y1;
    };
    const L vis[] = {{64, 64, 96, 80}, {16, 112, 48, 48}, {80, 96, 112, 104}};
    for (const L &l : vis) {
      const bool xmaj = std::fabs(l.x1 - l.x0) >= std::fabs(l.y1 - l.y0);
      const double a0 = xmaj ? l.x0 : l.y0, a1 = xmaj ? l.x1 : l.y1;
      const double b0 = xmaj ? l.y0 : l.x0, b1 = xmaj ? l.y1 : l.x1;
      for (int c = 0; c < W; c++) {
        const double m = c + 0.5;
        const bool in = a1 > a0 ? (m >= a0 && m < a1) : (m <= a0 && m > a1);
        if (!in)
          continue;
        const int k = int(std::floor(b0 + (b1 - b0) * (m - a0) / (a1 - a0)));
        ref[size_t((xmaj ? k : c) * W + (xmaj ? c : k))] = 0xffffffffu;
      }
    }
    // points at pixel centres: (32, 80) at depth 2; (20, 100) at depth
    // 1.25, in the near half; then one before the near plane, one past
    // the user plane, one beyond the right side
    tcl_prims(PRIM_POINT_LIST, {{-0.984375, -0.515625, -2},
                                {-0.849609375, -0.712890625, -1.25},
                                {0.1, -0.1, -0.5},
                                {0.2, 1.0, -2},
                                {2.4, -0.4, -2}});
    ref[size_t(80 * W + 32)] = 0xffffffffu;
    ref[size_t(100 * W + 20)] = 0xffffffffu;
    cp({pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1), 1u});
    int mx;
    const int d = compare("tcl-line-point-clip", ref, 0, &mx);
    // a clipped end is not on a sixteenth: its snapping may move one pixel
    scene_report("TCL: lines and points clipped (near plane, user plane)", d,
                 mx, 2);
  }

  // -- scene: spot lights, the dual cone, the eye vector ---------------------
  {
    // Four lit planes (a 4x4 grid of cells each, Gouraud): OpenGL's spot
    // light; the dual-cone spot (Direct3D's: inner cone 10 degrees, outer
    // 30) with falloff 1 and 2.5; a point light's specular highlight for a
    // viewer at infinity, the eye vector as Mesa loads it. The reference
    // lights the grid's vertices in double and interpolates.
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> ref(size_t(W * H), 0xff000000u);
    const double mv[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, -3, 0, 0, 0, 1};
    const double itmv[16] = {0.5, 0, 0, 0, 0, 0.5, 0, 0,
                             0,   0, 1, 0, 0, 0,   0, 1};
    tvec(4, mv, 4);
    tvec(8, itmv, 4);
    const double zero4[4] = {0, 0, 0, 0}, eyev[4] = {0, 0, -1, 1};
    tvec(122, zero4, 1); // no global ambient
    tvec(124, eyev, 1);
    tvec(64, zero4, 1);
    tscl(32, 1e30); // no range cutoff
    {
      std::vector<u32> mp = {pkt0(SE_TCL_MATERIAL_EMISSIVE, 17)};
      const float me[4] = {0, 0, 0, 1}, one[4] = {1, 1, 1, 1};
      for (const float *m : {me, me, one, one})
        for (int k = 0; k < 4; k++)
          mp.push_back(fbits(m[k]));
      mp.push_back(fbits(20.0f));
      cp(mp);
    }
    cp({pkt0(SE_TCL_MATRIX_SELECT_0, 2), 1u | (2u << 16), 0u,
        pkt0(SE_TCL_OUTPUT_VTX_FMT, 2), 0u, 1u | 2u,
        pkt0(SE_TCL_TEXTURE_PROC_CTL, 1), 0, pkt0(SE_TCL_LIGHT_MODEL_CTL, 1),
        1u | (1u << 5) | (1u << 6) | (1u << 16) | (1u << 18) | (1u << 20) |
            (1u << 22),
        pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1), 1u, pkt0(SE_CNTL_STATUS, 1), 0,
        pkt0(SE_CNTL, 1),
        1u | (3u << 1) | (3u << 3) | (3u << 6) | (2u << 8) | (2u << 10) |
            (2u << 12) | (2u << 14) | (3u << 24) | (1u << 27)});
    gl_viewport();
    const double deg = M_PI / 180;
    struct Lt {
      double pos[3], dif[3], spc[3];
      bool spot, dual;
      double cutoff, dcd, expo;
    };
    const Lt lights[4] = {{{0.3, 0.2, 0},
                           {1, 0.9, 0.8},
                           {0, 0, 0},
                           true,
                           false,
                           std::cos(25 * deg),
                           0,
                           8},
                          {{0.3, 0.2, 0},
                           {1, 0.9, 0.8},
                           {0, 0, 0},
                           true,
                           true,
                           std::cos(30 * deg),
                           1 / (std::cos(10 * deg) - std::cos(30 * deg)),
                           1},
                          {{0.3, 0.2, 0},
                           {1, 0.9, 0.8},
                           {0, 0, 0},
                           true,
                           true,
                           std::cos(30 * deg),
                           1 / (std::cos(10 * deg) - std::cos(30 * deg)),
                           2.5},
                          {{1.0, 0.5, -1.5},
                           {0.3, 0.3, 0.3},
                           {1, 1, 0.8},
                           false,
                           false,
                           0,
                           0,
                           0}};
    for (int q = 0; q < 4; q++) {
      const Lt &lt = lights[q];
      const double cx = (q & 1) ? 0.5 : -0.5, cy = (q & 2) ? -0.5 : 0.5;
      const double mvp[16] = {0.5, 0, 0, cx, 0, 0.5, 0, cy,
                              0,   0, 0, 0,  0, 0,   0, 1};
      tvec(0, mvp, 4);
      const double dif4[4] = {lt.dif[0], lt.dif[1], lt.dif[2], 1},
                   spc4[4] = {lt.spc[0], lt.spc[1], lt.spc[2], 1},
                   pos4[4] = {lt.pos[0], lt.pos[1], lt.pos[2], 1},
                   dir4[4] = {0, 0, 1, 0}; // the spot points down -z
      tvec(72, dif4, 1);
      tvec(80, spc4, 1);
      tvec(88, pos4, 1);
      tvec(96, dir4, 1);
      tscl(0, lt.dcd);
      tscl(8, lt.expo);
      tscl(16, lt.cutoff);
      cp({pkt0(SE_TCL_PER_LIGHT_CTL_0, 1),
          1u | 8u | (lt.spot ? 16u : 4u) | (lt.dual ? 32u : 0u)});
      // the lit colour of the vertex at (x, y) of the plane
      auto lit = [&](double x, double y, double col[3]) {
        const double P[3] = {2 * x, 2 * y, -3};
        double L[3] = {lt.pos[0] - P[0], lt.pos[1] - P[1], lt.pos[2] - P[2]};
        const double dl = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        for (double &k : L)
          k /= dl;
        double at = 1;
        if (lt.spot) {
          const double cs = L[2]; // against (0, 0, 1)
          if (cs < lt.cutoff)
            at = 0;
          else if (lt.dual)
            at = std::pow(
                std::min(1.0, std::max(0.0, (cs - lt.cutoff) * lt.dcd)),
                lt.expo);
          else
            at = std::pow(cs, lt.expo);
        }
        const double ndl = L[2]; // the normal is (0, 0, 1)
        double sp = 0;
        if (!lt.spot && ndl > 0) {
          // the viewer at infinity towards +z
          const double hn =
              std::sqrt(L[0] * L[0] + L[1] * L[1] + (L[2] + 1) * (L[2] + 1));
          const double ndh = (L[2] + 1) / hn;
          if (ndh > 0)
            sp = std::pow(ndh, 20.0);
        }
        for (int k = 0; k < 3; k++)
          col[k] =
              std::min(1.0, std::max(0.0, at * (std::max(0.0, ndl) * lt.dif[k] +
                                                sp * lt.spc[k])));
      };
      std::vector<u32> p = {pkt3(0x29, 2 + 16 * 6 * 6), VTX_Z | VTX_N0,
                            PRIM_TRI_LIST | (WALK_DATA << 4) | (1u << 9) |
                                (96u << 16)};
      for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
          const double x0 = -1 + 0.5 * i, y0 = -1 + 0.5 * j;
          const double c4[4][2] = {
              {x0, y0}, {x0 + 0.5, y0}, {x0 + 0.5, y0 + 0.5}, {x0, y0 + 0.5}};
          for (const auto &tri :
               {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
            struct SV {
              double x, y, col[3];
            } sv[3];
            for (int k = 0; k < 3; k++) {
              const double *c = c4[tri[size_t(k)]];
              for (float v : {float(c[0]), float(c[1]), 0.0f, 0.0f, 0.0f, 1.0f})
                p.push_back(fbits(v));
              sv[k].x = (0.5 * c[0] + cx) * 64 + 64;
              sv[k].y = (0.5 * c[1] + cy) * -64 + 64;
              lit(c[0], c[1], sv[k].col);
            }
            const double area = (sv[1].x - sv[0].x) * (sv[2].y - sv[0].y) -
                                (sv[2].x - sv[0].x) * (sv[1].y - sv[0].y);
            ref_triangle(
                {sv[0].x, sv[0].y}, {sv[1].x, sv[1].y}, {sv[2].x, sv[2].y}, W,
                H, [&](int x, int y, double, double, double) {
                  const double px = x + 0.5, py = y + 0.5;
                  const double la = ((sv[1].x - px) * (sv[2].y - py) -
                                     (sv[2].x - px) * (sv[1].y - py)) /
                                    area;
                  const double lb = ((sv[2].x - px) * (sv[0].y - py) -
                                     (sv[0].x - px) * (sv[2].y - py)) /
                                    area;
                  const double lc = 1 - la - lb;
                  float o[4] = {0, 0, 0, 1};
                  for (int k = 0; k < 3; k++)
                    o[k] = float(la * sv[0].col[k] + lb * sv[1].col[k] +
                                 lc * sv[2].col[k]);
                  ref[size_t(y * W + x)] = argbf(o);
                });
          }
        }
      cp(p);
    }
    cp({pkt0(SE_TCL_PER_LIGHT_CTL_0, 1), 0u, pkt0(SE_TCL_LIGHT_MODEL_CTL, 1),
        0u, pkt0(SE_CNTL_STATUS, 1), 1u << 8});
    int mx;
    const int d = compare("tcl-spot-dualcone-eye", ref, 2, &mx);
    scene_report("TCL: spot light, dual-cone spot, eye vector (far viewer)", d,
                 mx);
  }
}
