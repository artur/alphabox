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
 * ALPHABOX_RADEON_SELFTEST=1 (or =exit): the Radeon's engines driven the
 * way a driver drives them -- register writes through the MMIO BAR and
 * packets through the command processor's ring in VRAM -- and each result
 * compared with a software reference written here from the documented
 * behaviour, independently of the engine's own code (its own ROP3 truth
 * table, its own barycentric rasteriser in double precision, its own
 * texture decoders and lighting).
 *
 * Runs at the end of init(), before the machine starts, and puts the
 * card back as it found it (registers, PLLs, VRAM, engine state). Prints
 * one line per check,
 *   %RADEON-I-SELFTEST: <check>   ok | FAILED <what differed>
 * and PASS or FAIL. The 3D scenes are written as PNG files to
 * ALPHABOX_RADEON_SELFTEST_DIR (default $ALPHABOX_WORK/radeon-3d, else
 * ./radeon-3d), each with a comparison beside it (<scene>-cmp.png: the
 * frame, the reference and the differing pixels in white).
 **/

#include "Radeon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>

using namespace radeon;

namespace {

// --- a minimal PNG writer (stored deflate blocks) ----------------------------
u32 crc_table[256];
void crc_init() {
  for (u32 n = 0; n < 256; n++) {
    u32 c = n;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
u32 crc(const u8 *b, size_t n, u32 c = 0xffffffffu) {
  for (size_t i = 0; i < n; i++)
    c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
  return c;
}
void be32(std::vector<u8> &v, u32 x) {
  for (int s = 24; s >= 0; s -= 8)
    v.push_back(u8(x >> s));
}
void chunk(FILE *f, const char *type, const std::vector<u8> &data) {
  std::vector<u8> b;
  be32(b, u32(data.size()));
  b.insert(b.end(), type, type + 4);
  b.insert(b.end(), data.begin(), data.end());
  const u32 c = crc(b.data() + 4, b.size() - 4) ^ 0xffffffffu;
  be32(b, c);
  fwrite(b.data(), 1, b.size(), f);
}
/// An RGB image (0x00RRGGBB pixels) as a PNG file.
bool write_png(const std::string &path, int w, int h,
               const std::vector<u32> &px) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    return false;
  static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  std::vector<u8> ihdr;
  be32(ihdr, u32(w));
  be32(ihdr, u32(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(f, "IHDR", ihdr);
  std::vector<u8> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      const u32 p = px[size_t(y * w + x)];
      raw.push_back(u8(p >> 16));
      raw.push_back(u8(p >> 8));
      raw.push_back(u8(p));
    }
  }
  std::vector<u8> z = {0x78, 0x01};
  u32 a = 1, b = 0;
  for (u8 v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n == raw.size() ? 1 : 0);
    z.push_back(u8(n));
    z.push_back(u8(n >> 8));
    z.push_back(u8(~n));
    z.push_back(u8(~n >> 8));
    z.insert(z.end(), raw.begin() + long(i), raw.begin() + long(i + n));
  }
  be32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
  return true;
}

// --- small helpers -----------------------------------------------------------
u32 fbits(float f) {
  u32 v;
  memcpy(&v, &f, 4);
  return v;
}
u32 rng_state = 12345;
u32 rnd() {
  rng_state = rng_state * 1103515245u + 12345u;
  return (rng_state >> 8) ^ (rng_state << 13);
}
float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
u32 to8(float v) { return u32(std::lround(clampf(v) * 255.0f)); }
u32 argbf(const float c[4]) {
  return (to8(c[3]) << 24) | (to8(c[0]) << 16) | (to8(c[1]) << 8) | to8(c[2]);
}
void unargb(u32 v, float c[4]) {
  c[0] = float((v >> 16) & 0xff) / 255;
  c[1] = float((v >> 8) & 0xff) / 255;
  c[2] = float(v & 0xff) / 255;
  c[3] = float(v >> 24) / 255;
}

/// Packet headers.
u32 pkt0(u32 reg, u32 count) { return ((count - 1) << 16) | (reg >> 2); }
u32 pkt0_one(u32 reg, u32 count) {
  return ((count - 1) << 16) | 0x8000 | (reg >> 2);
}
u32 pkt3(u32 op, u32 count) {
  return 0xc0000000u | ((count - 1) << 16) | (op << 8);
}

/// The reference rasteriser: every pixel whose centre a triangle covers
/// (top-left rule), with its barycentric weights.
struct RefVtx {
  double x, y;
};
void ref_triangle(
    RefVtx a, RefVtx b, RefVtx c, int w, int h,
    const std::function<void(int, int, double, double, double)> &shade) {
  double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
  if (area == 0)
    return;
  if (area < 0) {
    std::swap(b, c);
    area = -area;
  }
  const RefVtx v[3] = {a, b, c};
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const double px = x + 0.5, py = y + 0.5;
      double l[3];
      bool in = true;
      for (int i = 0; i < 3 && in; i++) {
        const RefVtx &p = v[(i + 1) % 3], &q = v[(i + 2) % 3];
        // the edge opposite vertex i, from p to q
        const double e = (q.x - p.x) * (py - p.y) - (q.y - p.y) * (px - p.x);
        const double dx = q.x - p.x, dy = q.y - p.y;
        const bool tl = dy < 0 || (dy == 0 && dx > 0);
        in = e > 0 || (e == 0 && tl);
        l[i] = e / area;
      }
      if (in)
        shade(x, y, l[0], l[1], l[2]);
    }
}

} // namespace

bool CRadeon::selftest() {
  crc_init();
  int failures = 0, checks = 0;
  auto report = [&](const std::string &what, bool ok,
                    const std::string &detail = "") {
    checks++;
    if (!ok)
      failures++;
    printf("%%RADEON-I-SELFTEST: %-46s %s%s%s\n", what.c_str(),
           ok ? "ok" : "FAILED", detail.empty() ? "" : " ", detail.c_str());
  };

  // Put everything back afterwards.
  std::vector<u32> saved_regs(m_regs, m_regs + REG_APERTURE_BYTES / 4);
  std::vector<u32> saved_pll(m_pll, m_pll + PLL_REGS);
  std::vector<u8> saved_vram(vga.memory, vga.memory + m_vram_bytes);
  const engine_t saved_eng = eng;

  std::string dir;
  if (const char *d = getenv("ALPHABOX_RADEON_SELFTEST_DIR"))
    dir = d;
  else if (const char *w = getenv("ALPHABOX_WORK"))
    dir = std::string(w) + "/radeon-3d";
  else
    dir = "radeon-3d";
  mkdir(dir.c_str(), 0755);

  // Saved separately: the micro-engine and the CP's queues.
  u32 saved_me[256][2];
  memcpy(saved_me, m_me_ram, sizeof(saved_me));
  u32 saved_me_written[8];
  memcpy(saved_me_written, m_me_written, sizeof(saved_me_written));
  const bool saved_me_loaded = m_me_loaded;

  // Through the MMIO BAR, as a driver. The engine works behind the
  // command FIFO, so the CPU's accesses to memory the engine draws in
  // wait for it as a driver does (r100_gui_wait_for_idle: 64 free FIFO
  // entries, then GUI_ACTIVE clear).
  // A driver that switches from the CP to MMIO (or reads what the CP
  // wrote) waits for the CP's stream first: the two feed one engine
  // (X.org's RADEONWaitForIdleCP before its MMIO paths).
  bool dirty = false, cp_used = false;
  int idle_timeouts = 0;
  std::function<void()> sync;
  auto wr = [&](u32 r, u32 v) {
    if (cp_used && r != 0x0714)
      sync();
    WriteMem_Bar(0, 2, r, 32, v);
    dirty = true;
  };
  auto rd = [&](u32 r) {
    if (cp_used)
      sync();
    return ReadMem_Bar(0, 2, r, 32);
  };
  sync = [&]() {
    cp_used = false;
    if (!dirty)
      return;
    dirty = false;
    const auto give_up =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
      const u32 s = ReadMem_Bar(0, 2, RBBM_STATUS, 32);
      if ((s & RBBM_FIFOCNT_MASK) == m_chip->cmdfifo_entries &&
          !(s & RBBM_ACTIVE))
        return;
      if (std::chrono::steady_clock::now() > give_up) {
        idle_timeouts++;
        return;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  };
  auto vrd = [&](u32 a, int n) {
    sync();
    return vram_read(a, n);
  };
  auto vwr = [&](u32 a, int n, u32 v) {
    sync();
    vram_write(a, n, v);
  };
  auto vr32 = [&](u32 a) { return vrd(a, 4); };
  auto vw32 = [&](u32 a, u32 v) { vwr(a, 4, v); };

  // The framebuffer at 0 in the memory controller's space.
  wr(MC_FB_LOCATION, ((m_vram_bytes - 1) & 0xffff0000u));
  wr(SURFACE_CNTL, 0);

  // The command FIFO, the CP without and with microcode, the GART
  // (RadeonSelfTestQueue.cpp); leaves the CP running with microcode.
  selftest_queue(report);

  // The command processor's ring in VRAM, 64K dwords.
  const u32 RING = 0x800000, IB = 0x880000, SCRATCH = 0x8f0000;
  wr(0x0740, 0);               // CP_CSQ_CNTL: off while the ring is set up
  wr(0x0700, RING);            // CP_RB_BASE
  wr(0x0704, 15 | (1u << 31)); // CP_RB_CNTL: 2^16 dwords, RPTR_WR_ENA
  wr(0x070c, SCRATCH);         // CP_RB_RPTR_ADDR
  wr(0x071c, 0);               // CP_RB_RPTR_WR
  wr(0x0714, 0);               // CP_RB_WPTR: the read pointer follows
  wr(0x0704, 15);              // CP_RB_CNTL: RPTR_WR_ENA off again
  wr(0x0740, 4u << 28);        // CP_CSQ_CNTL: primary and indirect BM
  u32 ring_pos = 0;
  auto cp = [&](const std::vector<u32> &p) {
    for (u32 d : p) {
      vw32(RING + ring_pos * 4, d);
      ring_pos = (ring_pos + 1) & 0xffff;
    }
    wr(0x0714, ring_pos);
    cp_used = true;
  };

  // ======================================================================
  // 2D engine
  // ======================================================================
  const u32 S2D = 0x100000; // a 256x256 surface at 32 bpp, pitch 1024
  const u32 PO32 = (16u << 22) | (S2D >> 10);
  auto px32 = [&](int x, int y) {
    return vr32(S2D + u32(y) * 1024 + u32(x) * 4);
  };
  auto setpx = [&](int x, int y, u32 v) {
    vw32(S2D + u32(y) * 1024 + u32(x) * 4, v);
  };
  auto fill_random = [&]() {
    for (int y = 0; y < 256; y++)
      for (int x = 0; x < 256; x++)
        setpx(x, y, rnd());
  };
  auto snapshot = [&]() {
    std::vector<u32> s(256 * 256);
    for (int y = 0; y < 256; y++)
      for (int x = 0; x < 256; x++)
        s[size_t(y * 256 + x)] = px32(x, y);
    return s;
  };
  // the common setup: 32 bpp, default pitch/offset, the whole surface
  wr(0x16e0, PO32);                // DEFAULT_PITCH_OFFSET
  wr(0x16e8, (255u << 16) | 255u); // DEFAULT_SC_BOTTOM_RIGHT
  const u32 GMC_DST32 = 6u << 8;
  const u32 GMC_SRC_COLOR = 3u << 12, GMC_SRC_MEM = 2u << 24;
  const u32 GMC_BRUSH_SOLID = 13u << 4, GMC_BRUSH_NONE = 15u << 4;
  const u32 GMC_CLR_CMP_DIS = 1u << 28, GMC_WRMSK_DIS = 1u << 30;

  // -- every ROP3, through a screen-to-screen blit with a solid brush ------
  {
    fill_random();
    const std::vector<u32> before = snapshot();
    const u32 P = 0x5a3cc3a5u;
    int bad = 0;
    for (u32 rop = 0; rop < 256; rop++) {
      const int dx = int(rop % 16) * 8, dy = 128 + int(rop / 16) * 8;
      const int sx = int(rop % 16) * 8, sy = int(rop / 16) * 8;
      wr(0x146c, GMC_BRUSH_SOLID | GMC_DST32 | GMC_SRC_COLOR | (rop << 16) |
                     GMC_SRC_MEM | GMC_CLR_CMP_DIS | GMC_WRMSK_DIS);
      wr(0x147c, P); // DP_BRUSH_FRGD_CLR
      wr(0x16c0, 3); // DP_CNTL
      wr(0x1434, (u32(sy) << 16) | u32(sx));
      wr(0x1438, (u32(dy) << 16) | u32(dx));
      wr(0x143c, (8u << 16) | 8u);
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
          const u32 s = before[size_t((sy + y) * 256 + sx + x)];
          const u32 d = before[size_t((dy + y) * 256 + dx + x)];
          u32 want = 0;
          for (int b = 0; b < 32; b++) {
            const u32 idx =
                (((P >> b) & 1) << 2) | (((s >> b) & 1) << 1) | ((d >> b) & 1);
            want |= ((rop >> idx) & 1) << b;
          }
          if (px32(dx + x, dy + y) != want)
            bad++;
        }
    }
    report("2D: all 256 ROP3s (pattern, source, dest)", bad == 0,
           bad ? std::to_string(bad) + " pixels" : "");
  }

  // -- overlapping blits in the four directions (MMIO, as X.org) ----------
  {
    int bad = 0;
    const int dirs[4][2] = {{6, 3}, {-6, 3}, {6, -3}, {-5, -7}};
    for (const auto &dv : dirs) {
      fill_random();
      std::vector<u32> before = snapshot();
      const int sx = 50, sy = 50, w = 60, h = 40;
      const int dx = sx + dv[0], dy = sy + dv[1];
      const int xdir = sx < dx ? -1 : 1, ydir = sy < dy ? -1 : 1;
      wr(0x146c, GMC_BRUSH_NONE | GMC_DST32 | GMC_SRC_COLOR | (0xccu << 16) |
                     GMC_SRC_MEM | GMC_CLR_CMP_DIS);
      wr(0x16cc, 0xffffffffu);
      wr(0x16c0, (xdir > 0 ? 1u : 0u) | (ydir > 0 ? 2u : 0u));
      const int x0 = xdir > 0 ? 0 : w - 1, y0 = ydir > 0 ? 0 : h - 1;
      wr(0x1434, (u32(sy + y0) << 16) | u32(sx + x0));
      wr(0x1438, (u32(dy + y0) << 16) | u32(dx + x0));
      wr(0x143c, (u32(h) << 16) | u32(w));
      std::vector<u32> want = before;
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          want[size_t((dy + y) * 256 + dx + x)] =
              before[size_t((sy + y) * 256 + sx + x)];
      bad += int(snapshot() != want);
    }
    report("2D: overlapping blits, all four directions", bad == 0);
    // the same through the CP's BITBLT_MULTI, which picks the direction
    bad = 0;
    for (const auto &dv : dirs) {
      fill_random();
      std::vector<u32> before = snapshot();
      const int sx = 70, sy = 60, w = 33, h = 21;
      const int dx = sx + dv[0], dy = sy + dv[1];
      cp({pkt3(0x9b, 4),
          GMC_BRUSH_NONE | GMC_DST32 | GMC_SRC_COLOR | (0xccu << 16) |
              GMC_SRC_MEM | GMC_CLR_CMP_DIS | GMC_WRMSK_DIS,
          (u32(sx) << 16) | u32(sy), (u32(dx) << 16) | u32(dy),
          (u32(w) << 16) | u32(h)});
      std::vector<u32> want = before;
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          want[size_t((dy + y) * 256 + dx + x)] =
              before[size_t((sy + y) * 256 + sx + x)];
      bad += int(snapshot() != want);
    }
    report("CP: BITBLT_MULTI overlapping, four directions", bad == 0);
  }

  // -- lines: Bresenham properties, LAST_PEL, the 32x1 dash pattern -------
  {
    int bad = 0;
    const int ends[][4] = {{10, 10, 200, 60},  {200, 60, 10, 10},
                           {30, 200, 40, 20},  {100, 100, 100, 180},
                           {5, 128, 250, 128}, {77, 33, 12, 250},
                           {60, 60, 61, 200}};
    for (const auto &L : ends)
      for (int last = 0; last < 2; last++) {
        for (int y = 0; y < 256; y++)
          for (int x = 0; x < 256; x++)
            setpx(x, y, 0);
        wr(0x146c, GMC_BRUSH_SOLID | GMC_DST32 | GMC_SRC_COLOR | (0xf0u << 16) |
                       GMC_CLR_CMP_DIS | GMC_WRMSK_DIS);
        wr(0x147c, 0x00ffffff);
        wr(0x16c0, 3 | (last ? 0x20u : 0));
        wr(0x1600, (u32(L[1]) << 16) | u32(L[0]));
        wr(0x1604, (u32(L[3]) << 16) | u32(L[2]));
        // the pixel count is the major extent (+1 with the last pixel),
        // the start drawn, the end only with LAST_PEL, each pixel within
        // half a pixel of the ideal line along the minor axis
        const int adx = std::abs(L[2] - L[0]), ady = std::abs(L[3] - L[1]);
        const int major = std::max(adx, ady);
        int count = 0;
        bool near_ok = true;
        for (int y = 0; y < 256; y++)
          for (int x = 0; x < 256; x++) {
            if (!px32(x, y))
              continue;
            count++;
            double dist;
            if (adx >= ady)
              dist = std::fabs(L[1] +
                               double(L[3] - L[1]) * (x - L[0]) /
                                   (adx ? double(L[2] - L[0]) : 1) -
                               y);
            else
              dist = std::fabs(
                  L[0] +
                  double(L[2] - L[0]) * (y - L[1]) / double(L[3] - L[1]) - x);
            if (dist > 0.5 + 1e-9)
              near_ok = false;
          }
        const bool ok = count == major + last && px32(L[0], L[1]) != 0 &&
                        (px32(L[2], L[3]) != 0) == (last != 0) && near_ok;
        bad += !ok;
      }
    report("2D: lines (count, ends, LAST_PEL, closeness)", bad == 0,
           bad ? std::to_string(bad) + " of 14 lines" : "");
    // a dashed line: 32x1 mono brush FG/BG, from DST_LINE_PATCOUNT
    for (int x = 0; x < 256; x++)
      setpx(x, 5, 0);
    wr(0x146c, (6u << 4) | GMC_DST32 | GMC_SRC_COLOR | (0xf0u << 16) |
                   GMC_CLR_CMP_DIS | GMC_WRMSK_DIS | (1u << 14));
    wr(0x147c, 0x00ff0000);
    wr(0x1478, 0x000000ff);
    wr(0x1480, 0xf0f0ff00u);
    wr(0x16c0, 3);
    wr(0x1608, 3); // DST_LINE_PATCOUNT
    wr(0x1600, (5u << 16) | 0u);
    wr(0x1604, (5u << 16) | 64u);
    bad = 0;
    for (int x = 0; x < 64; x++) {
      const u32 bit = (0xf0f0ff00u >> ((x + 3) & 31)) & 1; // LSB first
      bad += px32(x, 5) != (bit ? 0x00ff0000u : 0x000000ffu);
    }
    report("2D: dashed line, 32x1 brush and pattern count", bad == 0);
  }

  // -- host data: colour, mono MSB/LSB, FG/LA, byte-aligned, big endian ----
  {
    int bad = 0;
    for (int y = 0; y < 256; y++)
      for (int x = 0; x < 256; x++)
        setpx(x, y, 0x11111111);
    // colour, 5x3 pixels, then one with HOST_BIG_ENDIAN_EN
    for (int be = 0; be < 2; be++) {
      wr(0x146c, GMC_BRUSH_NONE | GMC_DST32 | GMC_SRC_COLOR | (0xccu << 16) |
                     (3u << 24) | GMC_CLR_CMP_DIS | GMC_WRMSK_DIS);
      if (be)
        wr(0x16c4, rd(0x16c4) | (1u << 29));
      wr(0x1438, (u32(10 + 10 * be) << 16) | 10u);
      wr(0x143c, (3u << 16) | 5u);
      for (int i = 0; i < 15; i++)
        wr(i == 14 ? 0x17e0 : 0x17c0, 0x01020300u + u32(i));
      for (int i = 0; i < 15; i++) {
        u32 v = 0x01020300u + u32(i);
        if (be)
          v = (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
        bad += px32(10 + i % 5, 10 + 10 * be + i / 5) != v;
      }
    }
    report("2D: host data, colour (and big-endian)", bad == 0);
    bad = 0;
    // mono: 12x2, FG/BG then FG/leave-alone, MSB and LSB first
    const u32 data[] = {0xa5c3f00fu};
    for (int lsb = 0; lsb < 2; lsb++)
      for (int la = 0; la < 2; la++) {
        const int y0 = 40 + 4 * (lsb * 2 + la);
        wr(0x146c, GMC_BRUSH_NONE | GMC_DST32 | (u32(la) << 12) |
                       (0xccu << 16) | (3u << 24) | GMC_CLR_CMP_DIS |
                       GMC_WRMSK_DIS | (u32(lsb) << 14));
        wr(0x15d8, 0x00ff00ff);
        wr(0x15dc, 0x0000ff00);
        wr(0x1438, (u32(y0) << 16) | 20u);
        wr(0x143c, (2u << 16) | 12u);
        wr(0x17e0, data[0]);
        for (int i = 0; i < 24; i++) {
          const int byte = i / 8, b = i % 8;
          const bool on = (data[0] >> (8 * byte + (lsb ? b : 7 - b))) & 1;
          const u32 want = on ? 0x00ff00ffu : la ? 0x11111111u : 0x0000ff00u;
          bad += px32(20 + i % 12, y0 + i / 12) != want;
        }
      }
    // byte-aligned mono: each row starts on a new byte
    wr(0x146c, GMC_BRUSH_NONE | GMC_DST32 | (0u << 12) | (0xccu << 16) |
                   (4u << 24) | GMC_CLR_CMP_DIS | GMC_WRMSK_DIS);
    wr(0x1438, (60u << 16) | 20u);
    wr(0x143c, (3u << 16) | 5u);
    wr(0x17e0, 0x00a0f8b0u); // rows 10110, 11111, 10100 (MSB first)
    const char *rows[3] = {"10110", "11111", "10100"};
    for (int r = 0; r < 3; r++)
      for (int x = 0; x < 5; x++)
        bad += px32(20 + x, 60 + r) !=
               (rows[r][x] == '1' ? 0x00ff00ffu : 0x0000ff00u);
    report("2D: host data, mono (MSB/LSB, BG/LA, byte-aligned)", bad == 0);
  }

  // -- clipping, write mask, colour compare --------------------------------
  {
    int bad = 0;
    for (int y = 0; y < 256; y++)
      for (int x = 0; x < 256; x++)
        setpx(x, y, 0);
    wr(0x146c, GMC_BRUSH_SOLID | GMC_DST32 | GMC_SRC_COLOR | (0xf0u << 16) |
                   GMC_CLR_CMP_DIS | GMC_WRMSK_DIS | 8 /* DST_CLIPPING */);
    wr(0x16ec, (20u << 16) | 30u); // SC_TOP_LEFT: y 20, x 30
    wr(0x16f0, (40u << 16) | 50u); // SC_BOTTOM_RIGHT: y 40, x 50
    wr(0x147c, 0x00abcdef);
    wr(0x1438, (10u << 16) | 10u);
    wr(0x143c, (100u << 16) | 100u);
    for (int y = 0; y < 120; y++)
      for (int x = 0; x < 120; x++) {
        const bool in = x >= 30 && x <= 50 && y >= 20 && y <= 40;
        bad += px32(x, y) != (in ? 0x00abcdefu : 0u);
      }
    report("2D: scissor rectangle (inclusive)", bad == 0);
    bad = 0;
    // write mask: GMC_WR_MSK_DIS sets DP_WRITE_MSK to ones (RRG 3-175);
    // without it the mask written last applies
    wr(0x146c, GMC_BRUSH_SOLID | GMC_DST32 | GMC_SRC_COLOR | (0xf0u << 16) |
                   GMC_CLR_CMP_DIS | GMC_WRMSK_DIS);
    bad += rd(0x16cc) != 0xffffffffu;
    wr(0x146c, GMC_BRUSH_SOLID | GMC_DST32 | GMC_SRC_COLOR | (0xf0u << 16) |
                   GMC_CLR_CMP_DIS);
    wr(0x16cc, 0x00ff00ffu);
    for (int x = 0; x < 8; x++)
      setpx(200 + x, 200, 0x12345678);
    wr(0x147c, 0xffffffffu);
    wr(0x1438, (200u << 16) | 200u);
    wr(0x143c, (1u << 16) | 8u);
    for (int x = 0; x < 8; x++)
      bad += px32(200 + x, 200) != 0x12ff56ffu;
    report("2D: write mask and GMC_WR_MSK_DIS", bad == 0);
    bad = 0;
    // GMC writes set DP_CNTL's directions (RRG 3-165)
    wr(0x16c0, 0);
    wr(0x146c, GMC_BRUSH_SOLID | GMC_DST32 | GMC_SRC_COLOR | (0xf0u << 16) |
                   GMC_CLR_CMP_DIS | GMC_WRMSK_DIS);
    for (int x = 0; x < 4; x++)
      setpx(100 + x, 100, 0);
    wr(0x147c, 0x00777777);
    wr(0x1438, (100u << 16) | 100u);
    wr(0x143c, (1u << 16) | 4u);
    for (int x = 0; x < 4; x++)
      bad += px32(100 + x, 100) != 0x00777777u;
    report("2D: GMC write sets DP_CNTL left-to-right, top-down", bad == 0);
    bad = 0;
    // colour compare through TRANS_BITBLT: source pixels equal to the
    // reference are not written (SRC_CMP_EQ_COLOR on the Radeon)
    fill_random();
    for (int x = 0; x < 16; x++)
      setpx(x, 0, (x & 1) ? 0x00c0ffeeu : 0x00000000u + u32(x));
    std::vector<u32> before = snapshot();
    cp({pkt3(0x9c, 7),
        GMC_BRUSH_NONE | GMC_DST32 | GMC_SRC_COLOR | (0xccu << 16) |
            GMC_SRC_MEM | GMC_WRMSK_DIS,
        4u | (1u << 24), 0x00c0ffeeu, 0, (0u << 16) | 0u, (0u << 16) | 1u,
        (16u << 16) | 1u});
    for (int x = 0; x < 16; x++) {
      const u32 want = (x & 1) ? before[size_t(256 + x)] : u32(x);
      bad += px32(x, 1) != want;
    }
    report("CP: TRANS_BITBLT, source colour key", bad == 0);
    // GMC_CLR_CMP_CNTL_DIS clears the compare functions
    wr(0x146c, GMC_BRUSH_NONE | GMC_DST32 | GMC_SRC_COLOR | GMC_CLR_CMP_DIS);
    report("2D: GMC_CLR_CMP_CNTL_DIS clears CLR_CMP_CNTL",
           (rd(0x15c0) & 0x707) == 0);
  }

  // -- brushes ------------------------------------------------------------
  {
    int bad = 0;
    for (int y = 0; y < 256; y++)
      for (int x = 0; x < 256; x++)
        setpx(x, y, 0x01010101);
    // 8x8 mono FG/BG with a pattern origin (BRUSH_Y_X)
    wr(0x146c, (0u << 4) | GMC_DST32 | GMC_SRC_COLOR | (0xf0u << 16) |
                   GMC_CLR_CMP_DIS | GMC_WRMSK_DIS);
    wr(0x147c, 0x00ffffff);
    wr(0x1478, 0x00000080);
    wr(0x1480, 0x99a5c3e7u);
    wr(0x1484, 0x0f1e3c78u);
    wr(0x1474, (3u << 8) | 5u);
    wr(0x1438, (16u << 16) | 16u);
    wr(0x143c, (16u << 16) | 16u);
    const u32 pat[2] = {0x99a5c3e7u, 0x0f1e3c78u};
    for (int y = 16; y < 32; y++)
      for (int x = 16; x < 32; x++) {
        const int bx = (x + 5) & 7, by = (y + 3) & 7;
        const u8 row = u8(pat[by >> 2] >> (8 * (by & 3)));
        const bool on = (row >> (7 - bx)) & 1;
        bad += px32(x, y) != (on ? 0x00ffffffu : 0x00000080u);
      }
    report("2D: 8x8 mono brush with BRUSH_Y_X", bad == 0);
    bad = 0;
    // 8x8 colour brush through PAINT_MULTI: 64 pixels in the packet
    std::vector<u32> pk = {pkt3(0x9a, 1 + 64 + 2),
                           (10u << 4) | GMC_DST32 | GMC_SRC_COLOR |
                               (0xf0u << 16) | GMC_CLR_CMP_DIS | GMC_WRMSK_DIS};
    for (u32 i = 0; i < 64; i++)
      pk.push_back(0x00010203u * i);
    pk.push_back((64u << 16) | 64u);
    pk.push_back((16u << 16) | 16u);
    cp(pk);
    for (int y = 64; y < 80; y++)
      for (int x = 64; x < 80; x++)
        bad += px32(x, y) != 0x00010203u * u32((y & 7) * 8 + (x & 7));
    report("CP: PAINT_MULTI with an 8x8 colour brush", bad == 0);
  }

  // -- the other CP packets ----------------------------------------------
  {
    int bad = 0;
    for (int y = 0; y < 256; y++)
      for (int x = 0; x < 256; x++)
        setpx(x, y, 0);
    const u32 gmc = GMC_BRUSH_SOLID | GMC_DST32 | GMC_SRC_COLOR |
                    (0xf0u << 16) | GMC_CLR_CMP_DIS | GMC_WRMSK_DIS;
    // PAINT: rectangles by corners, [top|left] [bottom|right]
    cp({pkt3(0x91, 4), gmc, 0x00aa0000u, (10u << 16) | 20u, (14u << 16) | 25u});
    for (int y = 8; y < 16; y++)
      for (int x = 18; x < 27; x++) {
        const bool in = x >= 20 && x < 25 && y >= 10 && y < 14;
        bad += px32(x, y) != (in ? 0x00aa0000u : 0u);
      }
    report("CP: PAINT (corners, bottom-right exclusive)", bad == 0);
    bad = 0;
    // POLYLINE: [y|x] vertices
    cp({pkt3(0x95, 5), gmc, 0x0000bb00u, (30u << 16) | 40u, (30u << 16) | 50u,
        (35u << 16) | 50u});
    for (int x = 40; x < 50; x++)
      bad += px32(x, 30) != 0x0000bb00u;
    for (int y = 30; y < 35; y++)
      bad += px32(50, y) != 0x0000bb00u;
    bad += px32(50, 35) != 0; // the polyline's last pixel is not drawn
    report("CP: POLYLINE ([y|x] vertices)", bad == 0);
    bad = 0;
    // POLYSCANLINES: one scan of two segments, 2 rows high
    cp({pkt3(0x98, 7), gmc, 0x000000ccu, 1, 2, (2u << 16) | 60u,
        (70u << 16) | 64u, (90u << 16) | 80u});
    for (int y = 59; y < 63; y++)
      for (int x = 60; x < 92; x++) {
        const bool in =
            y >= 60 && y < 62 && ((x >= 64 && x < 70) || (x >= 80 && x < 90));
        bad += px32(x, y) != (in ? 0x000000ccu : 0u);
      }
    report("CP: POLYSCANLINES", bad == 0);
    bad = 0;
    // HOSTDATA_BLT (two bitmaps) and NEXTCHAR (current colours)
    cp({pkt3(0x94, 1 + 2 + 3 + 1 + 3 + 1),
        GMC_BRUSH_NONE | GMC_DST32 | (0u << 12) | (0xccu << 16) | (3u << 24) |
            GMC_CLR_CMP_DIS | GMC_WRMSK_DIS,
        0x00dddddd, 0x00222222, (100u << 16) | 100u, (1u << 16) | 8u, 1,
        0x000000f0u, (102u << 16) | 100u, (1u << 16) | 8u, 1, 0x0000000fu});
    cp({pkt3(0x19, 3), (104u << 16) | 100u, (1u << 16) | 8u, 0x000000aau});
    for (int x = 0; x < 8; x++) {
      bad += px32(100 + x, 100) != (x < 4 ? 0x00ddddddu : 0x00222222u);
      bad += px32(100 + x, 102) != (x >= 4 ? 0x00ddddddu : 0x00222222u);
      bad += px32(100 + x, 104) != ((x & 1) ? 0x00222222u : 0x00ddddddu);
    }
    report("CP: HOSTDATA_BLT (two bitmaps) and NEXTCHAR", bad == 0);
    bad = 0;
    // SET_SCISSORS, then a fill clipped by them
    cp({pkt3(0x1e, 2), (120u << 16) | 120u, (121u << 16) | 121u});
    wr(0x146c, gmc | 8);
    wr(0x147c, 0x00eeeeee);
    wr(0x1438, (118u << 16) | 118u);
    wr(0x143c, (6u << 16) | 6u);
    int n = 0;
    for (int y = 115; y < 126; y++)
      for (int x = 115; x < 126; x++)
        n += px32(x, y) != 0;
    report("CP: SET_SCISSORS", n == 4);
    // type-0 (consecutive and one-register), type-1, type-2 fillers
    cp({pkt0(0x15e0, 3), 0x11, 0x22, 0x33, 0x80000000u, pkt0_one(0x15f0, 2),
        0x44, 0x55, 0x40000000u | ((0x15f4 >> 2) << 11) | (0x15d8 >> 2), 0x66,
        0x77});
    report("CP: type-0, one-register type-0, type-1, type-2",
           rd(0x15e0) == 0x11 && rd(0x15e4) == 0x22 && rd(0x15e8) == 0x33 &&
               rd(0x15f0) == 0x55 && rd(0x15d8) == 0x66 && rd(0x15f4) == 0x77);
    // an indirect buffer, the scratch write-back and the read pointer
    vw32(IB, pkt0(0x15e4, 1));
    vw32(IB + 4, 0xabcd1234u);
    wr(0x0774, SCRATCH + 0x100); // SCRATCH_ADDR
    wr(0x0770, 1);               // SCRATCH_UMSK: scratch 0
    cp({pkt0(0x0738, 2), IB, 2, pkt0(0x15e0, 1), 0xfeedf00du});
    report("CP: indirect buffer, scratch and RPTR write-back",
           rd(0x15e4) == 0xabcd1234u && vr32(SCRATCH + 0x100) == 0xfeedf00du &&
               vr32(SCRATCH) == ring_pos && rd(0x0710) == ring_pos);
    wr(0x0770, 0);
  }

  // -- 8 and 16 bpp destinations ------------------------------------------
  {
    int bad = 0;
    // 16 bpp (RGB 565), pitch 512 bytes: a fill and a blit
    const u32 S16 = 0x200000, PO16 = (8u << 22) | (S16 >> 10);
    for (u32 i = 0; i < 256 * 256 * 2; i += 4)
      vw32(S16 + i, rnd());
    wr(0x146c, GMC_BRUSH_SOLID | (4u << 8) | GMC_SRC_COLOR | (0xf0u << 16) |
                   GMC_CLR_CMP_DIS | GMC_WRMSK_DIS | 3);
    wr(0x142c, PO16);
    wr(0x1428, PO16);
    wr(0x147c, 0xf81f);
    wr(0x1438, (3u << 16) | 5u);
    wr(0x143c, (4u << 16) | 7u);
    for (int y = 3; y < 7; y++)
      for (int x = 5; x < 12; x++)
        bad += vrd(S16 + u32(y) * 512 + u32(x) * 2, 2) != 0xf81f;
    wr(0x146c, GMC_BRUSH_NONE | (4u << 8) | GMC_SRC_COLOR | (0xccu << 16) |
                   GMC_SRC_MEM | GMC_CLR_CMP_DIS | GMC_WRMSK_DIS | 3);
    wr(0x16c0, 3);
    wr(0x1434, (3u << 16) | 5u);
    wr(0x1438, (50u << 16) | 60u);
    wr(0x143c, (4u << 16) | 7u);
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 7; x++)
        bad += vrd(S16 + u32(50 + y) * 512 + u32(60 + x) * 2, 2) != 0xf81f;
    // 8 bpp, pitch 256
    const u32 S8 = 0x300000, PO8 = (4u << 22) | (S8 >> 10);
    wr(0x146c, GMC_BRUSH_SOLID | (2u << 8) | GMC_SRC_COLOR | (0x5au << 16) |
                   GMC_CLR_CMP_DIS | GMC_WRMSK_DIS | 2);
    wr(0x142c, PO8);
    for (int x = 0; x < 9; x++)
      vwr(S8 + 10 * 256 + u32(x), 1, u32(x * 17));
    wr(0x147c, 0x3c);
    wr(0x1438, (10u << 16) | 0u);
    wr(0x143c, (1u << 16) | 9u);
    for (int x = 0; x < 9; x++)
      bad += vrd(S8 + 10 * 256 + u32(x), 1) != (u32(x * 17) ^ 0x3cu);
    report("2D: 16 bpp fill and blit, 8 bpp XOR fill", bad == 0);
  }

  // -- the hardware cursor ---------------------------------------------------
  {
    int bad = 0;
    // a 64x64 32 bpp mode at 0, the cursor image at 0x40000
    wr(CRTC_GEN_CNTL, CRTC_EXT_DISP_EN | CRTC_EN | (u32(PIX_32BPP) << 8) |
                          CRTC_CUR_EN | (2u << 20));
    wr(CRTC_H_TOTAL_DISP, (7u << 16) | 20);
    wr(CRTC_V_TOTAL_DISP, (63u << 16) | 80);
    wr(CRTC_PITCH, 8);
    wr(CRTC_OFFSET, 0);
    wr(DISPLAY_BASE_ADDR, 0);
    wr(CUR_OFFSET, 0x40000);
    wr(CUR_HORZ_VERT_POSN, (4u << 16) | 4u);
    wr(CUR_HORZ_VERT_OFF, 0);
    for (u32 i = 0; i < 64 * 64; i++)
      vw32(i * 4, 0x00406080u);
    // premultiplied ARGB: alpha 0x80 over the screen
    for (u32 i = 0; i < 64 * 64; i++) {
      const u32 a = (i & 63) * 4, c = (a * 0xc0) / 255;
      vw32(0x40000 + i * 4, (a << 24) | (c << 16) | (c << 8) | c);
    }
    bitmap_rgb32 bm;
    bm.allocate(64, 64);
    render_native(bm);
    draw_hw_cursor(bm);
    for (int y = 4; y < 64; y++)
      for (int x = 4; x < 64; x++) {
        const u32 a = u32(x - 4) * 4, cc = (a * 0xc0) / 255;
        const u32 dst[3] = {0x40, 0x60, 0x80};
        u32 want = 0;
        for (int k = 0; k < 3; k++) {
          const u32 v = std::min(255u, cc + (dst[k] * (255 - a) + 127) / 255);
          want |= v << (16 - 8 * k);
        }
        bad += (bm.pix(y, x) & 0xffffff) != want;
      }
    report("cursor: 64x64 ARGB, premultiplied alpha", bad == 0);
    // the mono cursor: AND/XOR rows of 16 bytes
    bad = 0;
    wr(CRTC_GEN_CNTL,
       CRTC_EXT_DISP_EN | CRTC_EN | (u32(PIX_32BPP) << 8) | CRTC_CUR_EN);
    wr(CUR_CLR0, 0x00ff0000);
    wr(CUR_CLR1, 0x000000ff);
    for (u32 r = 0; r < 64; r++)
      for (u32 b = 0; b < 8; b++) {
        vwr(0x40000 + r * 16 + b, 1, 0x0f); // AND: left half 0
        vwr(0x40000 + r * 16 + 8 + b, 1, 0x33);
      }
    bitmap_rgb32 bm2;
    bm2.allocate(64, 64);
    render_native(bm2);
    draw_hw_cursor(bm2);
    for (int x = 4; x < 12; x++) {
      const int col = x - 4, bit = 7 - (col & 7);
      const bool andb = (0x0f >> bit) & 1, xorb = (0x33 >> bit) & 1;
      u32 want = !andb ? (xorb ? 0x0000ffu : 0xff0000u)
                       : (xorb ? (0x406080u ^ 0xffffffu) : 0x406080u);
      bad += (bm2.pix(10, x) & 0xffffff) != want;
    }
    report("cursor: mono AND/XOR", bad == 0);
    wr(CRTC_GEN_CNTL, 0);
  }

  // ======================================================================
  // 3D engine
  // ======================================================================
  using namespace radeon3d;
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
    // light 1: a local orange light with linear attenuation
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
                 spc1[4] = {0, 0, 0, 1}, att1[4] = {0, 0.3, 1.0, 0},
                 dir1[4] = {0, 0, 0, 0};
    vec_upload(65, amb1, 1);
    vec_upload(73, dif1, 1);
    vec_upload(81, spc1, 1);
    vec_upload(89, P1, 1);
    vec_upload(97, dir1, 1);
    vec_upload(105, att1, 1);
    const double glob[4] = {0.05, 0.05, 0.1, 1}, eye[4] = {0, 0, 1, 1};
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
        (1u | 2u | 4u) | ((1u | 2u | 8u | 64u) << 16),
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
    // s' = 2s + 0.25, t' = 3t
    const double tm[16] = {2, 0, 0, 0.25, 0, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
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
        pkt0(SE_TCL_OUTPUT_VTX_SEL, 1),
        1u | (8u << 16),
        pkt0(SE_TCL_TEXTURE_PROC_CTL, 1),
        (1u << 4) | (0u << 16),
        pkt0(SE_TCL_LIGHT_MODEL_CTL, 1),
        0,
        pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        1u << 2,
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
    // its coverage solved per pixel in clip space
    base_state();
    clear_cb(0xff000000u);
    std::vector<u32> nref(size_t(W * H), 0xff000000u);
    const double f = 1.0, n = 1, fa = 10;
    const double pr[16] = {
        f, 0, 0,  0, 0, f, 0, 0, 0, 0, fa / (n - fa), n * fa / (n - fa),
        0, 0, -1, 0};
    vec_upload(0, pr, 4);
    cp({pkt0(SE_TCL_MATRIX_SELECT_0, 2),
        0u,
        0u,
        pkt0(SE_TCL_OUTPUT_VTX_SEL, 1),
        1u,
        pkt0(SE_TCL_TEXTURE_PROC_CTL, 1),
        0,
        pkt0(SE_TCL_UCP_VERT_BLEND_CTL, 1),
        0,
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
          if (w > 0 && z >= 0 && z <= w)
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
        // the line interpolates from x = 0 (0.0) towards 128 (1.0): the
        // pixel at x has t = x / 128 (its start, the model's line walk)
        const double v = double(x) / 128.0 * 31.0;
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
    m_3d->m_r200_packets = true;
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
    m_3d->m_r200_packets = false;
    for (int y = 60; y < 92; y++)
      for (int x = 8; x < 40; x++)
        ref[size_t(y * W + x)] = 0xffffffffu;
    // and without the switch the same pair draws nothing (R100 microcode)
    cp({pkt3(0x2a, 2), 0, PRIM_TRI_LIST | (WALK_INDEX << 4) | (6u << 16)});
    cp({pkt3(0x33, 3), (1u << 16) | 0x810, IXB, 4});
    // the line reference: Bresenham along the edges, the last pixel out
    auto ref_line = [&](int x0, int y0, int x1, int y1) {
      const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
      const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
      int err = dx + dy;
      const int steps = std::max(dx, -dy);
      for (int i = 0; i < steps; i++) {
        ref[size_t(y0 * W + x0)] = 0xffffffffu;
        const int e2 = 2 * err;
        if (e2 >= dy) {
          err += dy;
          x0 += sx;
        }
        if (e2 <= dx) {
          err += dx;
          y0 += sy;
        }
      }
    };
    // OpenGL pixel centres: a vertex at 60.5 is in pixel 60
    ref_line(60, 30, 100, 30);
    ref_line(100, 30, 100, 50);
    ref_line(100, 50, 60, 30);
    int mx;
    const int d = compare("prims-r200-packets", ref, 0, &mx);
    scene_report("TRI_TYPE_2, 3-vertex lists, INDX_BUFFER (R200 packets)", d,
                 mx);
  }

  // -- 2D: the source scissor; LOAD_PALETTE --------------------------------
  {
    for (int y = 0; y < 256; y++)
      for (int x = 0; x < 256; x++)
        setpx(x, y, (y < 128) ? u32(0x00010101u * u32(x)) : 0u);
    // a 64x64 copy from (40, 40) with the source scissor at x 60, y 70:
    // only the source pixels up to it reach the destination
    wr(0x146c, GMC_BRUSH_NONE | GMC_DST32 | GMC_SRC_COLOR | (0xccu << 16) |
                   GMC_SRC_MEM | GMC_CLR_CMP_DIS | GMC_WRMSK_DIS | 4u);
    wr(0x16f4, (70u << 16) | 60u); // SRC_SC_BOTTOM_RIGHT
    wr(0x16c0, 3);
    wr(0x1434, (40u << 16) | 40u);
    wr(0x1438, (140u << 16) | 140u);
    wr(0x143c, (64u << 16) | 64u);
    int bad = 0;
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
        const bool in = 40 + x <= 60 && 40 + y <= 70;
        bad += px32(140 + x, 140 + y) != (in ? 0x00010101u * u32(40 + x) : 0u);
      }
    report("2D: the source scissor (GMC_SRC_CLIPPING)", bad == 0,
           bad ? std::to_string(bad) + " pixels" : "");
    std::vector<u32> pal = {pkt3(0x2c, 17), 1};
    for (u32 i = 0; i < 16; i++)
      pal.push_back(0x00102030u * i);
    cp(pal);
    sync();
    report("CP: LOAD_PALETTE (the scaler's 16 entries kept)",
           m_3d->m_palette[15] == 0x00102030u * 15 && m_3d->m_palette[16] == 0,
           "");
  }

  // ----------------------------------------------------------------------
  // put the card back
  sync();
  report("FIFO: every wait for idle ended", idle_timeouts == 0,
         idle_timeouts ? std::to_string(idle_timeouts) + " timed out" : "");
  engine_drain();
  memcpy(m_me_ram, saved_me, sizeof(saved_me));
  memcpy(m_me_written, saved_me_written, sizeof(saved_me_written));
  m_me_loaded = saved_me_loaded;
  memcpy(m_regs, saved_regs.data(), saved_regs.size() * 4);
  memcpy(m_pll, saved_pll.data(), saved_pll.size() * 4);
  memcpy(vga.memory, saved_vram.data(), saved_vram.size());
  eng = saved_eng;
  m_3d->reset();
  post_restore(); // the engine, the CP's parser, the PLL's dividers
  state.vga_mem_updated = 1;

  printf("%%RADEON-I-SELFTEST: %d checks, %d failed: %s (frames in %s)\n",
         checks, failures, failures ? "FAIL" : "PASS", dir.c_str());
  return failures == 0;
}
