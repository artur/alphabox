/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

/**
 * \file
 * Contains the code for emulated S3 Trio 64 Video Card device.
 **/
#include "S3Trio64.hpp"
#include "AliM1543C.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "emu/emu.hpp"
#include "gui/gui.hpp"
#include "xtal.hpp"
#include <algorithm>
#include <chrono>

// begin MAME code

#define LOG_WARN (1U << 1)
#define LOG_REGS (1U << 2) // deprecated
#define LOG_DSW (1U << 3)  // Input sense at $3c2
#define LOG_CRTC (1U << 4) // CRTC setups with monitor geometry

//#define VERBOSE (LOG_GENERAL | LOG_CRTC | LOG_WARN | LOG_REGS)
//#define LOG_OUTPUT_FUNC osd_printf_info
#include "logmacro.hpp"

// TODO: remove this enum
enum {
  IBM8514_IDLE = 0,
  IBM8514_DRAWING_RECT,
  IBM8514_DRAWING_LINE,
  IBM8514_DRAWING_BITBLT,
  IBM8514_DRAWING_PATTERN,
  IBM8514_DRAWING_SSV_1,
  IBM8514_DRAWING_SSV_2,
  MACH8_DRAWING_SCAN
};

#define LOGWARN(...) LOGMASKED(LOG_WARN, __VA_ARGS__)
#define LOGREGS(...) LOGMASKED(LOG_REGS, __VA_ARGS__)
#define LOGDSW(...) LOGMASKED(LOG_DSW, __VA_ARGS__)
#define LOGCRTC(...) LOGMASKED(LOG_CRTC, __VA_ARGS__)

/***************************************************************************

        Local variables

***************************************************************************/

//#define TEXT_LINES (LINES_HELPER)
#define LINES ((vga.crtc.vert_disp_end + 1) * (get_interlace_mode() + 1))
#define TEXT_LINES (vga.crtc.vert_disp_end + 1)

#define GRAPHIC_MODE (vga.gc.alpha_dis) /* else text mode */

#define EGA_COLUMNS (vga.crtc.horz_disp_end + 1)
#define EGA_LINE_LENGTH (vga.crtc.offset << 1)

#define VGA_COLUMNS (vga.crtc.horz_disp_end + 1)
#define VGA_LINE_LENGTH (vga.crtc.offset << 3)

#define VGA_CH_WIDTH ((vga.sequencer.data[1] & 1) ? 8 : 9)

#define TEXT_COLUMNS (vga.crtc.horz_disp_end + 1)
#define TEXT_START_ADDRESS (vga.crtc.start_addr << 3)
#define TEXT_LINE_LENGTH (vga.crtc.offset << 1)

#define TEXT_COPY_9COLUMN(ch)                                                  \
  (((ch & 0xe0) == 0xc0) && (vga.attribute.data[0x10] & 4))

// Special values for SVGA Trident - Mode Vesa 110h
#define TLINES (LINES)
#define TGA_COLUMNS (EGA_COLUMNS)
#define TGA_LINE_LENGTH (vga.crtc.offset << 3)

// MAME FUNCTIONS - not all present yet

// s3vision864_vga_device::s3vision864_vga_device(const machine_config& mconfig,
// const char* tag, device_t* owner, uint32_t clock) 	:
//s3vision864_vga_device(mconfig, S3_VISION864_VGA, tag, owner, clock)

// s3vision864_vga_device::s3vision864_vga_device(const machine_config& mconfig,
// device_type type, const char* tag, device_t* owner, uint32_t clock) 	:
//svga_device(mconfig, type, tag, owner, clock)

// void s3vision864_vga_device::device_add_mconfig(machine_config &config)

uint32_t CS3Trio64::latch_start_addr() {
  if (s3.memory_config & 0x08) {
    // - SDD scrolling test expects a << 2 for 8bpp and no shift for anything
    // else
    // - Slackware 3.x XF86_S3 expect a << 2 shift (to be confirmed)
    // - przonegd expect no shift (RGB16)
    return vga.crtc.start_addr_latch << (svga.rgb8_en ? 2 : 0);
  }
  return vga.crtc.start_addr_latch;
}

// void s3vision864_vga_device::device_start()

// void s3vision864_vga_device::device_reset()

u16 CS3Trio64::line_compare_mask() {
  // TODO: pinpoint condition
  return svga.rgb8_en ? 0x7ff : 0x3ff;
}

uint16_t CS3Trio64::offset() {
  if (s3.memory_config & 0x08)
    return vga.crtc.offset << 3;
  return CVGA::offset();
}

void CS3Trio64::s3_define_video_mode() {
  int divisor = 1;
  int xtal =
      ((vga.miscellaneous_output & 0xc) ? XTAL(28'636'363) : XTAL(25'174'800))
          .value();
  double freq;

  if ((vga.miscellaneous_output & 0xc) == 0x0c) {
    // DCLK calculation
    freq = ((double)(s3.clk_pll_m + 2) /
            (double)((s3.clk_pll_n + 2) * (pow(2.0, s3.clk_pll_r)))) *
           14.318; // clock between XIN and XOUT
    xtal = freq * 1000000;
  }

  if ((s3.ext_misc_ctrl_2) >> 4) {
    svga.rgb8_en = 0;
    svga.rgb15_en = 0;
    svga.rgb16_en = 0;
    svga.rgb32_en = 0;
    // FIXME: vision864 has only first 7 modes
    switch ((s3.ext_misc_ctrl_2) >> 4) {
      // 0001 Mode 8: 2x 8-bit 1 VCLK/2 pixels
    case 0x01:
      svga.rgb8_en = 1;
      break;
      // 0010 Mode 1: 15-bit 2 VCLK/pixel
    case 0x02:
      svga.rgb15_en = 1;
      break;
      // 0011 Mode 9: 15-bit 1 VCLK/pixel
    case 0x03:
      svga.rgb15_en = 1;
      divisor = 2;
      break;
      // 0100 Mode 2: 24-bit 3 VCLK/pixel
    case 0x04:
      svga.rgb24_en = 1;
      break;
      // 0101 Mode 10: 16-bit 1 VCLK/pixel
    case 0x05:
      svga.rgb16_en = 1;
      divisor = 2;
      break;
      // 0110 Mode 3: 16-bit 2 VCLK/pixel
    case 0x06:
      svga.rgb16_en = 1;
      break;
      // 0111 Mode 11: 24/32-bit 2 VCLK/pixel
    case 0x07:
      svga.rgb32_en = 1;
      divisor = 4;
      break;
    case 0x0d:
      svga.rgb32_en = 1;
      divisor = 1;
      break;
    default:
      popmessage("pc_vga_s3: PA16B-COLOR-MODE %02x\n",
                 ((s3.ext_misc_ctrl_2) >> 4));
      break;
    }
  } else {
    // 0000: Mode 0 8-bit 1 VCLK/pixel
    svga.rgb8_en = (s3.memory_config & 8) >> 3;
    svga.rgb15_en = 0;
    svga.rgb16_en = 0;
    svga.rgb32_en = 0;
  }

#ifdef DEBUG_VGA_RENDER
  LOG("S3: define_video_mode: ext_misc_ctrl_2=%02x memory_config=%02x "
      "rgb8=%d rgb15=%d rgb16=%d rgb32=%d crtc_offset=%03x offset()=%d\n",
      s3.ext_misc_ctrl_2, s3.memory_config, svga.rgb8_en, svga.rgb15_en,
      svga.rgb16_en, svga.rgb32_en, vga.crtc.offset, offset());
#endif

  recompute_params_clock(divisor, xtal);
}

void CS3Trio64::refresh_pitch_offset() {
  // bit 2 = bit 8 of offset register, but only if bits 4-5 of CR51 are 00h.
  vga.crtc.offset &= 0xff;
  if ((s3.cr51 & 0x30) == 0)
    vga.crtc.offset |= (s3.cr43 & 0x04) << 6;
  else
    vga.crtc.offset |= (s3.cr51 & 0x30) << 4;
}

void CS3Trio64::crtc_map(address_map &map) {
  map(0x2d, 0x2d).lr8(NAME([this](offs_t offset) { return s3.id_high; }));
  map(0x2e, 0x2e).lr8(NAME([this](offs_t offset) { return s3.id_low; }));
  map(0x2f, 0x2f).lr8(NAME([this](offs_t offset) { return s3.revision; }));
  // CR30 Chip ID/REV register
  map(0x30, 0x30).lr8(NAME([this](offs_t offset) { return s3.id_cr30; }));
  // CR31 Memory Configuration Register
  map(0x31, 0x31)
      .lrw8(NAME([this](offs_t offset) { return s3.memory_config; }),
            NAME([this](offs_t offset, u8 data) {
              s3.memory_config = data;
              vga.crtc.start_addr_latch &= ~0x30000;
              vga.crtc.start_addr_latch |= ((data & 0x30) << 12);
              s3_define_video_mode();
            }));
  // TODO: CR32, CR33 & CR34 (backward compatibility) - in our ES40 we do these
  // ! :) CR32: Backward Compatibility 1 (BKWD_1)
  map(0x32, 0x32)
      .lrw8(NAME([this](offs_t offset) { return s3.cr32; }),
            NAME([this](offs_t offset, u8 data) { s3.cr32 = data; }));
  // CR33: Backward Compatibility 2 (BKWD_2)
  map(0x33, 0x33)
      .lrw8(NAME([this](offs_t offset) { return s3.cr33; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr33 = data;
              // ES40 extension: CR33 bit5 forces 8-dot chars
              recompute_scanline_layout();
              state.vga_mem_updated = 1;
            }));
  // CR34: Backward Compatibility 3 (BKWD_3)
  map(0x34, 0x34)
      .lrw8(NAME([this](offs_t offset) { return s3.cr34; }),
            NAME([this](offs_t offset, u8 data) { s3.cr34 = data; }));
  map(0x35, 0x35)
      .lrw8(NAME([this](offs_t offset) { return s3.crt_reg_lock; }),
            NAME([this](offs_t offset, u8 data) {
              // lock register
              if ((s3.reg_lock1 & 0xc) != 8 || ((s3.reg_lock1 & 0xc0) == 0))
                return;
              s3.crt_reg_lock = data;
              svga.bank_w = data & 0xf;
              svga.bank_r = svga.bank_w;
            }));
  // Configuration register 1
  map(0x36, 0x36)
      .lrw8(NAME([this](offs_t offset) {
              // PCI (not really), Fast Page Mode DRAM
              return s3.strapping & 0x000000ff;
            }),
            NAME([this](offs_t offset, u8 data) {
              if (s3.reg_lock2 == 0xa5) {
                s3.strapping = (s3.strapping & 0xffffff00) | data;
                LOG("CR36: Strapping data = %08x\n", s3.strapping);
              }
            }));
  // Configuration register 2
  map(0x37, 0x37)
      .lrw8(NAME([this](offs_t offset) {
              return (s3.strapping & 0x0000ff00) >>
                     8; // enable chipset, 64k BIOS size, internal DCLK/MCLK
            }),
            NAME([this](offs_t offset, u8 data) {
              // TODO: monitor ID at 7-5 (PD15-13)
              if (s3.reg_lock2 == 0xa5) {
                s3.strapping = (s3.strapping & 0xffff00ff) | (data << 8);
                LOG("CR37: Strapping data = %08x\n", s3.strapping);
              }
            }));
  map(0x38, 0x38)
      .lrw8(NAME([this](offs_t offset) { return s3.reg_lock1; }),
            NAME([this](offs_t offset, u8 data) { s3.reg_lock1 = data; }));
  map(0x39, 0x39)
      .lrw8(NAME([this](offs_t offset) { return s3.reg_lock2; }),
            NAME([this](offs_t offset, u8 data) {
              // TODO: reg lock mechanism
              s3.reg_lock2 = data;
            }));
  // CR3A: Miscellaneous 1 Register (MISC_1)
  map(0x3a, 0x3a)
      .lrw8(NAME([this](offs_t offset) { return s3.cr3a; }),
            NAME([this](offs_t offset, u8 data) { s3.cr3a = data; }));
  // CR3B: Data Transfer Position (DT_EX-POS)
  map(0x3b, 0x3b)
      .lrw8(NAME([this](offs_t offset) { return s3.cr3b; }),
            NAME([this](offs_t offset, u8 data) { s3.cr3b = data; }));
  // CR3C: Interlace Retrace Start (IL_RTSTART)
  map(0x3c, 0x3c)
      .lrw8(NAME([this](offs_t offset) { return s3.cr3c; }),
            NAME([this](offs_t offset, u8 data) { s3.cr3c = data; }));
  // CR40: System Configuration Register (SYS_CNFG)
  map(0x40, 0x40)
      .lrw8(NAME([this](offs_t offset) { return s3.cr40; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr40 = data;
              // enable 8514/A registers (x2e8, x6e8, xae8, xee8)
              s3.enable_8514 = BIT(data, 0);
            }));
  // CR41
  map(0x41, 0x41)
      .lrw8(NAME([this](offs_t offset) { return s3.cr41; }),
            NAME([this](offs_t offset, u8 data) { s3.cr41 = data; }));
  // CR42 Mode Control
  map(0x42, 0x42)
      .lrw8(NAME([this](offs_t offset) { return s3.cr42; }),
            NAME([this](offs_t offset, u8 data) {
              // bit 5 = interlace, bits 0-3 = dot clock (seems to be
              // undocumented)
              s3.cr42 = data;
              s3_define_video_mode();
            }));
  map(0x43, 0x43)
      .lrw8(NAME([this](offs_t offset) { return s3.cr43; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr43 = data; // bit 2 = bit 8 of offset register, but only if
                              // bits 4-5 of CR51 are 00h.
              refresh_pitch_offset();
              s3_define_video_mode();
            }));
  /*
  CR45 Hardware Graphics Cursor Mode
  bit    0  HWGC ENB. Hardware Graphics Cursor Enable. Set to enable the
                    HardWare Cursor in VGA and enhanced modes.
             1  (911/24) Delay Timing for Pattern Data Fetch
             2  (801/5,928) Hardware Cursor Horizontal Stretch 2. If set the
  cursor pixels are stretched horizontally to two bytes and items 0 and 1 of the
  fore/background stacks in 3d4h index 4Ah/4Bh are used. 3  (801/5,928) Hardware
  Cursor Horizontal Stretch 3. If set the cursor pixels are stretched
  horizontally to three bytes and items 0,1 and 2 of the fore/background stacks
  in 3d4h index 4Ah/4Bh are used. 2-3  (805i,864/964) HWC-CSEL. Hardware Cursor
  Color Select. 0: 4/8bit, 1: 15/16bt, 2: 24bit, 3: 32bit Note: So far I've had
  better luck with: 0: 8/15/16bit, 1: 32bit?? 4  (80x +) Hardware Cursor Right
  Storage. If set the cursor data is stored in the last 256 bytes of 4 1Kyte
  lines (4bits/pixel) or the last 512 bytes of 2 2Kbyte lines (8bits/pixel).
  Intended for 1280x1024 modes where there are no free lines at the bottom. 5
  (928) Cursor Control Enable for Brooktree Bt485 DAC. If set and 3d4h index 55h
  bit 5 is set the HC1 output becomes the ODF and the HC0 output becomes the CDE
                    (964) BT485 ODF Selection for Bt485A RAMDAC. If set pin 185
  (RS3 /ODF) is the ODF output to a Bt485A compatible RamDAC (low for even
                     fields and high for odd fields), if clear pin185 is the RS3
  output.
   */
  map(0x45, 0x45)
      .lrw8(NAME([this](offs_t offset) {
              const u8 res = s3.cursor_mode;
              if (!machine().side_effects_disabled()) {
                s3.cursor_fg_ptr = 0;
                s3.cursor_bg_ptr = 0;
              }
              return res;
            }),
            NAME([this](offs_t offset, u8 data) { s3.cursor_mode = data; }));
  /*
  CR46/7 Hardware Graphics Cursor Origin-X
  bit 0-10  The HardWare Cursor X position. For 64k modes this value should be
                    twice the actual X co-ordinate.
   */
  map(0x46, 0x46)
      .lrw8(NAME([this](offs_t offset) { return (s3.cursor_x & 0xff00) >> 8; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cursor_x = (s3.cursor_x & 0x00ff) | (data << 8);
            }));
  map(0x47, 0x47)
      .lrw8(NAME([this](offs_t offset) { return s3.cursor_x & 0x00ff; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cursor_x = (s3.cursor_x & 0xff00) | data;
            }));
  /*
  CR48/9 Hardware Graphics Cursor Origin-Y
  bit  0-9  (911/24) The HardWare Cursor Y position.
          0-10  (80x +) The HardWare Cursor Y position.
  Note: The position is activated when the high byte of the Y coordinate (index
            48h) is written, so this byte should be written last (not 911/924 ?)
   */
  map(0x48, 0x48)
      .lrw8(NAME([this](offs_t offset) { return (s3.cursor_y & 0xff00) >> 8; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cursor_y = (s3.cursor_y & 0x00ff) | (data << 8);
            }));
  map(0x49, 0x49)
      .lrw8(NAME([this](offs_t offset) { return s3.cursor_y & 0x00ff; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cursor_y = (s3.cursor_y & 0xff00) | data;
            }));
  /*
  CR4A Hardware Graphics Cursor Foreground Stack       (80x +)
  bit  0-7  The Foreground Cursor color. Three bytes (4 for the 864/964) are
                    stacked here. When the Cursor Mode register (3d4h index 45h)
  is read the stackpointer is reset. When a byte is written the byte is written
  into the current top of stack and the stackpointer is increased. The first
  byte written (item 0) is allways used, the other two(3) only when Hardware
  Cursor Horizontal Stretch (3d4h index 45h bit 2-3) is enabled.
   */
  map(0x4a, 0x4a)
      .lrw8(NAME([this](offs_t offset) {
              const u8 res = s3.cursor_fg[s3.cursor_fg_ptr];
              if (!machine().side_effects_disabled()) {
                s3.cursor_fg_ptr++;
                s3.cursor_fg_ptr %= 4;
              }
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              s3.cursor_fg[s3.cursor_fg_ptr++] = data;
              s3.cursor_fg_ptr %= 4;
            }));
  /*
  CR4B Hardware Graphics Cursor Background Stack       (80x +)
  bit  0-7  The Background Cursor color. Three bytes (4 for the 864/964) are
                    stacked here. When the Cursor Mode register (3d4h index 45h)
  is read the stackpointer is reset. When a byte is written the byte is written
  into the current top of stack and the stackpointer is increased. The first
  byte written (item 0) is allways used, the other two(3) only when Hardware
  Cursor Horizontal Stretch (3d4h index 45h bit 2-3) is enabled.
   */
  map(0x4b, 0x4b)
      .lrw8(NAME([this](offs_t offset) {
              const u8 res = s3.cursor_bg[s3.cursor_bg_ptr];
              if (!machine().side_effects_disabled()) {
                s3.cursor_bg_ptr++;
                s3.cursor_bg_ptr %= 4;
              }
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              s3.cursor_bg[s3.cursor_bg_ptr++] = data;
              s3.cursor_bg_ptr %= 4;
            }));
  /*
  CR4C/D Hardware Graphics Cursor Storage Start Address
  bit  0-9  (911,924) HCS_STADR. Hardware Graphics Cursor Storage Start Address
          0-11  (80x,928) HWGC_STA. Hardware Graphics Cursor Storage Start
  Address 0-12  (864,964) HWGC_STA. Hardware Graphics Cursor Storage Start
  Address Address of the HardWare Cursor Map in units of 1024 bytes (256 bytes
                    for planar modes). The cursor map is a 64x64 bitmap with 2
  bits (A and B) per pixel. The map is stored as one word (16 bits) of bit A,
                    followed by one word with the corresponding 16 B bits.
                    The bits are interpreted as:
                           A    B    MS-Windows:         X-11:
                           0    0    Background          Screen data
                           0    1    Foreground          Screen data
                           1    0    Screen data         Background
                           1    1    Inverted screen     Foreground
                    The Windows/X11 switch is only available for the 80x +.
                    (911/24) For 64k color modes the cursor is stored as one
  byte (8 bits) of A bits, followed by the 8 B-bits, and each bit in the cursor
  should be doubled to provide a consistent cursor image. (801/5,928) For
  Hi/True color modes use the Horizontal Stretch bits (3d4h index 45h bits 2 and
  3).
   */
  map(0x4c, 0x4c)
      .lrw8(NAME([this](offs_t offset) {
              return (s3.cursor_start_addr & 0xff00) >> 8;
            }),
            NAME([this](offs_t offset, u8 data) {
              s3.cursor_start_addr =
                  (s3.cursor_start_addr & 0x00ff) | (data << 8);
              // popmessage("HW Cursor Data Address
              // %04x\n",s3.cursor_start_addr);
            }));
  map(0x4d, 0x4d)
      .lrw8(
          NAME([this](offs_t offset) { return s3.cursor_start_addr & 0x00ff; }),
          NAME([this](offs_t offset, u8 data) {
            s3.cursor_start_addr = (s3.cursor_start_addr & 0xff00) | data;
            // popmessage("HW Cursor Data Address %04x\n",s3.cursor_start_addr);
          }));
  /*
  CR4E HGC Pattern Disp Start X-Pixel Position
  bit  0-5  Pattern Display Start X-Pixel Position.
   */
  map(0x4e, 0x4e)
      .lrw8(
          NAME([this](offs_t offset) { return s3.cursor_pattern_x; }),
          NAME([this](offs_t offset, u8 data) { s3.cursor_pattern_x = data; }));
  /*
  CR4F HGC Pattern Disp Start Y-Pixel Position
  bit  0-5  Pattern Display Start Y-Pixel Position.
   */
  map(0x4f, 0x4f)
      .lrw8(
          NAME([this](offs_t offset) { return s3.cursor_pattern_y; }),
          NAME([this](offs_t offset, u8 data) { s3.cursor_pattern_y = data; }));
  // CR50
  map(0x50, 0x50)
      .lrw8(NAME([this](offs_t offset) { return s3.cr50; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr50 = data;
              ibm8514a_device *dev = get_8514();
              dev->ibm8514.color_bpp = (data >> 4) & 3;
            }));
  map(0x51, 0x51)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.start_addr_latch & 0x0c0000) >> 18;
              res |= ((svga.bank_w & 0x30) >> 2);
              //          res   |= ((vga.crtc.offset & 0x0300) >> 4);
              res |= (s3.cr51 & 0x30);
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr51 = data;
              vga.crtc.start_addr_latch &= ~0xc0000;
              vga.crtc.start_addr_latch |= ((data & 0x3) << 18);
              svga.bank_w = (svga.bank_w & 0xcf) | ((data & 0x0c) << 2);
              svga.bank_r = svga.bank_w;
              refresh_pitch_offset();
              s3_define_video_mode();
            }));
  // Extended BIOS flag 1 register (EXT_BBFLG1) (CR52)
  map(0x52, 0x52)
      .lrw8(NAME([this](offs_t offset) { return s3.cr52; }),
            NAME([this](offs_t offset, u8 data) { s3.cr52 = data; }));
  map(0x53, 0x53)
      .lrw8(NAME([this](offs_t offset) { return s3.cr53; }),
            NAME([this](offs_t offset, u8 data) { s3.cr53 = data; }));
  // Extended Memory Control 2 Register (EX_MCTL_2) (CR54)
  map(0x54, 0x54)
      .lrw8(NAME([this](offs_t offset) { return s3.cr54; }),
            NAME([this](offs_t offset, u8 data) { s3.cr54 = data; }));
  /*
  CR55 Extended Video DAC Control Register             (80x +)
  bit 0-1  DAC Register Select Bits. Passed to the RS2 and RS3 pins on the
                   RAMDAC, allowing access to all 8 or 16 registers on advanced
  RAMDACs. If this field is 0, 3d4h index 43h bit 1 is active. 2  Enable General
  Input Port Read. If set DAC reads are disabled and the STRD strobe for reading
  the General Input Port is enabled for reading while DACRD is active, if clear
  DAC reads are enabled. 3  (928) Enable External SID Operation if set. If set
  video data is passed directly from the VRAMs to the DAC rather than through
  the VGA chip 4  Hardware Cursor MS/X11 Mode. If set the Hardware Cursor is in
  X11 mode, if clear in MS-Windows mode 5  (80x,928) Hardware Cursor External
  Operation Mode. If set the two bits of cursor data ,is output on the HC[0-1]
  pins for the video DAC The SENS pin becomes HC1 and the MID2 pin becomes HC0.
            6  ??
            7  (80x,928) Disable PA Output. If set PA[0-7] and VCLK are
  tristated. (864/964) TOFF VCLK. Tri-State Off VCLK Output. VCLK output tri
                    -stated if set
   */
  map(0x55, 0x55)
      .lrw8(NAME([this](offs_t offset) { return s3.extended_dac_ctrl; }),
            NAME([this](offs_t offset, u8 data) {
              s3.extended_dac_ctrl = data;
            }));
  // External Sync Control 1 Register (EX_SYNC_1) (CR56)
  map(0x56, 0x56)
      .lrw8(NAME([this](offs_t offset) { return s3.cr56; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr56 = data & 0x1F; // bits 7-5 reserved
            }));
  // External Sync Control 2 Register (EX_SYNC_2) (CR57)
  map(0x57, 0x57)
      .lrw8(NAME([this](offs_t offset) { return s3.cr57; }),
            NAME([this](offs_t offset, u8 data) { s3.cr57 = data; }));
  // Linear Address Window Control Register (LAW_CTL) (CR58) - dosbox calls
  // VGA_StartUpdateLFB() after storing the value
  map(0x58, 0x58)
      .lrw8(NAME([this](offs_t offset) { return s3.cr58; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr58 = data;
              on_crtc_linear_regs_changed();
              redraw_area(0, 0, old_iWidth, old_iHeight);
            }));
  // Linear Address Window Position High
  map(0x59, 0x59)
      .lrw8(NAME([this](offs_t offset) { return s3.cr59; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr59 = data;
              on_crtc_linear_regs_changed();
            }));
  // Linear Address Window Position Low
  map(0x5a, 0x5a)
      .lrw8(NAME([this](offs_t offset) { return s3.cr5a; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr5a = data;
              on_crtc_linear_regs_changed();
            }));
  // undocumented on trio64?
  map(0x5b, 0x5b)
      .lrw8(NAME([this](offs_t offset) { return s3.cr5b; }),
            NAME([this](offs_t offset, u8 data) { s3.cr5b = data; }));
  // TODO: bits 7-4 (w/o?) for GPIO
  map(0x5c, 0x5c).lr8(NAME([this](offs_t offset) {
    u8 res = 0;
    // if VGA dot clock is set to 3 (misc reg bits 2-3), then selected dot clock
    // is read, otherwise read VGA clock select
    if ((vga.miscellaneous_output & 0xc) == 0x0c)
      res = s3.cr42 & 0x0f;
    else
      res = (vga.miscellaneous_output & 0xc) >> 2;
    return res;
  }));
  // TODO: following two registers must be read-backable
  /*
  CR5D Extended Horizontal Overflow Register           (80x +)
  bit    0  Horizontal Total bit 8. Bit 8 of the Horizontal Total register (3d4h
                    index 0)
             1  Horizontal Display End bit 8. Bit 8 of the Horizontal Display
  End register (3d4h index 1) 2  Start Horizontal Blank bit 8. Bit 8 of the
  Horizontal Start Blanking register (3d4h index 2). 3  (864,964) EHB+64. End
  Horizontal Blank +64. If set the /BLANK pulse is extended by 64 DCLKs. Note:
  Is this bit 6 of 3d4h index 3 or does it really extend by 64 ? 4  Start
  Horizontal Sync Position bit 8. Bit 8 of the Horizontal Start Retrace register
  (3d4h index 4). 5  (864,964) EHS+32. End Horizontal Sync +32. If set the HSYNC
  pulse is extended by 32 DCLKs. Note: Is this bit 5 of 3d4h index 5 or does it
  really extend by 32 ? 6  (928,964) Data Transfer Position bit 8. Bit 8 of the
  Data Transfer Position register (3d4h index 3Bh) 7  (928,964) Bus-Grant
  Terminate Position bit 8. Bit 8 of the Bus Grant Termination register (3d4h
  index 5Fh).
  */
  map(0x5d, 0x5d)
      .lrw8(NAME([this](offs_t offset) {
              // Recompose CR5D from the extended CRTC fields
              u8 res = 0;
              res |= (vga.crtc.horz_total >> 8) & 0x01;      // bit 0
              res |= ((vga.crtc.horz_disp_end >> 7) & 0x02); // bit 1
              res |= ((vga.crtc.horz_blank_start >> 6) &
                      0x04); // bit 2 (from vga.crtc.horz_blank_start if needed)
              // bit 3: EHB+64 extension — stored in s3.cr5d
              res |= (s3.cr5d & 0x08);
              res |= ((vga.crtc.horz_retrace_start >> 4) & 0x10); // bit 4
              // bit 5: EHS+32 extension — stored in s3.cr5d
              res |= (s3.cr5d & 0x20);
              // bits 6-7: DTP bit8, BGT bit8 — stored in s3.cr5d
              res |= (s3.cr5d & 0xC0);
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr5d = data; // ES40: cache for readback of non-decomposed bits
              vga.crtc.horz_total =
                  (vga.crtc.horz_total & 0xfeff) | ((data & 0x01) << 8);
              vga.crtc.horz_disp_end =
                  (vga.crtc.horz_disp_end & 0xfeff) | ((data & 0x02) << 7);
              vga.crtc.horz_blank_start =
                  (vga.crtc.horz_blank_start & 0xfeff) | ((data & 0x04) << 6);
              vga.crtc.horz_blank_end =
                  (vga.crtc.horz_blank_end & 0xffbf) | ((data & 0x08) << 3);
              vga.crtc.horz_retrace_start =
                  (vga.crtc.horz_retrace_start & 0xfeff) | ((data & 0x10) << 4);
              vga.crtc.horz_retrace_end =
                  (vga.crtc.horz_retrace_end & 0xffdf) | (data & 0x20);
              s3_define_video_mode();
              // ES40 extension: recompute derived layout
              recompute_scanline_layout();
            }));

  /*
  CR5E: Extended Vertical Overflow Register             (80x +)
  bit    0  Vertical Total bit 10. Bit 10 of the Vertical Total register (3d4h
                    index 6). Bits 8 and 9 are in 3d4h index 7 bit 0 and 5.
             1  Vertical Display End bit 10. Bit 10 of the Vertical Display End
                    register (3d4h index 12h). Bits 8 and 9 are in 3d4h index 7
  bit 1 and 6 2  Start Vertical Blank bit 10. Bit 10 of the Vertical Start
  Blanking register (3d4h index 15h). Bit 8 is in 3d4h index 7 bit 3 and bit 9
                    in 3d4h index 9 bit 5
             4  Vertical Retrace Start bit 10. Bit 10 of the Vertical Start
  Retrace register (3d4h index 10h). Bits 8 and 9 are in 3d4h index 7 bit 2
                    and 7.
             6  Line Compare Position bit 10. Bit 10 of the Line Compare
  register (3d4h index 18h). Bit 8 is in 3d4h index 7 bit 4 and bit 9 in 3d4h
                    index 9 bit 6.
   */
  map(0x5e, 0x5e)
      .lrw8(NAME([this](offs_t offset) {
              // Recompose CR5E from the extended CRTC fields
              u8 res = 0;
              res |= (vga.crtc.vert_total >> 10) & 0x01;        // bit 0
              res |= ((vga.crtc.vert_disp_end >> 9) & 0x02);    // bit 1
              res |= ((vga.crtc.vert_blank_start >> 8) & 0x04); // bit 2
              // bit 3: reserved
              res |= ((vga.crtc.vert_retrace_start >> 6) & 0x10); // bit 4
              // bit 5: reserved
              res |= ((vga.crtc.line_compare >> 4) & 0x40); // bit 6
              // bit 7: reserved
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.vert_total =
                  (vga.crtc.vert_total & 0xfbff) | ((data & 0x01) << 10);
              vga.crtc.vert_disp_end =
                  (vga.crtc.vert_disp_end & 0xfbff) | ((data & 0x02) << 9);
              vga.crtc.vert_blank_start =
                  (vga.crtc.vert_blank_start & 0xfbff) | ((data & 0x04) << 8);
              vga.crtc.vert_retrace_start =
                  (vga.crtc.vert_retrace_start & 0xfbff) | ((data & 0x10) << 6);
              vga.crtc.line_compare =
                  (vga.crtc.line_compare & 0xfbff) | ((data & 0x40) << 4);
              s3_define_video_mode();
            }));
  map(0x5f, 0x5f)
      .lrw8(NAME([this](offs_t offset) { return s3.cr5f; }),
            NAME([this](offs_t offset, u8 data) { s3.cr5f = data; }));
  // Extended Memory Control 3 Register (EXT-MCTL-3) (CR60)
  map(0x60, 0x60)
      .lrw8(NAME([this](offs_t offset) { return s3.cr60; }),
            NAME([this](offs_t offset, u8 data) { s3.cr60 = data; }));
  // Extended Memory Control 4 Register (EXT-MCTL-4) (CR61)
  map(0x61, 0x61)
      .lrw8(NAME([this](offs_t offset) { return s3.cr61; }),
            NAME([this](offs_t offset, u8 data) { s3.cr61 = data; }));
  // undocumented?
  map(0x62, 0x62)
      .lrw8(NAME([this](offs_t offset) { return s3.cr62; }),
            NAME([this](offs_t offset, u8 data) { s3.cr62 = data; }));
  // External Sync Control 3 Register (EX-SYNC-3) (CR63)
  map(0x63, 0x63)
      .lrw8(NAME([this](offs_t offset) { return s3.cr63; }),
            NAME([this](offs_t offset, u8 data) { s3.cr63 = data; }));
  // undocumented?
  map(0x64, 0x64)
      .lrw8(NAME([this](offs_t offset) { return s3.cr64; }),
            NAME([this](offs_t offset, u8 data) { s3.cr64 = data; }));
  // Extended Miscellaneous Control Register (EXT-MISC-CTL) (CR6S)
  map(0x65, 0x65)
      .lrw8(NAME([this](offs_t offset) { return s3.cr65; }),
            NAME([this](offs_t offset, u8 data) { s3.cr65 = data; }));
  // Extended Miscellaneous Control 1 Register (EXT-MISC-1) (CR66) - S3 BIOS
  // writes 0 here - normal operation & PCI bus disconnect disabled
  map(0x66, 0x66)
      .lrw8(NAME([this](offs_t offset) { return s3.cr66; }),
            NAME([this](offs_t offset, u8 data) {
              s3.cr66 = data;
              if (data & 0x02) {
                accel_reset();
              }
            }));
  map(0x67, 0x67)
      .lrw8(NAME([this](offs_t offset) { return s3.ext_misc_ctrl_2; }),
            NAME([this](offs_t offset, u8 data) {
              s3.ext_misc_ctrl_2 = data;
              s3_define_video_mode();
            }));
  map(0x68, 0x68)
      .lrw8(NAME([this](offs_t offset) { // Configuration register 3
              // no /CAS,/OE stretch time, 32-bit data bus size
              return (s3.strapping & 0x00ff0000) >> 16;
            }),
            NAME([this](offs_t offset, u8 data) {
              if (s3.reg_lock2 == 0xa5) {
                s3.strapping = (s3.strapping & 0xff00ffff) | (data << 16);
                LOG("CR68: Strapping data = %08x\n", s3.strapping);
              }
            }));
  map(0x69, 0x69)
      .lrw8(NAME([this](offs_t offset) {
              return vga.crtc.start_addr_latch >> 16;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.start_addr_latch &= ~0x1f0000;
              vga.crtc.start_addr_latch |= ((data & 0x1f) << 16);
              s3_define_video_mode();
            }));
  // TODO: doesn't match number of bits
  map(0x6a, 0x6a)
      .lrw8(NAME([this](offs_t offset) { return svga.bank_r & 0x7f; }),
            NAME([this](offs_t offset, u8 data) {
              svga.bank_w = data & 0x3f;
              svga.bank_r = svga.bank_w;
            }));
  // Extended BIOS Flag 3 Register (EBIOS-FLG3) (CR6B) - Bios scratchpad
  map(0x6b, 0x6b)
      .lrw8(NAME([this](offs_t offset) {
              const u8 cr53 = s3.cr53;
              const u8 cr59 = m_crtc_map.read_byte(0x59);
              if (cr53 & 0x08) {
                // Trio64 (not Trio64V2): mask per 86Box for non-V chips 0xFE
                // (Trio64V would use &0xFC, but ES40 emulates Trio64.)
                return (u8)(cr59 & 0xFE);
              } else {
                return cr59;
              }
            }),
            NAME([this](offs_t offset, u8 data) { s3.cr6b = data; }));
  // Extended BIOS Flag 4 Register (EBIOS-FLG3) (CR6C) - Bios scratchpad
  map(0x6c, 0x6c)
      .lrw8(NAME([this](offs_t offset) {
              const u8 cr53 = s3.cr53;
              if (cr53 & 0x08) {
                // When NEWMMIO bit is set, readback is 00h.
                return 0x00;
              } else {
                // Otherwise mirror the documented bit from CR5A (mask to 0x80)
                return (s3.cr5a & 0x80);
              }
            }),
            NAME([this](offs_t offset, u8 data) { s3.cr6c = data; }));
  // undocumented
  map(0x6d, 0x6d)
      .lrw8(NAME([this](offs_t offset) { return s3.cr6d; }),
            NAME([this](offs_t offset, u8 data) { s3.cr6d = data; }));
  // Configuration register 4 (Trio64V+)
  map(0x6f, 0x6f)
      .lrw8(NAME([this](offs_t offset) {
              // LPB(?) mode, Serial port I/O at port 0xe8, Serial port I/O
              // disabled (MMIO only), no WE delay
              return (s3.strapping & 0xff000000) >> 24;
            }),
            NAME([this](offs_t offset, u8 data) {
              if (s3.reg_lock2 == 0xa5) {
                s3.strapping = (s3.strapping & 0x00ffffff) | (data << 24);
                LOG("CR6F: Strapping data = %08x\n", s3.strapping);
              }
            }));
}

void CS3Trio64::sequencer_map(address_map &map) {
  //  map(0x01, 0x01) Clocking Mode Register
  // SR01: Clocking Mode Register
  map(0x01, 0x01)
      .lw8( // es40 deviation
          NAME([this](offs_t offset, u8 data) {
            u8 newreg1 = data & 0x3f;
            if (m_crtc_map.read_byte(0x34) & 0x20) {
              newreg1 = (newreg1 & ~0x01) | (vga.sequencer.data[1] & 0x01);
            }
            vga.sequencer.data[1] = newreg1;
          }));

  // (undocumented) Sequencer Horizontal Character Counter Reset
  // Any write strobe to this register will lock the character generator until
  // another write to other regs happens.
  //  map(0x07, 0x07)
  // TODO: SR8 (unlocks SRD)
  // SR08: PLL Unlock
  map(0x08, 0x08)
      .lrw8(NAME([this](offs_t offset) { return vga.sequencer.data[0x08]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.sequencer.data[0x08] = data;
            }));
  // SR09: Extended (reserved)
  map(0x09, 0x09)
      .lrw8(NAME([this](offs_t offset) { return vga.sequencer.data[0x09]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.sequencer.data[0x09] = data;
            }));
  // SR0A
  map(0x0a, 0x0a)
      .lrw8(NAME([this](offs_t offset) { return vga.sequencer.data[0x0a]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.sequencer.data[0x0a] = data;
            }));
  // SR0B
  map(0x0b, 0x0b)
      .lrw8(NAME([this](offs_t offset) { return vga.sequencer.data[0x0b]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.sequencer.data[0x0b] = data;
            }));
  // SR0D
  map(0x0d, 0x0d)
      .lrw8(NAME([this](offs_t offset) { return vga.sequencer.data[0x0d]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.sequencer.data[0x0d] = data;
            }));
  // Memory CLK PLL
  map(0x10, 0x10)
      .lrw8(NAME([this](offs_t offset) { return s3.sr10; }),
            NAME([this](offs_t offset, u8 data) { s3.sr10 = data; }));
  map(0x11, 0x11)
      .lrw8(NAME([this](offs_t offset) { return s3.sr11; }),
            NAME([this](offs_t offset, u8 data) { s3.sr11 = data; }));
  // Video CLK PLL
  map(0x12, 0x12)
      .lrw8(NAME([this](offs_t offset) { return s3.sr12; }),
            NAME([this](offs_t offset, u8 data) { s3.sr12 = data; }));
  map(0x13, 0x13)
      .lrw8(NAME([this](offs_t offset) { return s3.sr13; }),
            NAME([this](offs_t offset, u8 data) { s3.sr13 = data; }));
  // SR14: CLKSYN Control 1
  map(0x14, 0x14)
      .lrw8(NAME([this](offs_t offset) { return vga.sequencer.data[0x14]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.sequencer.data[0x14] = data;
            }));
  map(0x15, 0x15)
      .lrw8(NAME([this](offs_t offset) { return s3.sr15; }),
            NAME([this](offs_t offset, u8 data) {
              // load DCLK frequency (would normally have a small variable
              // delay)
              if (data & 0x02) {
                s3.clk_pll_n = s3.sr12 & 0x1f;
                s3.clk_pll_r = (s3.sr12 & 0x60) >> 5;
                s3.clk_pll_m = s3.sr13 & 0x7f;
                s3_define_video_mode();
              }
              // immediate DCLK/MCLK load
              if (data & 0x20) {
                s3.clk_pll_n = s3.sr12 & 0x1f;
                s3.clk_pll_r = (s3.sr12 & 0x60) >> 5;
                s3.clk_pll_m = s3.sr13 & 0x7f;
                s3_define_video_mode();
              }
              s3.sr15 = data;
            }));
  map(0x17, 0x17).lr8(NAME([this](offs_t offset) {
    // CLKSYN test register
    const u8 res = s3.sr17;
    // who knows what it should return, docs only say it defaults to 0, and is
    // reserved for testing of the clock synthesiser
    if (!machine().side_effects_disabled())
      s3.sr17--;
    return res;
  }));
  // SR18: RAMDAC/CLKSYN Control
  map(0x18, 0x18)
      .lrw8(NAME([this](offs_t offset) { return s3.sr18; }),
            NAME([this](offs_t offset, u8 data) {
              s3.sr18 = data;
              vga.sequencer.data[0x18] = data;
            }));
}

uint8_t CS3Trio64::mem_r(uint32_t offset) {
  if (svga.rgb8_en || svga.rgb15_en || svga.rgb16_en || svga.rgb32_en) {
    int data;
    if (offset & 0x10000)
      return 0;
    data = 0;
    if (vga.sequencer.data[4] & 0x8) {
      if ((offset + (svga.bank_r * 0x10000)) < vga.svga_intf.vram_size)
        data = vga.memory[(offset + (svga.bank_r * 0x10000))];
    } else {
      for (int i = 0; i < 4; i++) {
        if (vga.sequencer.map_mask & 1 << i) {
          if ((offset * 4 + i + (svga.bank_r * 0x10000)) <
              vga.svga_intf.vram_size)
            data |= vga.memory[(offset * 4 + i + (svga.bank_r * 0x10000))];
        }
      }
    }
    return (uint8_t)data;
  }

  // Standard VGA fallback — full read-mode/latch pipeline via base class
  if ((offset + (svga.bank_r * 0x10000)) < vga.svga_intf.vram_size)
    return CVGA::mem_r(offset);
  return 0xff;
}

void CS3Trio64::mem_w(offs_t offset, uint8_t data) {
  ibm8514a_device *dev = get_8514();
  // bit 4 of CR53 enables memory-mapped I/O
  // 0xA0000-0xA7fff maps to port 0xE2E8 (pixel transfer)
  if (s3.cr53 & 0x10) {
    if (offset < 0x8000) {
      // Lower half of the MMIO window is PIX_TRANS FIFO.
      // Feed it through the same bus-size aware path as port I/O.
      AccelIOWrite(0xE2E8 + (offset & 3), data);
      return;
    }

#ifdef DEBUG_VGA_MEMW
    printf("mem_w offset: 0x%05x data: 0x%02x\n", (unsigned)offset,
           (unsigned)data);
#endif

    switch (offset) {
    case 0x8100:
    case 0x82e8:
      dev->ibm8514.curr_y = (dev->ibm8514.curr_y & 0xff00) | data;
      dev->ibm8514.prev_y = (dev->ibm8514.prev_y & 0xff00) | data;
      break;
    case 0x8101:
    case 0x82e9:
      dev->ibm8514.curr_y = (dev->ibm8514.curr_y & 0x00ff) | (data << 8);
      dev->ibm8514.prev_y = (dev->ibm8514.prev_y & 0x00ff) | (data << 8);
      break;
    case 0x8102:
    case 0x86e8:
      dev->ibm8514.curr_x = (dev->ibm8514.curr_x & 0xff00) | data;
      dev->ibm8514.prev_x = (dev->ibm8514.prev_x & 0xff00) | data;
      break;
    case 0x8103:
    case 0x86e9:
      dev->ibm8514.curr_x = (dev->ibm8514.curr_x & 0x00ff) | (data << 8);
      dev->ibm8514.prev_x = (dev->ibm8514.prev_x & 0x00ff) | (data << 8);
      break;
    case 0x8108:
    case 0x8ae8:
      dev->ibm8514.line_axial_step =
          (dev->ibm8514.line_axial_step & 0xff00) | data;
      dev->ibm8514.dest_y = (dev->ibm8514.dest_y & 0xff00) | data;
      break;
    case 0x8109:
    case 0x8ae9:
      dev->ibm8514.line_axial_step =
          (dev->ibm8514.line_axial_step & 0x00ff) | ((data & 0x3f) << 8);
      dev->ibm8514.dest_y =
          (dev->ibm8514.dest_y & 0x00ff) | ((data & 0x0f) << 8);
      break;
    case 0x810a:
    case 0x8ee8:
      dev->ibm8514.line_diagonal_step =
          (dev->ibm8514.line_diagonal_step & 0xff00) | data;
      dev->ibm8514.dest_x = (dev->ibm8514.dest_x & 0xff00) | data;
      break;
    case 0x810b:
    case 0x8ee9:
      dev->ibm8514.line_diagonal_step =
          (dev->ibm8514.line_diagonal_step & 0x00ff) | ((data & 0x3f) << 8);
      dev->ibm8514.dest_x =
          (dev->ibm8514.dest_x & 0x00ff) | ((data & 0x0f) << 8);
      break;
    case 0x8118:
    case 0x9ae8:
      s3.mmio_9ae8 = (s3.mmio_9ae8 & 0xff00) | data;
      break;
    case 0x8119:
    case 0x9ae9:
      s3.mmio_9ae8 = (s3.mmio_9ae8 & 0x00ff) | (data << 8);
      dev->ibm8514_cmd_w(s3.mmio_9ae8);
      break;
    case 0x8120:
    case 0xa2e8:
      dev->ibm8514.bgcolour = (dev->ibm8514.bgcolour & 0xffffff00) | data;
      break;
    case 0x8121:
    case 0xa2e9:
      dev->ibm8514.bgcolour =
          (dev->ibm8514.bgcolour & 0xffff00ff) | (data << 8);
      break;
    case 0x8122:
    case 0xa2ea:
      dev->ibm8514.bgcolour =
          (dev->ibm8514.bgcolour & 0xff00ffff) | (data << 16);
      break;
    case 0x8123:
    case 0xa2eb:
      dev->ibm8514.bgcolour =
          (dev->ibm8514.bgcolour & 0x00ffffff) | (data << 24);
      break;
    case 0x8124:
    case 0xa6e8:
      dev->ibm8514.fgcolour = (dev->ibm8514.fgcolour & 0xffffff00) | data;
      break;
    case 0x8125:
    case 0xa6e9:
      dev->ibm8514.fgcolour =
          (dev->ibm8514.fgcolour & 0xffff00ff) | (data << 8);
      break;
    case 0x8126:
    case 0xa6ea:
      dev->ibm8514.fgcolour =
          (dev->ibm8514.fgcolour & 0xff00ffff) | (data << 16);
      break;
    case 0x8127:
    case 0xa6eb:
      dev->ibm8514.fgcolour =
          (dev->ibm8514.fgcolour & 0x00ffffff) | (data << 24);
      break;
    case 0x8128:
    case 0xaae8:
      dev->ibm8514.write_mask = (dev->ibm8514.write_mask & 0xffffff00) | data;
      break;
    case 0x8129:
    case 0xaae9:
      dev->ibm8514.write_mask =
          (dev->ibm8514.write_mask & 0xffff00ff) | (data << 8);
      break;
    case 0x812a:
    case 0xaaea:
      dev->ibm8514.write_mask =
          (dev->ibm8514.write_mask & 0xff00ffff) | (data << 16);
      break;
    case 0x812b:
    case 0xaaeb:
      dev->ibm8514.write_mask =
          (dev->ibm8514.write_mask & 0x00ffffff) | (data << 24);
      break;
    case 0x812c:
    case 0xaee8:
      dev->ibm8514.read_mask = (dev->ibm8514.read_mask & 0xffffff00) | data;
      break;
    case 0x812d:
    case 0xaee9:
      dev->ibm8514.read_mask =
          (dev->ibm8514.read_mask & 0xffff00ff) | (data << 8);
      break;
    case 0x812e:
    case 0xaeea:
      dev->ibm8514.read_mask =
          (dev->ibm8514.read_mask & 0xff00ffff) | (data << 16);
      break;
    case 0x812f:
    case 0xaeeb:
      dev->ibm8514.read_mask =
          (dev->ibm8514.read_mask & 0x00ffffff) | (data << 24);
      break;
    case 0x8130:
    case 0xb2e8:
      dev->ibm8514.color_cmp = (dev->ibm8514.color_cmp & 0xffffff00) | data;
      break;
    case 0x8131:
    case 0xb2e9:
      dev->ibm8514.color_cmp =
          (dev->ibm8514.color_cmp & 0xffff00ff) | (data << 8);
      break;
    case 0x8132:
    case 0xb2ea:
      dev->ibm8514.color_cmp =
          (dev->ibm8514.color_cmp & 0xff00ffff) | (data << 16);
      break;
    case 0x8133:
    case 0xb2eb:
      dev->ibm8514.color_cmp =
          (dev->ibm8514.color_cmp & 0x00ffffff) | (data << 24);
      break;
    case 0xb6e8:
    case 0x8134:
      dev->ibm8514.bgmix = (dev->ibm8514.bgmix & 0xff00) | data;
      break;
    case 0x8135:
    case 0xb6e9:
      dev->ibm8514.bgmix = (dev->ibm8514.bgmix & 0x00ff) | (data << 8);
      dev->ibm8514.bkgd_sel = (dev->ibm8514.bgmix >> 5) & 3;
      dev->ibm8514.bkgd_mix_mode = dev->ibm8514.bgmix & 0x0f;
      break;
    case 0x8136:
    case 0xbae8:
      dev->ibm8514.fgmix = (dev->ibm8514.fgmix & 0xff00) | data;
      break;
    case 0x8137:
    case 0xbae9:
      dev->ibm8514.fgmix = (dev->ibm8514.fgmix & 0x00ff) | (data << 8);
      dev->ibm8514.frgd_sel = (dev->ibm8514.fgmix >> 5) & 3;
      dev->ibm8514.frgd_mix_mode = dev->ibm8514.fgmix & 0x0f;
      break;
    case 0x8138:
      dev->ibm8514.scissors_top = (dev->ibm8514.scissors_top & 0xff00) | data;
      break;
    case 0x8139:
      dev->ibm8514.scissors_top =
          (dev->ibm8514.scissors_top & 0x00ff) | (data << 8);
      break;
    case 0x813a:
      dev->ibm8514.scissors_left = (dev->ibm8514.scissors_left & 0xff00) | data;
      break;
    case 0x813b:
      dev->ibm8514.scissors_left =
          (dev->ibm8514.scissors_left & 0x00ff) | (data << 8);
      break;
    case 0x813c:
      dev->ibm8514.scissors_bottom =
          (dev->ibm8514.scissors_bottom & 0xff00) | data;
      break;
    case 0x813d:
      dev->ibm8514.scissors_bottom =
          (dev->ibm8514.scissors_bottom & 0x00ff) | (data << 8);
      break;
    case 0x813e:
      dev->ibm8514.scissors_right =
          (dev->ibm8514.scissors_right & 0xff00) | data;
      break;
    case 0x813f:
      dev->ibm8514.scissors_right =
          (dev->ibm8514.scissors_right & 0x00ff) | (data << 8);
      break;
    case 0x8140:
      dev->ibm8514.pixel_control = (dev->ibm8514.pixel_control & 0xff00) | data;
      break;
    case 0x8141:
      dev->ibm8514.pixel_control =
          (dev->ibm8514.pixel_control & 0x00ff) | (data << 8);
      break;
    case 0x8146:
      dev->ibm8514.multifunc_sel = (dev->ibm8514.multifunc_sel & 0xff00) | data;
      break;
    case 0x8148:
      dev->ibm8514.rect_height = (dev->ibm8514.rect_height & 0xff00) | data;
      break;
    case 0x8149:
      dev->ibm8514.rect_height =
          (dev->ibm8514.rect_height & 0x00ff) | (data << 8);
      break;
    case 0x814a:
      dev->ibm8514.rect_width = (dev->ibm8514.rect_width & 0xff00) | data;
      break;
    case 0x814b:
      dev->ibm8514.rect_width =
          (dev->ibm8514.rect_width & 0x00ff) | (data << 8);
      break;
    case 0x8150:
    case 0x8151:
    case 0x8152:
    case 0x8153:
      // MMIO mirror of PIX_TRANS inside the upper half window.
      AccelIOWrite(0xE2E8 + (offset - 0x8150), data);
      break;
    case 0xbee8:
      s3.mmio_bee8 = (s3.mmio_bee8 & 0xff00) | data;
      break;
    case 0xbee9:
      s3.mmio_bee8 = (s3.mmio_bee8 & 0x00ff) | (data << 8);
      dev->ibm8514_multifunc_w(s3.mmio_bee8);
      break;
    case 0x96e8:
      s3.mmio_96e8 = (s3.mmio_96e8 & 0xff00) | data;
      break;
    case 0x96e9:
      s3.mmio_96e8 = (s3.mmio_96e8 & 0x00ff) | (data << 8);
      dev->ibm8514_width_w(s3.mmio_96e8);
      break;
    case 0x92e8:
      s3.mmio_92e8 = (s3.mmio_92e8 & 0xff00) | data;
      break;
    case 0x92e9:
      s3.mmio_92e8 = (s3.mmio_92e8 & 0x00ff) | (data << 8);
      dev->ibm8514_line_error_w(s3.mmio_92e8);
      break;
    case 0xe2e8:
    case 0xe2e9:
    case 0xe2ea:
    case 0xe2eb:
      // Normal PIX_TRANS MMIO addresses used by the VMS X11 driver
      AccelIOWrite(offset, data);
      break;
    default:
      LOG("S3: MMIO offset %05x write %02x\n", offset + 0xa0000, data);
      break;
    }
    return;
  }

  if (svga.rgb8_en || svga.rgb15_en || svga.rgb16_en || svga.rgb32_en) {
    // printf("%08x %02x (%02x %02x)
    // %02X\n",offset,data,vga.sequencer.map_mask,svga.bank_w,(vga.sequencer.data[4]
    // & 0x08));
    if (offset & 0x10000)
      return;
    if (vga.sequencer.data[4] & 0x8) {
      if ((offset + (svga.bank_w * 0x10000)) < vga.svga_intf.vram_size)
        vga.memory[(offset + (svga.bank_w * 0x10000))] = data;
    } else {
      int i;
      for (i = 0; i < 4; i++) {
        if (vga.sequencer.map_mask & 1 << i) {
          if ((offset * 4 + i + (svga.bank_w * 0x10000)) <
              vga.svga_intf.vram_size)
            vga.memory[(offset * 4 + i + (svga.bank_w * 0x10000))] = data;
        }
      }
    }
    return;
  }

  if ((offset + (svga.bank_w * 0x10000)) < vga.svga_intf.vram_size)
    CVGA::mem_w(offset, data);
}

uint32_t CS3Trio64::screen_update(bitmap_rgb32 &bitmap,
                                  const rectangle &cliprect) {
  CVGA::screen_update(bitmap, cliprect);

  uint8_t cur_mode = pc_vga_choosevideomode();

#ifdef DEBUG_VGA_RENDER
  static uint8_t last_mode = 0xff;
  if (cur_mode != last_mode) {
    LOG("S3: screen mode changed to %d (rgb8=%d sync=%d graphic=%d)\n",
        cur_mode, svga.rgb8_en, vga.crtc.sync_en, vga.gc.alpha_dis);
    last_mode = cur_mode;
  }

  // log screen configuration
  static bool screen_diag = false;
  if (cur_mode == 6 && !screen_diag) { // 6 = RGB8_MODE
    const rectangle &vis = cliprect;
    LOG("DIAG: screen visible_area=(%d,%d)-(%d,%d) bitmap=%dx%d\n", vis.min_x,
        vis.min_y, vis.max_x, vis.max_y, bitmap.width(), bitmap.height());
    LOG("DIAG: CRTC offset=%03x seq4=%02x chain4=%d height=%d LINES=%d "
        "VGA_COLUMNS=%d\n",
        vga.crtc.offset, vga.sequencer.data[4],
        (vga.sequencer.data[4] & 0x08) ? 1 : 0,
        vga.crtc.maximum_scan_line * (vga.crtc.scan_doubling + 1),
        (vga.crtc.vert_disp_end + 1), (vga.crtc.horz_disp_end + 1));
    screen_diag = true;

    static int dac_frame_count = 0;
    if (cur_mode == 6 && (dac_frame_count % 60 == 0) && dac_frame_count < 300) {
      LOG("DIAG[frame %d]: dac.dirty=%d DAC[0]=%02x,%02x,%02x "
          "DAC[2]=%02x,%02x,%02x\n",
          dac_frame_count, vga.dac.dirty, vga.dac.color[0], vga.dac.color[1],
          vga.dac.color[2], vga.dac.color[6], vga.dac.color[7],
          vga.dac.color[8]);
    }
    dac_frame_count++;
  }

  printf("PALETTE: dirty=%d DAC[0]=%02x,%02x,%02x DAC[2]=%02x,%02x,%02x "
         "DAC[130]=%02x,%02x,%02x\n",
         vga.dac.dirty, vga.dac.color[0], vga.dac.color[1], vga.dac.color[2],
         vga.dac.color[6], vga.dac.color[7], vga.dac.color[8],
         vga.dac.color[390], vga.dac.color[391], vga.dac.color[392]);

#endif

  draw_hardware_cursor(bitmap, cliprect, cur_mode);
  return 0;
}

// vision 964 stuff

// vision 968 stuff

// s3trio64_vga_device::s3trio64_vga_device(const machine_config& mconfig, const
// char* tag, device_t* owner, uint32_t clock)
//	: s3trio64_vga_device(mconfig, S3_TRIO64_VGA, tag, owner, clock)

// s3trio64_vga_device::s3trio64_vga_device(const machine_config& mconfig,
// device_type type, const char* tag, device_t* owner, uint32_t clock)
//	: s3vision968_vga_device(mconfig, type, tag, owner, clock)

// void s3trio64_vga_device::device_start()

// END MAME pc_vga_s3.cpp

// ES40 specific functions

/** PCI Configuration Space data block */
static u32 s3_cfg_data[64] = {
    /*00*/ 0x88115333, // CFID: vendor + device
    /*04*/ 0x02000000, // CFCS: command + status: medium DEVSEL, nothing else
                       // -- a real 86C764 reports 0x0200. The old 0x011F
                       // advertised a capability list (pointer 0), a
                       // master-data-parity error latched at reset on a card
                       // that never bus-masters, and reserved bits.
    /*08*/ 0x03000002, // CFRV: class + revision
    /*0c*/ 0x00000000, // CFLT: latency timer + cache line size
    /*10*/ 0x00000000, // BAR0: FB
    /*14*/ 0x00000000, // BAR1:
    /*18*/ 0x00000000, // BAR2:
    /*1c*/ 0x00000000, // BAR3:
    /*20*/ 0x00000000, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x281401ff, // CFIT: interrupt configuration
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0};

/** PCI Configuration Space mask block */
static u32 s3_cfg_mask[64] = {
    /*00*/ 0x00000000, // CFID: vendor + device
    /*04*/ 0x0000ffff, // CFCS: command + status
    /*08*/ 0x00000000, // CFRV: class + revision
    /*0c*/ 0x0000ffff, // CFLT: latency timer + cache line size
    /*10*/ 0xfc000000, // BAR0: FB
    /*14*/ 0x00000000, // BAR1:
    /*18*/ 0x00000000, // BAR2:
    /*1c*/ 0x00000000, // BAR3:
    /*20*/ 0x00000000, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x000000ff, // CFIT: interrupt configuration
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0};

/**
 * Constructor.
 *
 * Don't do anything, the real initialization is done by init()
 **/
CS3Trio64::CS3Trio64(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CVGACard(cfg, c, pcibus, pcidev) {}

// --- S3 CR36 -----------------------------------------------------------------
// CR36 (Reset State Read 1) encodes DRAM type in the low and the actual VRAM
// size in the high.
//   EDO: <1M=0xFE, 1M=0xDE, 2M=0x9E, 3M=0x5E, 4M=0x1E, 8M=0x7E
// Trio64 max display memory is 4 MiB
// see DOSBox-X Reset State Read 1
// `use_edo` to false for FPM instead of EDO memory
static inline uint8_t s3_cr36_from_memsize(uint32_t bytes,
                                           bool use_edo /*=true*/) {
  const uint8_t type_nibble = use_edo ? 0x0E : 0x0A; // low nibble
  uint8_t size_nibble_high;
  if (bytes < (1u * 1024 * 1024))
    size_nibble_high = 0xF0; // <1MB
  else if (bytes < (2u * 1024 * 1024))
    size_nibble_high = 0xD0; // 1MB
  else if (bytes < (3u * 1024 * 1024))
    size_nibble_high = 0x90; // 2MB
  else if (bytes < (4u * 1024 * 1024))
    size_nibble_high = 0x50; // 3MB
  else
    size_nibble_high = 0x10; // 4MB (or more -> clamp)
  return uint8_t(size_nibble_high | type_nibble);
}

// CR32 (Backward Compatibility 1 - BKWD_1).
// Writing 01xx10xx (e.g. 0x48) unlocks S3 VGA regs.
static inline bool s3_cr32_is_unlock(uint8_t v) {
  return ((v & 0xC0) == 0x40) && ((v & 0x0C) == 0x08);
}

inline uint32_t CS3Trio64::s3_lfb_base_from_regs() {
  // CR59 high, CR5A low, reported as (la_window << 16)
  const uint16_t la_window = (uint16_t(m_crtc_map.read_byte(0x59)) << 8) |
                             uint16_t(m_crtc_map.read_byte(0x5A));
  return uint32_t(la_window) << 16;
}

inline uint8_t CS3Trio64::current_char_width_px() const {
  // If special blanking (CR33 bit5) is set, S3 forces 8-dot chars.
  if (m_crtc_map.read_byte(0x33) & 0x20)
    return 8;
  // Otherwise, use Sequencer reg1 bit0 like the rest of our text logic.
  return (vga.sequencer.data[1] & 0x01) ? 8 : 9;
}

// MMIO alias base offset helper (CR53 bit5: 0=A0000, 1=B8000).
inline uint32_t CS3Trio64::s3_mmio_base_off(SS3_state &s) {
  // Trio64 CR53 bit5 (A0000/B8000 MMIO base select):
  //  - 0: A0000...AFFFF is MMIO alias space when CR53 bit4=0
  //  - 1: B8000..BFFFF is MMIO alias space when CR53 bit4=1
  return (s3.cr53 & 0x20) ? 0x18000u : 0u;
}

// Trio64: "new" MMIO 64 KiB window at LFB + 0x0100_0000 when CR53 bit3 = 1.
// We'll at minimum wire the lower 0x8000 to PIXTRANS FIFO (0xE2E8 port
// semantics).
inline bool CS3Trio64::s3_new_mmio_enabled() {
  // CR53 - see 86Box behavior gating several "new-mmio" aliases.
  return (s3.cr53 & 0x08) != 0;
}

static inline uint32_t s3_lfb_size_from_cr58(uint8_t cr58) {
  switch (cr58 & 0x03) {
  case 0:
    return 64 * 1024;
  case 1:
    return 1u * 1024 * 1024;
  case 2:
    return 2u * 1024 * 1024;
  default:
    return 4u * 1024 * 1024;
  }
}

static inline bool s3_lfb_enabled(uint8_t cr58) {
  return (cr58 & 0x10) != 0; // ENB LA (Enable Linear Addressing)
}

// The S3's own state for a snapshot: the extended registers and cursor
// (s3), the 8514 drawing engine's registers, and the linear-window cache
// that the register decodes maintain. All plain data.
namespace {
constexpr u32 kS3Magic = 0x53335431; // 'S3T1'
struct S3LfbState {
  u32 lfb_base_, lfb_size_;
  bool lfb_enabled_;
  u32 lfb_base, lfb_size;
  u64 lfb_phys;
  bool lfb_active, pci_mem_enable;
  u32 pci_bar0;
  bool vga_subsys_enable;
  u8 video_subsys_enable_46e8;
};
int write_block(FILE *f, const void *p, long n) {
  fwrite(&n, sizeof(long), 1, f);
  return fwrite(p, 1, (size_t)n, f) == (size_t)n ? 0 : -1;
}
int read_block(FILE *f, void *p, long n, const char *what, const char *dev) {
  long got;
  if (fread(&got, sizeof(long), 1, f) != 1 || got != n) {
    printf("%s: %s STRUCT SIZE does not match!\n", dev, what);
    return -1;
  }
  if (fread(p, 1, (size_t)n, f) != (size_t)n) {
    printf("%s: unexpected end of file in %s!\n", dev, what);
    return -1;
  }
  return 0;
}
} // namespace

int CS3Trio64::save_card_state(FILE *f) {
  const S3LfbState x = {lfb_base_,   lfb_size_,      lfb_enabled_,
                        lfb_base,    lfb_size,       lfb_phys,
                        lfb_active,  pci_mem_enable, pci_bar0,
                        m_vga_subsys_enable, m_video_subsys_enable_46e8};
  fwrite(&kS3Magic, sizeof(u32), 1, f);
  if (write_block(f, &s3, sizeof(s3)) ||
      write_block(f, &m_8514.ibm8514, sizeof(m_8514.ibm8514)) ||
      write_block(f, &x, sizeof(x)))
    return -1;
  fwrite(&kS3Magic, sizeof(u32), 1, f);
  return 0;
}

int CS3Trio64::restore_card_state(FILE *f) {
  u32 m;
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kS3Magic) {
    printf("%s: S3 MAGIC does not match!\n", devid_string);
    return -1;
  }
  S3LfbState x;
  if (read_block(f, &s3, sizeof(s3), "S3 registers", devid_string) ||
      read_block(f, &m_8514.ibm8514, sizeof(m_8514.ibm8514), "8514 engine",
                 devid_string) ||
      read_block(f, &x, sizeof(x), "S3 linear window", devid_string))
    return -1;
  lfb_base_ = x.lfb_base_;
  lfb_size_ = x.lfb_size_;
  lfb_enabled_ = x.lfb_enabled_;
  lfb_base = x.lfb_base;
  lfb_size = x.lfb_size;
  lfb_phys = x.lfb_phys;
  lfb_active = x.lfb_active;
  pci_mem_enable = x.pci_mem_enable;
  pci_bar0 = x.pci_bar0;
  m_vga_subsys_enable = x.vga_subsys_enable;
  m_video_subsys_enable_46e8 = x.video_subsys_enable_46e8;
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kS3Magic) {
    printf("%s: S3 end MAGIC does not match!\n", devid_string);
    return -1;
  }
  return 0;
}

void CS3Trio64::post_restore() {
  lfb_recalc_and_cache();      // BAR0 / COMMAND, and the direct offer
  update_linear_mapping();    // the CR58/59/5A decode
  refresh_pitch_offset();     // CR43/CR51 bits of the pitch
  recompute_scanline_layout(); // CR5D extension bits
  s3_define_video_mode();     // colour mode, and the pixel clock divisor
                              // with it (recompute_params_clock)
  recompute_params();         // refresh interval, screen timing
}

void CS3Trio64::update_linear_mapping() {
  // BAR-only mode: no per-device mapping. PCI core decodes BAR0 and gates
  // access via COMMAND.MSE. We keep these fields for debug only.
  lfb_active = s3_lfb_enabled(m_crtc_map.read_byte(0x58));
  lfb_size = s3_lfb_size_from_cr58(m_crtc_map.read_byte(0x58));
  lfb_base = s3_lfb_base_from_regs();
  refresh_direct_lfb();
#ifdef S3_LFB_TRACE
  printf("LFB (BAR-only): CR58=%02x base=%08x size=%x active=%d\n",
         m_crtc_map.read_byte(0x58), lfb_base, lfb_size, lfb_active);
#endif
}

void CS3Trio64::on_crtc_linear_regs_changed() {
  const u8 cr58 = m_crtc_map.read_byte(0x58);
  const u8 cr59 = m_crtc_map.read_byte(0x59);
  const u8 cr5a = m_crtc_map.read_byte(0x5A);

  // Enable via CR58.ENB_LA (bit 4)
  lfb_active = s3_lfb_enabled(cr58);

  // Trio64 size: CR58[1:0] 00=64K, 01=1M, 10=2M, 11=4M
  lfb_size = s3_lfb_size_from_cr58(cr58);

  // Base: CR59:CR5A form LA_WINDOW; reported/programmed as (la_window << 16)
  const u32 la_window = (u32(cr59) << 8) | u32(cr5a);
  lfb_base = la_window << 16;

  // Apply/unapply the mapping now that CR regs changed
  lfb_recalc_and_map();
  refresh_direct_lfb();
  trace_lfb_if_changed("CR58/59/5A");
}

/** The S3's own legacy I/O ranges, beyond the standard VGA ones. */
static const struct {
  int id;
  u32 port;
  u32 length;
} s3_legacy_ports[] = {
    {32, 0x0102, 1}, // Setup Option Select
    {10, 0x42E8, 2}, // SUBSYS_CNTL/STAT
    {11, 0x4AE8, 2}, // ADVFUNC_CNTL
    {12, 0x46E8, 2}, // MODE_SETUP / video subsystem enable
    {13, 0x4EE8, 2}, // legacy compatibility stub
    {14, 0x86E8, 2}, // CUR_X
    {15, 0x8EE8, 2}, // DESTX_DIASTP
    {16, 0x96E8, 2}, // MAJ_AXIS_PCNT
    {17, 0x9AE8, 2}, // CMD
    {18, 0xA2E8, 4}, // BKGD_COLOR
    {19, 0xA6E8, 4}, // FRGD_COLOR
    {20, 0xAAE8, 4}, // WRT_MASK
    {21, 0xAEE8, 4}, // RD_MASK
    {22, 0xB6E8, 2}, // BKGD_MIX
    {23, 0xBAE8, 2}, // FRGD_MIX
    {24, 0xE2E8, 8}, // PIX_TRANS (0xE2E8..0xE2EF)
    {25, 0xB2E8, 4}, // PIX_CNTL or ALT PIX_TRANS (gated by MULTIFUNC[E].bit8)
    {26, 0xBEE8, 2}, // MULTIFUNC_CNTL (word)
    {27, 0xD2E8, 2}, // ROP_MIX       (word; some paths read this)
    {28, 0x9EE8, 2}, // SHORT_STROKE  (word; latch)
    {29, 0xCAE8, 2}, // DESTY/AXSTP alias (might be wrong)
    {30, 0x82E8, 2}, // CUR_Y
    {31, 0x92E8, 2}, // ERR_TERM
    {33, 0x8AE8, 2}, // DESTY_AXSTP
};

/** Base port of one of the S3's own legacy ranges, or 0 if not ours. */
static u32 s3_legacy_port(int index) {
  for (const auto &r : s3_legacy_ports)
    if (r.id == index)
      return r.port;
  return 0;
}

/**
 * Initialize the S3 device.
 **/
void CS3Trio64::init() {
  // Register PCI device
  add_function(0, s3_cfg_data, s3_cfg_mask);

  // Make the 21272/21274 PCI config window visible for this device now.
  // NetBSD reads PCI_ID_REG at 0/1/0 during console bring-up; without this
  // mapping it sees ~0/0 and panics with "no device at 255/255/0".
  ResetPCI();

  // Initialize all state variables to 0
  memset((void *)&state, 0, sizeof(state));

  // initialize MAME S3 and VGA fields and values to 0
  memset(&s3, 0, sizeof(s3));
  memset(&vga, 0, sizeof(vga));
  memset(&svga, 0, sizeof(svga));
  memset(&timing, 0, sizeof(timing));
  memset(vga.dac.color, 0, sizeof(vga.dac.color));
  memset(vga.dac.loading, 0, sizeof(vga.dac.loading));

  // Standard VGA ports, BIOS message port and the 0xa0000 window
  add_vga_legacy_ranges();

  // Register CRTC address-map handlers.  Must be called before any
  // m_crtc_map.write_byte() so that writes dispatch through handlers
  // and update decomposed vga.crtc.* / s3.* fields + trigger recomputes.
  init_maps();

  vga.attribute.state = atc_flip_flop() ? 1 : 0;
  vga.attribute.index = vga.attribute.index & 0x1f;

  vga.gc.bit_mask = 0xFF;

  vga.sequencer.char_sel.base[0] = 0x20000; // font B (attr bit3=0)
  vga.sequencer.char_sel.base[1] = 0x20000; // font A (attr bit3=1)

  // S3 setup (0x102) and 8514/A-style accel ports (byte-wide) - always
  // register; runtime gating of the accel block is done via CR40.
  for (const auto &r : s3_legacy_ports)
    add_legacy_io(r.id, r.port, r.length);

  // Default: no linear window until guest enables CR58 bit 0.
  // Seed base/size from PCI config defaults; CR58/59 will override when
  // written.
  lfb_active = false;
  lfb_base = s3_cfg_data[0x10 >> 2] & 0xFC000000; // BAR0 default (aligned)
  lfb_size = vga.svga_intf.vram_size;             // clamp to VRAM for now

  // Reset the base PCI device
  ResetPCI();

  /* The configuration file variable "rom" should point to a VGA BIOS
     image. If not, try "vgabios.bin". */
  load_option_rom("vgabios.bin");

  vga.attribute.state = 1;

  vga.crtc.line_compare = 1023;
  vga.crtc.vert_disp_end = 399;

  vga.dac.mask = 0xff;
  vga.dac.dirty = 1;
  vga.dac.state = 0;
  vga.dac.read = 0;

  vga.gc.memory_map_sel = 2; // monochrome text mode

  vga.sequencer.data[0] = 0x03; // reset1=1, reset2=1
  vga.sequencer.data[4] = 0x06; // extended_mem=1, odd_even=1, chain_four=0
  s3.sr15 = 0;                  // CLKSYN Control 2 Register (SR15) 00H poweron
  vga.sequencer.data[0x0A] =
      0; // External Bus Control Register (SRA) 00H poweron
  vga.sequencer.data[0x0B] =
      0; // Miscellaneous Extended Sequencer Register 00H poweron
  vga.sequencer.data[0x0D] =
      0; // Extended Sequencer Register (EX_SR_D) (SRD) 00H poweron
  vga.sequencer.data[0x09] =
      0; // Extended Sequencer Register 9 (SR9) poweron 00H

  // MCLK PLL defaults (MAME values)
  s3.sr10 = 0x42;
  s3.sr11 = 0x41;

  // DCLK PLL defaults (MAME values)
  s3.sr12 = 0x00;
  s3.sr13 = 0x00;
  s3.clk_pll_n = 0x00;
  s3.clk_pll_r = 0x00;

  // Use VIDEO_RAM_SIZE (in bits) to size VRAM. With 22 this is 4 MB.
  vga.svga_intf.vram_size = 1u << VIDEO_RAM_SIZE;
  vga.memory = new u8[vga.svga_intf.vram_size];
  memset(vga.memory, 0, vga.svga_intf.vram_size);

  state.last_bpp = 8;

  state.x_tilesize = X_TILESIZE;
  state.y_tilesize = Y_TILESIZE;

  s3.id_cr30 =
      0xE1; // Chip ID/REV register CR30, dosbox-x implementation returns 0x00
            // for our use case. poweron default is E1H however.
  m_crtc_map.write_byte(0x32, 0x00); // Locked by default
  m_crtc_map.write_byte(
      0x33, 0x00); // CR33 (Backward Compatibility 2) default 00h (no locks).
  m_crtc_map.write_byte(
      0x36,
      s3_cr36_from_memsize(vga.svga_intf.vram_size,
                           true)); // Configuration 2 Register (CONFG_REG1)
                                   // (CR36) - bootstrap config
  m_crtc_map.write_byte(0x37,
                        0xE5); // Configuration 2 Register (CONFG_REG2) (CR37)
                               // - bootstrap read, sane value from 86box
  m_crtc_map.write_byte(0x3B, 0x00); // CR3B: Data Transfer Position (DTPC)
  m_crtc_map.write_byte(0x3C, 0x00); // CR3C: IL_RTSTART defaulting
  m_crtc_map.write_byte(
      0x40, 0x30); // System Configuration Register, power on default 30h
  m_crtc_map.write_byte(0x42, 0x00); // Mode Control 2- can set interlace vs non
  s3.strapping = (uint32_t)s3_cr36_from_memsize(vga.svga_intf.vram_size, true);
  vga.gc.memory_map_sel = 3; // color text mode
  state.vga_mem_updated = 1;

  // init MAME fields
  s3.id_high = 0x88; // 86C764 Trio64
  s3.id_low = 0x11;
  s3.revision = 0x00;
  s3.id_cr30 = 0xE1; // Trio64 (per datasheet)

  // lets make it decent - redraw time
  timing.vrefresh_hz = 60.0;
  timing.refresh_interval_ms = 16; // ~60Hz
  m_last_refresh_time = std::chrono::steady_clock::now();

  // Hardware cursor defaults (MAME says windows 95 doesn't program these but it
  // applies it regardless to everything)
  for (int i = 0; i < 4; i++) {
    s3.cursor_fg[i] = 0xFF;
    s3.cursor_bg[i] = 0x00;
  }

  // CR56: External Sync Control 1 (EX_SYNC_1) power-on default 00h
  m_crtc_map.write_byte(0x56, 0x00);

  // CR57: EX_SYNC_2 (VSYNC reset adjust), power-on default 00h
  m_crtc_map.write_byte(0x57, 0x00);

  // EX_SYNC_3 (CR63)
  m_crtc_map.write_byte(0x63, 0x00);

  // CNFG-REG-3 (CR68) poweron strap; datasheet says power-on samples PD[23:16].
  // 00h per 86Box-compatible. If needed, set CRTC.reg[0x68] before
  // recompute_config3() for different.
  m_crtc_map.write_byte(0x68, 0x00);

  // CR65: Extended Miscellaneous Control Register (EXT-MISC-CTL) (CR65)
  m_crtc_map.write_byte(0x65, 0x00);

  m_8514.set_vga_ptr(this);
  m_8514.start();

  refresh_pitch_offset(); // do it initially, just for sanity sake

  myThread = nullptr;

  printf("%s: $Id$\n", devid_string);
}

void CS3Trio64::recompute_scanline_layout() {
  const uint8_t cr5d = m_crtc_map.read_byte(0x5d);

  auto xbit = [&](int b) -> uint16_t { return (cr5d >> b) & 1u; };

  // still some compute/reliance, need to sync up here...

  // vga.crtc.horz_total - MAME sets low 8, we overlay bit8 from CR5D.
  vga.crtc.horz_total = (vga.crtc.horz_total & 0xff) | (xbit(0) << 8);

  // vga.crtc.horz_disp_end — same pattern.
  vga.crtc.horz_disp_end = (vga.crtc.horz_disp_end & 0xff) | (xbit(1) << 8);

  // vga.crtc.horz_blank_start — can only hold low 8 bits.
  // Compose full 9-bit value
  vga.crtc.horz_blank_start =
      uint16_t(vga.crtc.horz_blank_start) | (xbit(2) << 8);

  // vga.crtc.horz_retrace_start — same.
  vga.crtc.horz_retrace_start =
      uint16_t(vga.crtc.horz_retrace_start) | (xbit(4) << 8);

  // vga.crtc.horz_blank_end, holds bits 0-5 (CR03 + CR05).
  uint16_t hb_end_base = uint16_t(vga.crtc.horz_blank_end & 0x3f);
  vga.crtc.horz_blank_end = hb_end_base + (xbit(3) ? 64 : 0);

  // vga.crtc.horz_retrace_end, holds bits 0-4 (from CR05).
  uint16_t hs_end_base = uint16_t(vga.crtc.horz_retrace_end & 0x1f);
  vga.crtc.horz_retrace_end = hs_end_base + (xbit(5) ? 32 : 0);

  // S3 special blanking (CR33 bit5)
  if (m_crtc_map.read_byte(0x33) & 0x20) {
    vga.crtc.horz_blank_start = vga.crtc.horz_disp_end;
    vga.crtc.horz_blank_end = (vga.crtc.horz_total - 1) & 0x1FF;
  }

  // 9-bit masking
  vga.crtc.horz_total &= 0x1FF;
  vga.crtc.horz_disp_end &= 0x1FF;
  vga.crtc.horz_blank_start &= 0x1FF;
  vga.crtc.horz_blank_end &= 0x1FF;
  vga.crtc.horz_retrace_start &= 0x1FF;
  vga.crtc.horz_retrace_end &= 0x1FF;
}

void CS3Trio64::recompute_params() {
  recompute_scanline_layout();
  refresh_pitch_offset();
  redraw_area(0, 0, old_iWidth, old_iHeight);
}

void CS3Trio64::attribute_map(address_map &map) {
  // Standard registers only (CVGACard::vga_attribute_map); the CR33
  // palette lock is atc_palette_locked().
}

/**************************************
 *
 * GC
 *
 *************************************/

void CS3Trio64::gc_map(address_map &map) {
  // Standard registers only (CVGACard::vga_gc_map).
}

void CS3Trio64::recompute_params_clock(int divisor, int xtal) {
  // Store timing parameters for renderer/debug use -- ES40 specific
  timing.xtal_hz = xtal;
  timing.divisor = divisor;
  // es40 specific end

  int vblank_period, hblank_period;
  attoseconds_t refresh;
  uint8_t hclock_m = (!GRAPHIC_MODE) ? VGA_CH_WIDTH : 8;
  int pixel_clock;

  /* safety check */
  if (!vga.crtc.horz_disp_end || !vga.crtc.vert_disp_end ||
      !vga.crtc.horz_total) // check needs 'vga.crtc.vert_total' but we don't
                            // implement.... yet
    return;

  const u8 is_interlace_mode = get_interlace_mode() + 1;
  const int display_lines = vga.crtc.vert_disp_end * is_interlace_mode;

  // rectangle visarea(0, ((vga.crtc.horz_disp_end + 1) * ((float)(hclock_m) /
  // divisor)) - 1, 0, display_lines);

  vblank_period = (vga.crtc.vert_total + 2) * is_interlace_mode;
  hblank_period = ((vga.crtc.horz_total + 5) * ((float)(hclock_m) / divisor));

  // TODO: improve/complete clocking modes
  pixel_clock = xtal / ((x_dotclockdiv2() >> 3) + 1);

  refresh = HZ_TO_ATTOSECONDS(pixel_clock) * (hblank_period)*vblank_period;
  // screen().configure((hblank_period), (vblank_period), visarea, refresh);
  // m_vblank_timer->adjust(screen().time_until_pos(vga.crtc.vert_blank_start +
  // vga.crtc.vert_blank_end));

  if (hblank_period > 0 && vblank_period > 0 && pixel_clock > 0) {
    timing.vrefresh_hz =
        (double)pixel_clock / ((double)hblank_period * (double)vblank_period);
    // Clamp to sane range
    if (timing.vrefresh_hz < 1.0)
      timing.vrefresh_hz = 1.0;
    if (timing.vrefresh_hz > 240.0)
      timing.vrefresh_hz = 240.0;
    timing.refresh_interval_ms = (uint64_t)(1000.0 / timing.vrefresh_hz);
    if (timing.refresh_interval_ms < 4)
      timing.refresh_interval_ms = 4; // cap at ~250Hz
  }

  // ES40 specific here - MAME: pixel_clock = xtal / (((vga.sequencer.data[1]&8)
  // >> 3) + 1);
  const int seq_div = ((vga.sequencer.data[1] & 0x08) >> 3) + 1;
  timing.pixel_clock_hz = (seq_div > 0) ? (xtal / seq_div) : xtal;

  // Recompute line offset (pitch) — ES40's existing function
  refresh_pitch_offset();

  // Mark display dirty so the renderer picks up changes
  state.vga_mem_updated = 1;
}

/**
 * Destructor. Stops the render thread while this object is still whole: the
 * thread calls this card's hooks (see CVGACard::~CVGACard).
 **/
CS3Trio64::~CS3Trio64() { stop_threads(); }

u32 CS3Trio64::card_legacy_read(int index, u32 address, int dsize) {
  const u32 port = s3_legacy_port(index);
  return port ? io_read(address + port, dsize) : 0;
}

void CS3Trio64::card_legacy_write(int index, u32 address, int dsize, u32 data) {
  const u32 port = s3_legacy_port(index);
  if (port)
    io_write(address + port, dsize, data);
}

int CS3Trio64::BytesPerPixel() const {
  const uint8_t pf = (s3.ext_misc_ctrl_2 >> 4) & 0x0F;
  switch (pf) {
  case 0x01:
    return 1; // Mode 8: 8-bit packed
  case 0x02:
    return 2; // Mode 1: 15-bit (2 VCLK/pixel)
  case 0x03:
    return 2; // Mode 9: 15-bit (1 VCLK/pixel)
  case 0x04:
    return 3; // Mode 2: 24-bit (3 VCLK/pixel)
  case 0x05:
    return 2; // Mode 10: 16-bit (1 VCLK/pixel)
  case 0x06:
    return 2; // Mode 3: 16-bit (2 VCLK/pixel)
  case 0x07:
    return 4; // Mode 11: 32-bit (2 VCLK/pixel)
  case 0x0D:
    return 4; // Mode 13: 32-bit alternate
  default:
    return 1; // Mode 0: 8bpp indexed (or VGA)
  }
}

u32 CS3Trio64::PitchBytes() const { return (uint32_t)vga.crtc.offset; }

static inline u32 clamp_vram_addr(u32 a, u32 vram_size) {
  return (vram_size == 0) ? a : (a % vram_size);
}

void CS3Trio64::accel_reset() {
  m_8514.start();
  m_8514.ibm8514.enabled = (s3.cr40 & 0x01);
}

// -------------------------
// Minimal accel window I/O
// -------------------------
u8 CS3Trio64::AccelIORead(u32 port) {
  ibm8514a_device *dev = get_8514();
  if (!s3.enable_8514)
    return 0xFF;

  switch (port & 0xFFFE) {
  case 0x9AE8: {
    uint16_t ret = dev->ibm8514_gpstatus_r();
    return (port & 1) ? (uint8_t)(ret >> 8) : (uint8_t)(ret & 0xff);
  }

  case 0x42E8: {
    uint16_t v = dev->ibm8514_substatus_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }

  // coordinate & size readbacks
  case 0x82E8: {
    uint16_t v = dev->ibm8514_currenty_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0x86E8: {
    uint16_t v = dev->ibm8514_currentx_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0x8AE8: {
    uint16_t v = dev->ibm8514_desty_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0x8EE8: {
    uint16_t v = dev->ibm8514_destx_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0x92E8: {
    uint16_t v = dev->ibm8514_line_error_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0x96E8: {
    uint16_t v = dev->ibm8514_width_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0x9EE8: {
    uint16_t v = dev->ibm8514_ssv_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xA2E8: {
    uint16_t v = dev->ibm8514_bgcolour_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xA2EA: {
    uint16_t v = dev->ibm8514_bgcolour_r_hi();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xA6E8: {
    uint16_t v = dev->ibm8514_fgcolour_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xA6EA: {
    uint16_t v = dev->ibm8514_fgcolour_r_hi();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xAAE8: {
    uint16_t v = dev->ibm8514_write_mask_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xAAEA: {
    uint16_t v = dev->ibm8514_write_mask_r_hi();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xAEE8: {
    uint16_t v = dev->ibm8514_read_mask_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xAEEA: {
    uint16_t v = dev->ibm8514_read_mask_r_hi();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xB6E8: {
    uint16_t v = dev->ibm8514_backmix_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xBAE8: {
    uint16_t v = dev->ibm8514_foremix_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xB2E8: {
    uint16_t v = dev->ibm8514_color_cmp_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xB2EA: {
    uint16_t v = dev->ibm8514_color_cmp_r_hi();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }
  case 0xBEE8: {
    uint16_t v = dev->ibm8514_multifunc_r();
    return (port & 1) ? (v >> 8) : (v & 0xff);
  }

  default:
    return 0x00;
  }
}

static inline void write16_low_high(u16 &reg, u32 port, u8 data) {
  if ((port & 1) == 0)
    reg = (reg & 0xFF00u) | data;
  else
    reg = (reg & 0x00FFu) | (u16)data << 8;
}

void CS3Trio64::AccelIOWrite(u32 port, u8 data) {
  ibm8514a_device *dev = get_8514();
  if (!s3.enable_8514)
    return;

  switch (port & 0xFFFE) {

    // SUBSYS_CNTL (42E8h write)
  case 0x42E8:
    if ((port & 1) == 0)
      s3.mmio_42e8 = (s3.mmio_42e8 & 0xff00) | data;
    else
      s3.mmio_42e8 = (s3.mmio_42e8 & 0x00ff) | (data << 8);
    dev->ibm8514_subcontrol_w(s3.mmio_42e8);
    break;

    // ADVFUNC_CNTL (4AE8h)
  case 0x4AE8:
    if ((port & 1) == 0)
      s3.mmio_4ae8 = (s3.mmio_4ae8 & 0xff00) | data;
    else
      s3.mmio_4ae8 = (s3.mmio_4ae8 & 0x00ff) | (data << 8);
    dev->ibm8514_advfunc_w(s3.mmio_4ae8);
    break;

    // CUR_Y (82E8h) — MAME sets prev_y too
  case 0x82E8:
    if ((port & 1) == 0) {
      dev->ibm8514.curr_y = (dev->ibm8514.curr_y & 0xff00) | data;
      dev->ibm8514.prev_y = (dev->ibm8514.prev_y & 0xff00) | data;
    } else {
      dev->ibm8514.curr_y = (dev->ibm8514.curr_y & 0x00ff) | (data << 8);
      dev->ibm8514.prev_y = (dev->ibm8514.prev_y & 0x00ff) | (data << 8);
    }
    break;

    // CUR_X (86E8h) — MAME sets prev_x too
  case 0x86E8:
    if ((port & 1) == 0) {
      dev->ibm8514.curr_x = (dev->ibm8514.curr_x & 0xff00) | data;
      dev->ibm8514.prev_x = (dev->ibm8514.prev_x & 0xff00) | data;
    } else {
      dev->ibm8514.curr_x = (dev->ibm8514.curr_x & 0x00ff) | (data << 8);
      dev->ibm8514.prev_x = (dev->ibm8514.prev_x & 0x00ff) | (data << 8);
    }
    break;

    // DESTY/AXSTP (8AE8h) — dual-purpose register
  case 0x8AE8:
    if ((port & 1) == 0) {
      dev->ibm8514.line_axial_step =
          (dev->ibm8514.line_axial_step & 0xff00) | data;
      dev->ibm8514.dest_y = (dev->ibm8514.dest_y & 0xff00) | data;
    } else {
      dev->ibm8514.line_axial_step =
          (dev->ibm8514.line_axial_step & 0x00ff) | ((data & 0x3f) << 8);
      dev->ibm8514.dest_y =
          (dev->ibm8514.dest_y & 0x00ff) | ((data & 0x0f) << 8);
    }
    break;

    // COLOR_CMP (B2E8h)
  case 0xB2E8:
  case 0xB2EA: {
    unsigned s = (port & 3) * 8;
    dev->ibm8514.color_cmp =
        (dev->ibm8514.color_cmp & ~(0xFFu << s)) | ((u32)data << s);
  } break;

  // DESTX/DIASTP (8EE8h) — dual-purpose register
  case 0x8EE8:
    if ((port & 1) == 0) {
      dev->ibm8514.line_diagonal_step =
          (dev->ibm8514.line_diagonal_step & 0xff00) | data;
      dev->ibm8514.dest_x = (dev->ibm8514.dest_x & 0xff00) | data;
    } else {
      dev->ibm8514.line_diagonal_step =
          (dev->ibm8514.line_diagonal_step & 0x00ff) | ((data & 0x3f) << 8);
      dev->ibm8514.dest_x =
          (dev->ibm8514.dest_x & 0x00ff) | ((data & 0x0f) << 8);
    }
    break;

    // ERR_TERM (92E8h)
  case 0x92E8:
    if ((port & 1) == 0)
      s3.mmio_92e8 = (s3.mmio_92e8 & 0xff00) | data;
    else {
      s3.mmio_92e8 = (s3.mmio_92e8 & 0x00ff) | (data << 8);
      dev->ibm8514_line_error_w(s3.mmio_92e8);
    }
    break;

    // MAJ_AXIS_PCNT (96E8h)
  case 0x96E8:
    if ((port & 1) == 0)
      s3.mmio_96e8 = (s3.mmio_96e8 & 0xff00) | data;
    else {
      s3.mmio_96e8 = (s3.mmio_96e8 & 0x00ff) | (data << 8);
      dev->ibm8514_width_w(s3.mmio_96e8);
    }
    break;

    // CMD (9AE8h) — high byte triggers execution!
  case 0x9AE8:
    if ((port & 1) == 0)
      s3.mmio_9ae8 = (s3.mmio_9ae8 & 0xff00) | data;
    else {
      s3.mmio_9ae8 = (s3.mmio_9ae8 & 0x00ff) | (data << 8);
      dev->ibm8514_cmd_w(s3.mmio_9ae8);
    }
    break;

    // SSV (9EE8h) — high byte triggers execution
  case 0x9EE8:
    if ((port & 1) == 0)
      s3.mmio_9ee8 = (s3.mmio_9ee8 & 0xff00) | data;
    else {
      s3.mmio_9ee8 = (s3.mmio_9ee8 & 0x00ff) | (data << 8);
      dev->ibm8514_ssv_w(s3.mmio_9ee8);
    }
    break;

    // BKGD_COLOR (A2E8h)
  case 0xA2E8:
  case 0xA2EA: {
    unsigned s = (port & 3) * 8;
    dev->ibm8514.bgcolour =
        (dev->ibm8514.bgcolour & ~(0xFFu << s)) | ((u32)data << s);
  } break;

  // FRGD_COLOR (A6E8h)
  case 0xA6E8:
  case 0xA6EA: {
    unsigned s = (port & 3) * 8;
    dev->ibm8514.fgcolour =
        (dev->ibm8514.fgcolour & ~(0xFFu << s)) | ((u32)data << s);
  } break;

  // WRT_MASK (AAE8h)
  case 0xAAE8:
  case 0xAAEA: {
    unsigned s = (port & 3) * 8;
    dev->ibm8514.write_mask =
        (dev->ibm8514.write_mask & ~(0xFFu << s)) | ((u32)data << s);
  } break;

  // RD_MASK (AEE8h)
  case 0xAEE8:
  case 0xAEEA: {
    unsigned s = (port & 3) * 8;
    dev->ibm8514.read_mask =
        (dev->ibm8514.read_mask & ~(0xFFu << s)) | ((u32)data << s);
  } break;

  // BKGD_MIX (B6E8h)
  case 0xB6E8:
    if ((port & 1) == 0)
      dev->ibm8514.bgmix = (dev->ibm8514.bgmix & 0xff00) | data;
    else {
      dev->ibm8514.bgmix = (dev->ibm8514.bgmix & 0x00ff) | (data << 8);
      dev->ibm8514.bkgd_sel = (dev->ibm8514.bgmix >> 5) & 3;
      dev->ibm8514.bkgd_mix_mode = dev->ibm8514.bgmix & 0x0f;
    }
    break;

    // FRGD_MIX (BAE8h)
  case 0xBAE8:
    if ((port & 1) == 0)
      dev->ibm8514.fgmix = (dev->ibm8514.fgmix & 0xff00) | data;
    else {
      dev->ibm8514.fgmix = (dev->ibm8514.fgmix & 0x00ff) | (data << 8);
      dev->ibm8514.frgd_sel = (dev->ibm8514.fgmix >> 5) & 3;
      dev->ibm8514.frgd_mix_mode = dev->ibm8514.fgmix & 0x0f;
    }
    break;

    // MULTIFUNC_CNTL (BEE8h) — high byte triggers dispatch
  case 0xBEE8:
    if ((port & 1) == 0)
      s3.mmio_bee8 = (s3.mmio_bee8 & 0xff00) | data;
    else {
      s3.mmio_bee8 = (s3.mmio_bee8 & 0x00ff) | (data << 8);
      dev->ibm8514_multifunc_w(s3.mmio_bee8);
    }
    break;

    // PIX_TRANS (E2E8h..E2EFh) — host data upload
    // Uses the ibm8514a's bus_size-aware accumulation + wait_draw()
  case 0xE2E8:
  case 0xE2EA:
  case 0xE2EC:
  case 0xE2EE: {
    if (dev->ibm8514.bus_size == 0) {
      dev->ibm8514.pixel_xfer = (dev->ibm8514.pixel_xfer & 0xffffff00) | data;
      dev->ibm8514_pixel_xfer_complete();
    } else if (dev->ibm8514.bus_size == 1) {
      if ((dev->ibm8514.current_cmd & 0x02) || (dev->ibm8514.color_bpp == 0)) {
        switch (port & 0x0001) {
        case 0:
          dev->ibm8514.pixel_xfer =
              (dev->ibm8514.pixel_xfer & 0xffffff00) | data;
          break;
        case 1:
          dev->ibm8514.pixel_xfer =
              (dev->ibm8514.pixel_xfer & 0xffff00ff) | (data << 8);
          dev->ibm8514_pixel_xfer_complete();
          break;
        }
      } else {
        switch (port & 0x0003) {
        case 0:
          dev->ibm8514.pixel_xfer =
              (dev->ibm8514.pixel_xfer & 0xffffff00) | data;
          break;
        case 1:
          dev->ibm8514.pixel_xfer =
              (dev->ibm8514.pixel_xfer & 0xffff00ff) | (data << 8);
          break;
        case 2:
          dev->ibm8514.pixel_xfer =
              (dev->ibm8514.pixel_xfer & 0xff00ffff) | (data << 16);
          break;
        case 3:
          dev->ibm8514.pixel_xfer =
              (dev->ibm8514.pixel_xfer & 0x00ffffff) | (data << 24);
          dev->ibm8514.bus_size = 2; // Windows NT background pattern hack
          dev->ibm8514_pixel_xfer_complete();
          break;
        }
      }
    } else if (dev->ibm8514.bus_size >= 2) {
      switch (port & 0x0003) {
      case 0:
        dev->ibm8514.pixel_xfer = (dev->ibm8514.pixel_xfer & 0xffffff00) | data;
        break;
      case 1:
        dev->ibm8514.pixel_xfer =
            (dev->ibm8514.pixel_xfer & 0xffff00ff) | (data << 8);
        break;
      case 2:
        dev->ibm8514.pixel_xfer =
            (dev->ibm8514.pixel_xfer & 0xff00ffff) | (data << 16);
        break;
      case 3:
        dev->ibm8514.pixel_xfer =
            (dev->ibm8514.pixel_xfer & 0x00ffffff) | (data << 24);
        dev->ibm8514_pixel_xfer_complete();
        break;
      }
    }
    break;
  }

  default:
    LOG("S3 Accel: unhandled I/O write port=%04x data=%02x\n", port, data);
    break;
  }
}

bool CS3Trio64::IsAccelPort(u32 p) const {
  switch (p & 0xFFFE) { // word regs, we accept low/high bytes
                        // status/control
  case 0x42E8:          // SUBSYS_CNTL / SUBSYS_STAT (w/r)
  case 0x4AE8:          // ADVFUNC_CNTL
                        // coordinates
  case 0x4EE8:          // legacy compatibility stub
  case 0x82E8:          // CUR_Y
  case 0x86E8:          // CUR_X
  case 0x8AE8:          // DESTY_AXSTP
  case 0x8EE8:          // DESTX_DIASTP
  case 0xCAE8:          // DESTY / AXSTP alias (seen in 86Box mappings)
                        // dimensions / count
  case 0x96E8:          // MAJ_AXIS_PCNT
                        // mixes, masks, colors
  case 0xA2E8:          // BKGD_COLOR
  case 0xA6E8:          // FRGD_COLOR
  case 0xAAE8:          // WRT_MASK
  case 0xAEE8:          // RD_MASK
  case 0xB6E8:          // BKGD_MIX
  case 0xBAE8:          // FRGD_MIX
                        // extra control regs used by some S3 paths
  case 0xD2E8:          // ROP_MIX (word)
  case 0xBEE8:          // MULTIFUNC_CNTL (word)
  case 0x9EE8:          // SHORT_STROKE (word)
  case 0x9D48:          // SSV (older window alias)
  case 0xB2E8:          // COLOR_CMP (Color Compare register)
  case 0x9AE8:          // CMD
  case 0x92E8:
    return true;
  default:
    break;
  }
  // Host data (PIX_TRANS): accept 0xE2E8..0xE2EF byte-wise for color fills
  if ((p & 0xFFF0u) == 0xE2E0u)
    return true; // PIX_TRANS/host
  return false;
}

/**
 * Read from one of the PCI BAR (configurable address) memory ranges.
 **/
u32 CS3Trio64::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
#ifdef S3_LFB_TRACE
  if (lfb_trace_needs_first_access_note) {
    printf("%s: LFB first BAR access @+%llx size=%d\n", devid_string,
           (unsigned long long)address, dsize);
    lfb_trace_needs_first_access_note = false;
  }
#endif

  switch (bar) {
    // PCI memory range
  case 0:
    if (!lfb_active) {
      // No decode when LFB disabled  mimic bus-float/read-as-FFs
      // dsize is in bits here, as for mem_read.
      return (dsize == 8) ? 0xFFu : (dsize == 16) ? 0xFFFFu : 0xFFFFFFFFu;
    }
    return mem_read(address, dsize);
  }

  return 0;
}

/**
 * Write to one of the PCI BAR (configurable address) memory ranges.
 **/
void CS3Trio64::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                             u32 data) {
#ifdef DEBUG_PCI
  printf("[S3::WriteMem_Bar] func=%d bar=%d addr=%08X dsize=%d data=%08X\n",
         func, bar, address, dsize, data);
#endif
#ifdef S3_LFB_TRACE
  if (lfb_trace_needs_first_access_note) {
    printf("%s: LFB first BAR access @+%llx size=%d (W)\n", devid_string,
           (unsigned long long)address, dsize);
    lfb_trace_needs_first_access_note = false;
  }
#endif

  switch (bar) {
    // PCI Memory range
  case 0:
    if (!lfb_active) {
      // Ignore writes when LFB disabled
      return;
    }
    mem_write(address, dsize, data);
    return;
  }
}

// --- Only include LFB here; legacy VGA paths fall through to CVGA ---
u64 CS3Trio64::ReadMem(int index, u64 address, int dsize) {
  // LFB window (registered by update_linear_mapping)
  if (index == DEV_LFB_IDX && lfb_active && lfb_size) {
    const u64 off = address; // dispatcher already subtracts base
    // Trio64 "new MMIO" 128 KiB window at LFB+0x0100_0000 (CR53 bit3)
    //  - Lower half : PIX_TRANS FIFO (0xE2E8..0xE2EB) for 8/16/32-bit reads
    //  - Upper half : 8514/A register mirror at *E8 offsets (IsAccelPort())
    if (s3_new_mmio_enabled()) {
      printf("NEW MMIO READ !!!\n");
      const u64 win_lo = 0x01000000ull;
      const u64 win_mid = 0x01008000ull;
      const u64 win_hi = 0x01020000ull;
      if (off >= win_lo && off < win_hi) {
        if (off < win_mid) {
          switch (dsize) {
          case 8:
            return (u64)AccelIORead(0xE2E8);
          case 16:
            return (u64)AccelIORead(0xE2E8) | ((u64)AccelIORead(0xE2E9) << 8);
          case 32:
            return (u64)AccelIORead(0xE2E8) | ((u64)AccelIORead(0xE2E9) << 8) |
                   ((u64)AccelIORead(0xE2EA) << 16) |
                   ((u64)AccelIORead(0xE2EB) << 24);
          default:
            FAILURE(InvalidArgument, "Unsupported dsize");
          }
        } else {
          const u32 p = (u32)(off - win_lo); // ports by offset
          if (IsAccelPort(p)) {
            switch (dsize) {
            case 8:
              return (u64)AccelIORead(p);
            case 16:
              return (u64)AccelIORead(p + 0) | ((u64)AccelIORead(p + 1) << 8);
            case 32:
              return (u64)AccelIORead(p + 0) | ((u64)AccelIORead(p + 1) << 8) |
                     ((u64)AccelIORead(p + 2) << 16) |
                     ((u64)AccelIORead(p + 3) << 24);
            default:
              FAILURE(InvalidArgument, "Unsupported dsize");
            }
          }
        }
      }
    }
    if (off >= lfb_size)
      return 0;

    // Read little-endian from linear VRAM
    switch (dsize) {
    case 1:
      return vga.memory[off];
    case 2:
      return *(u16 *)(vga.memory + off);
    case 4:
      return *(u32 *)(vga.memory + off);
    case 8: {
      u64 v = *(u32 *)(vga.memory + off);
      v |= (u64) * (u32 *)(vga.memory + off + 4) << 32;
#ifdef S3_LFB_TRACE
      printf("%s: LFB R size=%d @%llx => %08" PRIx64 " (off=%llx)\n",
             devid_string, dsize, (unsigned long long)address, v,
             (unsigned long long)(address - lfb_base));
#endif
      return v;
    }
    default: // fall back byte-by-byte
    {
      u64 v = 0;
      for (int i = 0; i < dsize; ++i)
        v |= (u64)vga.memory[off + i] << (i * 8);
      return v;
    }
    }
  }

  // Everything else (all legacy VGA ports & A0000 region, option ROM, etc.)
  return CVGA::ReadMem(index, address, dsize);
}

void CS3Trio64::WriteMem(int index, u64 address, int dsize, u64 data) {
  if (index == DEV_LFB_IDX && lfb_active && lfb_size) {
    const u64 off = address; // dispatcher already subtracts base
    // Trio64 "new MMIO" 128 KiB window at LFB+0x0100_0000 (CR53 bit3)
    //  - Lower half : PIX_TRANS FIFO (0xE2E8..0xE2EB)
    //  - Upper half : 8514/A registers mirrored at *E8 offsets
    if (s3_new_mmio_enabled()) {
      const u64 win_lo = 0x01000000ull;
      const u64 win_hi = 0x01020000ull;
      if (off >= win_lo && off < win_hi) {
        const u32 mmio_off = (u32)(off - win_lo);
        switch (dsize) {
        case 8:
          mem_w(mmio_off, (u8)(data));
          return;
        case 16:
          mem_w(mmio_off, (u8)(data));
          mem_w(mmio_off + 1, (u8)(data >> 8));
          return;
        case 32:
          mem_w(mmio_off, (u8)(data));
          mem_w(mmio_off + 1, (u8)(data >> 8));
          mem_w(mmio_off + 2, (u8)(data >> 16));
          mem_w(mmio_off + 3, (u8)(data >> 24));
          return;
        default:
          FAILURE(InvalidArgument, "Unsupported dsize");
        }
      }
    }
    if (off >= lfb_size)
      return;

    // Write little-endian into linear VRAM
    switch (dsize) {
#ifdef S3_LFB_TRACE
      printf("%s: LFB W size=%d @%llx <= %08" PRIx64 " (off=%llx)\n",
             devid_string, dsize, (unsigned long long)address, data,
             (unsigned long long)(address));
#endif
    case 1:
      vga.memory[off] = (u8)data;
      break;
    case 2:
      *(u16 *)(vga.memory + off) = (u16)data;
      break;
    case 4:
      *(u32 *)(vga.memory + off) = (u32)data;
      break;
    case 8: {
      *(u32 *)(vga.memory + off) = (u32)(data & 0xffffffffu);
      *(u32 *)(vga.memory + off + 4) = (u32)(data >> 32);
      break;
    }
    default:
      for (int i = 0; i < dsize; ++i)
        vga.memory[off + i] = (u8)(data >> (i * 8));
      break;
    }

    state.vga_mem_updated = 1;
    return;
  }

  CVGA::WriteMem(index, address, dsize, data);
}

static inline u64 alpha_pio_phys_from_linear_base(u32 base) {
  // Typhoon: PIO vs system memory is selected by physical bit<43>.
  // We map PCI memory space windows by setting that bit.
  // 0x0000_0800_0000_0000 is the simplest way to assert <43>.
  return U64(0x0000080000000000) | (u64)base;
}

void CS3Trio64::lfb_recalc_and_map() {
  // BAR-only implementation: rely on BAR0 decoding in PCI core.
  // Just refresh cached enable/base/size; no RegisterMemory calls here.
  lfb_recalc_and_cache();
}

u32 CS3Trio64::config_read_custom(int func, u32 address, int dsize, u32 cur) {
  // For Trio64 we can just return the base value for now.
  // (TODO: synthesize bits in BAR0 reads from CR58..5A)
  return cur;
}

void CS3Trio64::config_write_custom(int func, u32 address, int dsize,
                                    u32 old_data, u32 new_data, u32 raw) {
  // Watch COMMAND (0x04..0x05) and BAR0 (0x10..0x13)..
  const bool is_command = (address == 0x04 || address == 0x05);
  const bool is_bar0 = (address >= 0x10 && address <= 0x13);

  if (is_command || is_bar0) {
    lfb_recalc_and_cache();
    // Apply/unapply DEV_LFB mapping when MSE or BAR0 changes
    lfb_recalc_and_map();
    trace_lfb_if_changed(is_command ? "PCI COMMAND" : "PCI BAR0");
  }
}

void CS3Trio64::trace_lfb_if_changed(const char *reason) {
  const bool cr58_on = s3_lfb_enabled(m_crtc_map.read_byte(0x58));
  const uint32_t sz = s3_lfb_size_from_cr58(m_crtc_map.read_byte(0x58));
  const uint32_t base = pci_bar0; // BAR-only base of truth
  const bool eff = pci_mem_enable && cr58_on && (base != 0);

  if (!lfb_trace_initialized || eff != lfb_trace_enabled_prev ||
      base != lfb_trace_base_prev || sz != lfb_trace_size_prev) {

#ifdef S3_LFB_TRACE
    printf("%s: LFB %s - MSE=%d CR58=%02x base=%08x size=%x (reason=%s)\n",
           devid_string, eff ? "ACTIVE(BAR)" : "INACTIVE(BAR)",
           (int)pci_mem_enable, m_crtc_map.read_byte(0x58), base, sz,
           reason ? reason : "n/a");
#endif

    lfb_trace_initialized = true;
    lfb_trace_enabled_prev = eff;
    lfb_trace_base_prev = base;
    lfb_trace_size_prev = sz;
  }
  if (eff)
    lfb_trace_needs_first_access_note = true;
}

// The linear window, offered as plain memory. Through BAR0 the S3 does
// nothing a plain store would not: mem_write puts the bytes into VRAM masked
// by its size and sets the dirty flag (which the renderer now assumes while
// the offer stands), so the CPUs may write VRAM directly. Offered while the
// window is live (COMMAND.MSE, CR58 enable, BAR0 set) and the card sits on
// the hose (a bridge maps its ranges elsewhere); withdrawn the moment any of
// that changes, which also flushes every CPU's page cache. Its size is the
// VRAM's: that is what the BAR path exposes, whatever CR58 says.
void CS3Trio64::refresh_direct_lfb() {
  static const bool enabled = [] {
    const char *e = getenv("ALPHABOX_LFB_DIRECT");
    return !(e && e[0] == '0');
  }();
  const bool live = enabled && !myBridge && pci_mem_enable && lfb_active &&
                    pci_bar0 != 0 && vga.memory;
  u64 base = 0, size = 0;
  if (live) {
    // The whole VRAM, not CR58's window size: through BAR0 the card masks
    // every access by the VRAM size and ignores that field (Windows sets
    // it to 64K and draws through all 4 MB).
    base = bus_address(false, pci_bar0);
    size = vga.svga_intf.vram_size;
  }
  static const bool trace = getenv("ALPHABOX_TRACE_LFB") != nullptr;
  if (trace)
    printf("%s: direct LFB: enabled=%d bridge=%d mse=%d active=%d bar0=%08x "
           "lfb_size=%x vram=%zx -> %s %llx+%llx\n",
           devid_string, (int)enabled, myBridge ? 1 : 0, (int)pci_mem_enable,
           (int)lfb_active, pci_bar0, lfb_size, vga.svga_intf.vram_size,
           live ? "offer" : "none", (unsigned long long)base,
           (unsigned long long)size);
  if (base == lfb_direct_base && size == lfb_direct_size)
    return;
  lfb_direct_base = base;
  lfb_direct_size = size;
  printf("%s: linear window %s for direct access (%llx + %llx)\n", devid_string,
         size ? "offered" : "withdrawn", (unsigned long long)base,
         (unsigned long long)size);
  cSystem->set_direct_memory(base, size, size ? vga.memory : nullptr);
}

void CS3Trio64::lfb_recalc_and_cache() {
  // COMMAND bit 1 (Memory Space Enable)
  // config_read takes the width in bits: the old sizes 2 and 4 matched no
  // case and read 0, so Memory Space Enable never set and the LFB never went
  // live.
  const u32 cmd = config_read(0, 0x04, 16); // COMMAND

  // BAR0: 32-bit memory BAR, mask off attribute bits
  const u32 bar0 = config_read(0, 0x10, 32) & 0xFFFFFFF0u;

  pci_mem_enable = (cmd & 0x0002) != 0; // saner, i think...

  pci_bar0 = bar0;

  // Honor CR58 enable/size while keeping BAR0 as the effective mapping base.
  const u8 cr58 = m_crtc_map.read_byte(0x58);
  lfb_base_ = bar0;                        // effective CPU-visible base = BAR0
  lfb_size_ = s3_lfb_size_from_cr58(cr58); // 64K/1M/2M/4M per Trio64
  lfb_enabled_ = pci_mem_enable && s3_lfb_enabled(cr58) && (bar0 != 0);
  refresh_direct_lfb();
}

/**
 * Read from Framebuffer.
 *
 * Not functional.
 **/
u32 CS3Trio64::mem_read(u32 address, int dsize) {
  const u32 mv = s3_vram_mask(); // (memsize - 1)
  const u32 off = address & mv;

  if (address >= 0xA0000 && address <= 0xBFFFF) {
    uint32_t offset = address - 0xA0000;
    // For SVGA modes, use MAME banking path
    return mem_r(offset);
  }

  switch (dsize) {
  case 8:
    return (u32)vga.memory[off];
  case 16:
    return (u32)vga.memory[off] | ((u32)vga.memory[(off + 1) & mv] << 8);
  case 32:
    return (u32)vga.memory[off] | ((u32)vga.memory[(off + 1) & mv] << 8) |
           ((u32)vga.memory[(off + 2) & mv] << 16) |
           ((u32)vga.memory[(off + 3) & mv] << 24);
  default:
    FAILURE(InvalidArgument, "Unsupported dsize");
  }
}

/**
 * Write to Framebuffer.
 *
 * Not functional.
 **/
void CS3Trio64::mem_write(u32 address, int dsize, u32 data) {
  const u32 mv = s3_vram_mask(); // (memsize - 1)
  const u32 off = address & mv;

  // Little-endian store into VRAM
  switch (dsize) {
  case 8:
    vga.memory[off] = (u8)data;
    break;
  case 16:
    vga.memory[off] = (u8)(data);
    vga.memory[(off + 1) & mv] = (u8)(data >> 8);
    break;
  case 32:
    vga.memory[off] = (u8)(data);
    vga.memory[(off + 1) & mv] = (u8)(data >> 8);
    vga.memory[(off + 2) & mv] = (u8)(data >> 16);
    vga.memory[(off + 3) & mv] = (u8)(data >> 24);
    break;
  default:
    FAILURE(InvalidArgument, "Unsupported dsize");
  }

  // Mark the affected tiles dirty so update() will serialize to the screen.
  state.vga_mem_updated = 1;
  if (vga.crtc.offset) {
    const unsigned nbytes = (dsize == 8) ? 1u : (dsize == 16 ? 2u : 4u);
    for (unsigned i = 0; i < nbytes; ++i) {
      const u32 p = (off + i) & mv;
      const u32 line = (vga.crtc.offset ? (p / vga.crtc.offset) : 0);
      const u32 col = (vga.crtc.offset ? (p % vga.crtc.offset) : 0);
      const unsigned xti = col / X_TILESIZE;
      const unsigned yti = line / Y_TILESIZE;
    }
  }
}

/**
 * Read from Legacy VGA Memory
 *
 * Calls vga_mem_read to read the data 1 byte at a time.
 **/
u32 CS3Trio64::legacy_read(u32 address, int dsize) {
  // MMIO alias active ?
  if (s3.cr53 & 0x10) {
    const u32 base = s3_mmio_base_off(state);
    if (address >= base && address <= base + 0xFFFFu) {
      const u32 off = address - base;
      if (off < 0x8000) {
        // PIX_TRANS read — same as MAME
        switch (dsize) {
        case 8:
          return (u32)AccelIORead(0xE2E8);
        case 16:
          return (u32)AccelIORead(0xE2E8) | ((u32)AccelIORead(0xE2E9) << 8);
        case 32:
          return (u32)AccelIORead(0xE2E8) | ((u32)AccelIORead(0xE2E9) << 8) |
                 ((u32)AccelIORead(0xE2EA) << 16) |
                 ((u32)AccelIORead(0xE2EB) << 24);
        default:
          FAILURE(InvalidArgument, "Unsupported dsize");
        }
      }
      // Upper half: register reads via AccelIORead
      if (IsAccelPort(off)) {
        switch (dsize) {
        case 8:
          return AccelIORead(off);
        case 16:
          return AccelIORead(off) | ((u32)AccelIORead(off + 1) << 8);
        case 32:
          return AccelIORead(off) | ((u32)AccelIORead(off + 1) << 8) |
                 ((u32)AccelIORead(off + 2) << 16) |
                 ((u32)AccelIORead(off + 3) << 24);
        default:
          FAILURE(InvalidArgument, "Unsupported dsize");
        }
      }
    }
  }

  return CVGACard::legacy_read(address, dsize);
}

/**
 * Read from I/O Port
 */
u32 CS3Trio64::io_read(u32 address, int dsize) {
  // Always intercept S3 8514/A-style ports. If the port block is not enabled
  // yet (CR40 == 0), hardware behaves benignly: reads return bus pull-ups,
  // writes are ignored.
  if (IsAccelPort(address)) {
    const bool ge_enabled = (m_crtc_map.read_byte(0x40) & 0x01) != 0;
    if (!ge_enabled) {
      switch (dsize) {
      case 8:
        return 0xFF;
      case 16:
        return 0xFFFF;
      case 32:
        return 0xFFFFFFFF;
      default:
        FAILURE(InvalidArgument, "Unsupported dsize");
      }
    }
    if ((m_crtc_map.read_byte(0x40) & 0x01) && IsAccelPort(address)) {
      switch (dsize) {
      case 8:
        return AccelIORead(address);
      case 16:
        return (u32)AccelIORead(address + 0) |
               ((u32)AccelIORead(address + 1) << 8);
      case 32:
        return (u32)AccelIORead(address + 0) |
               ((u32)AccelIORead(address + 1) << 8) |
               ((u32)AccelIORead(address + 2) << 16) |
               ((u32)AccelIORead(address + 3) << 24);
      default:
        FAILURE(InvalidArgument, "Unsupported dsize");
      }
    }
  }

  return CVGACard::io_read(address, dsize);
}

/**
 * Read one byte from an I/O port: the S3's own ports and gates, then the
 * standard VGA ones.
 **/
u8 CS3Trio64::io_read_b(u32 address) {
  switch (address) {
  case 0x3c3:
    return m_vga_subsys_enable ? 0x01 : 0x00;

  case 0x3c5:
    // PLL lock gate: SR09+ reads raw unless SR08 == 0x06
    if (vga.sequencer.index > 0x08 && vga.sequencer.data[0x08] != 0x06)
      return vga.sequencer.data[vga.sequencer.index];
    break;

  case 0x46E8:
    return m_video_subsys_enable_46e8;

  case 0x0102:
    return m_setup_option_select_0102;
  }
  return CVGACard::io_read_b(address);
}

/**
 * Write to I/O Port
 *
 * Calls io_write_b to write the data 1 byte at a time.
 */
void CS3Trio64::io_write(u32 address, int dsize, u32 data) {
  // 8514/A-style accel window (S3 engine). Intercept first, and swallow writes
  // until CR40 enables the port block (to avoid falling through to VGA path).
  if (IsAccelPort(address)) {
    const bool ge_enabled = (m_crtc_map.read_byte(0x40) & 0x01) != 0;
    if (!ge_enabled) {
      // Ignore early probes safely (hardware no-op)
      return;
    }
#ifdef DEBUG_VGA
    printf("ACCEL HIT @%04X dsize=%d data=%08X\n", (unsigned)address, dsize,
           (unsigned)data);
#endif
    switch (dsize) {
    case 8:
      AccelIOWrite(address, (u8)data);
      return;
    case 16:
      AccelIOWrite(address + 0, (u8)(data & 0xFF));
      AccelIOWrite(address + 1, (u8)((data >> 8) & 0xFF));
      return;
    case 32:
      AccelIOWrite(address + 0, (u8)((data >> 0) & 0xFF));
      AccelIOWrite(address + 1, (u8)((data >> 8) & 0xFF));
      AccelIOWrite(address + 2, (u8)((data >> 16) & 0xFF));
      AccelIOWrite(address + 3, (u8)((data >> 24) & 0xFF));
      return;
    default:
      FAILURE(InvalidArgument, "Unsupported dsize");
    }
  }

  CVGACard::io_write(address, dsize, data);
}

/**
 * Write one byte to an I/O port: the S3's own ports and gates, then the
 * standard VGA ones.
 **/
void CS3Trio64::io_write_b(u32 address, u8 data) {
  switch (address) {
  case 0x3c3:
    m_vga_subsys_enable = (data & 0x01) != 0;
    return;

  case 0x3c5:
    // PLL lock gate: SR09+ requires SR08 == 0x06
    if (vga.sequencer.index > 0x08 && vga.sequencer.data[0x08] != 0x06)
      return;
    // SR1A/SR1B: not in sequencer_map, but in 86box
    if (vga.sequencer.index == 0x1a) {
      s3.sr1a = data;
      return;
    }
    if (vga.sequencer.index == 0x1b) {
      s3.sr1b = data;
      return;
    }
    break;

  case 0x3c6:
  case 0x3c8:
  case 0x3c9:
    // CR33 bit 4 locks the RAMDAC write registers
    if (m_crtc_map.read_byte(0x33) & 0x10)
      return;
    break;

  case 0x46E8:
    // S3 Trio32/Trio64 "Video Subsystem Enable" / setup register
    // bit3 AD_DEC: enable video I/O+memory decode
    // bit4 EN_SUP: setup enable
    m_video_subsys_enable_46e8 = data;
    return;

  case 0x0102:
    // Setup Option Select (used in chip-wakeup sequences)
    m_setup_option_select_0102 = data;
    return;
  }
  CVGACard::io_write_b(address, data);
}

/**
 * Miscellaneous Output (0x3c2): CR34 bit 7 locks the clock select bits.
 **/
void CS3Trio64::write_b_3c2(u8 value) {
  if (m_crtc_map.read_byte(0x34) & 0x80) {
    // Preserve current clock_select (bits 3:2), take everything else from value
    value = (value & ~0x0C) | (vga.miscellaneous_output & 0x0C);
  }
  CVGACard::write_b_3c2(value);
}

/**
 * Read from the VGA Enable register (0x3c3)
 *
 * (Not sure where this comes from; doesn't seem to be in the VGA specs.)
 **/
u8 CS3Trio64::read_b_3c3() {
#if DEBUG_VGA_NOISY
  printf("VGA: 3c3 READ VGA ENABLE 0x%02x\n", vga_enabled());
#endif
  return vga_enabled();
}

// The hardware cursor is not flagged by vga_mem_updated; its mode, position,
// pattern address and pattern display start feed the refresh dirty-gate
// instead. The display start belongs here because Windows parks the cursor
// with it: it hides the pointer by moving the origin off the screen and
// setting CR4F to the last pattern row at the same time.
uint64_t CS3Trio64::hw_cursor_signature() const {
  return ((uint64_t)s3.cursor_mode << 56) |
         ((uint64_t)(s3.cursor_pattern_x & 0x3F) << 50) |
         ((uint64_t)(s3.cursor_pattern_y & 0x3F) << 44) |
         ((uint64_t)s3.cursor_start_addr << 24) |
         ((uint64_t)(s3.cursor_x & 0x7FF) << 12) |
         (uint64_t)(s3.cursor_y & 0x7FF);
}

// CR33 bit 6 locks the palette and overscan registers.
bool CS3Trio64::atc_palette_locked() const {
  return (m_crtc_map.read_byte(0x33) & 0x40) != 0;
}

// S3 CR5D extends H* with bit8 (0x100) and CR5E extends V* with bit10 (0x400)
void CS3Trio64::apply_extended_timing(int &h, int &v) {
  if (m_crtc_map.read_byte(0x5D) & 0x02)
    h |= 0x400; // multiplied by 8/2 = 4
  if (m_crtc_map.read_byte(0x5E) & 0x02)
    v |= 0x400;
}

inline uint32_t CS3Trio64::s3_vram_mask() const {
  const uint32_t sz =
      vga.svga_intf.vram_size ? vga.svga_intf.vram_size : (8u * 1024u * 1024u);
  return sz - 1u;
}

uint8_t CS3Trio64::get_video_depth() {
  switch (pc_vga_choosevideomode()) {
  case VGA_MODE:
  case RGB8_MODE:
    return 8;
  case RGB15_MODE:
  case RGB16_MODE:
    return 16;
  case RGB24_MODE:
  case RGB32_MODE:
    return 32;
  default:
    return 0;
  }
}

void CS3Trio64::mem_linear_w(uint32_t offset, uint8_t data) {
  CVGA::mem_linear_w(offset, data);
  state.vga_mem_updated = 1;
}

// One of the two cursor colours, unpacked from the CR4A/CR4B stack the way
// this mode's renderer unpacks a pixel out of display memory. The stack holds
// a pixel in the frame buffer's own format: the guest resets the stack
// pointer by reading CR45 and then writes one byte per byte of a pixel -- one
// at 8 bits per pixel, two at 15 and 16, three at 24 and 32 -- so a colour
// read as a palette index is only right in the 8-bit modes.
uint32_t CS3Trio64::cursor_color(const uint8_t *stack, uint8_t cur_mode) const {
  switch (cur_mode) {
  case RGB15_MODE: {
    const unsigned v = stack[0] | (stack[1] << 8);
    const unsigned r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
    return 0xff000000u | (((r << 3) | (r & 7)) << 16) |
           (((g << 3) | (g & 7)) << 8) | ((b << 3) | (b & 7));
  }

  case RGB16_MODE: {
    const unsigned v = stack[0] | (stack[1] << 8);
    const unsigned r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
    return 0xff000000u | (((r << 3) | (r & 7)) << 16) |
           (((g << 2) | (g & 3)) << 8) | ((b << 3) | (b & 7));
  }

  case RGB24_MODE:
  case RGB32_MODE:
    // A pixel little end first: blue, green, red. The fourth byte of the
    // stack is the 964's alpha byte and means nothing here.
    return 0xff000000u | (stack[2] << 16) | (stack[1] << 8) | stack[0];

  default:
    return pen(stack[0]);
  }
}

// Draws the hardware cursor over the frame the renderer has just produced.
//
// The cursor is a 64x64 pattern of two-bit pixels in display memory, sixteen
// bytes to a row: for every sixteen pixels a word of A bits and then the
// matching word of B bits. A and B together choose the background colour, the
// foreground colour, the screen underneath, or the screen inverted -- with
// the four meanings rotated when CR55 puts the cursor in X11 rather than
// MS-Windows mode. The Trio64 stores the pattern that way whatever the colour
// depth (it is the older 911/924 that pack a 64k-colour cursor differently),
// so only the colours change from mode to mode.
void CS3Trio64::draw_hardware_cursor(bitmap_rgb32 &bitmap,
                                     const rectangle &cliprect,
                                     uint8_t cur_mode) {
  if (!(s3.cursor_mode & 0x01)) // CR45 bit 0: HWGC ENB
    return;

  // The cursor is part of the VGA and SVGA picture only.
  if (cur_mode == SCREEN_OFF || cur_mode == TEXT_MODE ||
      cur_mode == MONO_MODE || cur_mode == CGA_MODE || cur_mode == EGA_MODE)
    return;

  const uint32_t bg_col = cursor_color(s3.cursor_bg, cur_mode);
  const uint32_t fg_col = cursor_color(s3.cursor_fg, cur_mode);

  // CR46/47 and CR48/49 put the top left corner of the pattern on the screen;
  // CR4E/CR4F say which pattern pixel is displayed there. That pair is how a
  // guest walks the pointer off the left or the top edge of the screen with
  // an origin that cannot go negative -- and Windows uses it to park the
  // cursor when it hides it -- so the pattern is drawn from an origin the
  // display start is subtracted from.
  const int origin_x =
      (int)(s3.cursor_x & 0x07ff) - (int)(s3.cursor_pattern_x & 0x3f);
  const int origin_y =
      (int)(s3.cursor_y & 0x07ff) - (int)(s3.cursor_pattern_y & 0x3f);

  const uint32_t base = (uint32_t)s3.cursor_start_addr * 1024; // CR4C/CR4D
  const uint32_t vram = vga.svga_intf.vram_size;
  const bool x11 = (s3.extended_dac_ctrl & 0x10) != 0; // CR55 bit 4

  const int min_x = (std::max)(cliprect.min_x, 0);
  const int min_y = (std::max)(cliprect.min_y, 0);
  const int max_x = (std::min)(cliprect.max_x, bitmap.width() - 1);
  const int max_y = (std::min)(cliprect.max_y, bitmap.height() - 1);

  for (int y = 0; y < 64; y++) {
    const int sy = origin_y + y;
    if (sy < min_y || sy > max_y)
      continue;

    uint32_t *const dst = &bitmap.pix(sy);
    for (int x = 0; x < 64; x++) {
      const int sx = origin_x + x;
      if (sx < min_x || sx > max_x)
        continue;

      const uint32_t src = base + y * 16 + (x >> 4) * 4;
      const unsigned bit = 15 - (x & 15);
      const unsigned a =
          (vga.memory[(src + 0) % vram] << 8 | vga.memory[(src + 1) % vram]) >>
          bit;
      const unsigned b =
          (vga.memory[(src + 2) % vram] << 8 | vga.memory[(src + 3) % vram]) >>
          bit;

      const unsigned ab = ((a & 1) << 1) | (b & 1);
      if (x11) {
        // X11: the A bit says whether the pixel is the cursor's at all, the
        // B bit which of its two colours it takes.
        if (ab == 0x02)
          dst[sx] = bg_col;
        else if (ab == 0x03)
          dst[sx] = fg_col;
      } else {
        // MS-Windows: a clear A bit means one of the cursor's two colours, a
        // set one the screen underneath, as it is or inverted.
        if (ab == 0x00)
          dst[sx] = bg_col;
        else if (ab == 0x01)
          dst[sx] = fg_col;
        else if (ab == 0x03)
          dst[sx] = ~dst[sx] | 0xff000000u;
      }
    }
  }
}
