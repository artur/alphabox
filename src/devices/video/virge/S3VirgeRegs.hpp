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

/* S3 ViRGE family register offsets and fields.
 *
 * Names follow the S3 ViRGE/DX and ViRGE/GX data books where they are
 * known; MMIO offsets are byte offsets into the 64 KB register window
 * ("new MMIO" at BAR0 + 16 MB, or "old MMIO" at 0xa0000 with 0x8000
 * added). Where the data books and the Windows 2000 driver (s3m.sys,
 * s3mvirge.dll) disagree the driver's traffic wins, and the comment says
 * so. */

#if !defined(INCLUDED_S3VIRGEREGS_H)
#define INCLUDED_S3VIRGEREGS_H

#include <cstdint>

#include "datatypes.hpp"

namespace virge {

constexpr u16 PCI_VENDOR_S3 = 0x5333;

// BAR0: 64 MB on every part (the ViRGE and ViRGE/VX data books, 15.1.2).
// The low 16 MB are the linear framebuffer, the next 16 MB the MMIO
// window; the upper half repeats both with the bytes of each dword
// swapped, for big-endian hosts.
constexpr u32 BAR0_BYTES = 64u << 20;
constexpr u32 LFB_SPAN = 16u << 20;
constexpr u32 MMIO_OFFSET = 16u << 20;
constexpr u32 MMIO_BYTES = 64u << 10;
constexpr u32 BIG_ENDIAN_HALF = 32u << 20;

// --- CRTC extensions (3d4/3d5) -----------------------------------------
constexpr u8 CR_DEVICE_ID_HIGH = 0x2d;
constexpr u8 CR_DEVICE_ID_LOW = 0x2e;
constexpr u8 CR_REVISION = 0x2f;
constexpr u8 CR_CHIP_ID = 0x30; ///< 0xe1: the ViRGE family
constexpr u8 CR_MEM_CONFIG = 0x31;
constexpr u8 CR31_ENHANCED_MAP = 0x08; ///< banked 64 KB at 0xa0000
constexpr u8 CR_BKWD_1 = 0x32;
constexpr u8 CR32_INT_ENABLE = 0x10; ///< INTA follows the subsystem status
constexpr u8 CR_BKWD_2 = 0x33;
constexpr u8 CR_BKWD_3 = 0x34;
constexpr u8 CR_CRT_LOCK = 0x35; ///< bits 3..0: bank (old style)
constexpr u8 CR_CONFIG_1 = 0x36;
constexpr u8 CR_CONFIG_2 = 0x37;
constexpr u8 CR_LOCK_1 = 0x38; ///< 0x48 unlocks CR2D-CR3F
constexpr u8 CR_LOCK_2 = 0x39; ///< 0xa? unlocks CR40 up, 0xa5 also CR36
constexpr u8 CR_MISC_1 = 0x3a;
constexpr u8 CR3A_ENHANCED_256 = 0x10;
constexpr u8 CR_CURSOR_MODE = 0x45;
constexpr u8 CR_CURSOR_X_HI = 0x46;
constexpr u8 CR_CURSOR_X_LO = 0x47;
constexpr u8 CR_CURSOR_Y_HI = 0x48;
constexpr u8 CR_CURSOR_Y_LO = 0x49;
constexpr u8 CR_CURSOR_FG = 0x4a;
constexpr u8 CR_CURSOR_BG = 0x4b;
constexpr u8 CR_CURSOR_ADDR_HI = 0x4c;
constexpr u8 CR_CURSOR_ADDR_LO = 0x4d;
constexpr u8 CR_CURSOR_PAT_X = 0x4e;
constexpr u8 CR_CURSOR_PAT_Y = 0x4f;
constexpr u8 CR_EXT_SYS_CTL_2 = 0x51;
constexpr u8 CR_EXT_MEM_CTL_1 = 0x53;
constexpr u8 CR53_NEW_MMIO = 0x08; ///< MMIO at BAR0 + 16 MB
constexpr u8 CR53_OLD_MMIO = 0x10; ///< MMIO at 0xa0000 (0xa8000 = 0x8000)
constexpr u8 CR53_OLD_MMIO_B8 = 0x20;
constexpr u8 CR_EXT_DAC_CTL = 0x55;
constexpr u8 CR55_CURSOR_X11 = 0x10;
constexpr u8 CR_LAW_CTL = 0x58;
constexpr u8 CR58_LAW_ENABLE = 0x10;
constexpr u8 CR_LAW_POS_HI = 0x59;
constexpr u8 CR_LAW_POS_LO = 0x5a;
constexpr u8 CR_GENERAL_OUT = 0x5c;
constexpr u8 CR_EXT_H_OVF = 0x5d;
constexpr u8 CR_EXT_V_OVF = 0x5e;
constexpr u8 CR_EXT_MISC_1 = 0x66;
constexpr u8 CR66_ENHANCED = 0x01;
constexpr u8 CR66_ENGINE_RESET = 0x02;
constexpr u8 CR_EXT_MISC_2 = 0x67; ///< colour mode 7..4, streams 3..2
constexpr u8 CR67_STREAMS = 0x0c;
constexpr u8 CR_CONFIG_3 = 0x68;
constexpr u8 CR_EXT_SYS_CTL_3 = 0x69; ///< display start bits 20..16
constexpr u8 CR_EXT_SYS_CTL_4 = 0x6a; ///< bank, 64 KB units
constexpr u8 CR_CONFIG_4 = 0x6f;

// --- sequencer extensions (3c4/3c5) -------------------------------------
constexpr u8 SR_UNLOCK = 0x08; ///< 0x06 unlocks SR09 up
constexpr u8 SR_MCLK_N = 0x10; ///< R 6..5 (7..5 on the DX), N 4..0
constexpr u8 SR_MCLK_M = 0x11;
constexpr u8 SR_DCLK_N = 0x12;
constexpr u8 SR_DCLK_M = 0x13;
constexpr u8 SR_CLKSYN_1 = 0x14;
constexpr u8 SR_CLKSYN_2 = 0x15; ///< bit 1 DCLK load, bit 5 load both now
constexpr u8 SR_RAMDAC_CTL = 0x18;
constexpr u8 SR_CLKSYN_EXT = 0x29; ///< GX2: bit 0 is the DCLK PLL's R bit 2

// --- MMIO: streams processor (0x8180-0x81ff) ------------------------------
constexpr u32 PRI_STREAM_CTL = 0x8180;
constexpr u32 COLOR_KEY_CTL = 0x8184;
constexpr u32 SEC_STREAM_CTL = 0x8190;
constexpr u32 CHROMA_KEY_UPPER = 0x8194;
constexpr u32 SEC_STREAM_STRETCH = 0x8198;
constexpr u32 BLEND_CTL = 0x81a0;
constexpr u32 PRI_FB_ADDR0 = 0x81c0;
constexpr u32 PRI_FB_ADDR1 = 0x81c4;
constexpr u32 PRI_STRIDE = 0x81c8;
constexpr u32 DOUBLE_BUFFER = 0x81cc;
constexpr u32 SEC_FB_ADDR0 = 0x81d0;
constexpr u32 SEC_FB_ADDR1 = 0x81d4;
constexpr u32 SEC_STRIDE = 0x81d8;
constexpr u32 OPAQUE_OVERLAY = 0x81dc;
constexpr u32 K1_VSCALE = 0x81e0;
constexpr u32 K2_VSCALE = 0x81e4;
constexpr u32 DDA_VACCUM = 0x81e8;
constexpr u32 STREAMS_FIFO = 0x81ec;
constexpr u32 PRI_START = 0x81f0;
constexpr u32 PRI_SIZE = 0x81f4;
constexpr u32 SEC_START = 0x81f8;
constexpr u32 SEC_SIZE = 0x81fc;

// --- MMIO: memory port controller, VGA, system control -------------------
constexpr u32 MMIO_VGA_FIRST = 0x83b0; ///< the VGA ports 0x3b0-0x3df
constexpr u32 MMIO_VGA_LAST = 0x83df;
constexpr u32 SUBSYS_STATUS = 0x8504; ///< read status, write control
constexpr u32 ADV_FUNC_CTL = 0x850c;
constexpr u32 DMA_BASE = 0x8590; ///< command DMA
constexpr u32 DMA_WRITE_PTR = 0x8594;
constexpr u32 DMA_READ_PTR = 0x8598;
constexpr u32 DMA_ENABLE = 0x859c;
constexpr u32 SERIAL_PORT = 0xff20;

// Subsystem status bits 7..0 (interrupt status; write 1 to clear) and, in
// the high byte of a write, the matching enables.
constexpr u32 INT_VSYNC = 0x01;
constexpr u32 INT_3D_DONE = 0x02;
constexpr u32 INT_FIFO_OVERFLOW = 0x04;
constexpr u32 INT_FIFO_EMPTY = 0x08;
constexpr u32 INT_HOST_DMA_DONE = 0x10;
constexpr u32 INT_CMD_DMA_DONE = 0x20;
constexpr u32 INT_3D_FIFO_EMPTY = 0x40;
constexpr u32 INT_LPB = 0x80;
/// Status bits 15..8 of an idle engine: the FIFO's free slots (bits
/// 12..8, 16) and the engine idle bit 13, with 15..14 set as the
/// hardware reads them.
constexpr u32 STATUS_IDLE = 0xf000;

// Advanced function control.
constexpr u32 AFC_ENHANCED = 0x01;
constexpr u32 AFC_LAW_ENABLE = 0x10;  ///< the same as CR58 bit 4
constexpr u32 AFC_MMIO_ENABLE = 0x20; ///< the same as CR53 bit 4

// The serial port (DDC/I2C): write clock and data, read them back.
constexpr u32 SP_SCW = 0x01;
constexpr u32 SP_SDW = 0x02;
constexpr u32 SP_SCR = 0x04;
constexpr u32 SP_SDR = 0x08;
constexpr u32 SP_ENABLE = 0x10;

// --- MMIO: the S3d engine --------------------------------------------------
constexpr u32 IMAGE_WINDOW_BYTES = 0x8000; ///< host data, any address below
constexpr u32 PATTERN_RAM = 0xa000;        ///< 8x8 colour pattern, to 0xa1ff
constexpr u32 PATTERN_RAM_END = 0xa200;

// 2D register sets: BitBLT/rectangle fill at 0xa4xx-0xa5xx, line at
// 0xa8xx-0xa9xx, polygon at 0xacxx-0xadxx. The registers from 0x..d4 to
// 0x..fc and the command at 0x..00 are one register behind three names;
// the rest belong to one command each.
constexpr u32 S2D_SRC_BASE = 0xa4d4;
constexpr u32 S2D_DEST_BASE = 0xa4d8;
constexpr u32 S2D_CLIP_LR = 0xa4dc;
constexpr u32 S2D_CLIP_TB = 0xa4e0;
constexpr u32 S2D_STRIDE = 0xa4e4; ///< dest 27..16, source 11..0
constexpr u32 S2D_MONO_PAT0 = 0xa4e8;
constexpr u32 S2D_MONO_PAT1 = 0xa4ec;
constexpr u32 S2D_PAT_BG = 0xa4f0;
constexpr u32 S2D_PAT_FG = 0xa4f4;
constexpr u32 S2D_SRC_BG = 0xa4f8;
constexpr u32 S2D_SRC_FG = 0xa4fc;
constexpr u32 S2D_CMD = 0xa500;
constexpr u32 BLT_WIDTH_HEIGHT = 0xa504;
constexpr u32 BLT_SRC_XY = 0xa508;
constexpr u32 BLT_DEST_XY = 0xa50c;
constexpr u32 LINE_XEND = 0xa96c;
constexpr u32 LINE_DXDY = 0xa970;
constexpr u32 LINE_XSTART = 0xa974;
constexpr u32 LINE_YSTART = 0xa978;
constexpr u32 LINE_YCOUNT = 0xa97c;
constexpr u32 POLY_RIGHT_DX = 0xad68;
constexpr u32 POLY_RIGHT_XSTART = 0xad6c;
constexpr u32 POLY_LEFT_DX = 0xad70;
constexpr u32 POLY_LEFT_XSTART = 0xad74;
constexpr u32 POLY_YSTART = 0xad78;
constexpr u32 POLY_YCOUNT = 0xad7c;

// 3D register sets: 3D line at 0xb0xx-0xb1xx, triangle at 0xb4xx-0xb5xx;
// 0x..d4 to 0x..fc and the command are shared, as in 2D.
constexpr u32 S3D_Z_BASE = 0xb4d4;
constexpr u32 S3D_DEST_BASE = 0xb4d8;
constexpr u32 S3D_CLIP_LR = 0xb4dc;
constexpr u32 S3D_CLIP_TB = 0xb4e0;
constexpr u32 S3D_DEST_STRIDE = 0xb4e4;
constexpr u32 S3D_Z_STRIDE = 0xb4e8;
constexpr u32 S3D_TEX_BASE = 0xb4ec;
constexpr u32 S3D_TEX_BORDER = 0xb4f0;
constexpr u32 S3D_FOG_COLOR = 0xb4f4;
constexpr u32 S3D_COLOR0 = 0xb4f8;
constexpr u32 S3D_COLOR1 = 0xb4fc;
constexpr u32 S3D_CMD = 0xb500;

// The command set register (2D and 3D).
constexpr u32 CMD_AUTOEXEC = 1u << 0;
constexpr u32 CMD_CLIP = 1u << 1;
constexpr u32 CMD_FORMAT_SHIFT = 2; ///< 3 bits: 0 8 bpp, 1 16, 2 24
constexpr u32 CMD_DRAW_ENABLE = 1u << 5;
constexpr u32 CMD_MONO_SRC = 1u << 6;
constexpr u32 CMD_HOST_SRC = 1u << 7;
constexpr u32 CMD_MONO_PAT = 1u << 8;
constexpr u32 CMD_TRANSPARENT = 1u << 9;
constexpr u32 CMD_ALIGN_SHIFT = 10; ///< host data padding: byte, word, dword
constexpr u32 CMD_SKIP_SHIFT = 12;  ///< bytes skipped before the first line
constexpr u32 CMD_ROP_SHIFT = 17;
constexpr u32 CMD_X_POSITIVE = 1u << 25;
constexpr u32 CMD_Y_POSITIVE = 1u << 26;
constexpr u32 CMD_COMMAND_SHIFT = 27;
constexpr u32 CMD_3D = 1u << 31;

enum Command2D : u32 {
  CMD2D_BITBLT = 0,
  CMD2D_RECT = 2,
  CMD2D_LINE = 3,
  CMD2D_POLY = 5,
  CMD2D_NOP = 15,
};

} // namespace virge

#endif // !defined(INCLUDED_S3VIRGEREGS_H)
