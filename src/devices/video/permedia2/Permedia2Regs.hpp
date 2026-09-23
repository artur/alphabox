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

} // namespace permedia2

#endif // !defined(INCLUDED_PERMEDIA2REGS_H)
