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
 * The self-test's checks of the command FIFO, the engine's busy time, the
 * CP's streams and micro-engine, and the PCI GART (RadeonQueue.cpp,
 * RadeonCP.cpp, RadeonGart.cpp), driven through the MMIO BAR as a driver
 * would. Called first by selftest(), with the framebuffer at 0 in the
 * memory controller's space; it leaves the CP with microcode loaded.
 **/

#include "Radeon.hpp"

#include <atomic>
#include <cstring>

using namespace radeon;

namespace {
constexpr u32 CP_RB_BASE = 0x0700, CP_RB_CNTL = 0x0704,
              CP_RB_RPTR_ADDR = 0x070c, CP_RB_RPTR = 0x0710,
              CP_RB_WPTR = 0x0714, CP_RB_RPTR_WR = 0x071c,
              SCRATCH_UMSK = 0x0770, SCRATCH_ADDR = 0x0774,
              CP_ME_RAM_ADDR = 0x07d4, CP_ME_RAM_RADDR = 0x07d8,
              CP_ME_RAM_DATAH = 0x07dc, CP_ME_RAM_DATAL = 0x07e0,
              DP_GUI_MASTER_CNTL = 0x146c, DP_BRUSH_FRGD_CLR = 0x147c,
              DST_Y_X = 0x1438, DST_HEIGHT_WIDTH = 0x143c,
              SCRATCH_REG0 = 0x15e0, WAIT_UNTIL = 0x1720,
              DEFAULT_PITCH_OFFSET = 0x16e0, DEFAULT_SC_BOTTOM_RIGHT = 0x16e8,
              RB2D_DSTCACHE_CTLSTAT = 0x342c, AIC_CNTL = 0x01d0,
              AIC_PT_BASE = 0x01d8, AIC_LO_ADDR = 0x01dc, AIC_HI_ADDR = 0x01e0;
constexpr u32 S = 0x100000; // a 256x256 surface at 32 bpp, pitch 1024
constexpr u32 GMC_FILL = (13u << 4) | (6u << 8) | (3u << 12) | (0xf0u << 16) |
                         (1u << 28) | (1u << 30);
u32 pkt0(u32 reg, u32 count) { return ((count - 1) << 16) | (reg >> 2); }
u32 pkt3(u32 op, u32 count) {
  return 0xc0000000u | ((count - 1) << 16) | (op << 8);
}
long long elapsed_ns(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}
} // namespace

void CRadeon::selftest_queue(const selftest_report &report) {
  auto wr = [&](u32 r, u32 v) { WriteMem_Bar(0, 2, r, 32, v); };
  auto rd = [&](u32 r) { return ReadMem_Bar(0, 2, r, 32); };
  auto idle = [&]() {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      const u32 s = rd(RBBM_STATUS);
      if ((s & RBBM_FIFOCNT_MASK) == m_chip->cmdfifo_entries &&
          !(s & RBBM_ACTIVE))
        return true;
      if (elapsed_ns(t0) > 10000000000ll)
        return false;
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  };
  auto px = [&](int x, int y) {
    return vram_read(S + u32(y) * 1024 + u32(x) * 4, 4);
  };
  auto clear = [&]() {
    for (u32 i = 0; i < 256 * 256; i++)
      vram_write(S + i * 4, 4, 0);
  };
  wr(DEFAULT_PITCH_OFFSET, (16u << 22) | (S >> 10));
  wr(DEFAULT_SC_BOTTOM_RIGHT, (255u << 16) | 255u);
  idle();
  const u32 ME_PATTERN = 0x9e000000u;
  // a 256-entry image (a pattern, not ATI's microcode)
  auto load_microcode = [&]() {
    wr(CP_ME_RAM_ADDR, 0);
    for (u32 i = 0; i < m_chip->me_ram_entries; i++) {
      wr(CP_ME_RAM_DATAH, i & 0xff);
      wr(CP_ME_RAM_DATAL, ME_PATTERN | i);
    }
  };
  if (m_sync) {
    load_microcode();
    report("FIFO: (ALPHABOX_RADEON_SYNC: the queue checks skipped)", true, "");
    return;
  }

  // -- the FIFO: counted, a full FIFO holds the write, nothing is lost ----
  {
    wr(RBBM_SOFT_RESET, 1u << 5); // E2 held: the entries stay queued
    for (u32 i = 0; i < 10; i++)
      wr(DP_BRUSH_FRGD_CLR, i);
    const u32 s10 = rd(RBBM_STATUS);
    for (u32 i = 10; i < m_chip->cmdfifo_entries; i++)
      wr(DP_BRUSH_FRGD_CLR, i);
    const u32 sfull = rd(RBBM_STATUS);
    std::atomic<bool> done{false};
    std::thread t([&]() {
      wr(DP_BRUSH_FRGD_CLR, 0x1234);
      done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool held = !done;
    wr(RBBM_SOFT_RESET, 0);
    t.join();
    const bool drained = idle();
    const bool ok = (s10 & RBBM_FIFOCNT_MASK) == m_chip->cmdfifo_entries - 10 &&
                    (s10 & RBBM_ACTIVE) && (sfull & RBBM_FIFOCNT_MASK) == 0 &&
                    held && done && drained && rd(DP_BRUSH_FRGD_CLR) == 0x1234;
    char d[96];
    snprintf(d, sizeof(d), "(RBBM_STATUS %08x, full %08x, held %d)", s10, sfull,
             held);
    report("FIFO: free entries, a full FIFO holds the write", ok, ok ? "" : d);
  }

  // -- the engine's busy time: a 256x256 fill at the modelled rate --------
  {
    clear();
    wr(DP_GUI_MASTER_CNTL, GMC_FILL);
    wr(DP_BRUSH_FRGD_CLR, 0x00c0ffee);
    const auto t0 = std::chrono::steady_clock::now();
    wr(DST_Y_X, 0);
    wr(DST_HEIGHT_WIDTH, (256u << 16) | 256u);
    const bool active = (rd(RBBM_STATUS) & RBBM_ACTIVE) != 0;
    const bool drained = idle();
    const long long ns = elapsed_ns(t0);
    const long long want = (long long)(256 * 256 / m_chip->pixels_per_clock) *
                           1000000ll / m_sclk_khz;
    bool pixels = true;
    for (int y = 0; y < 256; y += 51)
      for (int x = 0; x < 256; x += 37)
        pixels = pixels && px(x, y) == 0x00c0ffeeu;
    char d[96];
    snprintf(d, sizeof(d), "(%lld us, at least %lld us; active %d)", ns / 1000,
             want / 1000, active);
    report("FIFO: GUI_ACTIVE for the fill's modelled time",
           active && drained && pixels && ns >= want, d);
  }

  // -- a cache flush is a point in the FIFO: DC_BUSY until it is reached --
  {
    clear();
    wr(DP_GUI_MASTER_CNTL, GMC_FILL);
    wr(DP_BRUSH_FRGD_CLR, 0x00abcdef);
    wr(DST_Y_X, 0);
    wr(DST_HEIGHT_WIDTH, (256u << 16) | 256u);
    wr(RB2D_DSTCACHE_CTLSTAT, 0xf);
    const bool busy = (rd(RB2D_DSTCACHE_CTLSTAT) & (1u << 31)) != 0;
    const auto t0 = std::chrono::steady_clock::now();
    while ((rd(RB2D_DSTCACHE_CTLSTAT) & (1u << 31)) &&
           elapsed_ns(t0) < 5000000000ll)
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    // once the flush is done, so is the fill before it
    const bool filled = px(255, 255) == 0x00abcdefu;
    // a WAIT_UNTIL for the CRTC's vertical line: the engine waits for the
    // next blank, no longer than two frames
    const auto t1 = std::chrono::steady_clock::now();
    wr(WAIT_UNTIL, 1u << 3);
    wr(DP_BRUSH_FRGD_CLR, 0x77);
    const bool drained = idle();
    const long long wait_ns = elapsed_ns(t1);
    char d[96];
    snprintf(d, sizeof(d), "(busy %d, filled %d, VLINE wait %lld us)", busy,
             filled, wait_ns / 1000);
    report("FIFO: DC_BUSY until the flush, WAIT_UNTIL VLINE",
           busy && filled && drained && wait_ns < 100000000ll, d);
  }

  // -- GEN_INT_STATUS GUI_IDLE latches when the engine goes idle ----------
  {
    wr(GEN_INT_STATUS, INT_GUI_IDLE);
    const bool cleared = !(rd(GEN_INT_STATUS) & INT_GUI_IDLE);
    wr(DP_GUI_MASTER_CNTL, GMC_FILL);
    wr(DST_Y_X, 0);
    wr(DST_HEIGHT_WIDTH, (64u << 16) | 64u);
    const bool drained = idle();
    const bool latched = (rd(GEN_INT_STATUS) & INT_GUI_IDLE) != 0;
    wr(GEN_INT_STATUS, INT_GUI_IDLE);
    report("FIFO: GUI_IDLE latches when the engine goes idle",
           cleared && drained && latched, "");
  }

  // -- the CP: nothing without microcode; then the ring, asynchronously ---
  const u32 RING = 0x900000, WB = 0x9f0000;
  memset(m_me_written, 0, sizeof(m_me_written));
  m_me_loaded = false;
  {
    clear();
    wr(0x0740, 0);
    wr(CP_RB_BASE, RING);
    wr(CP_RB_CNTL, 12 | (2u << 8) | (1u << 31)); // 2^13 dwords, blk 8
    wr(CP_RB_RPTR_ADDR, WB);
    wr(CP_RB_RPTR_WR, 0);
    wr(CP_RB_WPTR, 0);
    wr(CP_RB_CNTL, 12 | (2u << 8)); // RPTR_WR_ENA off again
    wr(0x0740, 4u << 28);           // primary and indirect bus mastering
    vram_write(WB, 4, 0xdeadbeef);
    // PAINT: the settings (a solid brush, its colour), one rectangle
    const u32 paint[] = {pkt3(0x91, 4), GMC_FILL, 0x00123456u, 0,
                         (8u << 16) | 8u};
    for (u32 i = 0; i < 5; i++)
      vram_write(RING + i * 4, 4, paint[i]);
    wr(CP_RB_WPTR, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const u32 st = rd(RBBM_STATUS);
    const bool refused = rd(CP_RB_RPTR) == 0 && px(0, 0) == 0 &&
                         (st & (1u << 16)) && (st & RBBM_ACTIVE);
    load_microcode();
    const bool drained = idle();
    const bool ran = rd(CP_RB_RPTR) == 5 && px(7, 7) == 0x00123456u &&
                     px(8, 8) == 0 && vram_read(WB, 4) == 5;
    wr(CP_ME_RAM_RADDR, 5);
    const u32 h = rd(CP_ME_RAM_DATAH), l = rd(CP_ME_RAM_DATAL);
    const bool readback = h == 5 && l == (ME_PATTERN | 5);
    char d[128];
    snprintf(d, sizeof(d), "(refused %d, ran %d, read-back %d: %02x %08x)",
             refused, ran, readback, h, l);
    report("CP: no packets before the microcode, then the ring",
           refused && drained && ran && readback, d);
  }

  // -- the ring is read while the CPU goes on; the fence lands last ------
  {
    clear();
    wr(SCRATCH_ADDR, WB + 0x100);
    wr(SCRATCH_UMSK, 1);
    vram_write(WB + 0x100, 4, 0);
    u32 pos = 5;
    auto put = [&](u32 v) { vram_write(RING + (pos++ & 8191) * 4, 4, v); };
    for (int n = 0; n < 64; n++) {
      put(pkt3(0x91, 4));
      put(GMC_FILL);
      put(0x00010101u * u32(n));
      put(0);
      put((256u << 16) | 256u);
    }
    put(pkt0(SCRATCH_REG0, 1));
    put(0xcafe);
    const auto t0 = std::chrono::steady_clock::now();
    wr(CP_RB_WPTR, pos);
    const long long write_ns = elapsed_ns(t0);
    const u32 r0 = rd(CP_RB_RPTR), fence0 = vram_read(WB + 0x100, 4);
    const bool drained = idle();
    const bool ok = r0 != pos && fence0 != 0xcafe && drained &&
                    rd(CP_RB_RPTR) == pos && vram_read(WB, 4) == pos &&
                    vram_read(WB + 0x100, 4) == 0xcafe &&
                    px(200, 200) == 0x003f3f3fu;
    char d[128];
    snprintf(d, sizeof(d),
             "(WPTR write %lld us, read pointer then %u of %u, fence %x)",
             write_ns / 1000, r0, pos, fence0);
    report("CP: the ring drains after WPTR returns, fence last", ok, d);
    wr(SCRATCH_UMSK, 0);

    // RB_RPTR_WR_ENA: the read pointer is loaded when WPTR is written
    wr(0x0740, 0);
    wr(CP_RB_CNTL, 12 | (2u << 8) | (1u << 31));
    wr(CP_RB_RPTR_WR, 0x40);
    wr(CP_RB_WPTR, 0x40);
    const bool moved = rd(CP_RB_RPTR) == 0x40;
    wr(CP_RB_CNTL, 12 | (2u << 8));
    wr(CP_RB_RPTR_WR, 0);
    wr(CP_RB_WPTR, 0x40);
    report("CP: CP_RB_RPTR_WR takes effect on the WPTR write",
           moved && rd(CP_RB_RPTR) == 0x40, "");
  }

  // -- the primary PIO queue ----------------------------------------------
  {
    clear();
    wr(0x0740, 1u << 28); // primary PIO, indirect disabled
    const u32 room = rd(0x0740) & 0xff;
    const u32 paint[] = {pkt3(0x91, 4), GMC_FILL, 0x00654321u, (4u << 16) | 4u,
                         (12u << 16) | 12u};
    for (u32 i = 0; i < 5; i++)
      wr(0x1000 + 4 * i, paint[i]);
    const bool drained = idle();
    const bool ok = room == m_chip->csq_primary_dwords && drained &&
                    px(4, 4) == 0x00654321u && px(11, 11) == 0x00654321u &&
                    px(12, 12) == 0 && (rd(0x0740) & 0xff) == room;
    report("CP: the primary PIO queue (CP_CSQ_APER_PRIMARY)", ok, "");
    wr(0x0740, 0);
  }

  // -- the PCI GART: AIC_LO..HI through the page table at AIC_PT_BASE -----
  {
    const u32 PT = 0xf00000; // in the framebuffer
    for (u32 i = 0; i < 1024; i++)
      vram_write(PT + 4 * i, 4, 0);
    vram_write(PT + 0, 4, 0x12345000u);
    vram_write(PT + 12, 4, 0x00abc000u);
    wr(AIC_LO_ADDR, 0xe0000000u);
    wr(AIC_HI_ADDR, 0xe03fffffu);
    wr(AIC_PT_BASE, PT);
    bool vram = true;
    u32 a = 0, b = 0;
    const bool off = !cp_translate(0xe0000010u, &vram, &a);
    wr(AIC_CNTL, 1);
    const bool t1 = cp_translate(0xe0000010u, &vram, &a) && !vram;
    const bool t2 = cp_translate(0xe0003ffcu, &vram, &b) && !vram;
    const bool out = !cp_translate(0xe0400000u, &vram, &a) || vram;
    cp_translate(0xe0000010u, &vram, &a);
    wr(AIC_CNTL, 0);
    char d[96];
    snprintf(d, sizeof(d), "(%08x, %08x)", a, b);
    report("GART: AIC range, page table, off and out of range",
           off && t1 && t2 && out && a == 0x12345010u && b == 0x00abcffcu, d);
  }

  // -- the clocks: the PPLL's atomic update, the CRTC's frame rate --------
  {
    auto pllw = [&](u32 idx, u32 v) {
      wr(CLOCK_CNTL_INDEX, (rd(CLOCK_CNTL_INDEX) & ~0xffu) | idx | PLL_WR_EN);
      wr(CLOCK_CNTL_DATA, v);
    };
    auto pllr = [&](u32 idx) {
      wr(CLOCK_CNTL_INDEX, (rd(CLOCK_CNTL_INDEX) & ~0xffu) | idx);
      return rd(CLOCK_CNTL_DATA);
    };
    // 1280x1024 timing at 108 MHz (VESA: 1688 x 1066), the dividers
    // radeonfb's radeon_calc_pll_regs would pick (ref 27 MHz / 12)
    wr(CRTC_H_TOTAL_DISP, ((1280u / 8 - 1) << 16) | (1688u / 8 - 1));
    wr(CRTC_V_TOTAL_DISP, ((1024u - 1) << 16) | (1066u - 1));
    wr(CRTC_GEN_CNTL, CRTC_EXT_DISP_EN | CRTC_EN | (6u << 8));
    wr(CLOCK_CNTL_INDEX, 3u << 8);                // PPLL_DIV_SEL 3
    pllw(PLL_VCLK_ECP_CNTL, 3);                   // VCLK from the PPLL
    pllw(PLL_PPLL_CNTL, PPLL_RESET | (1u << 16)); // atomic update enabled
    const u32 fb = (108000u * 12 + m_ref_khz / 2) / m_ref_khz;
    pllw(PLL_PPLL_REF_DIV, 12);
    pllw(PLL_PPLL_DIV_0 + 3, fb | (1u << 16)); // post divider 2
    pllw(PLL_PPLL_DIV_0 + 3, fb);              // post divider 1
    pllw(PLL_PPLL_CNTL, 1u << 16);             // out of reset
    const u32 before = pixel_clock_khz();
    pllw(PLL_PPLL_REF_DIV, 12 | PPLL_ATOMIC_UPDATE);
    const bool pending = (pllr(PLL_PPLL_REF_DIV) & PPLL_ATOMIC_UPDATE) != 0;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    const bool settled = !(pllr(PLL_PPLL_REF_DIV) & PPLL_ATOMIC_UPDATE);
    const u32 pclk = pixel_clock_khz();
    const u32 want = u32(u64(m_ref_khz) * fb / 12);
    long long fns;
    u32 total, disp;
    crtc_frame_params(&fns, &total, &disp);
    const long long want_ns = 1688ll * 1066 * 1000000 / want;
    // the frame counter over 200 ms
    const u32 f0 = rd(CRTC_CRNT_FRAME);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const u32 frames = rd(CRTC_CRNT_FRAME) - f0;
    const int expect = int(200000000ll / want_ns);
    char d[160];
    snprintf(d, sizeof(d),
             "(before %u kHz, pending %d, settled %d, %u kHz, frame %lld ns "
             "of %lld, %u frames in 200 ms)",
             before, pending, settled, pclk, fns, want_ns, frames);
    report("PLL: atomic update, pixel clock, CRTC frame rate",
           before != pclk && pending && settled && pclk == want &&
               fns == want_ns && total == 1066 && disp == 1024 &&
               std::abs(int(frames) - expect) <= 2,
           d);

    // the test counter: reference clocks from the write, stopping at 255
    wr(CLOCK_CNTL_INDEX, PLL_TEST_CNTL | PLL_WR_EN);
    WriteMem_Bar(0, 2, CLOCK_CNTL_DATA + 3, 8, 0);
    const u32 c0 = rd(CLOCK_CNTL_DATA) >> 24;
    std::this_thread::sleep_for(std::chrono::microseconds(30));
    const u32 c1 = rd(CLOCK_CNTL_DATA) >> 24;
    // MC_STATUS: power-up complete again after a mode-register write
    wr(MEM_SDRAM_MODE_REG, rd(MEM_SDRAM_MODE_REG));
    const u32 m0 = rd(MC_STATUS) & 3;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    const u32 m1 = rd(MC_STATUS) & 3;
    snprintf(d, sizeof(d), "(counter %u then %u; MC_STATUS %u then %u)", c0, c1,
             m0, m1);
    report("PLL_TEST_CNTL counter, MC_STATUS power-up",
           c0 < 255 && c1 == 255 && m0 == 0 && m1 == 3, d);
    wr(CRTC_GEN_CNTL, 0);
  }
}
