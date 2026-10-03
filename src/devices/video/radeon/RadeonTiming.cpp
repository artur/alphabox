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
 * The Radeon's clocks: the PLL registers, the pixel clock, the CRTC's
 * timing that follows from it, and the memory controller's power-up
 * status.
 *
 * The PPLL (Linux radeonfb radeon_write_pll_regs, radeon_calc_pll_regs):
 * the pixel clock is ref * FB_DIV / (REF_DIV * POST_DIV), with REF_DIV
 * PPLL_REF_DIV<9:0>, FB_DIV PPLL_DIV_n<10:0> and the post divider code
 * PPLL_DIV_n<18:16> (0..7: 1, 2, 4, 8, 3, 16, 6, 12); the divider set n is
 * CLOCK_CNTL_INDEX PPLL_DIV_SEL <9:8>; the CRTC runs on the PPLL when
 * VCLK_ECP_CNTL VCLK_SRC_SEL <1:0> is 3 (PPLLCLK), on the reference (0,
 * CPUCLK) otherwise; the reference is the card's BIOS's (Radeon.cpp
 * read_bios_clocks). With PPLL_CNTL ATOMIC_UPDATE_EN <16> (or
 * VGA_ATOMIC_UPDATE_EN <17>) new dividers reach the PLL only through
 * the atomic update: writing PPLL_REF_DIV<15> starts it, and the bit reads
 * 1 until the PLL has taken them. radeonfb waits for the bit and notes
 * that most chips "pass at the very first test": the update completes
 * here a microsecond after it is started [inference]. A PLL held in reset
 * or asleep (PPLL_CNTL <0>, <1>) gives no clock: the CRTC runs on the
 * reference [inference].
 * In a VGA mode the clock is the PPLL divider set the VGA's clock select
 * (miscellaneous output <3:2>) names [inference: the BIOS programs
 * dividers 0..2 for the VGA clocks and the chip has a VGA atomic
 * update], 25.175 or 28.322 MHz while those dividers are unset.
 *
 * The CRTC (radeon_reg.h, radeonfb): CRTC_H_TOTAL_DISP <9:0> is the total
 * in characters less one (eight pixels each), CRTC_V_TOTAL_DISP <11:0>
 * the total lines less one and <27:16> the displayed ones less one; a
 * VGA mode takes the VGA CRTC's totals. A frame lasts total pixels /
 * pixel clock; the line, the vertical blank (CRTC_STATUS, the
 * VBLANK interrupt), the VLINE interrupt and CRTC_CRNT_FRAME follow from
 * the card's clock, and stay continuous when the mode changes. A frame
 * rate outside 10..400 Hz (a PLL not programmed yet) is taken as 60 Hz.
 *
 * PLL_TEST_CNTL <31:24>: the BIOS clears the byte and polls until it
 * reaches 27 (0x7154) or 135 (0x715d), its microsecond delays: a counter
 * of reference clocks, 27 a microsecond at 27 MHz [inference from the
 * BIOS's counts]. It counts from what was written and stops at 255
 * [inference: a wrapping counter could make a slow poller wait
 * forever, and nothing says which it does].
 *
 * MC_STATUS MEM_PWRUP_COMPL_A/B <1:0> (video_radeon.h): the BIOS writes
 * MEM_SDRAM_MODE_REG and polls for both bits (0x6c94); they clear on
 * the write and set again two microseconds later [inference: an SDRAM
 * mode-register set and the refreshes after it are a few memory clocks].
 **/

#include "Radeon.hpp"

#include <algorithm>

using namespace radeon;

namespace {
constexpr u32 PPLL_ATOMIC_UPDATE_EN = 1u << 16;
constexpr u32 PPLL_VGA_ATOMIC_UPDATE_EN = 1u << 17;
constexpr long long kPllUpdateNs = 1000;
constexpr long long kPwrupNs = 2000;
const int kPostDiv[8] = {1, 2, 4, 8, 3, 16, 6, 12};
} // namespace

/// Call with m_clk_mx held.
void CRadeon::pll_settle() const {
  if (m_ppll_update && radeon_clock_ns() >= m_ppll_update_done_ns) {
    m_ppll_update = false;
    m_ppll_eff_ref = m_pll[PLL_PPLL_REF_DIV] & 0x3ff;
    for (int i = 0; i < 4; i++)
      m_ppll_eff_div[i] = m_pll[PLL_PPLL_DIV_0 + i];
  }
}

u32 CRadeon::pll_read(u8 index) {
  index &= PLL_INDEX_MASK;
  u32 v = m_pll[index];
  {
    std::lock_guard<std::mutex> l(m_clk_mx);
    pll_settle();
    if (index == PLL_PPLL_REF_DIV)
      v = (v & ~PPLL_ATOMIC_UPDATE) | (m_ppll_update ? PPLL_ATOMIC_UPDATE : 0);
  }
  if (index == PLL_P2PLL_REF_DIV)
    v &= ~PPLL_ATOMIC_UPDATE; // the second PLL has no CRTC here
  if (index == PLL_TEST_CNTL) {
    const long long ticks =
        (radeon_clock_ns() - m_pll_test_t0_ns) * m_ref_khz / 1000000;
    const u32 count = u32(std::min<long long>(255, m_pll_test_start + ticks));
    v = (v & 0x00ffffffu) | (count << 24);
  }
  if (m_trace && m_trace_budget > 0) {
    m_trace_budget--;
    printf("%s: pll   read  %02x -> %08x\n", devid_string, index, v);
  }
  return v;
}

void CRadeon::pll_write(u8 index, u32 data, u32 byte_mask) {
  index &= PLL_INDEX_MASK;
  if (m_trace && m_trace_budget > 0) {
    m_trace_budget--;
    printf("%s: pll   write %02x <- %08x\n", devid_string, index, data);
  }
  if (index == PLL_TEST_CNTL && (byte_mask & 0xff000000u)) {
    m_pll_test_t0_ns = radeon_clock_ns();
    m_pll_test_start = data >> 24;
  }
  std::lock_guard<std::mutex> l(m_clk_mx);
  pll_settle();
  const bool atomic = (m_pll[PLL_PPLL_CNTL] & (PPLL_ATOMIC_UPDATE_EN |
                                               PPLL_VGA_ATOMIC_UPDATE_EN)) != 0;
  if (index == PLL_PPLL_REF_DIV) {
    m_pll[index] = data & ~PPLL_ATOMIC_UPDATE;
    if (data & PPLL_ATOMIC_UPDATE) {
      m_ppll_update = true;
      m_ppll_update_done_ns = radeon_clock_ns() + kPllUpdateNs;
    } else if (!atomic) {
      m_ppll_eff_ref = data & 0x3ff;
    }
  } else {
    m_pll[index] = data;
    if (index >= PLL_PPLL_DIV_0 && index <= PLL_PPLL_DIV_0 + 3 && !atomic)
      m_ppll_eff_div[index - PLL_PPLL_DIV_0] = data;
  }
  state.vga_mem_updated = 1;
}

u32 CRadeon::pixel_clock_khz() const {
  std::lock_guard<std::mutex> l(m_clk_mx);
  pll_settle();
  int sel;
  if (native_crtc_active()) {
    if ((m_pll[PLL_VCLK_ECP_CNTL] & 3) != 3)
      return m_ref_khz;
    sel = int((R(CLOCK_CNTL_INDEX) >> 8) & 3);
  } else {
    sel = (vga.miscellaneous_output >> 2) & 3;
  }
  const u32 div = m_ppll_eff_div[sel], ref = m_ppll_eff_ref;
  const u32 fb = div & 0x7ff;
  if ((m_pll[PLL_PPLL_CNTL] & (PPLL_RESET | PPLL_SLEEP)) || !ref || !fb) {
    if (!native_crtc_active())
      return sel == 1 ? 28322 : 25175;
    return m_ref_khz;
  }
  return u32(u64(m_ref_khz) * fb / (u64(ref) * kPostDiv[(div >> 16) & 7]));
}

void CRadeon::crtc_frame_params(long long *frame_ns, u32 *total,
                                u32 *disp) const {
  u32 htotal, vtotal, vdisp;
  if (native_crtc_active()) {
    htotal = ((R(CRTC_H_TOTAL_DISP) & 0x3ff) + 1) * 8;
    vtotal = (R(CRTC_V_TOTAL_DISP) & 0xfff) + 1;
    vdisp = ((R(CRTC_V_TOTAL_DISP) >> 16) & 0xfff) + 1;
  } else {
    const u32 cw = (vga.sequencer.data[1] & 1) ? 8 : 9;
    htotal = (u32(vga.crtc.horz_total) + 5) * cw;
    vtotal = u32(vga.crtc.vert_total) + 2;
    vdisp = u32(vga.crtc.vert_disp_end) + 1;
  }
  if (vtotal < vdisp + 1)
    vtotal = vdisp + 1;
  const u32 pclk = pixel_clock_khz();
  long long ns =
      pclk ? (long long)(u64(htotal) * vtotal * 1000000ull / pclk) : 0;
  if (ns < 2500000 || ns > 100000000) // outside 10..400 Hz
    ns = 1000000000ll / 60;
  *frame_ns = ns;
  *total = vtotal;
  *disp = vdisp;
}

u32 CRadeon::current_vline(bool *in_vblank, long long *frame) const {
  long long fns;
  u32 total, disp;
  crtc_frame_params(&fns, &total, &disp);
  const long long now = radeon_clock_ns();
  long long into, fr;
  {
    std::lock_guard<std::mutex> l(m_clk_mx);
    if (fns != m_crtc_frame_ns) {
      // a new mode or clock: the frames so far kept, the phase continues
      if (m_crtc_frame_ns > 0)
        m_crtc_anchor_frame += (now - m_crtc_anchor_ns) / m_crtc_frame_ns;
      m_crtc_anchor_ns = now;
      m_crtc_frame_ns = fns;
    }
    const long long t = now - m_crtc_anchor_ns;
    fr = m_crtc_anchor_frame + t / fns;
    into = t % fns;
  }
  const u32 line = u32((into * total) / fns);
  if (in_vblank)
    *in_vblank = line >= disp;
  if (frame)
    *frame = fr;
  return line;
}

u32 CRadeon::mc_pwrup_bits() const {
  return radeon_clock_ns() >= m_mc_pwrup_ns ? 3u : 0u;
}
