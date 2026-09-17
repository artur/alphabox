/* AXPbox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/axpbox
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

/* Cirrus Logic GD54xx register and identity definitions.
 *
 * Definitions only -- no logic, no state. The family base (CirrusGD54xx)
 * and the per-chip units include this; keeping it free of behaviour is what
 * lets the sequencer, CRTC, DAC, cursor and blitter live in separate
 * translation units without circular includes.
 *
 * Behaviour is modelled on what the QEMU and MAME GD54xx implementations
 * document (neither is copied); names follow Cirrus Logic's register names
 * (SRxx/GRxx/CRxx).
 */

#if !defined(INCLUDED_CIRRUS_REGS_H)
#define INCLUDED_CIRRUS_REGS_H

#include "StdAfx.hpp"

namespace cirrus {

/* ---------------------------------------------------------------------
 * Chip identity
 *
 * CR27 reports the part: bits 7..2 identify the chip, bits 1..0 the
 * revision. The PCI device ID is a separate value in config space; both
 * must agree or a driver that probes one and validates the other will
 * refuse the card.
 * ------------------------------------------------------------------ */
enum chip_id : u8 {
  // ISA/VLB generation, listed for completeness; the PCI parts below are
  // the ones ES40 SRM names.
  CHIP_GD5422 = 0x22,
  CHIP_GD5424 = 0x24,
  CHIP_GD5426 = 0x26,
  CHIP_GD5428 = 0x28,
  CHIP_GD5429 = 0x2A,

  // PCI generation.
  CHIP_GD5430 = 0xA0, ///< named by ES40 SRM
  CHIP_GD5434 = 0xA8, ///< named by ES40 SRM; our first target
  CHIP_GD5436 = 0xAC,
  CHIP_GD5446 = 0xB8,
};

/// PCI vendor/device identifiers (config space offset 0x00).
enum pci_id : u16 {
  PCI_VENDOR_CIRRUS = 0x1013,
  PCI_DEVICE_GD5430 = 0x00A0,
  PCI_DEVICE_GD5434 = 0x00A8, ///< the -8 variant (0x00A4 is the -4)
  PCI_DEVICE_GD5436 = 0x00AC,
  PCI_DEVICE_GD5446 = 0x00B8,
};

/* ---------------------------------------------------------------------
 * Extended sequencer registers (index at 0x3c4, data at 0x3c5)
 * ------------------------------------------------------------------ */

/// SR06: extension lock. Writing 0x12 unlocks the Cirrus extensions;
/// anything else locks them and the chip behaves as a plain VGA. Reads
/// return 0x12 when unlocked, 0x0F when locked.
constexpr u8 SEQ_UNLOCK = 0x06;
constexpr u8 SEQ_UNLOCK_MAGIC = 0x12;
constexpr u8 SEQ_UNLOCK_LOCKED_READ = 0x0F;

/// SR07: extended sequencer mode. Bit 0 selects the packed-pixel (SVGA)
/// memory path; bits 3..1 select the pixel depth, which pairs with the
/// hidden DAC for the 15/16 bpp formats.
constexpr u8 SEQ_EXT_MODE = 0x07;
constexpr u8 SEQ_EXT_MODE_SVGA = 0x01;
constexpr u8 SEQ_EXT_MODE_DEPTH_MASK = 0x0E;
constexpr u8 SEQ_DEPTH_8 = 0x00;
constexpr u8 SEQ_DEPTH_16_DOUBLE_VCLK = 0x02;
constexpr u8 SEQ_DEPTH_24 = 0x04;
constexpr u8 SEQ_DEPTH_16 = 0x06;
constexpr u8 SEQ_DEPTH_32 = 0x08;

/// SR0B..SR0E: VCLK0..3 numerators; SR1B..SR1E: the matching denominator
/// and post-scalar. The Miscellaneous Output clock select picks the pair.
constexpr u8 SEQ_VCLK0_NUM = 0x0B;
constexpr u8 SEQ_VCLK0_DENOM = 0x1B;

/// SR0F: DRAM control. Read-back straps give the memory configuration.
constexpr u8 SEQ_DRAM_CONTROL = 0x0F;

/// SR10/SR11: hardware cursor X/Y. The register holds the upper 8 bits of
/// the 11-bit position; the low 3 bits come from the top 3 bits of the
/// index used to write it (so the register answers at 0x10, 0x30, ... 0xF0).
constexpr u8 SEQ_CURSOR_X = 0x10;
constexpr u8 SEQ_CURSOR_Y = 0x11;

/// SR12: hardware cursor attributes.
constexpr u8 SEQ_CURSOR_ATTR = 0x12;
constexpr u8 CURSOR_ENABLE = 0x01;
constexpr u8 CURSOR_EXT_PALETTE = 0x02; ///< DAC accesses hit the ext palette
constexpr u8 CURSOR_LARGE = 0x04;       ///< 64x64 instead of 32x32

/// SR13: cursor pattern select (32x32 patterns only).
constexpr u8 SEQ_CURSOR_PATTERN = 0x13;

/// SR17: configuration read-back and extended mapping. Bits 5..3 are the
/// bus-type straps and read only; bit 2 enables memory-mapped BitBLT
/// registers, bit 6 moves them from 0xb8000 to the top of the linear
/// aperture.
constexpr u8 SEQ_CONFIG = 0x17;
constexpr u8 SEQ_CONFIG_STRAPS = 0x38;
constexpr u8 SEQ_CONFIG_MMIO = 0x04;
constexpr u8 SEQ_CONFIG_MMIO_LINEAR = 0x40;

constexpr u8 SEQ_MCLK = 0x1F;

/* ---------------------------------------------------------------------
 * Extended CRTC registers (index at 0x3d4, data at 0x3d5)
 * ------------------------------------------------------------------ */

constexpr u8 CRTC_MISC_CONTROL = 0x1A; ///< CR1A: bit 0 interlace
constexpr u8 CRTC_EXT_DISPLAY = 0x1B;  ///< CR1B: offset bit 8, start 16/17
constexpr u8 CRTC_EXT_OVERLAY = 0x1D;  ///< CR1D: start address bit 18
constexpr u8 CRTC_PART_STATUS = 0x25;  ///< CR25: part status (read only)
constexpr u8 CRTC_ATC_INDEX = 0x26;    ///< CR26: attribute index read-back
constexpr u8 CRTC_CHIP_ID = 0x27;      ///< CR27: chip id (7..2) + revision

/* ---------------------------------------------------------------------
 * Extended graphics controller registers (index 0x3ce, data 0x3cf)
 *
 * GR09/GR0A are the two banking registers that give the GD54xx its
 * dual-window access to more than 64 KB of framebuffer through the legacy
 * 0xa0000 aperture; GR0B selects between single- and dual-bank modes.
 * ------------------------------------------------------------------ */
constexpr u8 GC_BANK_0 = 0x09;
constexpr u8 GC_BANK_1 = 0x0A;
constexpr u8 GC_BANK_MODE = 0x0B;
constexpr u8 GC_BANK_MODE_DUAL = 0x01;
constexpr u8 GC_BANK_MODE_BY8 = 0x02;       ///< aperture addresses x8
constexpr u8 GC_BANK_MODE_EXT_WRITE = 0x04; ///< write modes 4/5
constexpr u8 GC_BANK_MODE_BY16 = 0x14;      ///< both bits: x16, 16bpp modes
constexpr u8 GC_BANK_GRANULARITY_16K = 0x20;

constexpr u8 GC_BG_COLOR_1 = 0x10; ///< write-mode background, byte 1
constexpr u8 GC_FG_COLOR_1 = 0x11; ///< write-mode foreground, byte 1
constexpr u8 GC_DRAM_EXT = 0x18;   ///< extended DRAM controls

/// GR20..GR3F: BitBLT engine registers. GR31 is start/status.
constexpr u8 GC_BLT_FIRST = 0x20;
constexpr u8 GC_BLT_LAST = 0x3F;
constexpr u8 GC_BLT_STATUS = 0x31;
constexpr u8 BLT_STATUS_BUSY = 0x01;
constexpr u8 BLT_START = 0x02;
constexpr u8 BLT_RESET = 0x04;

/* ---------------------------------------------------------------------
 * Hidden DAC
 *
 * Reads of the pixel-mask register (0x3c6) arm the hidden register: the
 * fifth consecutive read returns it, and a write after four reads sets
 * it. Any access to 0x3c7..0x3c9 disarms. The low nibble selects the
 * 15/16 bpp pixel format (0 = 5-5-5 Sierra, 1 = 5-6-5 XGA).
 * ------------------------------------------------------------------ */
constexpr int DAC_HIDDEN_ARM_COUNT = 4;
constexpr u8 DAC_FORMAT_MASK = 0x0F;
constexpr u8 DAC_FORMAT_565 = 0x01;

} // namespace cirrus

#endif // !defined(INCLUDED_CIRRUS_REGS_H)
