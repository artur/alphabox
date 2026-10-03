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
 * Here: the checks every generation shares (the FIFO, the CP, the GART
 * and the clocks in RadeonSelfTestQueue.cpp; the 2D engine, the CP's 2D
 * packets, the cursor), and the helpers the generations' 3D scenes use
 * (RadeonSelfTest.hpp). The 3D scenes are the generation's
 * (CRadeonEngine3D::selftest_scenes: r100/RadeonR100SelfTest.cpp), run
 * between the 2D checks and the last common ones.
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

#include "RadeonSelfTest.hpp"
#include "Radeon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>

using namespace radeon;
using namespace radeon::selftest;

namespace radeon {
namespace selftest {

namespace {

// --- a minimal PNG writer (stored deflate blocks) ----------------------------
u32 crc_table[256];
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
} // namespace

void crc_init() {
  for (u32 n = 0; n < 256; n++) {
    u32 c = n;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
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

namespace {
u32 rng_state = 12345;
} // namespace
u32 rnd() {
  rng_state = rng_state * 1103515245u + 12345u;
  return (rng_state >> 8) ^ (rng_state << 13);
}

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

} // namespace selftest
} // namespace radeon

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
  // The microcode lookups (RadeonMicrocode.cpp); the queue's image is a
  // pattern, unknown and accepted.
  selftest_microcode(*m_chip, report);
  report("microcode: the self-test's pattern image, unknown, runs the CP",
         cp_microcode_ok() && !m_ucode.known && !m_ucode.wrong_width, "");

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
  // the generation's 3D engine (r100/RadeonR100SelfTest.cpp)
  // ======================================================================
  {
    SelfTest t;
    t.report = report;
    t.wr = wr;
    t.sync = sync;
    t.vrd = vrd;
    t.vwr = vwr;
    t.vr32 = vr32;
    t.vw32 = vw32;
    t.cp = cp;
    t.dir = dir;
    m_3d->selftest_scenes(t);
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
           m_scaler_palette[15] == 0x00102030u * 15 &&
               m_scaler_palette[16] == 0,
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
