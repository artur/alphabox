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
 * The 3D RAGE's additions to the drawing engine: the trapezoid, and the
 * 3D pipe that shades it.
 *
 * A GT draws a triangle as up to two trapezoids. The line engine walks the
 * leading edge (DST_Y_X, DST_BRES_*), a second one walks the trailing edge
 * (TRAIL_X, TRAIL_BRES_*), and every scan line between the two is filled.
 * Each pixel's colour comes from the 3D pipe -- interpolated colour, Z and
 * alpha, a texel from the mip map, blending, fog -- selected as DP_SRC's
 * source 5, and is then written through the ordinary 2D path: mix, write
 * mask, scissor. The driver does the triangle setup; the chip only steps.
 *
 * Written from ATI's RRG-G02700 (mach64 Register Reference Guide,
 * ATI-264VT and 3D RAGE, 1996): chapter 4 for the trajectories and Z,
 * chapter 6 for the 3D pipe. There is no open implementation to port.
 **/

#include "Mach64.hpp"

using namespace mach64;

/**
 * DST_BRES_LNTH on a GT is four things in one register (RRG 4-46): the
 * leading edge's length in bits 14:0 (DST_WIDTH[14:0] under another
 * name), DRAW_TRAP in bit 15 (DST_WIDTH[15]), the trailing edge's X in
 * bits 28:16 (DST_HEIGHT[12:0]) and LINE_DIS in bit 31. Bits 31 and 15
 * choose what the write does:
 *
 *   31 15
 *    0  0   a line; TRAIL_X and the length are loaded
 *    0  1   a trapezoid; the length is loaded, TRAIL_X is kept
 *    1  0   TRAIL_X and the length are loaded, nothing is drawn
 *    1  1   a trapezoid; TRAIL_X and the length are loaded
 *
 * Keeping TRAIL_X is what lets the second trapezoid of a triangle start
 * where the first one's trailing edge ended.
 **/
void CMach64::gt_bres_lnth_written() {
  const u32 v = r.dst_bres_lnth;
  const bool trap = (v & GT_DRAW_TRAP) != 0;
  const bool no_line = (v & GT_LINE_DIS) != 0;

  // DST_WIDTH takes bits 15:0; DST_HEIGHT takes TRAIL_X unless kept.
  r.dst_height_width = (r.dst_height_width & 0x0000ffffu) | ((v & 0xffffu) << 16);
  if (!(trap && !no_line))
    r.dst_height_width = (r.dst_height_width & 0xffff0000u) |
                         ((v & GT_TRAIL_X_MASK) >> GT_TRAIL_X_SHIFT);

  if (trap) {
    engine_start_trap();
    return;
  }
  if (no_line)
    return;
  engine_start_line();
  if ((v & GT_LNTH_MASK) && (r.dp_src & 7) != SRC_HOST &&
      ((r.dp_src >> 8) & 7) != SRC_HOST &&
      ((r.dp_src >> 16) & 3) != MONO_SRC_HOST)
    engine_run(0, -1);
}

/**
 * A trapezoid. Not drawn yet: this reports what the driver asked for, once
 * per kind, so that bring-up can see the 3D pipe being used.
 **/
void CMach64::engine_start_trap() {
  static bool reported = false;
  if (!reported || m_trace) {
    reported = true;
    printf("%s: trapezoid: DST_Y_X %08x LNTH %08x DST_CNTL %08x DP_SRC %08x "
           "SCALE_3D_CNTL %08x Z_CNTL %08x (not drawn yet)\n",
           devid_string, r.dst_y_x, r.dst_bres_lnth, r.dst_cntl, r.dp_src,
           gt_reg(SCALE_3D_CNTL), gt_reg(Z_CNTL));
  }
  if (m_trace_trap && m_traps_traced < 64) {
    m_traps_traced++;
    printf("%s: TRAP %d  lead y_x %08x err %08x inc %08x dec %08x lnth %08x "
           "cntl %08x | trail hw %08x err %08x inc %08x dec %08x\n",
           devid_string, m_traps_traced, r.dst_y_x, r.dst_bres_err,
           r.dst_bres_inc, r.dst_bres_dec, r.dst_bres_lnth, r.dst_cntl,
           r.dst_height_width, gt_reg(TRAIL_BRES_ERR), gt_reg(TRAIL_BRES_INC),
           gt_reg(TRAIL_BRES_DEC));
    printf("%s:   pix %08x src %08x mix %08x 3d %08x z %08x/%08x "
           "tex %08x off0 %08x\n",
           devid_string, r.dp_pix_width, r.dp_src, r.dp_mix,
           gt_reg(SCALE_3D_CNTL), gt_reg(Z_CNTL), gt_reg(Z_OFF_PITCH),
           gt_reg(TEX_SIZE_PITCH), gt_reg(TEX_0_OFF));
    printf("%s:   R %08x %08x %08x  G %08x %08x %08x  B %08x %08x %08x\n",
           devid_string, gt_reg(RED_START), gt_reg(RED_X_INC),
           gt_reg(RED_Y_INC), gt_reg(GREEN_START), gt_reg(GREEN_X_INC),
           gt_reg(GREEN_Y_INC), gt_reg(BLUE_START), gt_reg(BLUE_X_INC),
           gt_reg(BLUE_Y_INC));
    printf("%s:   A %08x %08x %08x  Z %08x %08x %08x\n", devid_string,
           gt_reg(ALPHA_START), gt_reg(ALPHA_X_INC), gt_reg(ALPHA_Y_INC),
           gt_reg(Z_START), gt_reg(Z_X_INC), gt_reg(Z_Y_INC));
    printf("%s:   S %08x xs %08x y %08x x2 %08x y2 %08x xy2 %08x  "
           "T %08x xs %08x y %08x x2 %08x y2 %08x xy2 %08x\n",
           devid_string, gt_reg(S_START), gt_reg(S_XINC_START),
           gt_reg(S_Y_INC), gt_reg(S_X_INC2), gt_reg(S_Y_INC2),
           gt_reg(S_XY_INC2), gt_reg(T_START), gt_reg(T_XINC_START),
           gt_reg(T_Y_INC), gt_reg(T_X_INC2), gt_reg(T_Y_INC2),
           gt_reg(T_XY_INC2));
  }
}

