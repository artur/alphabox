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

/* ATI Radeon (R100 family, RV200) register and identity definitions.
 *
 * Definitions only, shared by the translation units of the Radeon device
 * (see Radeon.hpp). Offsets are byte offsets in the 64 KB register
 * aperture (PCI BAR2); the I/O BAR mirrors its first 256 bytes, and
 * MM_INDEX/MM_DATA reach the rest from there.
 *
 * Names are ATI's as the open drivers spell them: Linux's radeon_reg.h
 * and include/video/radeon.h (MIT-style licence, ATI and VA Linux), and
 * QEMU's ati_regs.h (GPL-2.0-or-later). Values here are re-typed, not
 * copied, and only the registers this device gives a meaning to.
 */

#if !defined(INCLUDED_RADEON_REGS_H)
#define INCLUDED_RADEON_REGS_H

#include "datatypes.hpp"

namespace radeon {

constexpr u16 PCI_VENDOR_ATI = 0x1002;
constexpr u16 PCI_DEVICE_RV200_QW = 0x5157; ///< "QW": Radeon 7500

/// The register aperture (BAR2) and its first 256 bytes, the I/O BAR.
constexpr u32 REG_APERTURE_BYTES = 0x10000;
constexpr u32 IO_BAR_BYTES = 0x100;
/// The framebuffer BAR: two apertures over the same memory, 64 MB each
/// (CONFIG_APER_SIZE), which SURFACE_CNTL may byte-swap separately.
constexpr u32 FB_APERTURE_BYTES = 128u << 20;
constexpr u32 FB_APERTURE_HALF = 64u << 20;
/// The expansion ROM BAR's size.
constexpr u32 ROM_BAR_BYTES = 128u << 10;

// --- bus interface and configuration ----------------------------------------
constexpr u32 MM_INDEX = 0x0000;
constexpr u32 MM_DATA = 0x0004;
constexpr u32 CLOCK_CNTL_INDEX = 0x0008;
constexpr u32 CLOCK_CNTL_DATA = 0x000c;
constexpr u32 BIOS_0_SCRATCH = 0x0010; ///< ... BIOS_7_SCRATCH 0x002c
constexpr u32 BUS_CNTL = 0x0030;
constexpr u32 GEN_INT_CNTL = 0x0040;
constexpr u32 GEN_INT_STATUS = 0x0044;
constexpr u32 CRTC_GEN_CNTL = 0x0050;
constexpr u32 CRTC_EXT_CNTL = 0x0054;
constexpr u32 DAC_CNTL = 0x0058;
constexpr u32 CRTC_STATUS = 0x005c;
constexpr u32 GPIO_VGA_DDC = 0x0060;
constexpr u32 GPIO_DVI_DDC = 0x0064;
constexpr u32 GPIO_MONID = 0x0068;
constexpr u32 GPIO_CRT2_DDC = 0x006c;
constexpr u32 DAC_CNTL2 = 0x007c;
constexpr u32 PALETTE_INDEX = 0x00b0;
constexpr u32 PALETTE_DATA = 0x00b4;
constexpr u32 PALETTE_30_DATA = 0x00b8;
constexpr u32 CONFIG_CNTL = 0x00e0;
constexpr u32 CONFIG_XSTRAP = 0x00e4;
constexpr u32 CONFIG_BONDS = 0x00e8;
constexpr u32 RBBM_SOFT_RESET = 0x00f0;
constexpr u32 CONFIG_MEMSIZE = 0x00f8;
constexpr u32 CONFIG_APER_0_BASE = 0x0100;
constexpr u32 CONFIG_APER_1_BASE = 0x0104;
constexpr u32 CONFIG_APER_SIZE = 0x0108;
constexpr u32 CONFIG_REG_1_BASE = 0x010c;
constexpr u32 CONFIG_REG_APER_SIZE = 0x0110;
constexpr u32 HOST_PATH_CNTL = 0x0130;
constexpr u32 MEM_CNTL = 0x0140;
constexpr u32 MC_FB_LOCATION = 0x0148;
constexpr u32 MC_AGP_LOCATION = 0x014c;
constexpr u32 MC_STATUS = 0x0150;
constexpr u32 MEM_SDRAM_MODE_REG = 0x0158;
constexpr u32 AGP_CNTL = 0x0174;
constexpr u32 PCI_CONFIG_MIRROR = 0x0f00; ///< 0xf00-0xfff: config space
constexpr u32 SURFACE_CNTL = 0x0b00;

// --- the primary CRTC -------------------------------------------------------
constexpr u32 CRTC_H_TOTAL_DISP = 0x0200;
constexpr u32 CRTC_H_SYNC_STRT_WID = 0x0204;
constexpr u32 CRTC_V_TOTAL_DISP = 0x0208;
constexpr u32 CRTC_V_SYNC_STRT_WID = 0x020c;
constexpr u32 CRTC_VLINE_CRNT_VLINE = 0x0210;
constexpr u32 CRTC_CRNT_FRAME = 0x0214;
constexpr u32 CRTC_OFFSET = 0x0224;
constexpr u32 CRTC_OFFSET_CNTL = 0x0228;
constexpr u32 CRTC_PITCH = 0x022c;
constexpr u32 DISPLAY_BASE_ADDR = 0x023c;
constexpr u32 CUR_OFFSET = 0x0260;
constexpr u32 CUR_HORZ_VERT_POSN = 0x0264;
constexpr u32 CUR_HORZ_VERT_OFF = 0x0268;
constexpr u32 CUR_CLR0 = 0x026c;
constexpr u32 CUR_CLR1 = 0x0270;
constexpr u32 CRTC2_GEN_CNTL = 0x03f8;
constexpr u32 CRTC2_STATUS = 0x03fc;

/// The VGA's own I/O ports, mirrored byte for byte in the register
/// aperture at their port numbers.
constexpr u32 VGA_MIRROR_FIRST = 0x03b0;
constexpr u32 VGA_MIRROR_LAST = 0x03df;

// --- the engine's status and the command processor --------------------------
constexpr u32 CP_CSQ_CNTL = 0x0740;
constexpr u32 CP_CSQ_STAT = 0x07f8;
constexpr u32 RBBM_STATUS = 0x0e40;
constexpr u32 RBBM_STATUS_ALT = 0x1740;

// --- bits --------------------------------------------------------------------
// CRTC_GEN_CNTL
constexpr u32 CRTC_DBL_SCAN_EN = 1u << 0;
constexpr u32 CRTC_INTERLACE_EN = 1u << 1;
constexpr u32 CRTC_PIX_WIDTH_SHIFT = 8;
constexpr u32 CRTC_PIX_WIDTH_MASK = 0xfu << 8;
constexpr u32 CRTC_CUR_EN = 1u << 16;
constexpr u32 CRTC_CUR_MODE_SHIFT = 20;
constexpr u32 CRTC_CUR_MODE_MASK = 7u << 20;
constexpr u32 CRTC_EXT_DISP_EN = 1u << 24;
constexpr u32 CRTC_EN = 1u << 25;
constexpr u32 CRTC_DISP_REQ_EN_B = 1u << 26;
// CRTC_EXT_CNTL
constexpr u32 VGA_ATI_LINEAR = 1u << 3;
constexpr u32 CRTC_HSYNC_DIS = 1u << 8;
constexpr u32 CRTC_VSYNC_DIS = 1u << 9;
constexpr u32 CRTC_DISPLAY_DIS = 1u << 10;
constexpr u32 CRTC_CRT_ON = 1u << 15;
// CRTC_STATUS, GEN_INT_STATUS, GEN_INT_CNTL
constexpr u32 CRTC_VBLANK_CUR = 1u << 0;
constexpr u32 CRTC_VBLANK_SAVE = 1u << 1;
constexpr u32 INT_CRTC_VBLANK = 1u << 0;
constexpr u32 INT_CRTC_VLINE = 1u << 1;
constexpr u32 INT_GUI_IDLE = 1u << 19;
constexpr u32 INT_SW = 1u << 25;
constexpr u32 INT_SW_FIRE = 1u << 26; ///< GEN_INT_STATUS: write to raise SW
// DAC_CNTL
constexpr u32 DAC_CMP_EN = 1u << 3;
constexpr u32 DAC_CMP_OUTPUT = 1u << 7;
constexpr u32 DAC_8BIT_EN = 1u << 8;
constexpr u32 DAC_VGA_ADR_EN = 1u << 13;
constexpr u32 DAC_PDWN = 1u << 15;
// GPIO_* (DDC lines): drive A, read Y, output enable EN
constexpr u32 GPIO_A_0 = 1u << 0;
constexpr u32 GPIO_A_1 = 1u << 1;
constexpr u32 GPIO_Y_0 = 1u << 8;
constexpr u32 GPIO_Y_1 = 1u << 9;
constexpr u32 GPIO_EN_0 = 1u << 16;
constexpr u32 GPIO_EN_1 = 1u << 17;
// RBBM_STATUS: free command FIFO entries <6:0>, busy <31>
constexpr u32 RBBM_FIFOCNT_MASK = 0x7f;
constexpr u32 RBBM_ACTIVE = 1u << 31;
// CUR_OFFSET / CUR_HORZ_VERT_POSN: the update lock
constexpr u32 CUR_LOCK = 1u << 31;

/// CRTC_PIX_WIDTH codes.
enum : u8 {
  PIX_4BPP = 1,
  PIX_8BPP = 2,
  PIX_15BPP = 3,
  PIX_16BPP = 4,
  PIX_24BPP = 5,
  PIX_32BPP = 6
};

// --- the overlay scaler (RadeonOverlay.cpp) ---------------------------------
/// The double-buffered block.
constexpr u32 OV0_BLOCK_FIRST = 0x0400;
constexpr u32 OV0_BLOCK_LAST = 0x04fc;
constexpr u32 OV0_Y_X_START = 0x0400;
constexpr u32 OV0_Y_X_END = 0x0404;
constexpr u32 OV0_EXCLUSIVE_HORZ = 0x0408;
constexpr u32 OV0_EXCLUSIVE_VERT = 0x040c;
constexpr u32 OV0_REG_LOAD_CNTL = 0x0410;
constexpr u32 OV0_SCALE_CNTL = 0x0420;
constexpr u32 OV0_V_INC = 0x0424;
constexpr u32 OV0_P1_V_ACCUM_INIT = 0x0428;
constexpr u32 OV0_P23_V_ACCUM_INIT = 0x042c;
constexpr u32 OV0_P1_BLANK_LINES_AT_TOP = 0x0430;
constexpr u32 OV0_P23_BLANK_LINES_AT_TOP = 0x0434;
constexpr u32 OV0_BASE_ADDR = 0x043c;
constexpr u32 OV0_VID_BUF0_BASE_ADRS = 0x0440; ///< ... BUF5 0x0454
constexpr u32 OV0_VID_BUF_PITCH0_VALUE = 0x0460;
constexpr u32 OV0_VID_BUF_PITCH1_VALUE = 0x0464;
constexpr u32 OV0_AUTO_FLIP_CNTL = 0x0470;
constexpr u32 OV0_DEINTERLACE_PATTERN = 0x0474;
constexpr u32 OV0_H_INC = 0x0480;
constexpr u32 OV0_STEP_BY = 0x0484;
constexpr u32 OV0_P1_H_ACCUM_INIT = 0x0488;
constexpr u32 OV0_P23_H_ACCUM_INIT = 0x048c;
constexpr u32 OV0_P1_X_START_END = 0x0494;
constexpr u32 OV0_P2_X_START_END = 0x0498;
constexpr u32 OV0_P3_X_START_END = 0x049c;
constexpr u32 OV0_FILTER_CNTL = 0x04a0;
constexpr u32 OV0_FOUR_TAP_COEF_0 = 0x04b0; ///< ... COEF_4 0x04c0
constexpr u32 OV0_VIDEO_KEY_CLR_LOW = 0x04e4;
constexpr u32 OV0_VIDEO_KEY_CLR_HIGH = 0x04e8;
constexpr u32 OV0_GRAPHICS_KEY_CLR_LOW = 0x04ec;
constexpr u32 OV0_GRAPHICS_KEY_CLR_HIGH = 0x04f0;
constexpr u32 OV0_KEY_CNTL = 0x04f4;
constexpr u32 OV0_TEST = 0x04f8;
constexpr u32 FCP_CNTL = 0x0910;
constexpr u32 OV0_LIN_TRANS_A = 0x0d20; ///< ... F 0x0d34
constexpr u32 OV0_LIN_TRANS_B = 0x0d24;
constexpr u32 OV0_LIN_TRANS_C = 0x0d28;
constexpr u32 OV0_LIN_TRANS_D = 0x0d2c;
constexpr u32 OV0_LIN_TRANS_E = 0x0d30;
constexpr u32 OV0_LIN_TRANS_F = 0x0d34;
constexpr u32 DISP_MERGE_CNTL = 0x0d60;
// OV0_REG_LOAD_CNTL
constexpr u32 OV0_LOCK = 1u << 0;
constexpr u32 OV0_LOCK_READBACK = 1u << 3;
constexpr u32 OV0_FLIP_READBACK = 1u << 4;
// OV0_SCALE_CNTL
constexpr u32 OV0_HORZ_PICK_NEAREST = 1u << 2;
constexpr u32 OV0_VERT_PICK_NEAREST = 1u << 3;
constexpr u32 OV0_SIGNED_UV = 1u << 4;
constexpr u32 OV0_SCALER_CRTC_SEL = 1u << 14;
constexpr u32 OV0_DOUBLE_BUFFER = 1u << 24;
constexpr u32 OV0_LIN_TRANS_BYPASS = 1u << 28;
constexpr u32 OV0_SCALER_ENABLE = 1u << 30;
constexpr u32 OV0_SCALER_SOFT_RESET = 1u << 31;

// --- the PLL registers (CLOCK_CNTL_INDEX <5:0>, write enable <7>)
// --------------
constexpr u32 PLL_INDEX_MASK = 0x3f;
constexpr u32 PLL_WR_EN = 1u << 7;
constexpr int PLL_REGS = 64;
constexpr u8 PLL_CLK_PIN_CNTL = 0x01;
constexpr u8 PLL_PPLL_CNTL = 0x02;
constexpr u8 PLL_PPLL_REF_DIV = 0x03;
constexpr u8 PLL_PPLL_DIV_0 = 0x04; ///< ... PPLL_DIV_3 0x07
constexpr u8 PLL_VCLK_ECP_CNTL = 0x08;
constexpr u8 PLL_HTOTAL_CNTL = 0x09;
constexpr u8 PLL_M_SPLL_REF_FB_DIV = 0x0a;
constexpr u8 PLL_SPLL_CNTL = 0x0c;
constexpr u8 PLL_SCLK_CNTL = 0x0d;
constexpr u8 PLL_MPLL_CNTL = 0x0e;
constexpr u8 PLL_MCLK_CNTL = 0x12;
constexpr u8 PLL_TEST_CNTL = 0x13;
constexpr u8 PLL_P2PLL_REF_DIV = 0x2b;
/// PPLL_REF_DIV/P2PLL_REF_DIV <15>: an atomic divider update, written to
/// start it and read as 1 until the PLL has taken the new dividers.
constexpr u32 PPLL_ATOMIC_UPDATE = 1u << 15;
/// PPLL_CNTL: the PLL held in reset <0>, asleep <1>; the update request <16>.
constexpr u32 PPLL_RESET = 1u << 0;
constexpr u32 PPLL_SLEEP = 1u << 1;

} // namespace radeon

#endif // !defined(INCLUDED_RADEON_REGS_H)
