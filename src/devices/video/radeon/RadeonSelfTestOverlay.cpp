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
 * The self-test's checks of the overlay scaler (RadeonOverlay.cpp). A
 * small mode of the card's own CRTC (256 x 192, 32 bits) over a
 * patterned desktop, a video surface in off-screen memory, the scaler
 * programmed through the register aperture under its lock -- its set-up
 * computed as ATI's sample code computes it (vidix radeon_vid.c
 * ComputeAccumInit: the accumulators start at 2.5 and 1.5 plus half a
 * step, half a pixel more for nearest) -- and the frame the display path
 * renders compared with a reference written here: the surface resampled
 * at the centres of the window's pixels, in double, with the filter the
 * registers ask for, converted with the transform's coefficients and
 * mixed by the keys.
 *
 * The scales are ones whose positions the accumulators' five fractional
 * bits hold exactly (1:1, 2:1, a planar surface's chroma at 4:1), so the
 * reference needs none of the scaler's own quantising. Its rounding is
 * the model's (to the nearest 32nd part and the nearest step of eight
 * bits): what the chip rounds is not documented. As everywhere in this
 * self-test, a pass shows the model does what the drivers' reading says,
 * not that the silicon does.
 **/

#include "Radeon.hpp"
#include "RadeonOverlay.hpp"
#include "RadeonSelfTest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace radeon;
using namespace radeon::selftest;

namespace {
/// A video surface as the reference keeps it: three planes of bytes (for
/// RGB: R, G, B at full size).
struct Surface {
  int w = 0, h = 0, cw = 0, ch = 0;
  std::vector<int> p[3];
  int at(int plane, int x, int y) const {
    const int pw = plane ? cw : w, ph = plane ? ch : h;
    x = std::min(std::max(x, 0), pw - 1);
    y = std::min(std::max(y, 0), ph - 1);
    return p[plane][size_t(y * pw + x)];
  }
};
inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
} // namespace

void CRadeon::selftest_overlay(const selftest_report &report,
                               const std::string &dir) {
  auto wr = [&](u32 r, u32 v) { WriteMem_Bar(0, 2, r, 32, v); };
  auto rd = [&](u32 r) { return ReadMem_Bar(0, 2, r, 32); };
  engine_drain();

  // -- the mode and the desktop -------------------------------------------
  const int W = 256, H = 192;
  const u32 G = 0x1800000, V = 0x1900000; // the desktop, the surface
  wr(DISPLAY_BASE_ADDR, 0);
  wr(CRTC_OFFSET, G);
  wr(CRTC_PITCH, u32(W / 8));
  wr(CRTC_H_TOTAL_DISP, u32(W / 8 - 1) << 16);
  wr(CRTC_V_TOTAL_DISP, u32(H - 1) << 16);
  wr(CRTC_EXT_CNTL, CRTC_CRT_ON);
  wr(CRTC_GEN_CNTL,
     CRTC_EXT_DISP_EN | CRTC_EN | (u32(PIX_32BPP) << CRTC_PIX_WIDTH_SHIFT));
  m_pll[PLL_VCLK_ECP_CNTL] &= ~(3u << 8);
  const u32 KEY = 0x00102030;
  // a gradient with a block of the key colour and a block of half alpha
  auto desktop = [&](int x, int y) -> u32 {
    if (x >= 24 && x < 200 && y >= 16 && y < 150)
      return 0xff000000u | KEY;
    return (u32(x >= 200 ? 0x80 : 0xff) << 24) | (u32(x) << 16) |
           (u32(y) << 8) | u32((x + y) & 0xff);
  };
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      vram_write(G + u32(y * W + x) * 4, 4, desktop(x, y));

  // -- the frame the display path makes -------------------------------------
  bitmap_rgb32 bm;
  bm.allocate(W, H);
  auto frame = [&]() {
    std::vector<u32> f(size_t(W * H));
    rectangle clip = bm.cliprect();
    screen_update(bm, clip);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        f[size_t(y * W + x)] = bm.pix(y, x) & 0xffffffu;
    return f;
  };
  // locked updates, as the drivers make them; true when the flip came
  auto lock = [&]() {
    wr(OV0_REG_LOAD_CNTL, OV0_LOCK);
    return (rd(OV0_REG_LOAD_CNTL) & OV0_LOCK_READBACK) != 0;
  };
  auto unlock = [&]() {
    wr(OV0_REG_LOAD_CNTL, 0);
    const auto give_up =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!(rd(OV0_REG_LOAD_CNTL) & OV0_FLIP_READBACK)) {
      if (std::chrono::steady_clock::now() > give_up)
        return false;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
  };

  // -- surfaces ---------------------------------------------------------------
  // smooth enough to filter, busy enough to show a wrong tap
  auto make = [&](int w, int h, int cw, int ch) {
    Surface s;
    s.w = w;
    s.h = h;
    s.cw = cw;
    s.ch = ch;
    s.p[0].resize(size_t(w * h));
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        s.p[0][size_t(y * w + x)] =
            16 + ((x * 7 + y * 13 + int(rnd() % 24)) % 220);
    for (int k = 1; k < 3; k++) {
      s.p[k].resize(size_t(cw * ch));
      for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
          s.p[k][size_t(y * cw + x)] =
              16 + ((x * (k == 1 ? 11 : 5) + y * (k == 1 ? 3 : 17) +
                     int(rnd() % 16)) %
                    224);
    }
    return s;
  };
  // the surface into VRAM in the scaler's format `fmt`; returns the
  // three buffers' offsets from V and the pitches
  struct Layout {
    u32 buf[3], pitch0, pitch1;
  };
  auto store = [&](const Surface &s, u32 fmt) {
    Layout l{};
    l.pitch0 = l.pitch1 = 0;
    switch (fmt) {
    case 11: // YUY2: Y0 U Y1 V
    case 12: // UYVY: U Y0 V Y1
      l.pitch0 = u32(s.w * 2 + 15) & ~15u;
      for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++) {
          const u32 a = V + u32(y) * l.pitch0 + u32(x) * 2;
          const int c = s.at((x & 1) ? 2 : 1, x / 2, y);
          vram_write(a + (fmt == 11 ? 0 : 1), 1, u32(s.at(0, x, y)));
          vram_write(a + (fmt == 11 ? 1 : 0), 1, u32(c));
        }
      break;
    case 10: // planar 4:2:0: Y, then U, then V, the chroma pitch half
      l.pitch0 = u32(s.w + 31) & ~31u;
      l.pitch1 = l.pitch0 / 2;
      l.buf[1] = l.pitch0 * u32(s.h);
      l.buf[2] = l.buf[1] + l.pitch1 * u32(s.ch);
      for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++)
          vram_write(V + u32(y) * l.pitch0 + u32(x), 1, u32(s.at(0, x, y)));
      for (int k = 1; k < 3; k++)
        for (int y = 0; y < s.ch; y++)
          for (int x = 0; x < s.cw; x++)
            vram_write(V + l.buf[k] + u32(y) * l.pitch1 + u32(x), 1,
                       u32(s.at(k, x, y)));
      l.buf[1] |= 1; // PITCH_SEL: the second pitch
      l.buf[2] |= 1;
      break;
    case 4: // RGB565
      l.pitch0 = u32(s.w * 2 + 15) & ~15u;
      for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++)
          vram_write(V + u32(y) * l.pitch0 + u32(x) * 2, 2,
                     (u32(s.at(0, x, y) >> 3) << 11) |
                         (u32(s.p[1][size_t(y * s.w + x)] >> 2) << 5) |
                         u32(s.p[2][size_t(y * s.w + x)] >> 3));
      break;
    default: // 6: xRGB8888
      l.pitch0 = u32(s.w * 4 + 15) & ~15u;
      for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++)
          vram_write(V + u32(y) * l.pitch0 + u32(x) * 4, 4,
                     (u32(s.at(0, x, y)) << 16) |
                         (u32(s.p[1][size_t(y * s.w + x)]) << 8) |
                         u32(s.p[2][size_t(y * s.w + x)]));
      break;
    }
    return l;
  };

  // -- the scaler's set-up
  // ------------------------------------------------------
  struct Setup {
    u32 fmt;
    int dx, dy, dw, dh;  // the window
    OvFilter hf, vf;     // what the registers are to ask for
    bool four_tap_vert;  // STEP_BY 0
    u32 key_cntl, merge; // OV0_KEY_CNTL, DISP_MERGE_CNTL
    u32 vkey_lo, vkey_hi;
    u32 ecp_div;
  };
  // X.org's coefficient set for no downscaling (radeon_video.c TapCoeffs,
  // 1.00): rows for the phases 0, 1/8 .. 4/8
  const int taps[5][4] = {{0, 32, 0, 0},
                          {-2, 29, 5, 0},
                          {-3, 27, 9, -1},
                          {-4, 24, 14, -2},
                          {-3, 19, 19, -3}};
  auto program = [&](const Surface &s, const Layout &l, const Setup &c) {
    const bool yuv = c.fmt >= 9, planar = c.fmt == 10;
    const bool near_h = c.hf == OvFilter::Nearest,
               near_v = c.vf == OvFilter::Nearest;
    const u32 h_inc = u32(4096.0 * s.w / c.dw + 0.5) << c.ecp_div;
    const u32 h_inc_uv = yuv ? h_inc / 2 : h_inc;
    const u32 v_inc = u32(1048576.0 * s.h / c.dh + 0.5);
    const double hstep = double(s.w) / c.dw, vstep = double(s.h) / c.dh;
    auto h_init = [&](double step) {
      const u32 q = u32((2.5 + step / 2 + (near_h ? 0.5 : 0)) * 32 + 0.5);
      return ((q & 31) << 15) | ((q >> 5) << 28);
    };
    auto v_init = [&](double step) {
      const u32 q = u32(
          (1.5 + step / 2 + (near_v ? 0.5 : 0) + (c.four_tap_vert ? 1.0 : 0)) *
              32 +
          0.5);
      return (q << 15) | 1;
    };
    wr(OV0_H_INC, h_inc | (h_inc_uv << 16));
    wr(OV0_STEP_BY, c.four_tap_vert ? 0 : 0x0101);
    wr(OV0_Y_X_START, u32(c.dx + 8) | (u32(c.dy) << 16));
    wr(OV0_Y_X_END, u32(c.dx + c.dw - 1 + 8) | (u32(c.dy + c.dh - 1) << 16));
    wr(OV0_V_INC, v_inc);
    wr(OV0_P1_BLANK_LINES_AT_TOP, 0xfffu | (u32(s.h - 1) << 16));
    wr(OV0_P23_BLANK_LINES_AT_TOP,
       0x7ffu | (u32((planar ? s.ch : s.h) - 1) << 16));
    wr(OV0_VID_BUF_PITCH0_VALUE, l.pitch0);
    wr(OV0_VID_BUF_PITCH1_VALUE, l.pitch1 ? l.pitch1 : l.pitch0);
    wr(OV0_P1_X_START_END, u32(s.w - 1));
    wr(OV0_P2_X_START_END, u32((yuv ? s.cw : s.w) - 1));
    wr(OV0_P3_X_START_END, u32((yuv ? s.cw : s.w) - 1));
    wr(OV0_BASE_ADDR, 0);
    for (u32 b = 0; b < 6; b++)
      wr(OV0_VID_BUF0_BASE_ADRS + 4 * b, V + l.buf[b % 3]);
    wr(OV0_P1_V_ACCUM_INIT, v_init(vstep));
    wr(OV0_P1_H_ACCUM_INIT, h_init(hstep));
    wr(OV0_P23_V_ACCUM_INIT, planar ? v_init(vstep / 2) : 0);
    wr(OV0_P23_H_ACCUM_INIT, h_init(yuv ? hstep / 2 : hstep));
    // the filter: the programmed coefficients for four taps, the chip's
    // own for the linear one
    const bool hard_h = c.hf == OvFilter::Linear,
               hard_v = c.vf == OvFilter::Linear && c.four_tap_vert;
    wr(OV0_FILTER_CNTL, (hard_h ? 3u : 0u) | (hard_v ? 12u : 0u));
    for (u32 i = 0; i < 5; i++)
      wr(OV0_FOUR_TAP_COEF_0 + 4 * i, (u32(taps[i][0]) & 0xf) |
                                          ((u32(taps[i][1]) & 0x7f) << 8) |
                                          ((u32(taps[i][2]) & 0x7f) << 16) |
                                          ((u32(taps[i][3]) & 0xf) << 24));
    wr(OV0_AUTO_FLIP_CNTL, 0);
    wr(OV0_TEST, 0);
    wr(OV0_KEY_CNTL, c.key_cntl);
    wr(OV0_GRAPHICS_KEY_CLR_LOW, KEY);
    wr(OV0_GRAPHICS_KEY_CLR_HIGH, 0xff000000u | KEY);
    wr(OV0_VIDEO_KEY_CLR_LOW, c.vkey_lo);
    wr(OV0_VIDEO_KEY_CLR_HIGH, c.vkey_hi);
    wr(DISP_MERGE_CNTL, c.merge);
    wr(OV0_SCALE_CNTL, OV0_SCALER_ENABLE | OV0_DOUBLE_BUFFER | (c.fmt << 8) |
                           (near_h ? OV0_HORZ_PICK_NEAREST : 0) |
                           (near_v ? OV0_VERT_PICK_NEAREST : 0) |
                           (yuv ? 0 : OV0_LIN_TRANS_BYPASS));
  };

  // -- the reference
  // ------------------------------------------------------------ the weights of
  // taps i - 1 .. i + 2 at a position
  auto weights = [&](OvFilter f, double pos, int *i0, int w[4]) {
    const double fl = std::floor(pos);
    *i0 = int(fl);
    const int f32 = int(std::lround((pos - fl) * 32));
    w[0] = w[1] = w[2] = w[3] = 0;
    if (f == OvFilter::Nearest) {
      w[1] = 32;
    } else if (f == OvFilter::Linear) {
      w[1] = 32 - f32;
      w[2] = f32;
    } else {
      const int ph = f32 / 4;
      for (int k = 0; k < 4; k++)
        w[k] = ph <= 4 ? taps[ph][k] : taps[8 - ph][3 - k];
    }
  };
  auto resample = [&](const Surface &s, int plane, OvFilter hf, OvFilter vf,
                      double px, double py) {
    int ix, iy, wh[4], wv[4];
    weights(hf, px, &ix, wh);
    weights(vf, py, &iy, wv);
    int acc = 0;
    for (int j = 0; j < 4; j++) {
      if (!wv[j])
        continue;
      int line = 0;
      for (int i = 0; i < 4; i++)
        line += wh[i] * s.at(plane, ix - 1 + i, iy - 1 + j);
      acc += wv[j] * clamp8((line + 16) >> 5);
    }
    return clamp8((acc + 16) >> 5);
  };
  // the transform's coefficients as X.org's defaults give them
  const double lumac = 0x12a2 / 2.0 / 2048, rcr = 0x198a / 2.0 / 2048,
               gcb = (0xf9da / 2 - 0x8000) / 2048.0,
               gcr = (0xf2fe / 2 - 0x8000) / 2048.0, bcb = 0x2046 / 2.0 / 2048,
               roff = (0x190e - 0x2000) / 2.0, goff = 0x0442 / 2.0,
               boff = (0x175f - 0x2000) / 2.0;
  auto yuv_rgb = [&](int y, int cb, int cr) -> u32 {
    const double r = lumac * y * 4 + rcr * cr * 4 + roff;
    const double g = lumac * y * 4 + gcb * cb * 4 + gcr * cr * 4 + goff;
    const double b = lumac * y * 4 + bcb * cb * 4 + boff;
    auto c8 = [](double v) {
      return u32(clamp8(int(std::floor(v / 4 + 0.5))));
    };
    return (c8(r) << 16) | (c8(g) << 8) | c8(b);
  };
  auto reference = [&](const Surface &s, const Setup &c) {
    std::vector<u32> ref(size_t(W * H));
    const bool yuv = c.fmt >= 9, planar = c.fmt == 10;
    const double hstep = double(s.w) / c.dw, vstep = double(s.h) / c.dh;
    const double hn = c.hf == OvFilter::Nearest ? 0.5 : 0,
                 vn = c.vf == OvFilter::Nearest ? 0.5 : 0;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const u32 g = desktop(x, y);
        u32 out = g & 0xffffffu;
        if (x >= c.dx && x < c.dx + c.dw && y >= c.dy && y < c.dy + c.dh) {
          // the centre of the window's pixel, in the surface's pixels
          const int n = x - c.dx, m = y - c.dy;
          const double px = (n + 0.5) * hstep - 0.5 + hn,
                       py = (m + 0.5) * vstep - 0.5 + vn;
          u32 vid;
          if (yuv) {
            // chroma: half the luma's width, and its lines for 4:2:0
            const double cx = (n + 0.5) * hstep / 2 - 0.5 + hn;
            const double cy = planar ? (m + 0.5) * vstep / 2 - 0.5 + vn : py;
            vid = yuv_rgb(resample(s, 0, c.hf, c.vf, px, py),
                          resample(s, 1, c.hf, c.vf, cx, cy),
                          resample(s, 2, c.hf, c.vf, cx, cy));
          } else {
            vid = 0;
            for (int k = 0; k < 3; k++) {
              int v = resample(s, k, c.hf, c.vf, px, py);
              if (c.fmt == 4) { // the 565 pixel's bits, replicated
                const int bits = k == 1 ? 6 : 5;
                v >>= 8 - bits;
                v = (v << (8 - bits)) | (v >> (2 * bits - 8));
              }
              vid |= u32(v) << (16 - 8 * k);
            }
          }
          auto in = [](u32 v, u32 lo, u32 hi) {
            for (int sft = 0; sft < 24; sft += 8)
              if (((v >> sft) & 0xff) < ((lo >> sft) & 0xff) ||
                  ((v >> sft) & 0xff) > ((hi >> sft) & 0xff))
                return false;
            return true;
          };
          auto fn = [](u32 f, bool eq) {
            return f == 0 ? false : f == 1 ? true : f == 2 ? eq : !eq;
          };
          const bool vk = fn(c.key_cntl & 3, in(vid, c.vkey_lo, c.vkey_hi));
          const bool gk = fn((c.key_cntl >> 4) & 3, (g & 0xffffffu) == KEY);
          const bool show = (c.key_cntl & 0x100) ? (vk && gk) : (vk || gk);
          const u32 mode = c.merge & 3;
          if (mode == 0) {
            if (show)
              out = vid;
          } else {
            const u32 a_v = mode == 2 ? c.merge >> 24 : 255 - (g >> 24);
            const u32 a_g = mode == 2 ? (c.merge >> 16) & 0xff : g >> 24;
            out = 0;
            for (int sft = 0; sft < 24; sft += 8)
              out |= std::min(255u, (((vid >> sft) & 0xff) * a_v +
                                     ((g >> sft) & 0xff) * a_g + 127) /
                                        255)
                     << sft;
          }
        }
        ref[size_t(y * W + x)] = out;
      }
    return ref;
  };
  // 565 surfaces: the reference filters the full values; keep them on the
  // 565 grid so that nearest sampling is exact
  auto quantise565 = [](Surface &s) {
    for (int k = 0; k < 3; k++)
      for (int &v : s.p[k]) {
        const int bits = k == 1 ? 6 : 5;
        v = (v >> (8 - bits)) << (8 - bits);
      }
  };

  int shot = 0;
  auto check = [&](const std::string &what, const Surface &s, const Setup &c,
                   int tol = 0) {
    const Layout l = store(s, c.fmt);
    m_pll[PLL_VCLK_ECP_CNTL] =
        (m_pll[PLL_VCLK_ECP_CNTL] & ~(3u << 8)) | (c.ecp_div << 8);
    const bool locked = lock();
    program(s, l, c);
    const bool flipped = unlock();
    const std::vector<u32> got = frame(), ref = reference(s, c);
    int bad = 0, worst = 0;
    for (size_t i = 0; i < got.size(); i++) {
      int d = 0;
      for (int sft = 0; sft < 24; sft += 8)
        d = std::max(d, std::abs(int((got[i] >> sft) & 0xff) -
                                 int((ref[i] >> sft) & 0xff)));
      worst = std::max(worst, d);
      bad += d > tol;
    }
    char nm[48];
    snprintf(nm, sizeof(nm), "/ov%02d", ++shot);
    write_png(dir + nm + ".png", W, H, got);
    if (bad)
      write_png(dir + nm + "-ref.png", W, H, ref);
    report("overlay: " + what, locked && flipped && bad == 0,
           !locked    ? "(no LOCK_READBACK)"
           : !flipped ? "(no FLIP_READBACK)"
           : bad      ? "(" + std::to_string(bad) + " pixels, largest " +
                            std::to_string(worst) + ")"
                      : "");
    return got;
  };
  const u32 KEY_COLOUR = 0x20; // graphics in the key's range OR video false
  const u32 KEY_ALL = 0x111;   // both true, ANDed

  // 1: a packed surface at 1:1 behind the colour key (the window reaches
  // past the key's block on the right and below)
  {
    Surface s = make(96, 64, 48, 64);
    check("YUY2 at 1:1, the graphics colour key", s,
          {11, 120, 100, 96, 64, OvFilter::Nearest, OvFilter::Nearest, false,
           KEY_COLOUR, 0, 0, 0, 0});
  }
  // 2: the other byte order, doubled, the linear filter; then the same
  // with the scaler's clock halved and H_INC doubled to make up for it
  {
    Surface s = make(64, 48, 32, 48);
    const Setup c = {
        12,      40, 20, 128, 96, OvFilter::Linear, OvFilter::Linear, false,
        KEY_ALL, 0,  0,  0,   0};
    const std::vector<u32> a = check("UYVY doubled, two-tap filter", s, c);
    Setup c2 = c;
    c2.ecp_div = 1;
    const std::vector<u32> b =
        check("the same at half the scaler clock (ECP_DIV)", s, c2);
    report("overlay: ECP_DIV leaves the picture alone", a == b, "");
  }
  // 3: a planar surface, its chroma at a quarter the window's size, the
  // programmed four-tap filter across and the two-tap one down
  {
    Surface s = make(64, 48, 32, 24);
    check("YV12 doubled, four-tap horizontal filter", s,
          {10, 60, 40, 128, 96, OvFilter::FourTap, OvFilter::Linear, false,
           KEY_ALL, 0, 0, 0, 0});
    // four taps down as well (STEP_BY 0)
    check("YV12 doubled, four taps both ways", s,
          {10, 60, 40, 128, 96, OvFilter::FourTap, OvFilter::FourTap, true,
           KEY_ALL, 0, 0, 0, 0});
  }
  // 4: RGB surfaces, no transform
  {
    Surface s = make(64, 48, 64, 48);
    quantise565(s);
    check("an RGB565 surface at 1:1", s,
          {4, 30, 30, 64, 48, OvFilter::Nearest, OvFilter::Nearest, false,
           KEY_ALL, 0, 0, 0, 0});
    Surface t = make(48, 40, 48, 40);
    check("an xRGB8888 surface doubled, two-tap filter", t,
          {6, 100, 60, 96, 80, OvFilter::Linear, OvFilter::Linear, false,
           KEY_ALL, 0, 0, 0, 0});
  }
  // 5: the other key functions and the alpha modes
  {
    Surface s = make(96, 64, 48, 64);
    // graphics outside the key's range
    check("key: graphics not equal", s,
          {11, 120, 100, 96, 64, OvFilter::Nearest, OvFilter::Nearest, false,
           0x30, 0, 0, 0, 0});
    // the video key: only video pixels in a range of colours, and only
    // over the graphics key
    check("key: video in range AND graphics equal", s,
          {11, 120, 100, 96, 64, OvFilter::Nearest, OvFilter::Nearest, false,
           0x122, 0, 0x00400000, 0x00ffc0ff, 0});
    // global alpha: a quarter video over three quarters graphics
    check("merge: global alpha", s,
          {11, 120, 100, 96, 64, OvFilter::Nearest, OvFilter::Nearest, false,
           0x100, 2u | (0xc0u << 16) | (0x40u << 24), 0, 0, 0});
    // per-pixel alpha: the graphics pixel's own
    check("merge: per-pixel alpha", s,
          {11, 150, 100, 96, 64, OvFilter::Nearest, OvFilter::Nearest, false,
           KEY_ALL, 1u, 0, 0, 0});
  }
  // 6: the lock. The scaler is showing the window of the last check;
  // under the lock a new position must not show, however long it waits,
  // and must once the lock is released and the blank has come
  {
    wr(DISP_MERGE_CNTL, 0); // (not one of the locked registers)
    const std::vector<u32> before = frame();
    const bool locked = lock();
    const u32 s0 = rd(OV0_Y_X_START), e0 = rd(OV0_Y_X_END);
    wr(OV0_Y_X_START, s0 - 60);
    wr(OV0_Y_X_END, e0 - 60);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const bool held = frame() == before && rd(OV0_Y_X_START) == s0 - 60;
    const bool flipped = unlock();
    const std::vector<u32> after = frame();
    // the window moved 60 pixels left: its first column is video now
    const int x_new = int(s0 & 0x1fff) - 8 - 60, y_in = int(s0 >> 16) + 4;
    const bool moved =
        after != before &&
        after[size_t(y_in * W + x_new)] != (desktop(x_new, y_in) & 0xffffffu);
    report("overlay: OV0_REG_LOAD_CNTL holds an update until unlock + blank",
           locked && held && flipped && moved,
           !locked    ? "(no LOCK_READBACK)"
           : !held    ? "(the update showed under the lock)"
           : !flipped ? "(no FLIP_READBACK)"
           : !moved   ? "(the update never showed)"
                      : "");
    // and the scaler off
    lock();
    wr(OV0_SCALE_CNTL, 0);
    unlock();
    const std::vector<u32> off = frame();
    bool plain = true;
    for (int y = 0; y < H && plain; y++)
      for (int x = 0; x < W; x++)
        if (off[size_t(y * W + x)] != (desktop(x, y) & 0xffffffu)) {
          plain = false;
          break;
        }
    report("overlay: disabled, the desktop alone", plain, "");
  }
  // the transform's defaults are BT.601's: a few colours against the
  // standard's equations
  {
    const u32 lin[6] = {R(OV0_LIN_TRANS_A), R(OV0_LIN_TRANS_B),
                        R(OV0_LIN_TRANS_C), R(OV0_LIN_TRANS_D),
                        R(OV0_LIN_TRANS_E), R(OV0_LIN_TRANS_F)};
    int worst = 0;
    for (int y : {16, 81, 145, 210, 235})
      for (int cb : {16, 90, 128, 200, 240})
        for (int cr : {16, 90, 128, 200, 240}) {
          int rgb[3];
          overlay_to_rgb(lin, y, cb, cr, rgb);
          const double e[3] = {1.164 * (y - 16) + 1.596 * (cr - 128),
                               1.164 * (y - 16) - 0.392 * (cb - 128) -
                                   0.813 * (cr - 128),
                               1.164 * (y - 16) + 2.017 * (cb - 128)};
          for (int k = 0; k < 3; k++)
            worst = std::max(worst,
                             std::abs(rgb[k] - clamp8(int(std::lround(e[k])))));
        }
    report("overlay: the transform's power-on values are BT.601", worst <= 3,
           worst > 3 ? "(off by " + std::to_string(worst) + ")" : "");
  }
}
