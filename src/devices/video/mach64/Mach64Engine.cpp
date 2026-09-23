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
 * Mach64 GUI engine: the registers from 0x100 up in block 0 and the
 * drawing they command.
 *
 * A command is a rectangle (started by the last byte of DST_HEIGHT_WIDTH
 * or DST_X_WIDTH) or a Bresenham line (started by DST_BRES_LNTH). Each
 * destination pixel is chosen between a foreground and a background
 * source -- a colour register, the host, another place in VRAM, or a
 * pattern -- by a monochrome source (all ones, the 8x8 pattern, host
 * bits or VRAM bits), combined with what is there by one of the sixteen
 * mixes, gated by the scissor and the colour compare, and masked by the
 * plane write mask. Host-sourced commands then wait for HOST_DATA
 * writes, each of which advances the command by the pixels or bits it
 * carries.
 *
 * Commands run to completion inside the write that starts them; the FIFO
 * and engine status do not say so, because a chip that is never busy is
 * one no driver can pace itself against -- the work is charged at the rate
 * the part draws it (engine_charge) and the status answers from that.
 * Pixels are addressed in units of their own width, as the chip does;
 * every VRAM access wraps at the installed size.
 *
 * Ported from 86Box's vid_ati_mach64_accel.c (GPL-2; Sarah Walker, Miran
 * Grca, Connor Hyde), minus the VT3 8x8x8 brush and the video overlay.
 * The ALPHABOX_BLIT_STATS report matches the 8514/A engine's.
 **/

#include "Mach64.hpp"

#include <chrono>
#include <cstdlib>

using namespace mach64;

// --- drawing statistics (ALPHABOX_BLIT_STATS) ------------------------------

namespace {
bool blit_stats_on() {
  static const bool on = getenv("ALPHABOX_BLIT_STATS") != nullptr;
  return on;
}
uint64_t g_pixels = 0;
uint64_t g_ns = 0;
uint64_t g_cmds = 0;
uint64_t g_xfers = 0;
std::chrono::steady_clock::time_point g_last_report;

void blit_stats_report_line() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_report < std::chrono::seconds(5))
    return;
  g_last_report = now;
  fprintf(stderr,
          "mach64 blit: %llu pixels in %.3f s over %llu commands and %llu "
          "transfers\n",
          (unsigned long long)g_pixels, (double)g_ns / 1e9,
          (unsigned long long)g_cmds, (unsigned long long)g_xfers);
}

struct blit_stats_report {
  ~blit_stats_report() {
    if (!blit_stats_on() || g_pixels == 0)
      return;
    fprintf(stderr,
            "mach64 blit: %llu pixels in %.3f s over %llu commands and %llu "
            "transfers, %.1f ns/pixel\n",
            (unsigned long long)g_pixels, (double)g_ns / 1e9,
            (unsigned long long)g_cmds, (unsigned long long)g_xfers,
            (double)g_ns / (double)g_pixels);
  }
} g_stats_report;
} // namespace

static inline void lane_set(u32 &v, u32 lane, u8 b) {
  const u32 sh = 8 * (lane & 3);
  v = (v & ~(0xffu << sh)) | (u32(b) << sh);
}

/// Sign-extend a 13-bit x or 15-bit y coordinate field.
static inline int coord_x(u32 v) {
  int x = int(v & 0xfff);
  return (v & 0x1000) ? x | ~0xfff : x;
}
static inline int coord_y(u32 v) {
  int y = int(v & 0x3fff);
  return (v & 0x4000) ? y | ~0x3fff : y;
}

// --- registers ---------------------------------------------------------------

/**
 * A byte of an engine register. The last byte of DST_HEIGHT_WIDTH,
 * DST_X_WIDTH (or DST_WIDTH's top byte) starts a rectangle, the last byte
 * of DST_BRES_LNTH a line, unless its bit 7 says not to. A host-data byte
 * is eight bits or one pixel for the command in flight.
 **/
void CMach64::engine_write8(u32 reg, u8 val) {
  // A rectangle starts now; one with a host source waits for its data.
  auto start_rect = [this]() {
    engine_start_rect();
    if ((r.dst_height_width & 0x7ff) && (r.dst_height_width & 0x7ff0000) &&
        (r.dp_src & 7) != SRC_HOST && ((r.dp_src >> 8) & 7) != SRC_HOST &&
        ((r.dp_src >> 16) & 3) != MONO_SRC_HOST)
      engine_run(0, -1);
  };

  // A GT reaches some registers at two addresses; take the canonical one.
  if (is_gt())
    reg = (reg & 3) | gt_canonical(reg & 0x3fc);

  switch (reg & 0x3ff) {
  case 0x100:
  case 0x101:
  case 0x102:
  case 0x103:
    lane_set(r.dst_off_pitch, reg, val);
    break;
  case 0x104:
  case 0x105:
  case 0x11c:
  case 0x11d:
    lane_set(r.dst_y_x, reg + 2, val);
    break;
  case 0x108:
  case 0x109:
  case 0x10c:
  case 0x10d:
  case 0x10e:
  case 0x10f:
    lane_set(r.dst_y_x, reg, val);
    break;
  case 0x2e8:
  case 0x2e9:
  case 0x2ea:
  case 0x2eb:
    lane_set(r.dst_y_x, reg ^ 2, val);
    break;
  case 0x110:
  case 0x111:
    lane_set(r.dst_height_width, reg + 2, val);
    break;
  case 0x114:
  case 0x115:
  case 0x118:
  case 0x119:
  case 0x11a:
  case 0x11b:
  case 0x11e:
  case 0x11f:
    lane_set(r.dst_height_width, reg, val);
    [[fallthrough]];
  case 0x113:
    if (((reg & 0x3ff) == 0x11b || (reg & 0x3ff) == 0x11f ||
         (reg & 0x3ff) == 0x113) &&
        !(val & 0x80))
      start_rect();
    break;
  case 0x2ec:
  case 0x2ed:
  case 0x2ee:
  case 0x2ef:
    lane_set(r.dst_height_width, reg ^ 2, val);
    if ((reg & 0x3ff) == 0x2ef)
      start_rect();
    break;
  case 0x120:
  case 0x121:
  case 0x122:
  case 0x123:
    lane_set(r.dst_bres_lnth, reg, val);
    if (is_gt()) { // the last byte decides, as the CT's does
      if ((reg & 0x3ff) == 0x123)
        gt_bres_lnth_written();
      break;
    }
    if ((reg & 0x3ff) == 0x123 && !(val & 0x80)) {
      engine_start_line();
      if ((r.dst_bres_lnth & 0x7fff) && (r.dp_src & 7) != SRC_HOST &&
          ((r.dp_src >> 8) & 7) != SRC_HOST &&
          ((r.dp_src >> 16) & 3) != MONO_SRC_HOST)
        engine_run(0, -1);
    }
    break;
  case 0x124:
  case 0x125:
  case 0x126:
  case 0x127:
    lane_set(r.dst_bres_err, reg, val);
    break;
  case 0x128:
  case 0x129:
  case 0x12a:
  case 0x12b:
    lane_set(r.dst_bres_inc, reg, val);
    break;
  case 0x12c:
  case 0x12d:
  case 0x12e:
  case 0x12f:
    lane_set(r.dst_bres_dec, reg, val);
    break;
  case 0x130:
  case 0x131:
  case 0x132:
  case 0x133:
    lane_set(r.dst_cntl, reg, val);
    break;
  case 0x180:
  case 0x181:
  case 0x182:
  case 0x183:
    lane_set(r.src_off_pitch, reg, val);
    break;
  case 0x184:
  case 0x185:
    lane_set(r.src_y_x, reg, val);
    break;
  case 0x188:
  case 0x189:
    lane_set(r.src_y_x, reg + 2, val);
    break;
  case 0x18c:
  case 0x18d:
  case 0x18e:
  case 0x18f:
    lane_set(r.src_y_x, reg, val);
    break;
  case 0x190:
  case 0x191:
    lane_set(r.src_height1_width1, reg + 2, val);
    break;
  case 0x194:
  case 0x195:
  case 0x198:
  case 0x199:
  case 0x19a:
  case 0x19b:
    lane_set(r.src_height1_width1, reg, val);
    break;
  case 0x19c:
  case 0x19d:
    lane_set(r.src_y_x_start, reg, val);
    break;
  case 0x1a0:
  case 0x1a1:
    lane_set(r.src_y_x_start, reg + 2, val);
    break;
  case 0x1a4:
  case 0x1a5:
  case 0x1a6:
  case 0x1a7:
    lane_set(r.src_y_x_start, reg, val);
    break;
  case 0x1a8:
  case 0x1a9:
    lane_set(r.src_height2_width2, reg + 2, val);
    break;
  case 0x1ac:
  case 0x1ad:
  case 0x1b0:
  case 0x1b1:
  case 0x1b2:
  case 0x1b3:
    lane_set(r.src_height2_width2, reg, val);
    break;
  case 0x1b4:
  case 0x1b5:
  case 0x1b6:
  case 0x1b7:
    lane_set(r.src_cntl, reg, val);
    break;
  case 0x240:
  case 0x241:
  case 0x242:
  case 0x243:
    lane_set(r.host_cntl, reg, val);
    break;
  case 0x280:
  case 0x281:
  case 0x282:
  case 0x283:
    lane_set(r.pat_reg0, reg, val);
    break;
  case 0x284:
  case 0x285:
  case 0x286:
  case 0x287:
    lane_set(r.pat_reg1, reg, val);
    break;
  case 0x288:
  case 0x289:
  case 0x28a:
  case 0x28b:
    lane_set(r.pat_cntl, reg, val);
    break;
  case 0x2a0:
  case 0x2a1:
  case 0x2a8:
  case 0x2a9:
    lane_set(r.sc_left_right, reg, val);
    break;
  case 0x2a4:
  case 0x2a5:
    lane_set(r.sc_left_right, reg + 2, val);
    break;
  case 0x2aa:
  case 0x2ab:
    lane_set(r.sc_left_right, reg, val);
    break;
  case 0x2ac:
  case 0x2ad:
  case 0x2b4:
  case 0x2b5:
    lane_set(r.sc_top_bottom, reg, val);
    break;
  case 0x2b0:
  case 0x2b1:
    lane_set(r.sc_top_bottom, reg + 2, val);
    break;
  case 0x2b6:
  case 0x2b7:
    lane_set(r.sc_top_bottom, reg, val);
    break;
  case 0x2c0:
  case 0x2c1:
  case 0x2c2:
  case 0x2c3:
    lane_set(r.dp_bkgd_clr, reg, val);
    break;
  case 0x2c4:
  case 0x2c5:
  case 0x2c6:
  case 0x2c7:
    lane_set(r.dp_frgd_clr, reg, val);
    break;
  case 0x2c8:
  case 0x2c9:
  case 0x2ca:
  case 0x2cb:
    lane_set(r.write_mask, reg, val);
    break;
  case 0x2cc:
  case 0x2cd:
  case 0x2ce:
  case 0x2cf:
    lane_set(r.chain_mask, reg, val);
    break;
  case 0x2d0:
  case 0x2d1:
  case 0x2d2:
  case 0x2d3:
    lane_set(r.dp_pix_width, reg, val);
    break;
  case 0x2d4:
  case 0x2d5:
  case 0x2d6:
  case 0x2d7:
    lane_set(r.dp_mix, reg, val);
    break;
  case 0x2d8:
  case 0x2d9:
  case 0x2da:
  case 0x2db:
    lane_set(r.dp_src, reg, val);
    break;
  case 0x2fc:
  case 0x2fd:
  case 0x2fe:
  case 0x2ff:
    lane_set(r.dp_set_gui_engine, reg, val);
    engine_set_gui_engine();
    break;
  case 0x300:
  case 0x301:
  case 0x302:
  case 0x303:
    lane_set(r.clr_cmp_clr, reg, val);
    break;
  case 0x304:
  case 0x305:
  case 0x306:
  case 0x307:
    lane_set(r.clr_cmp_mask, reg, val);
    break;
  case 0x308:
  case 0x309:
  case 0x30a:
  case 0x30b:
    lane_set(r.clr_cmp_cntl, reg, val);
    break;
  case 0x320:
  case 0x321:
  case 0x322:
  case 0x323:
    lane_set(r.context_mask, reg, val);
    break;
  case 0x32c:
  case 0x32d:
  case 0x32e:
  case 0x32f:
    lane_set(r.context_load_cntl, reg, val);
    if ((reg & 0x3ff) == 0x32f && (r.context_load_cntl & 0x30000))
      engine_load_context();
    break;
  case 0x330:
  case 0x331:
    lane_set(r.dst_cntl, reg, val);
    break;
  case 0x332:
    lane_set(r.src_cntl, 0, val);
    break;
  case 0x333:
    lane_set(r.pat_cntl, 0, val & 7);
    if (val & 0x10)
      r.host_cntl |= HOST_BYTE_ALIGN;
    else
      r.host_cntl &= ~HOST_BYTE_ALIGN;
    break;
  default:
    if ((reg & 0x3ff) >= HOST_DATA0 && (reg & 0x3ff) <= HOST_DATA_LAST)
      engine_run(val, 8);
    else if (has_3d_regs()) // the trailing edge, Z, texture, interpolators
      lane_set(r.gt[(reg & 0x3fc) >> 2], reg, val);
    break;
  }
}

void CMach64::engine_write16(u32 reg, u16 val) {
  reg &= 0x3fe;
  if ((reg & 2) && reg >= HOST_DATA0 && reg <= HOST_DATA_LAST) {
    engine_run(val, 16);
    return;
  }
  switch (reg) {
  case 0x2fc:
    r.dp_set_gui_engine = (r.dp_set_gui_engine & 0xffff0000) | val;
    engine_set_gui_engine();
    break;
  case 0x2fe:
    r.dp_set_gui_engine = (r.dp_set_gui_engine & 0xffff) | (u32(val) << 16);
    engine_set_gui_engine();
    break;
  case 0x32c:
    r.context_load_cntl = (r.context_load_cntl & 0xffff0000) | val;
    break;
  case 0x32e:
    r.context_load_cntl = (r.context_load_cntl & 0xffff) | (u32(val) << 16);
    if (r.context_load_cntl & 0x30000)
      engine_load_context();
    break;
  default:
    engine_write8(reg, u8(val));
    engine_write8(reg + 1, u8(val >> 8));
    break;
  }
}

/**
 * A dword of host data is 32 bits of monochrome source or up to four
 * pixels. Colour pixels and LSB-first monochrome go in as they are; the
 * default MSB-first monochrome data is byte-swapped so that the first
 * pixel is the top bit.
 **/
void CMach64::engine_write32(u32 reg, u32 val) {
  reg &= 0x3fc;
  if (reg >= HOST_DATA0 && reg <= HOST_DATA_LAST) {
    if (accel.source_host || (r.dp_pix_width & DP_BYTE_PIX_ORDER))
      engine_run(val, 32);
    else
      engine_run(((val & 0xff000000) >> 24) | ((val & 0x00ff0000) >> 8) |
                     ((val & 0x0000ff00) << 8) | ((val & 0x000000ff) << 24),
                 32);
    return;
  }
  if (is_gt() && gt_canonical(reg) == DST_BRES_LNTH) {
    r.dst_bres_lnth = val;
    gt_bres_lnth_written();
    return;
  }
  switch (reg) {
  case 0x32c:
    r.context_load_cntl = val;
    if (val & 0x30000)
      engine_load_context();
    break;
  case 0x2fc:
    r.dp_set_gui_engine = val;
    engine_set_gui_engine();
    break;
  default:
    engine_write16(reg, u16(val));
    engine_write16(reg + 2, u16(val >> 16));
    break;
  }
}

/**
 * DP_SET_GUI_ENGINE: one write that sets the engine up for a common
 * drawing configuration (pixel width, pitch, pattern size, source and mix
 * combination) in place of a dozen register writes.
 **/
void CMach64::engine_set_gui_engine() {
  static const unsigned pitches[16] = {320, 320,  352,  384,  640,  800,
                                       896, 512,  1024, 1152, 1280, 400,
                                       832, 1600, 448,  2048};
  const u32 s = r.dp_set_gui_engine;

  r.dst_y_x = 0;
  r.dst_height_width = 0;
  r.src_y_x = 0;
  r.sc_top_bottom = 0x3fff0000;
  r.sc_left_right = 0x1fff0000;
  r.write_mask = ~0u;
  r.clr_cmp_clr = 0;
  r.src_y_x_start = 0;
  r.src_cntl &= ~((3u << 13) | (1u << 5) | (1u << 12));
  r.dst_cntl &= ~(7u << 13);
  r.dp_pix_width &= (1u << 13);

  r.dp_pix_width = (r.dp_pix_width & ~7u) | ((s >> 3) & 7);
  r.dp_pix_width = (r.dp_pix_width & ~(0xfu << 8)) |
                   ((s & (1u << 6)) ? ((r.dp_pix_width & 7) << 8) : 0);

  r.dst_off_pitch = (262144u * ((s >> 7) & 3)) & ((1u << 20) - 1);
  r.dst_off_pitch |=
      ((pitches[(s >> 10) & 0xf] * ((s & (1u << 14)) ? 2 : 1)) / 8) << 22;

  r.src_off_pitch = 0;
  if (s & (1u << 15))
    r.src_off_pitch = r.dst_off_pitch;

  switch ((s >> 16) & 3) {
  case 0:
    r.src_height1_width1 = r.src_height2_width2 = 0x00080008;
    break;
  case 1:
    r.src_height1_width1 = r.src_height2_width2 = 0x00200001;
    break;
  case 2:
    r.src_height1_width1 = r.src_height2_width2 = 0x00180008;
    break;
  }

  switch ((s >> 20) & 0xf) {
  case 1:
    r.dp_mix = 0x070003;
    r.dp_src = 0x0000100;
    r.gui_traj_cntl = 0x23;
    break;
  case 2:
    r.dp_src = 0x200;
    r.dp_mix = 0x70007;
    r.gui_traj_cntl = 0x3;
    break;
  case 3:
    r.dp_src = 0x20100;
    r.dp_mix = 0x70007;
    r.gui_traj_cntl = 0x3;
    break;
  case 4:
  case 6:
    r.dp_src = 0x100;
    r.dp_mix = 0x70007;
    r.gui_traj_cntl = 0x3;
    break;
  case 5:
    r.dp_src = 0x10100;
    r.dp_mix = 0x70007;
    r.gui_traj_cntl = 0x01000003;
    break;
  case 7:
    r.dp_src = 0x300;
    r.dp_mix = 0x70007;
    r.gui_traj_cntl = 0x30003;
    break;
  case 8:
  case 9:
  case 10:
  case 11:
    r.dp_src = 0x300;
    r.dp_mix = 0x70007;
    r.gui_traj_cntl = ((s >> 20) & 0xf) - 8;
    break;
  case 12:
    r.dp_src = 0x20100;
    r.dp_mix = 0x70003;
    r.gui_traj_cntl = 0x1004001B;
    break;
  case 13:
    r.dp_src = 0x20100;
    r.dp_mix = 0x70003;
    r.gui_traj_cntl = 0x0004001B;
    break;
  case 15:
    r.dp_src = 0x300;
    r.dp_mix = 0x70007;
    r.gui_traj_cntl = 0x0004001B;
    break;
  default: // 0 and 14 are undefined combinations
    break;
  }
  engine_write32(GUI_TRAJ_CNTL, r.gui_traj_cntl);
}

/**
 * CONTEXT_LOAD_CNTL: load engine registers from a 256-byte context block
 * in VRAM, the ones CONTEXT_MASK selects, chaining to the next block the
 * block names.
 **/
void CMach64::engine_load_context() {
  static const struct {
    int bit;
    u32 reg;
    u32 at;
  } fields[] = {
      {2, DST_OFF_PITCH, 0x08},
      {3, DST_Y_X, 0x0c},
      {4, DST_HEIGHT_WIDTH, 0x10},
      {5, DST_BRES_ERR, 0x14},
      {6, DST_BRES_INC, 0x18},
      {7, DST_BRES_DEC, 0x1c},
      {8, SRC_OFF_PITCH, 0x20},
      {9, SRC_Y_X, 0x24},
      {10, SRC_HEIGHT1_WIDTH1, 0x28},
      {11, SRC_Y_X_START, 0x2c},
      {12, SRC_HEIGHT2_WIDTH2, 0x30},
      {13, PAT_REG0, 0x34},
      {14, PAT_REG1, 0x38},
      {15, SC_LEFT_RIGHT, 0x3c},
      {16, SC_TOP_BOTTOM, 0x40},
      {17, DP_BKGD_CLR, 0x44},
      {18, DP_FRGD_CLR, 0x48},
      {19, DP_WRITE_MASK, 0x4c},
      {20, DP_CHAIN_MASK, 0x50},
      {21, DP_PIX_WIDTH, 0x54},
      {22, DP_MIX, 0x58},
      {23, DP_SRC, 0x5c},
      {24, CLR_CMP_CLR, 0x60},
      {25, CLR_CMP_MASK, 0x64},
      {26, CLR_CMP_CNTL, 0x68},
      {27, GUI_TRAJ_CNTL, 0x6c},
  };
  int guard = 0;
  while ((r.context_load_cntl & 0x30000) && guard++ < 1024) {
    const u32 addr =
        ((0x3fff - (r.context_load_cntl & 0x3fff)) * 256) & vram_mask();
    r.context_mask = vram_read(addr, 4);
    for (const auto &f : fields)
      if (r.context_mask & (1u << f.bit))
        engine_write32(f.reg, vram_read(addr + f.at, 4));
    r.context_load_cntl = vram_read(addr + 0x70, 4);
  }
}

// --- command setup
// -------------------------------------------------------------

/**
 * Latch everything a rectangle needs from the registers.
 **/
void CMach64::engine_start_rect() {
  accel_t &a = accel;
  a.dst_pix_width = r.dp_pix_width & 7;
  a.src_pix_width = (r.dp_pix_width >> 8) & 7;
  a.host_pix_width = (r.dp_pix_width >> 16) & 7;
  a.dst_size = PIX_WIDTH_SIZE[a.dst_pix_width];
  a.src_size = PIX_WIDTH_SIZE[a.src_pix_width];
  a.host_size = PIX_WIDTH_SIZE[a.host_pix_width];

  a.dst_x = 0;
  a.dst_y = 0;
  a.dst_x_start = coord_x(r.dst_y_x >> 16);
  a.dst_y_start = coord_y(r.dst_y_x);

  a.dst_width = (r.dst_height_width >> 16) & 0x1fff;
  a.dst_height = r.dst_height_width & 0x1fff;

  if (((r.dp_src >> 16) & 7) == MONO_SRC_BLITSRC &&
      (r.src_cntl & (SRC_LINEAR_EN | SRC_BYTE_ALIGN)) ==
          (SRC_LINEAR_EN | SRC_BYTE_ALIGN)) {
    if (a.dst_width & 7)
      a.dst_width = (a.dst_width & ~7) + 8;
  }

  a.x_count = a.dst_width;
  a.xx_count = 0;

  a.src_x = 0;
  a.src_y = 0;
  a.src_x_start = coord_x(r.src_y_x >> 16);
  a.src_y_start = coord_y(r.src_y_x);

  if (r.src_cntl & SRC_LINEAR_EN)
    a.src_x_count = 0x7ffffff; // essentially infinite
  else
    a.src_x_count = (r.src_height1_width1 >> 16) & 0x7fff;
  if (!(r.src_cntl & SRC_PATT_EN))
    a.src_y_count = 0x7ffffff;
  else
    a.src_y_count = r.src_height1_width1 & 0x1fff;

  a.src_width1 = (r.src_height1_width1 >> 16) & 0x7fff;
  a.src_height1 = r.src_height1_width1 & 0x1fff;
  a.src_width2 = (r.src_height2_width2 >> 16) & 0x7fff;
  a.src_height2 = r.src_height2_width2 & 0x1fff;

  a.src_pitch = (r.src_off_pitch >> 22) << 3;
  a.src_offset = (r.src_off_pitch & 0xfffff) << 3;
  a.dst_pitch = (r.dst_off_pitch >> 22) << 3;
  a.dst_offset = (r.dst_off_pitch & 0xfffff) << 3;

  a.mix_fg = (r.dp_mix >> 16) & 0x1f;
  a.mix_bg = r.dp_mix & 0x1f;
  a.source_bg = r.dp_src & 7;
  a.source_fg = (r.dp_src >> 8) & 7;
  a.source_mix = (r.dp_src >> 16) & 7;

  // Offsets are in bytes; the engine addresses pixels.
  if (a.src_size == WIDTH_1BIT)
    a.src_offset <<= 3;
  else
    a.src_offset >>= a.src_size;
  if (a.dst_size == WIDTH_1BIT)
    a.dst_offset <<= 3;
  else
    a.dst_offset >>= a.dst_size;

  a.xinc = (r.dst_cntl & DST_X_DIR) ? 1 : -1;
  a.yinc = (r.dst_cntl & DST_Y_DIR) ? 1 : -1;

  a.source_host = a.source_bg == SRC_HOST || a.source_fg == SRC_HOST;

  if (r.pat_cntl & 1) {
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) {
        const u32 t = (y & 4) ? r.pat_reg1 : r.pat_reg0;
        a.pattern[y][7 - x] = (t >> (x + ((y & 3) << 3))) & 1;
      }
  }
  if (r.pat_cntl & 2) {
    for (int i = 0; i < 4; i++) {
      a.pattern_clr4x2[0][i] = u8(r.pat_reg0 >> (8 * i));
      a.pattern_clr4x2[1][i] = u8(r.pat_reg1 >> (8 * i));
    }
  }
  if (r.pat_cntl & 4) {
    for (int i = 0; i < 4; i++) {
      a.pattern_clr8x1[i] = u8(r.pat_reg0 >> (8 * i));
      a.pattern_clr8x1[4 + i] = u8(r.pat_reg1 >> (8 * i));
    }
  }

  a.sc_left = r.sc_left_right & 0x1fff;
  a.sc_right = (r.sc_left_right >> 16) & 0x1fff;
  a.sc_top = r.sc_top_bottom & 0x7fff;
  a.sc_bottom = (r.sc_top_bottom >> 16) & 0x7fff;

  a.dp_frgd_clr = r.dp_frgd_clr;
  a.dp_bkgd_clr = r.dp_bkgd_clr;
  a.write_mask = r.write_mask;

  a.clr_cmp_clr = r.clr_cmp_clr & r.clr_cmp_mask;
  a.clr_cmp_mask = r.clr_cmp_mask;
  a.clr_cmp_fn = r.clr_cmp_cntl & 7;
  a.clr_cmp_src = (r.clr_cmp_cntl & (1u << 24)) != 0;

  a.poly_draw = false;
  a.busy = true;
  a.op = accel_t::OP_RECT;
  if (blit_stats_on())
    g_cmds++;
}

void CMach64::engine_start_line() {
  accel_t &a = accel;
  a.dst_x = coord_x(r.dst_y_x >> 16);
  a.dst_y = coord_y(r.dst_y_x);
  a.src_x = coord_x(r.src_y_x >> 16);
  a.src_y = coord_y(r.src_y_x);

  a.src_pitch = (r.src_off_pitch >> 22) << 3;
  a.src_offset = (r.src_off_pitch & 0xfffff) << 3;
  a.dst_pitch = (r.dst_off_pitch >> 22) << 3;
  a.dst_offset = (r.dst_off_pitch & 0xfffff) << 3;

  a.mix_fg = (r.dp_mix >> 16) & 0x1f;
  a.mix_bg = r.dp_mix & 0x1f;
  a.source_bg = r.dp_src & 7;
  a.source_fg = (r.dp_src >> 8) & 7;
  a.source_mix = (r.dp_src >> 16) & 7;

  a.dst_pix_width = r.dp_pix_width & 7;
  a.src_pix_width = (r.dp_pix_width >> 8) & 7;
  a.host_pix_width = (r.dp_pix_width >> 16) & 7;
  a.dst_size = PIX_WIDTH_SIZE[a.dst_pix_width];
  a.src_size = PIX_WIDTH_SIZE[a.src_pix_width];
  a.host_size = PIX_WIDTH_SIZE[a.host_pix_width];

  if (a.src_size == WIDTH_1BIT)
    a.src_offset <<= 3;
  else
    a.src_offset >>= a.src_size;
  if (a.dst_size == WIDTH_1BIT)
    a.dst_offset <<= 3;
  else
    a.dst_offset >>= a.dst_size;

  a.source_host = a.source_bg == SRC_HOST || a.source_fg == SRC_HOST;

  if (r.pat_cntl & 1) {
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) {
        const u32 t = (y & 4) ? r.pat_reg1 : r.pat_reg0;
        a.pattern[y][7 - x] = (t >> (x + ((y & 3) << 3))) & 1;
      }
  }
  a.sc_left = r.sc_left_right & 0x1fff;
  a.sc_right = (r.sc_left_right >> 16) & 0x1fff;
  a.sc_top = r.sc_top_bottom & 0x7fff;
  a.sc_bottom = (r.sc_top_bottom >> 16) & 0x7fff;

  a.dp_frgd_clr = r.dp_frgd_clr;
  a.dp_bkgd_clr = r.dp_bkgd_clr;
  a.write_mask = r.write_mask;

  a.x_count = r.dst_bres_lnth & 0x7fff;
  a.err = int(r.dst_bres_err & 0x3ffff) |
          ((r.dst_bres_err & 0x40000) ? int(0xfffc0000) : 0);

  a.clr_cmp_clr = r.clr_cmp_clr & r.clr_cmp_mask;
  a.clr_cmp_mask = r.clr_cmp_mask;
  a.clr_cmp_fn = r.clr_cmp_cntl & 7;
  a.clr_cmp_src = (r.clr_cmp_cntl & (1u << 24)) != 0;

  a.xinc = (r.dst_cntl & DST_X_DIR) ? 1 : -1;
  a.yinc = (r.dst_cntl & DST_Y_DIR) ? 1 : -1;

  a.busy = true;
  a.op = accel_t::OP_LINE;
  if (blit_stats_on())
    g_cmds++;
}

// --- pixels
// ---------------------------------------------------------------------

/// A pixel of the given width (0 byte, 1 word, 2 dword, 3 bit) at a
/// pixel address.
static inline u32 pix_read(const u8 *vram, u32 mask, u32 addr, int width,
                           bool lsb_first) {
  switch (width) {
  case 0:
    return vram[addr & mask];
  case 1: {
    const u32 a = (addr << 1) & mask;
    return vram[a] | (u32(vram[(a + 1) & mask]) << 8);
  }
  case 2: {
    const u32 a = (addr << 2) & mask;
    return vram[a] | (u32(vram[(a + 1) & mask]) << 8) |
           (u32(vram[(a + 2) & mask]) << 16) |
           (u32(vram[(a + 3) & mask]) << 24);
  }
  default:
    if (lsb_first)
      return (vram[(addr >> 3) & mask] >> (addr & 7)) & 1;
    return (vram[(addr >> 3) & mask] >> (7 - (addr & 7))) & 1;
  }
}

static inline void pix_write(u8 *vram, u32 mask, u32 addr, int width, u32 v,
                             bool lsb_first) {
  switch (width) {
  case 0:
    vram[addr & mask] = u8(v);
    break;
  case 1: {
    const u32 a = (addr << 1) & mask;
    vram[a] = u8(v);
    vram[(a + 1) & mask] = u8(v >> 8);
    break;
  }
  case 2: {
    const u32 a = (addr << 2) & mask;
    vram[a] = u8(v);
    vram[(a + 1) & mask] = u8(v >> 8);
    vram[(a + 2) & mask] = u8(v >> 16);
    vram[(a + 3) & mask] = u8(v >> 24);
    break;
  }
  default: {
    const u32 bit = lsb_first ? (addr & 7) : (7 - (addr & 7));
    if (v & 1)
      vram[(addr >> 3) & mask] |= u8(1u << bit);
    else
      vram[(addr >> 3) & mask] &= u8(~(1u << bit));
    break;
  }
  }
}

/**
 * The sixteen mixes (plus the averaging one) between source and
 * destination, DP_MIX's encoding.
 **/
u32 CMach64::engine_mix(int mix, u32 src, u32 dst) const {
  switch (mix) {
  case 0x0:
    return ~dst;
  case 0x1:
    return 0;
  case 0x2:
    return 0xffffffff;
  case 0x3:
    return dst;
  case 0x4:
    return ~src;
  case 0x5:
    return src ^ dst;
  case 0x6:
    return ~(src ^ dst);
  case 0x7:
    return src;
  case 0x8:
    return ~(src & dst);
  case 0x9:
    return ~src | dst;
  case 0xa:
    return src | ~dst;
  case 0xb:
    return src | dst;
  case 0xc:
    return src & dst;
  case 0xd:
    return src & ~dst;
  case 0xe:
    return ~src & dst;
  case 0xf:
    return ~(src | dst);
  case 0x17:
    return (dst + src) >> 1;
  }
  return dst;
}

/**
 * CLR_CMP_CNTL: true when the pixel is to be left alone.
 **/
bool CMach64::engine_colour_compare(u32 src, u32 dst) const {
  const u32 v = (accel.clr_cmp_src ? src : dst) & accel.clr_cmp_mask;
  switch (accel.clr_cmp_fn) {
  case 1:
    return true;
  case 4:
    return v != accel.clr_cmp_clr;
  case 5:
    return v == accel.clr_cmp_clr;
  }
  return false;
}

/**
 * Feed the command in flight: `count` bits of host data (8, 16 or 32),
 * or -1 to run to completion when the host is not a source.
 **/
/// Charge the time this much drawing would have taken the part, so that a
/// driver asking whether the engine is ready is told what the hardware
/// would have told it. Work already in flight is not restarted: the charge
/// extends from whenever the engine was going to be free.
void CMach64::engine_charge(uint64_t pixels) {
  const long long now = mach64_clock_us();
  const long long from =
      m_engine_busy_until_us > now ? m_engine_busy_until_us : now;
  const long long ns = kEngineSetupNs + (long long)pixels * kEngineNsPerPixel;
  m_engine_busy_until_us = from + (ns + 999) / 1000;
}

void CMach64::engine_run(u32 host_data, int count) {
  if (!accel.busy)
    return;
  std::chrono::steady_clock::time_point t0;
  const bool stats = blit_stats_on();
  if (stats) {
    t0 = std::chrono::steady_clock::now();
    if (count > 0)
      g_xfers++;
  }
  if (accel.op == accel_t::OP_RECT)
    engine_run_rect(host_data, count);
  else
    engine_run_line(host_data, count);
  if (stats) {
    g_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
    blit_stats_report_line();
  }
  state.vga_mem_updated = 1;
}

/**
 * The colour a source selection yields for the pixel at hand. In 24 bpp
 * with DST_24_ROT_EN the engine runs at 8 bpp three bytes to the pixel,
 * and a colour register supplies one byte per step.
 **/
u32 CMach64::engine_source(int sel, u32 host, int src_x, int src_y, int dst_x,
                           int dst_y) const {
  const accel_t &a = accel;
  switch (sel) {
  case SRC_HOST:
    return host;
  case SRC_BLITSRC:
    return pix_read(vga.memory, vram_mask(),
                    a.src_offset + src_y * a.src_pitch + src_x, a.src_size,
                    (r.dp_pix_width & DP_BYTE_PIX_ORDER) != 0);
  case SRC_FG:
  case SRC_BG: {
    const u32 c = sel == SRC_FG ? a.dp_frgd_clr : a.dp_bkgd_clr;
    if (!(r.dst_cntl & DST_24_ROT_EN))
      return c;
    int byte = a.xx_count;
    if (a.xinc == -1)
      byte = 2 - byte;
    return (c >> (8 * byte)) & 0xff;
  }
  case SRC_PAT:
    if (r.pat_cntl & 2)
      return a.pattern_clr4x2[dst_y & 1][dst_x & 3];
    if (r.pat_cntl & 4)
      return a.pattern_clr8x1[dst_x & 7];
    return 0;
  }
  return 0;
}

void CMach64::engine_run_rect(u32 cpu_dat, int count) {
  accel_t &a = accel;
  u8 *const vram = vga.memory;
  const u32 mask = vram_mask();
  const bool lsb_first = (r.dp_pix_width & DP_BYTE_PIX_ORDER) != 0;
  int mix = 0;
  uint64_t drawn = 0;

  while (count) {
    u32 host_dat = 0;

    const int dst_x = (a.dst_x + a.dst_x_start) & 0xfff;
    const int dst_y = (a.dst_y + a.dst_y_start) & 0x3fff;
    int src_x;
    if (r.src_cntl & SRC_LINEAR_EN)
      src_x = a.src_x;
    else
      src_x = (a.src_x + a.src_x_start) & 0xfff;
    const int src_y = (a.src_y + a.src_y_start) & 0x3fff;

    if (a.source_host) {
      host_dat = cpu_dat;
      if (a.host_size < 2)
        cpu_dat >>= (8 << a.host_size);
      count -= (8 << a.host_size);
    } else {
      count--;
    }

    switch (a.source_mix) {
    case MONO_SRC_HOST:
      if (lsb_first) {
        mix = cpu_dat & 1;
        cpu_dat >>= 1;
      } else {
        mix = cpu_dat >> 31;
        cpu_dat <<= 1;
      }
      break;
    case MONO_SRC_PAT:
      if (r.dst_cntl & DST_24_ROT_EN) {
        if (!a.xx_count)
          mix = a.pattern[dst_y & 7][(dst_x / 3) & 7];
      } else {
        mix = a.pattern[dst_y & 7][dst_x & 7];
      }
      break;
    case MONO_SRC_1:
      mix = 1;
      break;
    case MONO_SRC_BLITSRC:
      if (r.src_cntl & SRC_LINEAR_EN)
        mix = pix_read(vram, mask, a.src_offset + src_x, WIDTH_1BIT, lsb_first);
      else
        mix = pix_read(vram, mask, a.src_offset + src_y * a.src_pitch + src_x,
                       WIDTH_1BIT, lsb_first);
      break;
    }

    if (dst_x >= a.sc_left && dst_x <= a.sc_right && dst_y >= a.sc_top &&
        dst_y <= a.sc_bottom) {
      const u32 src_dat = engine_source(mix ? a.source_fg : a.source_bg,
                                        host_dat, src_x, src_y, dst_x, dst_y);

      if (r.dst_cntl & DST_POLYGON_EN) {
        const u32 poly_src =
            pix_read(vram, mask, a.src_offset + src_y * a.src_pitch + src_x,
                     a.src_size, lsb_first);
        if (poly_src)
          a.poly_draw = !a.poly_draw;
      }

      if (!(r.dst_cntl & DST_POLYGON_EN) || a.poly_draw) {
        const u32 dst_addr = a.dst_offset + dst_y * a.dst_pitch + dst_x;
        u32 dest_dat = pix_read(vram, mask, dst_addr, a.dst_size, lsb_first);

        if (!engine_colour_compare(src_dat, dest_dat)) {
          const u32 old = dest_dat;
          dest_dat = engine_mix(mix ? a.mix_fg : a.mix_bg, src_dat, dest_dat);
          u32 wmask = a.write_mask;
          if (r.dst_cntl & DST_24_ROT_EN) {
            int byte = a.xx_count;
            if (a.xinc == -1)
              byte = 2 - byte;
            wmask = (a.write_mask >> (8 * byte)) & 0xff;
          }
          dest_dat = (dest_dat & wmask) | (old & ~wmask);
        }
        pix_write(vram, mask, dst_addr, a.dst_size, dest_dat, lsb_first);
        drawn++;
      }
    }

    a.src_x += a.xinc;
    a.dst_x += a.xinc;
    if (!(r.src_cntl & SRC_LINEAR_EN)) {
      a.src_x_count--;
      if (a.src_x_count <= 0) {
        a.src_x = 0;
        if ((r.src_cntl & (SRC_PATT_ROT_EN | SRC_PATT_EN)) ==
            (SRC_PATT_ROT_EN | SRC_PATT_EN)) {
          a.src_x_start = coord_x(r.src_y_x_start >> 16);
          a.src_x_count = a.src_width2;
        } else {
          a.src_x_count = a.src_width1;
        }
      }
    }

    a.x_count--;
    a.xx_count = (a.xx_count + 1) % 3;
    if (a.x_count <= 0) {
      a.x_count = a.dst_width;
      a.xx_count = 0;
      a.dst_x = 0;
      a.dst_y += a.yinc;
      a.src_x_start = (r.src_y_x >> 16) & 0xfff;
      a.src_x_count = a.src_width1;

      if (!(r.src_cntl & SRC_LINEAR_EN)) {
        a.src_x = 0;
        a.src_y += a.yinc;
        a.src_y_count--;
        if (a.src_y_count <= 0) {
          a.src_y = 0;
          if ((r.src_cntl & (SRC_PATT_ROT_EN | SRC_PATT_EN)) ==
              (SRC_PATT_ROT_EN | SRC_PATT_EN)) {
            a.src_y_start = coord_y(r.src_y_x_start);
            a.src_y_count = a.src_height2;
          } else {
            a.src_y_count = a.src_height1;
          }
        }
      }

      a.poly_draw = false;
      a.dst_height--;
      if (a.dst_height <= 0) {
        // Finished. Tiling advances the start for the next rectangle.
        a.busy = false;
        if (r.dst_cntl & DST_X_TILE)
          r.dst_y_x = (r.dst_y_x & 0xfff) |
                      ((r.dst_y_x + (u32(a.dst_width) << 16)) & 0xfff0000);
        if (r.dst_cntl & DST_Y_TILE)
          r.dst_y_x = (r.dst_y_x & 0xfff0000) |
                      ((r.dst_y_x + (r.dst_height_width & 0x1fff)) & 0xfff);
        break;
      }
      if (r.host_cntl & HOST_BYTE_ALIGN) {
        if (a.source_mix == MONO_SRC_HOST) {
          if (lsb_first)
            cpu_dat >>= (count & 7);
          else
            cpu_dat <<= (count & 7);
          count &= ~7;
        }
      }
    }
  }
  if (blit_stats_on())
    g_pixels += drawn;
  engine_charge(drawn);
}

void CMach64::engine_run_line(u32 cpu_dat, int count) {
  accel_t &a = accel;
  u8 *const vram = vga.memory;
  const u32 mask = vram_mask();
  const bool lsb_first = (r.dp_pix_width & DP_BYTE_PIX_ORDER) != 0;
  const bool bpp24 = native_bpp_code() == BPP_24;
  uint64_t drawn = 0;
  int x = 0;

  while (count) {
    u32 host_dat = 0;
    int mix = 0;
    bool draw_pixel = !(r.dst_cntl & DST_POLYGON_EN);

    if (a.source_host) {
      host_dat = cpu_dat;
      if (a.host_size < 2)
        cpu_dat >>= (8 << a.host_size);
      count -= (8 << a.host_size);
    } else {
      count--;
    }

    switch (a.source_mix) {
    case MONO_SRC_HOST:
      if (bpp24 && lsb_first) {
        mix = cpu_dat & 1;
        cpu_dat >>= 1;
      } else {
        mix = cpu_dat >> 31;
        cpu_dat <<= 1;
      }
      break;
    case MONO_SRC_PAT:
      mix = a.pattern[a.dst_y & 7][a.dst_x & 7];
      break;
    case MONO_SRC_BLITSRC:
      if (bpp24) {
        mix =
            pix_read(vram, mask, a.src_offset + a.src_y * a.src_pitch + a.src_x,
                     WIDTH_1BIT, lsb_first);
        break;
      }
      [[fallthrough]];
    default:
      mix = 1;
      break;
    }

    if (r.dst_cntl & DST_POLYGON_EN) {
      if (r.dst_cntl & DST_Y_MAJOR)
        draw_pixel = true;
      else if (a.err >= 0)
        draw_pixel = true;
    }
    if (!bpp24 && a.x_count == 1 && !(r.dst_cntl & DST_LAST_PEL))
      draw_pixel = false;

    if (a.dst_x >= a.sc_left && a.dst_x <= a.sc_right && a.dst_y >= a.sc_top &&
        a.dst_y <= a.sc_bottom && draw_pixel) {
      const int sel = mix ? a.source_fg : a.source_bg;
      u32 src_dat;
      if (bpp24 || sel != SRC_PAT)
        src_dat =
            engine_source(sel, host_dat, a.src_x, a.src_y, a.dst_x, a.dst_y);
      else
        src_dat = 0;
      if (sel == SRC_FG)
        src_dat = a.dp_frgd_clr; // a line never rotates 24 bpp colours
      else if (sel == SRC_BG)
        src_dat = a.dp_bkgd_clr;

      const u32 dst_addr = a.dst_offset + a.dst_y * a.dst_pitch + a.dst_x;
      u32 dest_dat = pix_read(vram, mask, dst_addr, a.dst_size, lsb_first);
      if (!engine_colour_compare(src_dat, dest_dat)) {
        // DP_WRITE_MASK guards every pixel the engine writes, a line's as
        // much as a rectangle's.
        const u32 old = dest_dat;
        dest_dat = engine_mix(mix ? a.mix_fg : a.mix_bg, src_dat, dest_dat);
        dest_dat = (dest_dat & a.write_mask) | (old & ~a.write_mask);
      }
      if (bpp24) {
        if (!(r.dst_cntl & DST_Y_MAJOR)) {
          if (!x)
            dest_dat &= ~1u;
        } else if (x == a.x_count - 1) {
          dest_dat &= ~1u;
        }
      }
      pix_write(vram, mask, dst_addr, a.dst_size, dest_dat, lsb_first);
      drawn++;
    }

    if (bpp24) {
      x++;
      if (x >= a.x_count) {
        a.busy = false;
        break;
      }
    } else {
      a.x_count--;
      if (a.x_count <= 0) {
        a.busy = false;
        break;
      }
    }

    if (r.dst_cntl & DST_Y_MAJOR) {
      a.dst_y += a.yinc;
      a.src_y += a.yinc;
      if (a.err >= 0) {
        a.err += int(r.dst_bres_dec);
        a.dst_x += a.xinc;
        a.src_x += a.xinc;
      } else {
        a.err += int(r.dst_bres_inc);
      }
    } else {
      a.dst_x += a.xinc;
      a.src_x += a.xinc;
      if (a.err >= 0) {
        a.err += int(r.dst_bres_dec);
        a.dst_y += a.yinc;
        a.src_y += a.yinc;
      } else {
        a.err += int(r.dst_bres_inc);
      }
    }
  }
  if (blit_stats_on())
    g_pixels += drawn;
  engine_charge(drawn);
}
