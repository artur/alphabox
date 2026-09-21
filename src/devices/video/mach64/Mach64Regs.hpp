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

/* ATI Mach64 register and identity definitions.
 *
 * Definitions only -- no logic, no state -- shared by the translation units
 * of the Mach64 device (see Mach64.hpp).
 *
 * The register file is one 2 KB block, reached four ways that all end in
 * the same place: the last 4 KB of each half of the memory aperture, the
 * 2 KB at 0xbf800 when the VGA aperture is enabled, the block I/O BAR, and
 * the sparse I/O ports at 0x02EC + n * 0x400. Offsets below are byte
 * offsets inside the 1 KB "block 0" (the CRTC, DAC, configuration and GUI
 * engine registers; bit 10 of the 2 KB offset set); "block 1" (bit 10
 * clear) holds the video overlay and scaler of the VT parts.
 *
 * Names are ATI's, from the mach64 Register Reference Guide; the
 * behaviour is modelled on 86Box's vid_ati_mach64.c.
 */

#if !defined(INCLUDED_MACH64_REGS_H)
#define INCLUDED_MACH64_REGS_H

#include "datatypes.hpp"

namespace mach64 {

constexpr u16 PCI_VENDOR_ATI = 0x1002;
constexpr u16 PCI_DEVICE_CT = 0x4354;  ///< "TC": Mach64 CT, the part SRM names
constexpr u16 PCI_DEVICE_VT2 = 0x5654; ///< "TV": 264VT2
constexpr u16 PCI_DEVICE_VT3 = 0x5655; ///< "UV": 264VT3

/// The register file as seen through every path (2 KB).
constexpr u32 REG_BLOCK_BYTES = 0x800;
constexpr u32 REG_BLOCK0 = 0x400; ///< bit 10: block 0 (main), else block 1

/// Memory aperture (BAR0): two 8 MB halves, little-endian then big-endian,
/// each ending in the 4 KB register page.
constexpr u32 APERTURE_BYTES = 16u << 20;
constexpr u32 APERTURE_HALF = 8u << 20;
constexpr u32 APERTURE_REG_PAGE = 0x1000;

/// Sparse I/O: 32 groups of four ports, 0x400 apart, from this base.
constexpr u32 SPARSE_IO_BASE = 0x02ec;
constexpr int SPARSE_IO_GROUPS = 32;

/// ATI extended VGA registers (index/data).
constexpr u32 ATI_EXT_INDEX = 0x1ce;

// --- block 0: CRTC ---------------------------------------------------------
constexpr u32 CRTC_H_TOTAL_DISP = 0x00;
constexpr u32 CRTC_H_SYNC_STRT_WID = 0x04;
constexpr u32 CRTC_V_TOTAL_DISP = 0x08;
constexpr u32 CRTC_V_SYNC_STRT_WID = 0x0c;
constexpr u32 CRTC_VLINE_CRNT_VLINE = 0x10;
constexpr u32 CRTC_OFF_PITCH = 0x14;
constexpr u32 CRTC_INT_CNTL = 0x18;
constexpr u32 CRTC_GEN_CNTL = 0x1c;
constexpr u32 DSP_CONFIG = 0x20;
constexpr u32 DSP_ON_OFF = 0x24;
constexpr u32 OVR_CLR = 0x40;
constexpr u32 OVR_WID_LEFT_RIGHT = 0x44;
constexpr u32 OVR_WID_TOP_BOTTOM = 0x48;
constexpr u32 VGA_DSP_CONFIG = 0x4c;
constexpr u32 VGA_DSP_ON_OFF = 0x50;
constexpr u32 CUR_CLR0 = 0x60;
constexpr u32 CUR_CLR1 = 0x64;
constexpr u32 CUR_OFFSET = 0x68;
constexpr u32 CUR_HORZ_VERT_POSN = 0x6c;
constexpr u32 CUR_HORZ_VERT_OFF = 0x70;
constexpr u32 GP_IO = 0x78;
constexpr u32 SCRATCH_REG0 = 0x80;
constexpr u32 SCRATCH_REG1 = 0x84;
constexpr u32 CLOCK_CNTL = 0x90;
constexpr u32 BUS_CNTL = 0xa0;
constexpr u32 MEM_CNTL = 0xb0;
constexpr u32 MEM_VGA_WP_SEL = 0xb4;
constexpr u32 MEM_VGA_RP_SEL = 0xb8;
constexpr u32 DAC_REGS = 0xc0;
constexpr u32 DAC_CNTL = 0xc4;
constexpr u32 GEN_TEST_CNTL = 0xd0;
constexpr u32 CONFIG_CNTL = 0xdc;
constexpr u32 CONFIG_CHIP_ID = 0xe0;
constexpr u32 CONFIG_STAT0 = 0xe4;
constexpr u32 CONFIG_STAT1 = 0xe8;

// --- block 0: GUI engine (0x100..0x3ff) --------------------------------------
constexpr u32 GUI_FIRST = 0x100;
constexpr u32 DST_OFF_PITCH = 0x100;
constexpr u32 DST_X = 0x104;
constexpr u32 DST_Y = 0x108;
constexpr u32 DST_Y_X = 0x10c;
constexpr u32 DST_WIDTH = 0x110;
constexpr u32 DST_HEIGHT = 0x114;
constexpr u32 DST_HEIGHT_WIDTH = 0x118;
constexpr u32 DST_X_WIDTH = 0x11c;
constexpr u32 DST_BRES_LNTH = 0x120;
constexpr u32 DST_BRES_ERR = 0x124;
constexpr u32 DST_BRES_INC = 0x128;
constexpr u32 DST_BRES_DEC = 0x12c;
constexpr u32 DST_CNTL = 0x130;
constexpr u32 SRC_OFF_PITCH = 0x180;
constexpr u32 SRC_X = 0x184;
constexpr u32 SRC_Y = 0x188;
constexpr u32 SRC_Y_X = 0x18c;
constexpr u32 SRC_WIDTH1 = 0x190;
constexpr u32 SRC_HEIGHT1 = 0x194;
constexpr u32 SRC_HEIGHT1_WIDTH1 = 0x198;
constexpr u32 SRC_X_START = 0x19c;
constexpr u32 SRC_Y_START = 0x1a0;
constexpr u32 SRC_Y_X_START = 0x1a4;
constexpr u32 SRC_WIDTH2 = 0x1a8;
constexpr u32 SRC_HEIGHT2 = 0x1ac;
constexpr u32 SRC_HEIGHT2_WIDTH2 = 0x1b0;
constexpr u32 SRC_CNTL = 0x1b4;
constexpr u32 HOST_DATA0 = 0x200;
constexpr u32 HOST_DATA_LAST = 0x23f;
constexpr u32 HOST_CNTL = 0x240;
constexpr u32 PAT_REG0 = 0x280;
constexpr u32 PAT_REG1 = 0x284;
constexpr u32 PAT_CNTL = 0x288;
constexpr u32 SC_LEFT = 0x2a0;
constexpr u32 SC_RIGHT = 0x2a4;
constexpr u32 SC_LEFT_RIGHT = 0x2a8;
constexpr u32 SC_TOP = 0x2ac;
constexpr u32 SC_BOTTOM = 0x2b0;
constexpr u32 SC_TOP_BOTTOM = 0x2b4;
constexpr u32 DP_BKGD_CLR = 0x2c0;
constexpr u32 DP_FRGD_CLR = 0x2c4;
constexpr u32 DP_WRITE_MASK = 0x2c8;
constexpr u32 DP_CHAIN_MASK = 0x2cc;
constexpr u32 DP_PIX_WIDTH = 0x2d0;
constexpr u32 DP_MIX = 0x2d4;
constexpr u32 DP_SRC = 0x2d8;
constexpr u32 DST_X_Y = 0x2e8;          ///< DST_Y_X with the halves swapped
constexpr u32 DST_WIDTH_HEIGHT = 0x2ec; ///< DST_HEIGHT_WIDTH, halves swapped
constexpr u32 DP_SET_GUI_ENGINE = 0x2fc;
constexpr u32 CLR_CMP_CLR = 0x300;
constexpr u32 CLR_CMP_MASK = 0x304;
constexpr u32 CLR_CMP_CNTL = 0x308;
constexpr u32 FIFO_STAT = 0x310;
constexpr u32 CONTEXT_MASK = 0x320;
constexpr u32 CONTEXT_LOAD_CNTL = 0x32c;
constexpr u32 GUI_TRAJ_CNTL = 0x330;
constexpr u32 GUI_STAT = 0x338;

// --- CRTC_GEN_CNTL bits
// -------------------------------------------------------
constexpr u32 CRTC_DBL_SCAN_EN = 1u << 0;
constexpr u32 CRTC_INTERLACE_EN = 1u << 1;
constexpr u32 CRTC_HSYNC_DIS = 1u << 2;
constexpr u32 CRTC_VSYNC_DIS = 1u << 3;
constexpr u32 CRTC_DISPLAY_DIS = 1u << 6;
constexpr u32 CRTC_PIX_WIDTH_SHIFT = 8;
constexpr u32 CRTC_PIX_WIDTH_MASK = 7u << 8;
constexpr u32 CRTC_EXT_DISP_EN = 1u << 24;
constexpr u32 CRTC_EN = 1u << 25;

/// Pixel widths, CRTC_GEN_CNTL bits 10..8 and the DP_PIX_WIDTH fields.
enum pix_width : u8 {
  BPP_1 = 0,
  BPP_4 = 1,
  BPP_8 = 2,
  BPP_15 = 3,
  BPP_16 = 4,
  BPP_24 = 5,
  BPP_32 = 6
};

// --- CRTC_INT_CNTL bits (byte)
// --------------------------------------------------
constexpr u8 CRTC_VBLANK = 1u << 0; ///< in vertical blank now (read only)
constexpr u8 CRTC_VBLANK_INT_EN = 1u << 1;
constexpr u8 CRTC_VBLANK_INT = 1u << 2;   ///< a vblank happened; write 1 clears
constexpr u8 CRTC_INT_STATUS_BITS = 0x75; ///< kept across a write

// --- DAC_CNTL bits
// -------------------------------------------------------------
constexpr u32 DAC_8BIT_EN = 1u << 8;
constexpr u32 DAC_TYPE_SHIFT = 16;
constexpr u32 DAC_TYPE_INTERNAL = 1u
                                  << DAC_TYPE_SHIFT; ///< integrated 24-bit DAC
constexpr u32 DAC_GIO_STATE_1 = 1u << 25;            ///< SDA output level
constexpr u32 DAC_GIO_STATE_0 = 1u << 26;            ///< SCL output level
constexpr u32 DAC_GIO_DIR_1 = 1u << 28;              ///< SDA driven
constexpr u32 DAC_GIO_DIR_0 = 1u << 29;              ///< SCL driven

// --- GEN_TEST_CNTL bits
// -----------------------------------------------------------
constexpr u32 GEN_EEPROM_DATA_OUT = 1u << 0; ///< host -> EEPROM (DI)
constexpr u32 GEN_EEPROM_CLK = 1u << 1;
constexpr u32 GEN_EEPROM_DATA_IN = 1u << 3; ///< EEPROM -> host (DO)
constexpr u32 GEN_EEPROM_CS = 1u << 4;
constexpr u32 GEN_CUR_EN = 1u << 7;
constexpr u32 GEN_GUI_EN = 1u << 8;

// --- CONFIG_CNTL bits
// -------------------------------------------------------------
constexpr u32 CFG_MEM_AP_SIZE_MASK = 0x3;
constexpr u32 CFG_MEM_VGA_AP_EN = 1u
                                  << 2; ///< registers at 0xbf800, banked window
constexpr u32 CFG_MEM_AP_LOC_SHIFT = 4; ///< aperture base, 4 MB units
constexpr u32 CFG_MEM_AP_LOC_MASK = 0x3ff0;

// --- MEM_CNTL: bits 2..0 memory size
// --------------------------------------------
enum mem_size_code : u8 {
  MEM_512K = 0,
  MEM_1M = 1,
  MEM_2M = 2,
  MEM_4M = 3,
  MEM_6M = 4,
  MEM_8M = 5
};

// --- DST_CNTL bits
// -------------------------------------------------------------
constexpr u32 DST_X_DIR = 0x01; ///< left to right
constexpr u32 DST_Y_DIR = 0x02; ///< top to bottom
constexpr u32 DST_Y_MAJOR = 0x04;
constexpr u32 DST_X_TILE = 0x08;
constexpr u32 DST_Y_TILE = 0x10;
constexpr u32 DST_LAST_PEL = 0x20;
constexpr u32 DST_POLYGON_EN = 0x40;
constexpr u32 DST_24_ROT_EN = 0x80;

// --- SRC_CNTL bits
// -------------------------------------------------------------
constexpr u32 SRC_PATT_EN = 1u << 0;
constexpr u32 SRC_PATT_ROT_EN = 1u << 1;
constexpr u32 SRC_LINEAR_EN = 1u << 2;
constexpr u32 SRC_BYTE_ALIGN = 1u << 3;
constexpr u32 SRC_8X8X8_BRUSH = 1u << 5;
constexpr u32 SRC_8X8X8_BRUSH_LOADED = 1u << 12;

// --- HOST_CNTL bits
// ------------------------------------------------------------
constexpr u32 HOST_BYTE_ALIGN = 1u << 0;

// --- DP_PIX_WIDTH bits
// ------------------------------------------------------------
constexpr u32 DP_BYTE_PIX_ORDER = 1u << 24; ///< LSB first in monochrome data

/// DP_SRC colour sources (bits 2..0 background, 10..8 foreground).
enum src_sel : u8 {
  SRC_BG = 0,
  SRC_FG = 1,
  SRC_HOST = 2,
  SRC_BLITSRC = 3,
  SRC_PAT = 4
};

/// DP_SRC monochrome source (bits 18..16): what chooses fg over bg.
enum mono_src_sel : u8 {
  MONO_SRC_1 = 0,
  MONO_SRC_PAT = 1,
  MONO_SRC_HOST = 2,
  MONO_SRC_BLITSRC = 3
};

/// Engine operand size per pixel width: 0 byte, 1 word, 2 dword, 3 one bit.
constexpr int WIDTH_1BIT = 3;
constexpr int PIX_WIDTH_SIZE[8] = {WIDTH_1BIT, 0, 0, 1, 1, 2, 2, 0};

} // namespace mach64

#endif // !defined(INCLUDED_MACH64_REGS_H)
