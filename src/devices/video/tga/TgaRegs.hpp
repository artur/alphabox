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
 * DECchip 21030 (TGA) register numbers and fields, from the DECchip 21030
 * PCI Graphics Accelerator Reference Manual (EC-N0683-72, September 1994):
 * Table 2-2 (the core registers), Table 4-17 (the graphics modes) and the
 * register descriptions of chapter 4. Names are the manual's mnemonics.
 **/

#if !defined(INCLUDED_TGAREGS_H)
#define INCLUDED_TGAREGS_H

#include "datatypes.hpp"

namespace tga {

/// PCI identity (Table 4-1, 4.2.3, 4.2.4).
constexpr u16 PCI_VENDOR_DEC = 0x1011;
constexpr u16 PCI_DEVICE_21030 = 0x0004;
/// The TGA2 of the PowerStorm 3D30 and 4D20 (DEC's "PBXGB").
constexpr u16 PCI_DEVICE_TGA2 = 0x000d;

/// The memory space (2.1): 128 MB, 4 to 32 copies of core space.
constexpr u32 SPACE_BYTES = 128u << 20;
/// The expansion ROM space (4.2.6): 256 KB.
constexpr u32 ROM_BYTES = 256u << 10;

/// Core space (2.2): alternate ROM space in its first megabyte, register
/// space in its second; the frame buffer space is the upper half.
constexpr u32 CORE_ALTROM = 0x000000;
constexpr u32 CORE_REGS = 0x100000;
constexpr u32 REGS_BYTES = 0x100000;

/// TGA2: the first megabyte of core space is the external-device window
/// instead of an alternate ROM (NetBSD's tgareg.h; tga2.sys addresses the
/// same): the ICS9110 clock synthesiser's serial port at 0x60000 and the
/// RAMDAC at 0x80000, a register a 256-byte step from 0x8e000.
constexpr u32 TGA2_EXT_CLOCK = 0x60000;
constexpr u32 TGA2_EXT_RAMDAC = 0x80000;
constexpr u32 TGA2_EXT_WINDOW = 0x20000;

/// Core register numbers: Dword index into a 512-byte register-space core
/// (Table 2-2). Every 512 bytes of the 1 MB register space alias them.
enum Reg : unsigned {
  GCBR0 = 0x00,  // copy buffer 0..7
  GFGR = 0x08,   // foreground
  GBGR = 0x09,   // background
  GPMR = 0x0a,   // plane mask
  GPXR_S = 0x0b, // pixel mask, one-shot
  GMOR = 0x0c,   // mode
  GOPR = 0x0d,   // raster operation
  GPSR = 0x0e,   // pixel shift
  GADR = 0x0f,   // address
  GB1R = 0x10,   // Bresenham 1
  GB2R = 0x11,   // Bresenham 2
  GB3R = 0x12,   // Bresenham 3
  GCTR = 0x13,   // continue
  GDER = 0x14,   // deep
  GREV = 0x15,   // reserved in the manual; start/version per NetBSD
  GSMR = 0x16,   // stencil mode
  GPXR_P = 0x17, // pixel mask, persistent
  CCBR = 0x18,   // cursor base address
  VHCR = 0x19,   // horizontal control
  VVCR = 0x1a,   // vertical control
  VVBR = 0x1b,   // video base address
  VVVR = 0x1c,   // video valid
  CXYR = 0x1d,   // cursor XY
  VSAR = 0x1e,   // video shift address
  SISR = 0x1f,   // interrupt status
  GDAR = 0x20,   // data
  GRIR = 0x21,   // red increment
  GGIR = 0x22,   // green increment
  GBIR = 0x23,   // blue increment
  GZIR_L = 0x24, // Z increment low
  GZIR_H = 0x25, // Z increment high
  GDBR = 0x26,   // DMA base address
  GBWR = 0x27,   // Bresenham width
  GZVR_L = 0x28, // Z value low
  GZVR_H = 0x29, // Z value high
  GZBR = 0x2a,   // Z base address
  GADR_ALIAS = 0x2b,
  GRVR = 0x2c,  // red value
  GGVR = 0x2d,  // green value
  GBVR = 0x2e,  // blue value
  GSWR = 0x2f,  // span width
  EPSR = 0x30,  // palette and DAC setup
  GSNR0 = 0x40, // slope-no-go 0..7
  GSLR0 = 0x48, // slope 0..7
  GBCR0 = 0x50, // block color 0..7
  GCSR = 0x58,  // copy 64 source (0x58, 0x5a, 0x5c, 0x5e)
  GCDR = 0x59,  // copy 64 destination (0x59, 0x5b, 0x5d, 0x5f)
  ERWR = 0x78,  // EEPROM write
  ECGR = 0x7a,  // clock generator
  EPDR = 0x7c,  // palette and DAC data
  SCSR = 0x7e,  // command status
  NUM_REGS = 0x80
};

/// GMOR<6:0>, the graphics mode (Table 4-17).
enum Mode : u32 {
  MODE_SIMPLE = 0x00,
  MODE_SIMPLE_Z = 0x10,
  MODE_OPAQUE_STIPPLE = 0x01,
  MODE_OPAQUE_FILL = 0x21,
  MODE_TRANSPARENT_STIPPLE = 0x05,
  MODE_TRANSPARENT_FILL = 0x25,
  MODE_BLOCK_STIPPLE = 0x0d,
  MODE_BLOCK_FILL = 0x2d,
  MODE_OPAQUE_LINE = 0x02,
  MODE_TRANSPARENT_LINE = 0x06,
  MODE_COPY = 0x07,
  MODE_DMA_READ = 0x17,
  MODE_DMA_READ_DITHER = 0x37,
  MODE_DMA_WRITE = 0x1f,
};
/// Every line mode has GMOR<1> set and GMOR<0> clear (Table 4-17): opaque
/// and transparent 2D lines, the interpolated and the Z-buffered ones.
inline bool mode_is_line(u32 m) { return (m & 0x03) == 0x02; }
/// Of the line modes, bit 2 selects transparent (the mask enables writes)
/// over opaque (the mask selects foreground or background).
inline bool line_is_transparent(u32 m) { return (m & 0x04) != 0; }

/// GMOR fields (4.4.1).
constexpr u32 GMOR_MODE = 0x0000007f;
constexpr u32 GMOR_SBM = 0x00000700; // source bitmap
constexpr u32 GMOR_SBY = 0x00001800; // source byte
constexpr u32 GMOR_GE = 0x00002000;  // Win32 graphics environment
constexpr u32 GMOR_Z16 = 0x00004000;
constexpr u32 GMOR_CE = 0x00008000; // cap ends
constexpr u32 GMOR_WRITABLE = 0x0000ff7f;
constexpr u32 GMOR_CD = 0x00100000; // read: copy direction (drain next)
constexpr u32 GMOR_BA = 0x00200000; // read: Bresenham age
constexpr u32 GMOR_AA = 0x00400000; // read: address age
constexpr u32 GMOR_GS = 0x00800000; // read: GPXR persistent

/// GOPR fields (4.4.3).
constexpr u32 GOPR_ROP = 0x0000000f;
constexpr u32 GOPR_DBM_SHIFT = 8;  // destination bitmap
constexpr u32 GOPR_DBY_SHIFT = 10; // destination byte

/// Destination and source bitmap codes (GOPR<9:8>; GMOR<9:8> are the same
/// for the source, with GMOR<10> choosing the high DC12 bitmap).
enum Bitmap : u32 { BM_PB8 = 0, BM_UB8 = 1, BM_DC12 = 2, BM_TC24 = 3 };

/// GDER fields (4.4.28).
constexpr u32 GDER_DEEP = 0x00000001;
constexpr u32 GDER_ADDR_MASK = 0x0000001c;
constexpr u32 GDER_CS = 0x00000200; // 128K x n VRAM: 2K-pixel rows

/// VHCR (4.5.1) and VVCR (4.5.2) fields.
constexpr u32 VHCR_ODD = 0x80000000;
constexpr u32 VVCR_ACTIVE = 0x000007ff;

/// VVVR (4.5.4).
constexpr u32 VVVR_VIDEO_VALID = 0x01;
constexpr u32 VVVR_BLANK = 0x02;
constexpr u32 VVVR_CURSOR = 0x04;

/// SISR (4.7.2): enables in <20:16>, pending bits (write one to clear) in
/// <4:0>.
constexpr u32 SISR_EOFIP = 0x00000001;
constexpr u32 SISR_SAIP = 0x00000002;
constexpr u32 SISR_TIP = 0x00000010;
constexpr u32 SISR_PENDING = SISR_EOFIP | SISR_SAIP | SISR_TIP;
constexpr u32 SISR_EOFIE = 0x00010000;
constexpr u32 SISR_SAIE = 0x00020000;
constexpr u32 SISR_TIE = 0x00100000;
constexpr u32 SISR_ENABLES = SISR_EOFIE | SISR_SAIE | SISR_TIE;

/// The Bt485 registers, by RS3..RS0 (the MPU control field's bits 4:1).
enum Bt485Reg : unsigned {
  BT485_PAL_WRADDR = 0x0,
  BT485_PAL_DATA = 0x1,
  BT485_PIXMASK = 0x2,
  BT485_PAL_RDADDR = 0x3,
  BT485_COC_WRADDR = 0x4,
  BT485_COC_DATA = 0x5,
  BT485_CMD0 = 0x6,
  BT485_COC_RDADDR = 0x7,
  BT485_CMD1 = 0x8,
  BT485_CMD2 = 0x9,
  BT485_STATUS = 0xa, // command register 3 when CR0<7> and address 1
  BT485_CURSOR_RAM = 0xb,
  BT485_CURSOR_XLO = 0xc,
  BT485_CURSOR_XHI = 0xd,
  BT485_CURSOR_YLO = 0xe,
  BT485_CURSOR_YHI = 0xf,
};

} // namespace tga

#endif // !defined(INCLUDED_TGAREGS_H)
