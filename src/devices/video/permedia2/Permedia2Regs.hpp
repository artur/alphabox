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

/* Permedia 2 (TVP4020) register offsets and fields, as the TVP4020
 * Hardware Reference Manual (3Dlabs/TI) names them. Region 0 offsets are
 * byte offsets into the 64 KB control space; the second 64 KB of the
 * region is the same registers byte-swapped. */

#if !defined(INCLUDED_PERMEDIA2REGS_H)
#define INCLUDED_PERMEDIA2REGS_H

#include <cstdint>

#include "datatypes.hpp"

namespace permedia2 {

constexpr u16 PCI_VENDOR_TI = 0x104c;
constexpr u16 PCI_DEVICE_PERMEDIA2 = 0x3d07;
constexpr u8 PCI_REVISION = 0x01;

constexpr u32 REGION0_BYTES = 128u << 10; // the registers, then byte-swapped
constexpr u32 REGION0_HALF = 64u << 10;
constexpr u32 APERTURE_BYTES = 8u << 20; // regions 1 and 2, fixed size
constexpr u32 ROM_BYTES = 64u << 10;

// PCI configuration: the indirect window onto the regions (2.14.24).
constexpr u32 CFG_INDIRECT_ADDR = 0xf8;
constexpr u32 CFG_INDIRECT_DATA = 0xfc;

// --- region 0 blocks (3.1) ---------------------------------------------
constexpr u32 R0_CONTROL = 0x0000; // control status
constexpr u32 R0_MEMORY = 0x1000;  // memory control
constexpr u32 R0_GP_FIFO = 0x2000; // graphics processor FIFO access
constexpr u32 R0_VIDEO = 0x3000;   // video timing generator
constexpr u32 R0_RAMDAC = 0x4000;
constexpr u32 R0_STREAMS = 0x5000;          // video streams (and 0x5800)
constexpr u32 R0_SVGA = 0x6000;             // the VGA's I/O ports, bits 9..0
constexpr u32 R0_GP = 0x8000;               // graphics processor registers
constexpr u32 R0_CONTROL_WORDS = R0_GP / 4; // 0x0000-0x7fff, kept as dwords

// --- control status (3.2) ---------------------------------------------
constexpr u32 RESET_STATUS = 0x0000;
constexpr u32 INT_ENABLE = 0x0008;
constexpr u32 INT_FLAGS = 0x0010;
constexpr u32 IN_FIFO_SPACE = 0x0018;
constexpr u32 OUT_FIFO_WORDS = 0x0020;
constexpr u32 DMA_ADDRESS = 0x0028;
constexpr u32 DMA_COUNT = 0x0030;
constexpr u32 ERROR_FLAGS = 0x0038;
constexpr u32 VCLK_CTL = 0x0040;
constexpr u32 TEST_REGISTER = 0x0048;
constexpr u32 APERTURE_ONE = 0x0050;
constexpr u32 APERTURE_TWO = 0x0058;
constexpr u32 DMA_CONTROL = 0x0060;
constexpr u32 FIFO_DISCON = 0x0068;
constexpr u32 CHIP_CONFIG = 0x0070;

// IntEnable / IntFlags bits (3.2.2, 3.2.3).
constexpr u32 INT_DMA = 1u << 0;
constexpr u32 INT_SYNC = 1u << 1;
constexpr u32 INT_ERROR = 1u << 3;
constexpr u32 INT_VRETRACE = 1u << 4;
constexpr u32 INT_SCANLINE = 1u << 5;

// ApertureOne / ApertureTwo (3.2.11, 3.2.12).
constexpr u32 AP_BYTE_CONTROL = 3u << 0; // 0 standard, 1 bytes, 2 halfwords
constexpr u32 AP_PACKED16 = 1u << 3;
constexpr u32 AP_SVGA = 1u << 8;
constexpr u32 AP_ROM = 1u << 9;
constexpr u32 AP_WRITABLE = 0x3fb;

// ChipConfig (3.2.15): what the board's configuration resistors say at
// reset. A PCI board with its SVGA on, at the fixed VGA addresses, taking
// its subsystem IDs from the ROM.
constexpr u32 CC_BASE_CLASS_ZERO = 1u << 0;
constexpr u32 CC_VGA_ENABLE = 1u << 1;
constexpr u32 CC_VGA_FIXED = 1u << 2;
constexpr u32 CC_SUBSYSTEM_FROM_ROM = 1u << 12;
constexpr u32 CHIP_CONFIG_RESET =
    CC_VGA_ENABLE | CC_VGA_FIXED | CC_SUBSYSTEM_FROM_ROM;

// --- memory control (3.3) -----------------------------------------------
constexpr u32 REBOOT = 0x1000;
constexpr u32 MEM_CONTROL = 0x1040;
constexpr u32 BOOT_ADDRESS = 0x1080;
constexpr u32 MEM_CONFIG = 0x10c0;
constexpr u32 BYPASS_WRITE_MASK = 0x1100;
constexpr u32 FB_WRITE_MASK = 0x1140;
constexpr u32 COUNT = 0x1180;
constexpr u32 MEM_CONFIG_RESET = 0x259fffff;
constexpr int MEM_CONFIG_BANKS_SHIFT = 29; // banks - 1, 2 MB each

// --- video timing generator (3.4) ----------------------------------------
constexpr u32 SCREEN_BASE = 0x3000;
constexpr u32 SCREEN_STRIDE = 0x3008;
constexpr u32 H_TOTAL = 0x3010;
constexpr u32 HG_END = 0x3018;
constexpr u32 HB_END = 0x3020;
constexpr u32 HS_START = 0x3028;
constexpr u32 HS_END = 0x3030;
constexpr u32 V_TOTAL = 0x3038;
constexpr u32 VB_END = 0x3040;
constexpr u32 VS_START = 0x3048;
constexpr u32 VS_END = 0x3050;
constexpr u32 VIDEO_CONTROL = 0x3058;
constexpr u32 INTERRUPT_LINE = 0x3060;
constexpr u32 DISPLAY_DATA = 0x3068;
constexpr u32 LINE_COUNT = 0x3070;
constexpr u32 FIFO_CONTROL = 0x3078;
constexpr u32 SCREEN_BASE_RIGHT = 0x3080;

constexpr u32 VC_ENABLE = 1u << 0;
constexpr u32 VC_LINE_DOUBLE = 1u << 2;
constexpr u32 VC_BYPASS_PENDING = 1u << 7;
constexpr u32 VC_GP_PENDING = 1u << 8;
constexpr u32 VC_DATA64 = 1u << 16;

// DisplayData (3.4.14): the DDC lines.
constexpr u32 DD_DATA_IN = 1u << 0;
constexpr u32 DD_CLK_IN = 1u << 1;
constexpr u32 DD_DATA_OUT = 1u << 2; // 1: released (tri-state)
constexpr u32 DD_CLK_OUT = 1u << 3;

// --- video streams (3.5.5) ---------------------------------------------
constexpr u32 VS_CONFIGURATION = 0x5800;
constexpr u32 VS_SERIAL_BUS_CONTROL = 0x5810;

// --- RAMDAC (3.6, 5.4): direct registers every 8 bytes -------------------
enum RamdacDirect {
  RD_PALETTE_WRITE_ADDRESS = 0x0,
  RD_PALETTE_DATA = 0x1,
  RD_PIXEL_MASK = 0x2,
  RD_PALETTE_READ_ADDRESS = 0x3,
  RD_CURSOR_COLOR_ADDRESS = 0x4,
  RD_CURSOR_COLOR_DATA = 0x5,
  RD_INDEXED_DATA = 0xa,
  RD_CURSOR_RAM_DATA = 0xb,
  RD_CURSOR_X_LOW = 0xc,
  RD_CURSOR_X_HIGH = 0xd,
  RD_CURSOR_Y_LOW = 0xe,
  RD_CURSOR_Y_HIGH = 0xf,
};

// Indirect registers, through RD_PALETTE_WRITE_ADDRESS and RD_INDEXED_DATA.
enum RamdacIndexed {
  RDI_CURSOR_CONTROL = 0x06,
  RDI_COLOR_MODE = 0x18,
  RDI_MODE_CONTROL = 0x19,
  RDI_PALETTE_PAGE = 0x1c,
  RDI_MISC_CONTROL = 0x1e,
  RDI_PIXEL_CLOCK_A1 = 0x20, // A1..C3: M, N, P for each of three clocks
  RDI_PIXEL_CLOCK_STATUS = 0x29,
  RDI_MEMORY_CLOCK_1 = 0x30,
  RDI_MEMORY_CLOCK_STATUS = 0x33,
  RDI_COLOR_KEY_CONTROL = 0x40,
};

// Cursor control (3.6.13).
constexpr u8 CUR_MODE = 3u << 0;      // 0 off, 1 three colour, 2 XGA, 3 X11
constexpr int CUR_RAM_ADDR_SHIFT = 2; // bits 3..2: RAM address bits 9..8
constexpr int CUR_SELECT_SHIFT = 4;   // which 32x32 cursor
constexpr u8 CUR_SIZE_64 = 1u << 6;

// Colour mode (3.6.14).
constexpr u8 CM_FORMAT = 0x0f;
constexpr u8 CM_GUI = 1u << 4;
constexpr u8 CM_RGB = 1u << 5;
constexpr u8 CM_TRUECOLOR_PALETTE = 1u << 7;
enum PixelFormat {
  PF_CI8 = 0,
  PF_RGB332 = 1,
  PF_RGB232_OFFSET = 2,
  PF_RGBA2321 = 3,
  PF_RGBA5551 = 4,
  PF_RGBA4444 = 5,
  PF_RGB565 = 6,
  PF_RGBA8888 = 8,
  PF_RGB888 = 9,
};

// Miscellaneous control (3.6.17).
constexpr u8 MISC_PALETTE_8BIT = 1u << 1;

constexpr u8 PLL_LOCKED = 1u << 4;

// --- SVGA extensions (3.5.3, 3.5.4) --------------------------------------
constexpr int SEQ_VGA_CONTROL = 5; // VGAControlReg
constexpr u8 VGACTL_HOST_MEMORY = 1u << 0;
constexpr u8 VGACTL_HOST_DAC = 1u << 1;
constexpr u8 VGACTL_INTERRUPTS = 1u << 2;
constexpr u8 VGACTL_VGA_DISPLAY = 1u << 3; // 0: the graphics processor's
constexpr int VGACTL_DAC_ADDR_SHIFT = 4;   // bits 5..4: RAMDAC address 3..2
constexpr u8 VGACTL_VTG = 1u << 6;
constexpr u8 VGACTL_RESET = 0x4b;

constexpr int GC_MODE640 = 9; // Mode640Reg
constexpr u8 M640_BANK_A = 7u << 0;
constexpr int M640_BANK_B_SHIFT = 3;
constexpr u8 M640_START_BIT16 = 1u << 6;
constexpr u8 M640_ENABLE = 1u << 7;


// --- graphics processor register tags (Programmer's Reference ch. 8) -----
// A register's region 0 offset is R0_GP + tag * 8.
enum GpTag : u32 {
  T_START_X_DOM = 0x000,
  T_DX_DOM = 0x001,
  T_START_X_SUB = 0x002,
  T_DX_SUB = 0x003,
  T_START_Y = 0x004,
  T_DY = 0x005,
  T_COUNT = 0x006,
  T_RENDER = 0x007,
  T_CONTINUE_NEW_LINE = 0x008,
  T_CONTINUE_NEW_DOM = 0x009,
  T_CONTINUE_NEW_SUB = 0x00a,
  T_CONTINUE = 0x00b,
  T_BIT_MASK_PATTERN = 0x00d,
  T_RASTERIZER_MODE = 0x014,
  T_Y_LIMITS = 0x015,
  T_WAIT_FOR_COMPLETION = 0x017,
  T_X_LIMITS = 0x019,
  T_RECTANGLE_ORIGIN = 0x01a,
  T_RECTANGLE_SIZE = 0x01b,
  T_PACKED_DATA_LIMITS = 0x02a,
  T_SCISSOR_MODE = 0x030,
  T_SCISSOR_MIN_XY = 0x031,
  T_SCISSOR_MAX_XY = 0x032,
  T_SCREEN_SIZE = 0x033,
  T_AREA_STIPPLE_MODE = 0x034,
  T_WINDOW_ORIGIN = 0x039,
  T_AREA_STIPPLE_PATTERN0 = 0x040, // 0x040-0x047
  T_TEXTURE_ADDRESS_MODE = 0x070,
  T_S_START = 0x071,
  T_DS_DX = 0x072,
  T_DS_DY_DOM = 0x073,
  T_T_START = 0x074,
  T_DT_DX = 0x075,
  T_DT_DY_DOM = 0x076,
  T_Q_START = 0x077,
  T_DQ_DX = 0x078,
  T_DQ_DY_DOM = 0x079,
  T_TEXEL_LUT_INDEX = 0x098,
  T_TEXEL_LUT_DATA = 0x099,
  T_TEXEL_LUT_ADDRESS = 0x09a,
  T_TEXEL_LUT_TRANSFER = 0x09b,
  T_TEXTURE_BASE_ADDRESS = 0x0b0,
  T_TEXTURE_MAP_FORMAT = 0x0b1,
  T_TEXTURE_DATA_FORMAT = 0x0b2,
  T_TEXEL0 = 0x0c0,
  T_TEXTURE_READ_MODE = 0x0ce,
  T_TEXEL_LUT_MODE = 0x0cf,
  T_TEXTURE_COLOR_MODE = 0x0d0,
  T_FOG_MODE = 0x0d2,
  T_FOG_COLOR = 0x0d3,
  T_F_START = 0x0d4,
  T_DF_DX = 0x0d5,
  T_DF_DY_DOM = 0x0d6,
  T_R_START = 0x0f0,
  T_DR_DX = 0x0f1,
  T_DR_DY_DOM = 0x0f2,
  T_G_START = 0x0f3,
  T_DG_DX = 0x0f4,
  T_DG_DY_DOM = 0x0f5,
  T_B_START = 0x0f6,
  T_DB_DX = 0x0f7,
  T_DB_DY_DOM = 0x0f8,
  T_A_START = 0x0f9,
  T_COLOR_DDA_MODE = 0x0fc,
  T_CONSTANT_COLOR = 0x0fd,
  T_COLOR = 0x0fe,
  T_ALPHA_BLEND_MODE = 0x102,
  T_DITHER_MODE = 0x103,
  T_FB_SOFTWARE_WRITE_MASK = 0x104,
  T_LOGICAL_OP_MODE = 0x105,
  T_FB_WRITE_DATA = 0x106,
  T_LB_READ_MODE = 0x110,
  T_LB_READ_FORMAT = 0x111,
  T_LB_SOURCE_OFFSET = 0x112,
  T_LB_WINDOW_BASE = 0x117,
  T_LB_WRITE_MODE = 0x118,
  T_LB_WRITE_FORMAT = 0x119,
  T_TEXTURE_DATA = 0x11d,
  T_TEXTURE_DOWNLOAD_OFFSET = 0x11e,
  T_WINDOW = 0x130,
  T_STENCIL_MODE = 0x131,
  T_STENCIL_DATA = 0x132,
  T_STENCIL = 0x133,
  T_DEPTH_MODE = 0x134,
  T_DEPTH = 0x135,
  T_Z_START_U = 0x136,
  T_Z_START_L = 0x137,
  T_DZ_DX_U = 0x138,
  T_DZ_DX_L = 0x139,
  T_DZ_DY_DOM_U = 0x13a,
  T_DZ_DY_DOM_L = 0x13b,
  T_FB_READ_MODE = 0x150,
  T_FB_SOURCE_OFFSET = 0x151,
  T_FB_PIXEL_OFFSET = 0x152,
  T_FB_COLOR = 0x153,
  T_FB_DATA = 0x154,
  T_FB_SOURCE_DATA = 0x155,
  T_FB_WINDOW_BASE = 0x156,
  T_FB_WRITE_MODE = 0x157,
  T_FB_HARDWARE_WRITE_MASK = 0x158,
  T_FB_BLOCK_COLOR = 0x159,
  T_FB_READ_PIXEL = 0x15a,
  T_FB_WRITE_CONFIG = 0x15d,
  T_FILTER_MODE = 0x180,
  T_STATISTIC_MODE = 0x181,
  T_MIN_REGION = 0x182,
  T_MAX_REGION = 0x183,
  T_RESET_PICK_RESULT = 0x184,
  T_MIN_HIT_REGION = 0x185,
  T_MAX_HIT_REGION = 0x186,
  T_PICK_RESULT = 0x187,
  T_SYNC = 0x188,
  T_FB_BLOCK_COLOR_U = 0x18d,
  T_FB_BLOCK_COLOR_L = 0x18e,
  T_SUSPEND_UNTIL_FRAME_BLANK = 0x18f,
  T_FB_SOURCE_BASE = 0x1b0,
  T_FB_SOURCE_DELTA = 0x1b1,
  T_CONFIG = 0x1b2,
  T_TEXEL_LUT0 = 0x1d0, // 0x1d0-0x1df
  T_YUV_MODE = 0x1e0,
  T_CHROMA_UPPER = 0x1e1,
  T_CHROMA_LOWER = 0x1e2,
  T_ALPHA_MAP_UPPER = 0x1e3,
  T_ALPHA_MAP_LOWER = 0x1e4,
  T_DELTA_MODE = 0x260,
  T_DRAW_TRIANGLE = 0x261,
  T_REPEAT_TRIANGLE = 0x262,
  T_DRAW_LINE01 = 0x263,
  T_DRAW_LINE10 = 0x264,
  T_REPEAT_LINE = 0x265,
};
constexpr u32 GP_TAGS = 0x400; // 9-bit tags, with room to spare

// Render (Programmer's Reference 7, Render).
constexpr u32 RENDER_AREA_STIPPLE = 1u << 0;
constexpr u32 RENDER_FAST_FILL = 1u << 3;
constexpr int RENDER_PRIMITIVE_SHIFT = 6; // 0 line, 1 trapezoid, 2 point, 3 rectangle
constexpr u32 RENDER_SYNC_ON_BIT_MASK = 1u << 11;
constexpr u32 RENDER_SYNC_ON_HOST_DATA = 1u << 12;
constexpr u32 RENDER_TEXTURE = 1u << 13;
constexpr u32 RENDER_FOG = 1u << 14;
constexpr u32 RENDER_SUBPIXEL_CORRECTION = 1u << 16;
constexpr u32 RENDER_REUSE_BIT_MASK = 1u << 17;
constexpr u32 RENDER_INCREASE_X = 1u << 21;
constexpr u32 RENDER_INCREASE_Y = 1u << 22;
enum Primitive { PRIM_LINE = 0, PRIM_TRAPEZOID = 1, PRIM_POINT = 2, PRIM_RECTANGLE = 3 };

// FBReadMode / FBWriteConfig.
constexpr u32 FBRM_READ_SOURCE = 1u << 9;
constexpr u32 FBRM_READ_DESTINATION = 1u << 10;
constexpr u32 FBRM_DATA_TYPE_COLOR = 1u << 15; // FBColor: an image upload
constexpr u32 FBRM_BOTTOM_LEFT = 1u << 16;
constexpr u32 FBRM_PATCH = 1u << 18;
constexpr u32 FBRM_PACKED_DATA = 1u << 19;
constexpr int FBRM_RELATIVE_OFFSET_SHIFT = 20;

// Config: fields of other registers in one (their order in 7, Config).
constexpr u32 CONFIG_READ_SOURCE = 1u << 0;
constexpr u32 CONFIG_READ_DESTINATION = 1u << 1;
constexpr u32 CONFIG_PACKED_DATA = 1u << 2;
constexpr u32 CONFIG_FB_WRITE = 1u << 3;
constexpr u32 CONFIG_COLOR_DDA = 1u << 4;
constexpr u32 CONFIG_LOGIC_OP_ENABLE = 1u << 5;
constexpr int CONFIG_LOGIC_OP_SHIFT = 6;

} // namespace permedia2

#endif // !defined(INCLUDED_PERMEDIA2REGS_H)
