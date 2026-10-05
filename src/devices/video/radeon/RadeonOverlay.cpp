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
 * The Radeon's overlay scaler ("back-end scaler", OV0): a window of the
 * screen filled from a video surface of its own -- packed or planar YUV,
 * or RGB --, scaled, converted to RGB and mixed with the graphics by a
 * colour key or an alpha. It is display hardware, the Rage 128's carried
 * through every Radeon of the family, so it lives in the common code.
 *
 * ATI published no register reference for it. What is here comes from
 * the drivers that program it (lab/docs-radeon/INDEX.md):
 *   - X.org's xf86-video-ati 6.14.6 radeon_video.c (RADEONDisplayVideo,
 *     RADEONSetColorKey, RADEONSetOverlayAlpha, RADEONSetTransform,
 *     RADEONResetVideo) and its radeon_reg.h;
 *   - MPlayer's vidix radeon_vid.c and radeon.h, whose scaler set-up
 *     (Calc_H_INC_STEP_BY, ComputeAccumInit, ComputeBorders, FilterSetup)
 *     is ATI's own sample code with its comments, and whose header names
 *     the fields;
 *   - the Rage 128 Pro register guide for the lock (RRG WAIT_UNTIL
 *     EVENT_OV0_FLIP: "OV0_FLIP will go low at unlock and then high
 *     during VBlank (when the hardware double buffering flips the
 *     registers)") and the Rage 128 controller specification 4.5.4 for
 *     what the scaler is (four-tap filters, formats, keying).
 * Each inference is marked.
 *
 * Registers, the block 0x0400-0x04ff (double buffered) and those beside
 * it:
 *   OV0_Y_X_START, OV0_Y_X_END  the window on the screen, X <12:0> and
 *     Y <28:16>, both ends inclusive [inference: ATI's set-up takes the
 *     scale from the destination width plus one; the drivers write the
 *     start plus the size, and the pixel beyond their box fails the
 *     colour key]. On the parts before the R200 the window's X is eight
 *     pixels to the right of the screen's (X.org's x_off, vidix's
 *     X_ADJUST): the part's row says so (RadeonChip.hpp ov0_x_shift).
 *   OV0_REG_LOAD_CNTL  LOCK <0>, LOCK_READBACK <3>, FLIP_READBACK <4>:
 *     see overlay_sync().
 *   OV0_SCALE_CNTL  SCALER_ENABLE <30>, SOFT_RESET <31>, the surface
 *     format <11:8> (3 RGB1555, 4 RGB565, 6 ARGB8888, 9 planar 4:1:0, 10
 *     planar 4:2:0, 11 "VYUY" = YUY2 in memory, 12 "YVYU" = UYVY in
 *     memory), HORZ_PICK_NEAREST <2>, VERT_PICK_NEAREST <3>, SIGNED_UV
 *     <4>, DOUBLE_BUFFER <24>, LIN_TRANS_BYPASS <28>, CRTC_SEL <14> (the
 *     second CRTC, which this card does not have: nothing is shown).
 *   OV0_H_INC  the horizontal step in source pixels a scaler clock, 2.12:
 *     the first plane's <13:0>, the chroma planes' <29:16>. OV0_STEP_BY
 *     <2:0> and <10:8> double it for each count above one (a count of 0
 *     selects four-tap vertical filtering); <4> and <12> (X.org's
 *     "predownscale") double it once more [inference]. The scaler's
 *     clock is the pixel clock divided by VCLK_ECP_CNTL's ECP_DIV (PLL
 *     register 8 <9:8>), which X.org raises above 175 MHz and makes up
 *     for in H_INC: the step a screen pixel is the step a clock shifted
 *     down by it.
 *   OV0_P1_H_ACCUM_INIT, OV0_P23_H_ACCUM_INIT  where the first output
 *     pixel samples: a 4.5 number, the integer in <31:28>, the fraction
 *     in <19:15>. ATI's ComputeAccumInit: "2.5 puts the kernel 50% of the
 *     way between the source pixel that is off screen and the first
 *     on-screen source pixel", to which it adds half a step ("the
 *     distance to the center of the first destination pixel"), half a
 *     pixel more with PICK_NEAREST and the start pixel's place in its
 *     fetch group. So the kernel sits at the value less 3, counted in
 *     source pixels from the first pixel of the group OV0_Pn_X_START is
 *     in (groups of four pixels for the first plane, of two for the
 *     chroma planes [inference: ATI's P1GroupSize and P23GroupSize as its
 *     sample code has them]).
 *   OV0_V_INC  the vertical step, 20 fractional bits. OV0_P1_V_ACCUM_INIT,
 *     OV0_P23_V_ACCUM_INIT: 6.5 in <25:15>; ATI: 1.5 plus half a step
 *     (plus a line for four taps, plus half a line with PICK_NEAREST), so
 *     the kernel sits at the value less 2 (less 3 with four taps). The
 *     chroma planes of a planar surface step at half (4:2:0) or a quarter
 *     (4:1:0) of V_INC; a packed surface's chroma follows its lines.
 *     X.org loads 1.5 without the half step: its picture is half a line
 *     low, as programmed.
 *   OV0_P1_X_START_END, _P2_, _P3_  the first and last source pixel of a
 *     line, <26:16> and <10:0>; OV0_P1_BLANK_LINES_AT_TOP,
 *     OV0_P23_BLANK_LINES_AT_TOP: the surface's lines less one in <27:16>
 *     (<26:16>). A tap beyond them takes the edge's pixel [inference].
 *     The blank lines at the top (<11:0>, less one) are not modelled.
 *   OV0_BASE_ADDR + OV0_VID_BUFn_BASE_ADRS <27:4>  the planes, in the
 *     memory controller's space; <0> picks OV0_VID_BUF_PITCH1_VALUE over
 *     PITCH0 (bytes a line). Buffer 0 is Y or the packed surface, 1 is U
 *     (Cb) and 2 is V (Cr), as X.org fills them; OV0_TEST <5> swaps the
 *     two. Buffers 3-5 are the other field's for the deinterlacer, which
 *     is not modelled (OV0_AUTO_FLIP_CNTL, OV0_DEINTERLACE_PATTERN): the
 *     picture is buffers 0-2 [inference].
 *   OV0_FILTER_CNTL, OV0_FOUR_TAP_COEF_0..4  see filter_weights().
 *   OV0_LIN_TRANS_A..F (0x0d20..)  the YUV to RGB transform: see
 *     to_rgb().
 *   OV0_GRAPHICS_KEY_CLR_LOW/HIGH, OV0_VIDEO_KEY_CLR_LOW/HIGH,
 *     OV0_KEY_CNTL, DISP_MERGE_CNTL  see draw_overlay().
 * Not modelled: the gamma curves (OV0_GAMMA_*, SCALE_CNTL's GAMMA_SEL:
 * the drivers' tables do not give the segments' arithmetic; gamma 1.0 is
 * assumed), OV0_EXCLUSIVE_HORZ/VERT, the deinterlacer, the capture port
 * (CAP0_*, FCP_CNTL), the subpicture, blending several source lines when
 * shrinking (V_ACCUM_INIT's MAX_LN_IN_PER_LN_OUT <1:0>: the two- or
 * four-tap filter alone is applied, so lines are dropped).
 **/

#include "RadeonOverlay.hpp"
#include "Radeon.hpp"

#include <algorithm>
#include <cstring>

using namespace radeon;

namespace {
inline int sext(u32 v, int bits) {
  const u32 m = 1u << (bits - 1);
  v &= (1u << bits) - 1;
  return int(v ^ m) - int(m);
}
inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
} // namespace

bool CRadeon::is_overlay_reg(u32 reg) {
  return reg >= OV0_BLOCK_FIRST && reg <= OV0_BLOCK_LAST &&
         reg != OV0_REG_LOAD_CNTL;
}

/// The frame the CRTC is in.
long long CRadeon::overlay_frame() const {
  long long f = 0;
  current_vline(nullptr, &f);
  return f;
}

/**
 * The lock and the double buffering. The scaler works from its own copy
 * of the block (m_ov); what a driver writes reaches it at a vertical
 * blank, and not while OV0_REG_LOAD_CNTL's LOCK is set, so that a set of
 * registers changes together (Rage 128 controller specification 4.5.4:
 * "a register locking mechanism allows autonomous updates of the overlay
 * characteristics ... thanks to sufficient double buffering of scaler
 * control register fields. Double buffering can be disabled"). The Rage
 * 128 Pro guide on the flip: OV0_FLIP "will go low at unlock and then
 * high during VBlank (when the hardware double buffering flips the
 * registers)" -- FLIP_READBACK <4>, which X.org waits for before it
 * fills the next buffer. LOCK_READBACK <3> is the lock as the scaler has
 * it, taken as immediate [inference: the drivers wait for it after
 * setting the lock]. With OV0_SCALE_CNTL's DOUBLE_BUFFER <24> clear a
 * write reaches the scaler at once [inference].
 *
 * The blank is the next frame of the CRTC's clock (RadeonTiming.cpp);
 * nothing runs at it, so whoever looks -- a read of the register, the
 * renderer, the refresh gate -- brings the copy up to date first.
 **/
void CRadeon::overlay_sync() const {
  std::lock_guard<std::mutex> l(m_ov_mx);
  if (!m_ov_dirty)
    return;
  const bool locked = (R(OV0_REG_LOAD_CNTL) & OV0_LOCK) != 0;
  const bool buffered = (R(OV0_SCALE_CNTL) & OV0_DOUBLE_BUFFER) != 0;
  if (buffered && (locked || overlay_frame() < m_ov_latch_frame))
    return;
  memcpy(m_ov, &m_regs[OV0_BLOCK_FIRST >> 2], sizeof(m_ov));
  m_ov_dirty = false;
  m_ov_flip = true;
}

void CRadeon::overlay_write(u32 reg, u32 data, u32 old) {
  {
    std::lock_guard<std::mutex> l(m_ov_mx);
    if (reg == OV0_REG_LOAD_CNTL) {
      R(reg) = data & 7;
      if ((old & OV0_LOCK) && !(data & OV0_LOCK)) {
        // unlocked: the flip is due at the next blank
        m_ov_flip = false;
        m_ov_dirty = true;
        m_ov_latch_frame = overlay_frame() + 1;
      }
    } else {
      R(reg) = data;
      if (!m_ov_dirty)
        m_ov_latch_frame = overlay_frame() + 1;
      m_ov_dirty = true;
    }
  }
  overlay_sync();
  state.vga_mem_updated = 1;
}

u32 CRadeon::overlay_load_cntl_read() const {
  overlay_sync();
  std::lock_guard<std::mutex> l(m_ov_mx);
  const u32 v = R(OV0_REG_LOAD_CNTL) & 7;
  return v | ((v & OV0_LOCK) ? OV0_LOCK_READBACK : 0) |
         (m_ov_flip ? OV0_FLIP_READBACK : 0);
}

/// The scaler's registers are the driver's (after a reset, a restored
/// state file).
void CRadeon::overlay_latch() {
  std::lock_guard<std::mutex> l(m_ov_mx);
  memcpy(m_ov, &m_regs[OV0_BLOCK_FIRST >> 2], sizeof(m_ov));
  m_ov_dirty = false;
  m_ov_flip = true;
}

/// The transform's power-on values: BT.601, as X.org's RADEONResetVideo
/// loads them on every part after the first Radeon [inference: what the
/// chip itself resets to is not documented].
void CRadeon::overlay_reset() {
  R(OV0_LIN_TRANS_A) = 0x12a20000;
  R(OV0_LIN_TRANS_B) = 0x198a190e;
  R(OV0_LIN_TRANS_C) = 0x12a2f9da;
  R(OV0_LIN_TRANS_D) = 0xf2fe0442;
  R(OV0_LIN_TRANS_E) = 0x12a22046;
  R(OV0_LIN_TRANS_F) = 0x0000175f;
  overlay_latch();
}

namespace radeon {

/**
 * The filter at a phase: four weights in 32nds for the taps at pixels
 * i - 1, i, i + 1 and i + 2, `frac` being the position's fraction past
 * pixel i in 32nds (the accumulators' five bits).
 *
 * The scaler's filter has four taps and eight phases, from five rows of
 * coefficients (OV0_FOUR_TAP_COEF_0..4: tap 0 <3:0> and tap 3 <27:24>
 * signed four bits, tap 1 <14:8> and tap 2 <22:16> signed seven, each row
 * summing to 32; "0th_tap means that the left most or top most pixel in a
 * set of four will be multiplied by this coefficient", vidix radeon.h).
 * Row n is the phase n eighths; the phases past a half are the rows
 * before it mirrored [inference: the drivers' tables are the halves of
 * symmetric filters -- row 0 is 0 32 0 0 at 1:1, row 4 is symmetric; the
 * header's names, PHASE_1_5 and so on, do not say how a row serves two
 * phases]. OV0_FILTER_CNTL's bits choose the chip's own coefficients over
 * the programmed ones, for horizontal Y <0>, horizontal UV <1>, vertical
 * Y <2> and vertical UV <3>; those are not published and are taken as
 * the two-tap linear filter [inference].
 **/
void overlay_filter_weights(OvFilter kind, const u32 coef[5], unsigned frac,
                            int w[4]) {
  switch (kind) {
  case OvFilter::Nearest:
    w[0] = w[2] = w[3] = 0;
    w[1] = 32;
    return;
  case OvFilter::Linear:
    w[0] = w[3] = 0;
    w[1] = 32 - int(frac);
    w[2] = int(frac);
    return;
  case OvFilter::FourTap: {
    const unsigned phase = frac >> 2;
    const u32 r = coef[phase <= 4 ? phase : 8 - phase];
    const int t[4] = {sext(r, 4), sext(r >> 8, 7), sext(r >> 16, 7),
                      sext(r >> 24, 4)};
    for (int k = 0; k < 4; k++)
      w[k] = phase <= 4 ? t[k] : t[3 - k];
    return;
  }
  }
}

/**
 * YUV to RGB, the linear transform of OV0_LIN_TRANS_A..F as X.org's
 * RADEONSetTransform builds it: with Y, Cb and Cr as ten-bit numbers,
 *   R = Luma Y + RCb Cb + RCr Cr + ROff   (and so for G and B)
 * the coefficients signed with eleven fractional bits -- Luma in <31:17>
 * of A, C and E, the Cb coefficients in their <15:1>, the Cr ones in
 * <31:17> of B, D and F -- and the offsets signed halves in <12:0> of B,
 * D and F. (X.org shifts twelve-bit values by 20 and 4 on the parts after
 * the first Radeon and fifteen-bit ones by 17 and 1 on that one: the
 * same scale; the defaults it loads use the finer bits on both.) The
 * result is ten bits; the eight shown are rounded [inference: the
 * rounding].
 **/
void overlay_to_rgb(const u32 lin[6], int y, int cb, int cr, int rgb[3]) {
  for (int k = 0; k < 3; k++) {
    const u32 a = lin[2 * k], b = lin[2 * k + 1];
    const int luma = sext(a >> 17, 15), ccb = sext(a >> 1, 15),
              ccr = sext(b >> 17, 15), off = sext(b, 13);
    // ten-bit result times 4096: the inputs times four, the coefficients
    // times 2048, the offset in halves
    const long long v =
        2ll * 4 * (luma * y + ccb * cb + ccr * cr) + 2048ll * off;
    rgb[k] = clamp255(int((v + 8192) >> 14));
  }
}

} // namespace radeon

namespace {
/// One plane of the surface, and how it is sampled.
struct plane_t {
  u32 base;             ///< VRAM offset of its first line
  u32 pitch;            ///< bytes a line
  int xmin, xmax, ymax; ///< the pixels and lines it has
  long long x0, xstep;  ///< position of output pixel 0, 20 bits
  long long y0, ystep;  ///< position of output line 0, 20 bits
  radeon::OvFilter hf, vf;
};
} // namespace

bool CRadeon::overlay_active() const {
  const u32 sc = m_ov[(OV0_SCALE_CNTL - OV0_BLOCK_FIRST) >> 2];
  return (sc & OV0_SCALER_ENABLE) && !(sc & OV0_SCALER_SOFT_RESET) &&
         !(sc & OV0_SCALER_CRTC_SEL);
}

/// What the refresh gate adds for the overlay: its registers and the
/// bytes of the surface it shows.
uint64_t CRadeon::overlay_signature() const {
  overlay_sync();
  std::lock_guard<std::mutex> l(m_ov_mx);
  if (!overlay_active())
    return 0;
  uint64_t h = 0x9e3779b97f4a7c15ull;
  for (u32 v : m_ov)
    h = h * 1000003u ^ v;
  for (u32 r :
       {DISP_MERGE_CNTL, OV0_LIN_TRANS_A, OV0_LIN_TRANS_B, OV0_LIN_TRANS_C,
        OV0_LIN_TRANS_D, OV0_LIN_TRANS_E, OV0_LIN_TRANS_F})
    h = h * 1000003u ^ R(r);
  auto ov = [&](u32 reg) { return m_ov[(reg - OV0_BLOCK_FIRST) >> 2]; };
  const u32 lines = ((ov(OV0_P1_BLANK_LINES_AT_TOP) >> 16) & 0xfff) + 1;
  for (int b = 0; b < 3; b++) {
    const u32 adrs = ov(OV0_VID_BUF0_BASE_ADRS + 4 * u32(b));
    const u32 pitch =
        ov((adrs & 1) ? OV0_VID_BUF_PITCH1_VALUE : OV0_VID_BUF_PITCH0_VALUE) &
        0xffff;
    h = hash_vram(mc_to_vram(ov(OV0_BASE_ADDR) + (adrs & 0x0ffffff0u)),
                  std::min(pitch * lines, 4u << 20), h);
  }
  return h;
}

/**
 * The overlay over the graphics, a scanline at a time. For every pixel
 * of the window the graphics pixel and the video pixel are tested and
 * mixed:
 *   - the graphics key: the graphics pixel, as eight bits a channel (a
 *     16-bit pixel's bits shifted up, an 8-bit pixel's index in all three:
 *     X.org's RADEONSetColorKey builds the key that way), within
 *     OV0_GRAPHICS_KEY_CLR_LOW..HIGH channel by channel; the video key:
 *     the video pixel within OV0_VIDEO_KEY_CLR_LOW..HIGH [inference: the
 *     video key on the converted pixel]. OV0_KEY_CNTL takes of each its
 *     function -- false, true, in the range, outside it (<1:0> video,
 *     <5:4> graphics) -- and CMP_MIX <8> ANDs or ORs the two: X.org's
 *     colour key is graphics-equal OR video-false;
 *   - DISP_MERGE_CNTL's mode <1:0>: 0, the key: the video where the mix
 *     is true, the graphics elsewhere; 2, global: the video times
 *     OV0_ALPHA <31:24> plus the graphics times GRPH_ALPHA <23:16> over
 *     the whole window (X.org's "global mode" sets the key functions
 *     false); 1, per pixel: by the graphics pixel's own alpha (32-bit
 *     modes), the video showing where it is clear [inference: how the two
 *     alpha modes combine is the names' and X.org's use, no more].
 **/
void CRadeon::draw_overlay(bitmap_rgb32 &bitmap) {
  overlay_sync();
  u32 ovr[64];
  {
    std::lock_guard<std::mutex> l(m_ov_mx);
    if (!overlay_active())
      return;
    memcpy(ovr, m_ov, sizeof(ovr));
  }
  auto ov = [&](u32 reg) { return ovr[(reg - OV0_BLOCK_FIRST) >> 2]; };
  const u32 sc = ov(OV0_SCALE_CNTL);
  const u32 fmt = (sc >> 8) & 15;
  const bool rgb_src = fmt == 3 || fmt == 4 || fmt == 6;
  const bool planar = fmt == 9 || fmt == 10;
  const bool packed = fmt == 11 || fmt == 12;
  if (!rgb_src && !planar && !packed)
    return; // a format the drivers never name
  const int cshift_y = fmt == 9 ? 2 : 1;

  // the window
  const int xs = int(m_chip->ov0_x_shift);
  const int wx0 = int(ov(OV0_Y_X_START) & 0x1fff) - xs,
            wy0 = int((ov(OV0_Y_X_START) >> 16) & 0x1fff);
  const int wx1 = int(ov(OV0_Y_X_END) & 0x1fff) - xs,
            wy1 = int((ov(OV0_Y_X_END) >> 16) & 0x1fff);

  // the planes
  const u32 ecp = (m_pll[PLL_VCLK_ECP_CNTL] >> 8) & 3;
  const u32 step_by = ov(OV0_STEP_BY), fc = ov(OV0_FILTER_CNTL);
  const u32 vinc = ov(OV0_V_INC) & 0x0fffffffu;
  const u32 coef[5] = {ov(OV0_FOUR_TAP_COEF_0), ov(OV0_FOUR_TAP_COEF_0 + 4),
                       ov(OV0_FOUR_TAP_COEF_0 + 8),
                       ov(OV0_FOUR_TAP_COEF_0 + 12),
                       ov(OV0_FOUR_TAP_COEF_0 + 16)};
  plane_t pl[3];
  const bool swap_uv = (ov(OV0_TEST) & (1u << 5)) != 0;
  for (int p = 0; p < 3; p++) {
    const bool chroma = p != 0;
    plane_t &q = pl[p];
    // the buffer: a packed surface's chroma is in buffer 0 too
    const int buf = (planar && chroma) ? ((p == 1) != swap_uv ? 1 : 2) : 0;
    const u32 adrs = ov(OV0_VID_BUF0_BASE_ADRS + 4 * u32(buf));
    q.base = mc_to_vram(ov(OV0_BASE_ADDR) + (adrs & 0x0ffffff0u));
    q.pitch =
        ov((adrs & 1) ? OV0_VID_BUF_PITCH1_VALUE : OV0_VID_BUF_PITCH0_VALUE) &
        0xffff;
    // horizontal: the first plane's registers, or the chroma planes'
    const bool p23 = chroma && !rgb_src;
    const u32 hinc = (ov(OV0_H_INC) >> (p23 ? 16 : 0)) & 0x3fff;
    const u32 sb = (step_by >> (p23 ? 8 : 0)) & 7;
    const u32 pre = (step_by >> (p23 ? 12 : 4)) & 1;
    q.xstep = ((long long)(hinc) << 8 << (sb > 1 ? sb - 1 : 0) << pre) >> ecp;
    const u32 hinit = ov(p23 ? OV0_P23_H_ACCUM_INIT : OV0_P1_H_ACCUM_INIT);
    const u32 xse = ov(p23 ? (p == 1 ? OV0_P2_X_START_END : OV0_P3_X_START_END)
                           : OV0_P1_X_START_END);
    q.xmin = int((xse >> 16) & 0x7ff);
    q.xmax = int(xse & 0x7ff);
    const int group = p23 ? 2 : 4;
    q.x0 = ((long long)((hinit >> 28) & 15) << 20) +
           ((long long)((hinit >> 15) & 31) << 15) - (3ll << 20) +
           ((long long)(q.xmin / group * group) << 20);
    // vertical: a planar surface's chroma has its own lines
    const bool own_lines = planar && chroma;
    const bool four_v = sb == 0;
    const u32 vinit =
        ov(own_lines ? OV0_P23_V_ACCUM_INIT : OV0_P1_V_ACCUM_INIT);
    q.ystep = own_lines ? (long long)(vinc >> cshift_y) : (long long)vinc;
    q.y0 = ((long long)((vinit >> 15) & (own_lines ? 0x3ffu : 0x7ffu)) << 15) -
           ((four_v ? 3ll : 2ll) << 20);
    const u32 bl =
        ov(own_lines ? OV0_P23_BLANK_LINES_AT_TOP : OV0_P1_BLANK_LINES_AT_TOP);
    q.ymax = int((bl >> 16) & (own_lines ? 0x7ffu : 0xfffu));
    // the filters
    const bool hard_h = (fc & (chroma && !rgb_src ? 2u : 1u)) != 0;
    const bool hard_v = (fc & (chroma && !rgb_src ? 8u : 4u)) != 0;
    q.hf = (sc & OV0_HORZ_PICK_NEAREST) ? OvFilter::Nearest
           : hard_h                     ? OvFilter::Linear
                                        : OvFilter::FourTap;
    q.vf = (sc & OV0_VERT_PICK_NEAREST) ? OvFilter::Nearest
           : (four_v && !hard_v)        ? OvFilter::FourTap
                                        : OvFilter::Linear;
  }
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;
  // a component of plane p at pixel (x, y) of its own grid
  auto comp = [&](int p, int k, int x, int y) -> int {
    const plane_t &q = pl[p];
    x = std::min(std::max(x, q.xmin), q.xmax);
    y = std::min(std::max(y, 0), q.ymax);
    const u32 row = q.base + u32(y) * q.pitch;
    switch (fmt) {
    case 3: { // xRGB1555
      const u32 a = (row + u32(x) * 2) & mask;
      const u32 v = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
      const u32 c = (v >> (10 - 5 * k)) & 31;
      return int((c << 3) | (c >> 2));
    }
    case 4: { // RGB565
      const u32 a = (row + u32(x) * 2) & mask;
      const u32 v = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
      if (k == 1) {
        const u32 c = (v >> 5) & 63;
        return int((c << 2) | (c >> 4));
      }
      const u32 c = (v >> (k == 0 ? 11 : 0)) & 31;
      return int((c << 3) | (c >> 2));
    }
    case 6: // xRGB8888
      return vram[(row + u32(x) * 4 + u32(2 - k)) & mask];
    case 11: // YUY2 in memory: Y0 U Y1 V
      return p == 0 ? vram[(row + u32(x) * 2) & mask]
                    : vram[(row + u32(x) * 4 + (p == 1 ? 1 : 3)) & mask];
    case 12: // UYVY in memory: U Y0 V Y1
      return p == 0 ? vram[(row + u32(x) * 2 + 1) & mask]
                    : vram[(row + u32(x) * 4 + (p == 1 ? 0 : 2)) & mask];
    default: // planar
      return vram[(row + u32(x)) & mask];
    }
  };
  // plane p (component k of an RGB surface) for output pixel (dx, dy)
  auto sample = [&](int p, int k, int dx, int dy) -> int {
    const plane_t &q = pl[p];
    const long long px = q.x0 + q.xstep * dx, py = q.y0 + q.ystep * dy;
    const int ix = int(px >> 20), iy = int(py >> 20);
    int wh[4], wv[4];
    overlay_filter_weights(q.hf, coef, unsigned(px >> 15) & 31, wh);
    overlay_filter_weights(q.vf, coef, unsigned(py >> 15) & 31, wv);
    int acc = 0;
    for (int j = 0; j < 4; j++) {
      if (!wv[j])
        continue;
      int line = 0;
      for (int i = 0; i < 4; i++)
        if (wh[i])
          line += wh[i] * comp(p, k, ix - 1 + i, iy - 1 + j);
      acc += wv[j] * clamp255((line + 16) >> 5);
    }
    return clamp255((acc + 16) >> 5);
  };

  // the graphics pixel as the key sees it
  const u32 gstart = native_start(), gpitch = native_pitch_bytes();
  const int dbl = (R(CRTC_GEN_CNTL) & CRTC_DBL_SCAN_EN) ? 1 : 0;
  const u32 code =
      (R(CRTC_GEN_CNTL) & CRTC_PIX_WIDTH_MASK) >> CRTC_PIX_WIDTH_SHIFT;
  auto gfx = [&](int x, int y, u32 *alpha) -> u32 {
    const u32 row = gstart + u32(y >> dbl) * gpitch;
    *alpha = 255;
    switch (code) {
    case PIX_15BPP: {
      const u32 a = (row + u32(x) * 2) & mask;
      const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
      return (((p >> 10) & 31) << 19) | (((p >> 5) & 31) << 11) |
             ((p & 31) << 3);
    }
    case PIX_16BPP: {
      const u32 a = (row + u32(x) * 2) & mask;
      const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
      return (((p >> 11) & 31) << 19) | (((p >> 5) & 63) << 10) |
             ((p & 31) << 3);
    }
    case PIX_24BPP: {
      const u32 a = (row + u32(x) * 3) & mask;
      return (u32(vram[(a + 2) & mask]) << 16) |
             (u32(vram[(a + 1) & mask]) << 8) | vram[a];
    }
    case PIX_32BPP: {
      const u32 a = (row + u32(x) * 4) & mask;
      *alpha = vram[(a + 3) & mask];
      return (u32(vram[(a + 2) & mask]) << 16) |
             (u32(vram[(a + 1) & mask]) << 8) | vram[a];
    }
    default: {
      const u32 i = vram[(row + u32(x)) & mask];
      return (i << 16) | (i << 8) | i;
    }
    }
  };
  auto in_range = [](u32 v, u32 lo, u32 hi) {
    for (int s = 0; s < 24; s += 8) {
      const u32 c = (v >> s) & 0xff;
      if (c < ((lo >> s) & 0xff) || c > ((hi >> s) & 0xff))
        return false;
    }
    return true;
  };
  auto key_fn = [](u32 fn, bool eq) {
    return fn == 0 ? false : fn == 1 ? true : fn == 2 ? eq : !eq;
  };
  const u32 kc = ov(OV0_KEY_CNTL), merge = R(DISP_MERGE_CNTL);
  const u32 lin[6] = {R(OV0_LIN_TRANS_A), R(OV0_LIN_TRANS_B),
                      R(OV0_LIN_TRANS_C), R(OV0_LIN_TRANS_D),
                      R(OV0_LIN_TRANS_E), R(OV0_LIN_TRANS_F)};
  const bool bypass = rgb_src || (sc & OV0_LIN_TRANS_BYPASS) ||
                      (ov(OV0_TEST) & 1u); // OV0_SCALER_Y2R_DISABLE
  const int uv_bias = (sc & OV0_SIGNED_UV) ? 128 : 0;

  for (int y = std::max(wy0, 0); y <= wy1 && y < bitmap.height(); y++) {
    uint32_t *line = &bitmap.pix(y);
    for (int x = std::max(wx0, 0); x <= wx1 && x < bitmap.width(); x++) {
      const int dx = x - wx0, dy = y - wy0;
      int rgb[3];
      if (rgb_src) {
        for (int k = 0; k < 3; k++)
          rgb[k] = sample(0, k, dx, dy);
      } else {
        const int yy = sample(0, 0, dx, dy);
        const int cb = (sample(1, 0, dx, dy) + uv_bias) & 255;
        const int cr = (sample(2, 0, dx, dy) + uv_bias) & 255;
        if (bypass) {
          // the components as they are [inference: which channel each
          // lands in without the transform is not documented]
          rgb[0] = cr;
          rgb[1] = yy;
          rgb[2] = cb;
        } else {
          overlay_to_rgb(lin, yy, cb, cr, rgb);
        }
      }
      const u32 vid = (u32(rgb[0]) << 16) | (u32(rgb[1]) << 8) | u32(rgb[2]);
      u32 galpha;
      const u32 g = gfx(x, y, &galpha);
      const bool vk = key_fn(kc & 3, in_range(vid, ov(OV0_VIDEO_KEY_CLR_LOW),
                                              ov(OV0_VIDEO_KEY_CLR_HIGH)));
      const bool gk =
          key_fn((kc >> 4) & 3, in_range(g, ov(OV0_GRAPHICS_KEY_CLR_LOW),
                                         ov(OV0_GRAPHICS_KEY_CLR_HIGH)));
      const bool show = (kc & (1u << 8)) ? (vk && gk) : (vk || gk);
      switch (merge & 3) {
      case 0:
        if (show)
          line[x] = 0xff000000u | vid;
        break;
      case 2: {
        const u32 oa = merge >> 24, ga = (merge >> 16) & 0xff;
        u32 out = 0xff000000u;
        for (int s = 0; s < 24; s += 8)
          out |= std::min(255u, (((vid >> s) & 0xff) * oa +
                                 ((line[x] >> s) & 0xff) * ga + 127) /
                                    255)
                 << s;
        line[x] = out;
        break;
      }
      default: {
        u32 out = 0xff000000u;
        for (int s = 0; s < 24; s += 8)
          out |= ((((line[x] >> s) & 0xff) * galpha +
                   ((vid >> s) & 0xff) * (255 - galpha) + 127) /
                  255)
                 << s;
        line[x] = out;
        break;
      }
      }
    }
  }
}
