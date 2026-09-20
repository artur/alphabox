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
 */

/**
 * \file
 * The device plumbing shared by every PCI VGA card on the CVGA core (see
 * VGACard.hpp). Lifted out of S3Trio64.cpp unchanged in behaviour.
 **/

#include "VGACard.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "emu/emu.hpp"
#include "gui/gui.hpp"

#define LOG_WARN (1U << 1)
#define LOG_REGS (1U << 2)
#define LOG_CRTC (1U << 4) // CRTC setups with monitor geometry

// #define VERBOSE (LOG_GENERAL | LOG_CRTC | LOG_WARN | LOG_REGS)
#include "logmacro.hpp"

#define LOGREGS(...) LOGMASKED(LOG_REGS, __VA_ARGS__)
#define LOGCRTC(...) LOGMASKED(LOG_CRTC, __VA_ARGS__)

CVGACard::CVGACard(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CVGA(cfg, c, pcibus, pcidev) {}

/**
 * Destructor. Only a backstop: by the time it runs the derived card is
 * already destroyed, while the render thread calls that card's hooks. A
 * card must therefore stop the thread in its own destructor; the second
 * call here is then a no-op.
 **/
CVGACard::~CVGACard() { stop_threads(); }

/**
 * Install the register handlers: the standard VGA ones first, then the
 * card's, which add extended registers and may replace standard ones.
 **/
void CVGACard::init_maps() {
  vga_crtc_map(m_crtc_map);
  vga_sequencer_map(m_seq_map);
  vga_gc_map(m_gc_map);
  vga_attribute_map(m_atc_map);
  crtc_map(m_crtc_map);
  sequencer_map(m_seq_map);
  gc_map(m_gc_map);
  attribute_map(m_atc_map);
}

/**************************************
 *
 * Standard VGA registers (MAME vga_device maps, with the ES40 redraw
 * side effects)
 *
 *************************************/

void CVGACard::vga_crtc_map(address_map &map) {
  map(0x00, 0x00)
      .lrw8(NAME([this](offs_t offset) { return vga.crtc.horz_total & 0xff; }),
            NAME([this](offs_t offset, u8 data) {
              // doom (DOS) tries to write to protected regs
              LOGCRTC("CR00 H total %02x %s", data,
                      vga.crtc.protect_enable ? "P?\n" : "-> ");
              if (vga.crtc.protect_enable)
                return;
              vga.crtc.horz_total =
                  (vga.crtc.horz_total & ~0xff) | (data & 0xff);
              LOGCRTC("%04d\n", vga.crtc.horz_total);
              recompute_params();
            }));
  map(0x01, 0x01)
      .lrw8(
          NAME([this](offs_t offset) { return vga.crtc.horz_disp_end & 0xff; }),
          NAME([this](offs_t offset, u8 data) {
            LOGCRTC("CR01 H display end %02x %s", data,
                    vga.crtc.protect_enable ? "P?\n" : "-> ");
            if (vga.crtc.protect_enable)
              return;
            vga.crtc.horz_disp_end =
                (vga.crtc.horz_disp_end & ~0xff) | (data & 0xff);
            LOGCRTC("%04d\n", vga.crtc.horz_disp_end);
            recompute_params();
          }));
  map(0x02, 0x02)
      .lrw8(NAME([this](offs_t offset) {
              return vga.crtc.horz_blank_start & 0xff;
            }),
            NAME([this](offs_t offset, u8 data) {
              LOGCRTC("CR02 H start blank %02x %s", data,
                      vga.crtc.protect_enable ? "P?\n" : "-> ");
              if (vga.crtc.protect_enable)
                return;
              vga.crtc.horz_blank_start =
                  (vga.crtc.horz_blank_start & ~0xff) | (data & 0xff);
              LOGCRTC("%04d\n", vga.crtc.horz_blank_start);
            }));
  map(0x03, 0x03)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = vga.crtc.horz_blank_end & 0x1f;
              res |= (vga.crtc.disp_enable_skew & 3) << 5;
              res |= (vga.crtc.evra & 1) << 7;
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              LOGCRTC("CR03 H blank end %02x %s", data,
                      vga.crtc.protect_enable ? "P?\n" : "-> ");
              if (vga.crtc.protect_enable)
                return;
              vga.crtc.horz_blank_end &= ~0x1f;
              vga.crtc.horz_blank_end |= data & 0x1f;
              vga.crtc.disp_enable_skew = (data & 0x60) >> 5;
              vga.crtc.evra = BIT(data, 7);
              LOGCRTC("%04d evra %d display enable skew %01x\n",
                      vga.crtc.horz_blank_end, vga.crtc.evra,
                      vga.crtc.disp_enable_skew);
            }));
  map(0x04, 0x04)
      .lrw8(NAME([this](offs_t offset) {
              return vga.crtc.horz_retrace_start & 0xff;
            }),
            NAME([this](offs_t offset, u8 data) {
              LOGCRTC("CR04 H retrace start %02x %s", data,
                      vga.crtc.protect_enable ? "P?\n" : "-> ");
              if (vga.crtc.protect_enable)
                return;
              vga.crtc.horz_retrace_start =
                  (vga.crtc.horz_retrace_start & ~0xff) | (data & 0xff);
              LOGCRTC("%04d\n", vga.crtc.horz_retrace_start);
            }));
  map(0x05, 0x05)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.horz_blank_end & 0x20) << 2;
              res |= (vga.crtc.horz_retrace_skew & 3) << 5;
              res |= (vga.crtc.horz_retrace_end & 0x1f);
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              LOGCRTC("CR05 H blank end %02x %s", data,
                      vga.crtc.protect_enable ? "P?\n" : "-> ");
              if (vga.crtc.protect_enable)
                return;
              vga.crtc.horz_blank_end &= ~0x20;
              vga.crtc.horz_blank_end |= ((data & 0x80) >> 2);
              vga.crtc.horz_retrace_skew = ((data & 0x60) >> 5);
              vga.crtc.horz_retrace_end =
                  (vga.crtc.horz_retrace_end & ~0x1f) | (data & 0x1f);
              LOGCRTC("%04d retrace skew %01d retrace end %02d\n",
                      vga.crtc.horz_blank_end, vga.crtc.horz_retrace_skew,
                      vga.crtc.horz_retrace_end);
            }));
  map(0x06, 0x06)
      .lrw8(NAME([this](offs_t offset) { return vga.crtc.vert_total & 0xff; }),
            NAME([this](offs_t offset, u8 data) {
              LOGCRTC("CR06 V total %02x %s", data,
                      vga.crtc.protect_enable ? "P?\n" : "-> ");
              if (vga.crtc.protect_enable)
                return;
              vga.crtc.vert_total &= ~0xff;
              vga.crtc.vert_total |= data & 0xff;
              LOGCRTC("%04d\n", vga.crtc.vert_total);
              recompute_params();
            }));
  // Overflow Register
  map(0x07, 0x07)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.line_compare & 0x100) >> 4;
              res |= (vga.crtc.vert_retrace_start & 0x200) >> 2;
              res |= (vga.crtc.vert_disp_end & 0x200) >> 3;
              res |= (vga.crtc.vert_total & 0x200) >> 4;
              res |= (vga.crtc.vert_blank_start & 0x100) >> 5;
              res |= (vga.crtc.vert_retrace_start & 0x100) >> 6;
              res |= (vga.crtc.vert_disp_end & 0x100) >> 7;
              res |= (vga.crtc.vert_total & 0x100) >> 8;
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.line_compare &= ~0x100;
              vga.crtc.line_compare |= ((data & 0x10) << (8 - 4));
              LOGCRTC("CR07 Overflow %02x -> line compare %04d %s", data,
                      vga.crtc.line_compare,
                      vga.crtc.protect_enable ? "P?\n" : "");
              if (vga.crtc.protect_enable)
                return;
              vga.crtc.vert_total &= ~0x300;
              vga.crtc.vert_retrace_start &= ~0x300;
              vga.crtc.vert_disp_end &= ~0x300;
              vga.crtc.vert_blank_start &= ~0x100;
              vga.crtc.vert_retrace_start |= ((data & 0x80) << (9 - 7));
              vga.crtc.vert_disp_end |= ((data & 0x40) << (9 - 6));
              vga.crtc.vert_total |= ((data & 0x20) << (9 - 5));
              vga.crtc.vert_blank_start |= ((data & 0x08) << (8 - 3));
              vga.crtc.vert_retrace_start |= ((data & 0x04) << (8 - 2));
              vga.crtc.vert_disp_end |= ((data & 0x02) << (8 - 1));
              vga.crtc.vert_total |= ((data & 0x01) << (8 - 0));
              LOGCRTC("V total %04d V retrace start %04d V display end %04d V "
                      "blank start %04d\n",
                      vga.crtc.vert_total, vga.crtc.vert_retrace_start,
                      vga.crtc.vert_disp_end, vga.crtc.vert_blank_start);
              recompute_params();
            }));
  // Preset Row Scan Register
  map(0x08, 0x08)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.byte_panning & 3) << 5;
              res |= (vga.crtc.preset_row_scan & 0x1f);
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.byte_panning = (data & 0x60) >> 5;
              vga.crtc.preset_row_scan = (data & 0x1f);
              LOGCRTC("CR08 Preset Row Scan %02x -> %02d byte panning %d\n",
                      data, vga.crtc.preset_row_scan, vga.crtc.byte_panning);
            }));
  // Maximum Scan Line Register
  map(0x09, 0x09)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.maximum_scan_line - 1) & 0x1f;
              res |= (vga.crtc.scan_doubling & 1) << 7;
              res |= (vga.crtc.line_compare & 0x200) >> 3;
              res |= (vga.crtc.vert_blank_start & 0x200) >> 4;
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.line_compare &= ~0x200;
              vga.crtc.vert_blank_start &= ~0x200;
              vga.crtc.scan_doubling = ((data & 0x80) >> 7);
              vga.crtc.line_compare |= ((data & 0x40) << (9 - 6));
              vga.crtc.vert_blank_start |= ((data & 0x20) << (9 - 5));
              vga.crtc.maximum_scan_line = (data & 0x1f) + 1;
              LOGCRTC("CR09 Maximum Scan Line %02x -> %02d V blank start %04d "
                      "line compare %04d scan doubling %d\n",
                      data, vga.crtc.maximum_scan_line,
                      vga.crtc.vert_blank_start, vga.crtc.line_compare,
                      vga.crtc.scan_doubling);
            }));
  map(0x0a, 0x0a)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.cursor_scan_start & 0x1f);
              res |= ((vga.crtc.cursor_enable & 1) ^ 1) << 5;
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.cursor_enable = ((data & 0x20) ^ 0x20) >> 5;
              vga.crtc.cursor_scan_start = data & 0x1f;
              state.vga_mem_updated =
                  1; // text mode: cursor shape change trigger
            }));
  map(0x0b, 0x0b)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.cursor_skew & 3) << 5;
              res |= (vga.crtc.cursor_scan_end & 0x1f);
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.cursor_skew = (data & 0x60) >> 5;
              vga.crtc.cursor_scan_end = data & 0x1f;
              state.vga_mem_updated =
                  1; // text mode: cursor shape change trigger
            }));
  map(0x0c, 0x0d)
      .lrw8(
          NAME([this](offs_t offset) {
            return (vga.crtc.start_addr_latch >> ((offset & 1) ^ 1) * 8) & 0xff;
          }),
          NAME([this](offs_t offset, u8 data) {
            vga.crtc.start_addr_latch &= ~(0xff << (((offset & 1) ^ 1) * 8));
            vga.crtc.start_addr_latch |= (data << (((offset & 1) ^ 1) * 8));
            state.vga_mem_updated = 1; // text mode: cursor shape change trigger
          }));
  map(0x0e, 0x0f)
      .lrw8(NAME([this](offs_t offset) {
              return (vga.crtc.cursor_addr >> ((offset & 1) ^ 1) * 8) & 0xff;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.cursor_addr &= ~(0xff << (((offset & 1) ^ 1) * 8));
              vga.crtc.cursor_addr |= (data << (((offset & 1) ^ 1) * 8));
              state.vga_mem_updated =
                  1; // text mode: cursor shape change trigger
            }));
  map(0x10, 0x10)
      .lrw8(NAME([this](offs_t offset) {
              return vga.crtc.vert_retrace_start & 0xff;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.vert_retrace_start &= ~0xff;
              vga.crtc.vert_retrace_start |= data & 0xff;
              LOGCRTC("CR10 V retrace start %02x -> %04d\n", data,
                      vga.crtc.vert_retrace_start);
            }));
  map(0x11, 0x11)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.protect_enable & 1) << 7;
              res |= (vga.crtc.bandwidth & 1) << 6;
              res |= (vga.crtc.vert_retrace_end & 0xf);
              res |= (vga.crtc.irq_clear & 1) << 4;
              res |= (vga.crtc.irq_disable & 1) << 5;
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.protect_enable = BIT(data, 7);
              vga.crtc.bandwidth = BIT(data, 6);
              // IRQ: Original VGA only supports this for PS/2, but clone cards
              // may supports this on ISA too see
              // https://scalibq.wordpress.com/2022/12/06/the-myth-of-the-vertical-retrace-interrupt/
              vga.crtc.irq_disable = BIT(data, 5);
              vga.crtc.irq_clear = BIT(data, 4);
              vga.crtc.vert_retrace_end =
                  (vga.crtc.vert_retrace_end & ~0xf) | (data & 0x0f);

              if (vga.crtc.irq_clear == 0) {
                vga.crtc.irq_latch = 0;
                m_vsync_cb(0);
              }

              LOGCRTC("CR11 V retrace end %02x -> %02d protect enable %d "
                      "bandwidth %d irq %02x\n",
                      data, vga.crtc.vert_retrace_end, vga.crtc.protect_enable,
                      vga.crtc.bandwidth, data & 0x30);
            }));
  map(0x12, 0x12)
      .lrw8(
          NAME([this](offs_t offset) { return vga.crtc.vert_disp_end & 0xff; }),
          NAME([this](offs_t offset, u8 data) {
            vga.crtc.vert_disp_end &= ~0xff;
            vga.crtc.vert_disp_end |= data & 0xff;
            LOGCRTC("CR12 V display end %02x -> %04d\n", data,
                    vga.crtc.vert_disp_end);
            recompute_params();
          }));
  map(0x13, 0x13)
      .lrw8(NAME([this](offs_t offset) { return vga.crtc.offset & 0xff; }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.offset &= ~0xff;
              vga.crtc.offset |= data & 0xff;
            }));
  map(0x14, 0x14)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.dw & 1) << 6;
              res |= (vga.crtc.div4 & 1) << 5;
              res |= (vga.crtc.underline_loc & 0x1f);
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.dw = (data & 0x40) >> 6;
              vga.crtc.div4 = (data & 0x20) >> 5;
              vga.crtc.underline_loc = (data & 0x1f);
            }));
  map(0x15, 0x15)
      .lrw8(NAME([this](offs_t offset) {
              return vga.crtc.vert_blank_start & 0xff;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.vert_blank_start &= ~0xff;
              vga.crtc.vert_blank_start |= data & 0xff;
              LOGCRTC("CR15 V blank start %02x -> %04d\n", data,
                      vga.crtc.vert_blank_start);
            }));
  map(0x16, 0x16)
      .lrw8(NAME([this](offs_t offset) {
              return vga.crtc.vert_blank_end & 0x7f;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.vert_blank_end =
                  (vga.crtc.vert_blank_end & ~0x7f) | (data & 0x7f);
              LOGCRTC("CR16 V blank end %02x -> %04d\n", data,
                      vga.crtc.vert_blank_end);
            }));
  map(0x17, 0x17)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.crtc.sync_en & 1) << 7;
              res |= (vga.crtc.word_mode & 1) << 6;
              res |= (vga.crtc.aw & 1) << 5;
              res |= (vga.crtc.div2 & 1) << 3;
              res |= (vga.crtc.sldiv & 1) << 2;
              res |= (vga.crtc.map14 & 1) << 1;
              res |= (vga.crtc.map13 & 1) << 0;
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.crtc.sync_en = BIT(data, 7);
              vga.crtc.word_mode = BIT(data, 6);
              vga.crtc.aw = BIT(data, 5);
              vga.crtc.div2 = BIT(data, 3);
              vga.crtc.sldiv = BIT(data, 2);
              vga.crtc.map14 = BIT(data, 1);
              vga.crtc.map13 = BIT(data, 0);
              LOGCRTC("CR17 Mode control %02x -> Sync Enable %d Word/Byte %d "
                      "Address Wrap select %d\n",
                      data, vga.crtc.sync_en, vga.crtc.word_mode, vga.crtc.aw);
              LOGCRTC("\tDIV2 %d Scan Line Divide %d MAP14 %d MAP13 %d\n",
                      vga.crtc.div2, vga.crtc.sldiv, vga.crtc.map14,
                      vga.crtc.map13);
            }));
  map(0x18, 0x18)
      .lrw8(
          NAME([this](offs_t offset) { return vga.crtc.line_compare & 0xff; }),
          NAME([this](offs_t offset, u8 data) {
            vga.crtc.line_compare &= ~0xff;
            vga.crtc.line_compare |= data & 0xff;
            LOGCRTC("CR18 Line Compare %02x -> %04d\n", data,
                    vga.crtc.line_compare);
          }));
  // TODO: (undocumented) CR22 Memory Data Latch Register (read only)
  // map(0x22, 0x22).lr8(
  // (undocumented) CR24 Attribute Controller Toggle Register (read only)
  // 0--- ---- index
  // 1--- ---- data
  map(0x24, 0x24).lr8(NAME([this](offs_t offset) {
    if (!machine().side_effects_disabled())
      LOG("CR24 read undocumented Attribute reg\n");
    return vga.attribute.state << 7;
  }));
}

void CVGACard::vga_sequencer_map(address_map &map) {
  // TODO: legacy fallback trick
  map(0x00, 0xff).lr8(NAME([this](offs_t offset) {
    const u8 res = vga.sequencer.data[offset];
    if (!machine().side_effects_disabled())
      LOGREGS(
          "Reading unmapped sequencer read register [%02x] -> %02x (SVGA?)\n",
          offset, res);
    return res;
  }));
  //  map(0x00, 0x00) Reset Register
  map(0x00, 0x00)
      .lw8( // es40 deviation
          NAME([this](offs_t offset, u8 data) {
            if ((vga.sequencer.data[0] & 0x01) && ((data & 0x01) == 0)) {
              vga.sequencer.data[3] = 0;
              vga.sequencer.char_sel.A = 0;
              vga.sequencer.char_sel.B = 0;
              vga.sequencer.char_sel.base[0] = 0x20000;
              vga.sequencer.char_sel.base[1] = 0x20000;
              bx_gui->lock();
              bx_gui->set_text_charmap(&vga.memory[0x20000]);
              bx_gui->unlock();
              state.vga_mem_updated = 1;
            }
            vga.sequencer.data[0] = data;
          }));
  // SR01: Clocking Mode Register
  map(0x01, 0x01).lw8(NAME([this](offs_t offset, u8 data) {
    vga.sequencer.data[1] = data & 0x3f;
  }));
  map(0x02, 0x02).lw8(NAME([this](offs_t offset, u8 data) {
    vga.sequencer.map_mask = data & 0xf;
  }));
  // SR03: Character Map Select
  map(0x03, 0x03).lw8(NAME([this](offs_t offset, u8 data) {
    /* --2- 84-- character select A
       ---2 --84 character select B */
    vga.sequencer.char_sel.A =
        (((data & 0xc) >> 2) << 1) | ((data & 0x20) >> 5);
    vga.sequencer.char_sel.B =
        (((data & 0x3) >> 0) << 1) | ((data & 0x10) >> 4);
    // optimization for screen update inner loop
    vga.sequencer.char_sel.base[0] =
        0x20000 + (vga.sequencer.char_sel.B * 0x2000);
    vga.sequencer.char_sel.base[1] =
        0x20000 + (vga.sequencer.char_sel.A * 0x2000);
    // if(data)
    //	popmessage("Char SEL checker (%02x
    //%02x)\n",vga.sequencer.char_sel.A,vga.sequencer.char_sel.B);
  }));
  // Sequencer Memory Mode Register
  //  map(0x04, 0x04)
  map(0x04, 0x04).lw8(NAME([this](offs_t offset, u8 data) {
    vga.sequencer.data[4] = data;
  }));
}

void CVGACard::vga_gc_map(address_map &map) {
  map.unmap_value_high();
  map(0x00, 0x00)
      .lrw8(NAME([this](offs_t offset) { return vga.gc.set_reset & 0xf; }),
            NAME([this](offs_t offset, u8 data) {
              vga.gc.set_reset = data & 0xf;
            }));
  map(0x01, 0x01)
      .lrw8(
          NAME([this](offs_t offset) { return vga.gc.enable_set_reset & 0xf; }),
          NAME([this](offs_t offset, u8 data) {
            vga.gc.enable_set_reset = data & 0xf;
          }));
  map(0x02, 0x02)
      .lrw8(NAME([this](offs_t offset) { return vga.gc.color_compare & 0xf; }),
            NAME([this](offs_t offset, u8 data) {
              vga.gc.color_compare = data & 0xf;
            }));
  map(0x03, 0x03)
      .lrw8(NAME([this](offs_t offset) {
              return ((vga.gc.logical_op & 3) << 3) | (vga.gc.rotate_count & 7);
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.gc.logical_op = (data & 0x18) >> 3;
              vga.gc.rotate_count = data & 7;
            }));
  map(0x04, 0x04)
      .lrw8(NAME([this](offs_t offset) { return vga.gc.read_map_sel & 3; }),
            NAME([this](offs_t offset, u8 data) {
              vga.gc.read_map_sel = data & 3;
            }));
  map(0x05, 0x05)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.gc.shift256 & 1) << 6;
              res |= (vga.gc.shift_reg & 1) << 5;
              res |= (vga.gc.host_oe & 1) << 4;
              res |= (vga.gc.read_mode & 1) << 3;
              res |= (vga.gc.write_mode & 3);
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              vga.gc.shift256 = BIT(data, 6);
              vga.gc.shift_reg = BIT(data, 5);
              vga.gc.host_oe = BIT(data, 4);
              vga.gc.read_mode = BIT(data, 3);
              vga.gc.write_mode = data & 3;
              // if(data & 0x10 && vga.gc.alpha_dis)
              //   popmessage("Host O/E enabled, contact MAMEdev");
            }));
  map(0x06, 0x06)
      .lrw8(NAME([this](offs_t offset) {
              u8 res = (vga.gc.memory_map_sel & 3) << 2;
              res |= (vga.gc.chain_oe & 1) << 1;
              res |= (vga.gc.alpha_dis & 1);
              return res;
            }),
            NAME([this](offs_t offset, u8 data) {
              u8 prev_memory_mapping = vga.gc.memory_map_sel; // es40ism
              bool prev_alpha_dis = vga.gc.alpha_dis;         // es40ism
              // MAME
              vga.gc.memory_map_sel = (data & 0xc) >> 2;
              vga.gc.chain_oe = BIT(data, 1);
              vga.gc.alpha_dis = BIT(data, 0);
              // if(data & 2 && vga.gc.alpha_dis)
              //   popmessage("Chain O/E enabled, contact MAMEdev");
              //  ES40 side-effects: redraw on mapping/mode change
              if (prev_memory_mapping != vga.gc.memory_map_sel)
                redraw_area(0, 0, old_iWidth, old_iHeight);
              if (prev_alpha_dis != vga.gc.alpha_dis) {
                redraw_area(0, 0, old_iWidth, old_iHeight);
                old_iHeight = 0;
              }
            }));
  map(0x07, 0x07)
      .lrw8(
          NAME([this](offs_t offset) { return vga.gc.color_dont_care & 0xf; }),
          NAME([this](offs_t offset, u8 data) {
            vga.gc.color_dont_care = data & 0xf;
          }));
  map(0x08, 0x08)
      .lrw8(NAME([this](offs_t offset) { return vga.gc.bit_mask & 0xff; }),
            NAME([this](offs_t offset, u8 data) {
              vga.gc.bit_mask = data & 0xff;
            }));
}

void CVGACard::vga_attribute_map(address_map &map) {
  map.global_mask(0x3f);
  map.unmap_value_high();

  // Palette Index Registers 0x00..0x0f
  map(0x00, 0x0f)
      .lrw8(NAME([this](offs_t offset) {
              return vga.attribute.data[offset & 0x1f];
            }),
            NAME([this](offs_t offset, u8 data) {
              if (atc_palette_locked())
                return;
              u8 idx = offset & 0x0f;
              if (vga.attribute.data[idx] != (data & 0x3f)) {
                vga.attribute.data[idx] = data & 0x3f;
                redraw_area(0, 0, old_iWidth, old_iHeight);
              }
            }));

  // 0x20-0x2f mirrors — NOP (MAME: map(0x20, 0x2f).noprw())
  // Our address_map doesn't have noprw(); unmap_value_high covers reads.
  // Writes to this range are simply ignored by having no handler installed.

  // Mode Control (index 0x10, mirrored at 0x30)
  map(0x10, 0x10)
      .mirror(0x20)
      .lrw8(NAME([this](offs_t offset) { return vga.attribute.data[0x10]; }),
            NAME([this](offs_t offset, u8 data) {
              const u8 prev = vga.attribute.data[0x10];
              vga.attribute.data[0x10] = data & 0x3f; // MAME canonical

              // ES40 side-effects: detect bit changes from previous value
              if (BIT(data, 2) != BIT(prev, 2)) { // enable_line_graphics
                bx_gui->lock();
                bx_gui->set_text_charmap(
                    &vga.memory[0x20000 + vga.sequencer.char_sel.A]);
                bx_gui->unlock();
                state.vga_mem_updated = 1;
              }
              if (BIT(data, 7) != BIT(prev, 7)) { // internal_palette_size
                redraw_area(0, 0, old_iWidth, old_iHeight);
              }
            }));

  // Overscan Color (index 0x11, mirrored at 0x31)
  map(0x11, 0x11)
      .mirror(0x20)
      .lrw8(NAME([this](offs_t offset) { return vga.attribute.data[0x11]; }),
            NAME([this](offs_t offset, u8 data) {
              if (atc_palette_locked())
                return;
              vga.attribute.data[0x11] = data & 0x3f; // MAME canonical
            }));

  // Color Plane Enable (index 0x12, mirrored at 0x32)
  map(0x12, 0x12)
      .mirror(0x20)
      .lrw8(NAME([this](offs_t offset) { return vga.attribute.data[0x12]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.attribute.data[0x12] = data & 0x3f; // MAME canonical
              redraw_area(0, 0, old_iWidth, old_iHeight);
            }));

  // Horizontal PEL Shift (index 0x13, mirrored at 0x33)
  map(0x13, 0x13)
      .mirror(0x20)
      .lrw8(NAME([this](offs_t offset) { return vga.attribute.data[0x13]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.attribute.pel_shift_latch = data & 0xf; // MAME canonical
              vga.attribute.data[0x13] = data & 0xf;      // MAME canonical
              redraw_area(0, 0, old_iWidth, old_iHeight);
            }));

  // Color Select (index 0x14, mirrored at 0x34)
  map(0x14, 0x14)
      .mirror(0x20)
      .lrw8(NAME([this](offs_t offset) { return vga.attribute.data[0x14]; }),
            NAME([this](offs_t offset, u8 data) {
              vga.attribute.pel_shift_latch =
                  data & 0xf; // MAME canonical (yes, same field)
              vga.attribute.data[0x14] = data & 0xf; // MAME canonical
              redraw_area(0, 0, old_iWidth, old_iHeight);
            }));
}

void CVGACard::add_vga_legacy_ranges() {
  add_legacy_io(LEGACY_IO_3B4, 0x3b4, 2);
  add_legacy_io(LEGACY_IO_3BA, 0x3ba, 2);
  add_legacy_io(LEGACY_IO_3C0, 0x3c0, 16);
  add_legacy_io(LEGACY_IO_3D4, 0x3d4, 2);
  add_legacy_io(LEGACY_IO_3DA, 0x3da, 1);

  /* The VGA BIOS we use sends text messages to port 0x500. That is not a
     port of the card at all -- it is a channel between the BIOS and us --
     so the machine listens for it, and a card behind a bridge is heard
     just as well as one on the hose. */
  add_hose_io(LEGACY_IO_BIOS_MSG, 0x500, 1);
  bios_message_size = 0;
  bios_message[0] = '\0';

  // Legacy video address space: A0000 -> bffff
  add_legacy_mem(LEGACY_MEM_VGA, 0xa0000, 128 * 1024);
}

/**
 * Read from one of the Legacy (fixed-address) memory ranges.
 **/
u32 CVGACard::ReadMem_Legacy(int index, u32 address, int dsize) {
  switch (index) {
  case LEGACY_IO_3B4:
    return io_read(address + 0x3b4, dsize);
  case LEGACY_IO_3C0:
    return io_read(address + 0x3c0, dsize);
  case LEGACY_IO_3BA:
    return io_read(address + 0x3ba, dsize);
  case LEGACY_MEM_VGA:
    return legacy_read(address, dsize);
  case LEGACY_MEM_ROM:
    return rom_read(address, dsize);
  case LEGACY_IO_BIOS_MSG:
    return 0;
  case LEGACY_IO_3D4:
    return io_read(address + 0x3d4, dsize);
  case LEGACY_IO_3DA:
    return io_read(address + 0x3da, dsize);
  default:
    return card_legacy_read(index, address, dsize);
  }
}

/**
 * Write to one of the Legacy (fixed-address) memory ranges.
 **/
void CVGACard::WriteMem_Legacy(int index, u32 address, int dsize, u32 data) {
  switch (index) {
  case LEGACY_IO_3B4:
    io_write(address + 0x3b4, dsize, data);
    return;
  case LEGACY_IO_3C0:
    io_write(address + 0x3c0, dsize, data);
    return;
  case LEGACY_IO_3BA:
    io_write(address + 0x3ba, dsize, data);
    return;
  case LEGACY_MEM_VGA:
    legacy_write(address, dsize, data);
    return;
  case LEGACY_MEM_ROM:
    return;
  case LEGACY_IO_BIOS_MSG: {
    const char c = (char)(data & 0xff);
    // A full buffer is flushed as a line rather than overrun.
    if (bios_message_size >= sizeof(bios_message) - 1 || c == '\n' ||
        c == '\r') {
      if (bios_message_size > 0) {
        bios_message[bios_message_size] = '\0';
        printf("%s: %s\n", thread_tag() + 1, bios_message);
      }
      bios_message_size = 0;
      if (c == '\n' || c == '\r')
        return;
    }
    bios_message[bios_message_size++] = c;
    return;
  }
  case LEGACY_IO_3D4:
    io_write(address + 0x3d4, dsize, data);
    return;
  case LEGACY_IO_3DA:
    io_write(address + 0x3da, dsize, data);
    return;
  default:
    card_legacy_write(index, address, dsize, data);
    return;
  }
}

/**
 * Read from I/O ports, one byte at a time.
 **/
u32 CVGACard::io_read(u32 address, int dsize) {
  switch (dsize) {
  case 8:
    return io_read_b(address);
  case 16:
    return (u32)io_read_b(address) | ((u32)io_read_b(address + 1) << 8);
  case 32:
    return (u32)io_read_b(address) | ((u32)io_read_b(address + 1) << 8) |
           ((u32)io_read_b(address + 2) << 16) |
           ((u32)io_read_b(address + 3) << 24);
  default:
    FAILURE(InvalidArgument, "Unsupported dsize");
  }
}

/**
 * Write to I/O ports, one byte at a time.
 **/
void CVGACard::io_write(u32 address, int dsize, u32 data) {
  switch (dsize) {
  case 8:
    io_write_b(address, (u8)data);
    break;

  case 16:
    io_write_b(address, (u8)data);
    io_write_b(address + 1, (u8)(data >> 8));
    break;

  case 32:
    printf("%s Weird Size io write: %" PRIx32 ", %d, %" PRIx32 "   \n",
           card_name(), address, dsize, data);
    io_write_b(address, (u8)data);
    io_write_b(address + 1, (u8)(data >> 8));
    io_write_b(address + 2, (u8)(data >> 16));
    io_write_b(address + 3, (u8)(data >> 24));
    break;

  default:
    FAILURE(InvalidArgument, "Weird IO size");
  }
}

/**
 * Read one byte from a standard VGA I/O port.
 **/
u8 CVGACard::io_read_b(u32 address) {
  switch (address) {
  case 0x3c0:
    return atc_address_r(0);

  case 0x3c1:
    return atc_data_r(0);

  case 0x3c2:
    return read_b_3c2();

  case 0x3c4:
    return sequencer_address_r(0);

  case 0x3c5:
    return sequencer_data_r(0);

  case 0x3c6:
    return ramdac_mask_r(0);

  case 0x3c7:
    return ramdac_state_r(0);

  case 0x3c8:
    return ramdac_write_index_r(0);

  case 0x3c9:
    return ramdac_data_r(0);

  case 0x3ca:
    return read_b_3ca();

  case 0x3cc:
    return miscellaneous_output_r(0);

  case 0x3ce:
    return gc_address_r(0);

  case 0x3cf:
    return gc_data_r(0);

  case 0x3b4:
  case 0x3d4:
    return crtc_address_r(0);

  case 0x3b5:
  case 0x3d5:
    return crtc_data_r(0);

  case 0x3ba:
  case 0x3da: {
    // Input Status Register 1 -- wall-clock timing, no CRT engine. Bit 3
    // (vertical retrace) for ~1 ms of each frame at the programmed refresh
    // (70 Hz if none), bit 0 (display enable NOT) during that vertical
    // blank and, as on the real part, during every horizontal blank: ~6 us
    // of each ~32 us line. A driver that syncs to display enable -- the
    // palette load, the classic wait-for-bit-0-clear-then-set -- used to
    // wait a whole frame per call.
    using clock = std::chrono::steady_clock;
    static auto t0 = clock::now();
    const u64 ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                       clock::now() - t0)
                       .count();
    const double hz =
        (timing.vrefresh_hz > 30.0 && timing.vrefresh_hz < 200.0)
            ? timing.vrefresh_hz
            : 70.0;
    const u64 frame_ns = (u64)(1e9 / hz);
    const u64 vblank_ns = 1000000;
    const u64 line_ns = 31778, hblank_ns = 6000;

    u8 data = 0;
    if ((ns % frame_ns) < vblank_ns)
      data |= 0x08 | 0x01;
    else if ((ns % line_ns) < hblank_ns)
      data |= 0x01;

    vga.attribute.state = 0; // ATC flip-flop reset
    return data;
  }

  case 0x3bb: /* Feature Control (mono) readback; mirror 3CA behavior */
  case 0x3db: /* Feature Control (color) readback; same treatment */
    return read_b_3ca();

  case 0x3b6:
  case 0x3b7:
  case 0x3b8:
  case 0x3b9:
  case 0x3d6:
  case 0x3d7:
  case 0x3d8:
  case 0x3d9:
    return 0xFF; // open bus

  default:
    printf("%s: Unhandled io port %x read\n", card_name(), address);
    return 0;
  }
}

/**
 * Write one byte to a standard VGA I/O port.
 **/
void CVGACard::io_write_b(u32 address, u8 data) {
  switch (address) {
  case 0x3c0: {
    bool was_index_phase = (vga.attribute.state == 0);
    // Snapshot previous video-enabled state BEFORE the MAME canonical write
    bool prev_ve = atc_video_enabled();
    atc_address_data_w(0, data);
    if (was_index_phase) {
      // Detect video enable/disable transitions from MAME canonical source
      bool new_ve = atc_video_enabled();
      if (!new_ve && prev_ve) {
        bx_gui->lock();
        bx_gui->clear_screen();
        bx_gui->unlock();
      } else if (new_ve && !prev_ve) {
        redraw_area(0, 0, old_iWidth, old_iHeight);
      }
    }
    break;
  }

  case 0x3c2:
    write_b_3c2(data);
    m_ioas = bool(BIT(data, 0));
    break;

  case 0x3c4:
    sequencer_address_w(0, data);
    break;

  case 0x3c5:
    sequencer_data_w(0, data);
    break;

  case 0x3c6:
    ramdac_mask_w(0, data);
    break;

  case 0x3c7:
    ramdac_read_index_w(0, data);
    break;

  case 0x3c8:
    ramdac_write_index_w(0, data);
    break;

  case 0x3c9:
    ramdac_data_w(0, data);
    break;

  case 0x3ce:
    gc_address_w(0, data);
    break;

  case 0x3cf:
    gc_data_w(0, data);
    break;

  case 0x3ba:
  case 0x3da:
    feature_control_w(0, data);
    break;

  case 0x3b4:
  case 0x3d4:
    vga.crtc.index = data & 0x7f;
    break;

  case 0x3b5:
  case 0x3d5:
    crtc_data_w(0, data);
    break;

  case 0x3bb:
    break;

  case 0x3b6:
  case 0x3b7:
  case 0x3b8:
  case 0x3b9:
  case 0x3d6:
  case 0x3d7:
  case 0x3d8:
  case 0x3d9:
    // Dead ports — 32-bit writes to the CRTC pair (3D4/3D5) spill here.
    // Real hardware silently ignores them.
    break;

  default:
    FAILURE_1(NotImplemented, "Unhandled port %x write", address);
  }
}

/**
 * Write to the VGA Miscellaneous Output Register (0x3c2)
 *
 * \code
 * +-+-+-+-+---+-+-+
 * |7|6|5| |3 2|1|0|
 * +-+-+-+-+---+-+-+
 *  ^ ^ ^    ^  ^ ^
 *  | | |    |  | +- 0: I/OAS -- Input/Output Address Select: Selects the CRT
 *  | | |    |  |       controller addresses.
 *  | | |    |  |         0: Compatibility with monochrome adapter
 *  | | |    |  |            (0x3b4,0x3b5,0x03ba)
 *  | | |    |  |         1: Compatibility with color graphics adapter (CGA)
 *  | | |    |  |            (0x3d4,0x3d5,0x03da)
 *  | | |    |  +--- 1: RAM Enable: Controls access from the system:
 *  | | |    |            0: Disables access to the display buffer
 *  | | |    |            1: Enables access to the display buffer
 *  | | |    +--- 2..3: Clock Select: Controls the selection of the dot clocks
 *  | | |               used in driving the display timing:
 *  | | |                 00: Select 25 Mhz clock (320/640 pixel wide modes)
 *  | | |                 01: Select 28 Mhz clock (360/720 pixel wide modes)
 *  | | |                 10: Undefined (possible external clock)
 *  | | |                 11: Undefined (possible external clock)
 *  | | +----------- 5: Odd/Even Page Select: Selects the upper/lower 64K page
 *  | |                 of memory when the system is in an even/odd mode.
 *  | |                   0: Selects the low page.
 *  | |                   1: Selects the high page.
 *  | +------------- 6: Horizontal Sync Polarity
 *  |                     0: Positive sync pulse.
 *  |                     1: Negative sync pulse.
 *  +--------------- 7: Vertical Sync Polarity
 *                        0: Positive sync pulse.
 *                        1: Negative sync pulse.
 * \endcode
 **/
void CVGACard::write_b_3c2(u8 value) { vga.miscellaneous_output = value; }

/**
 * Read from the VGA Input Status register (0x3c2)
 *
 * \code
 * +-----+-+-------+
 * |     |4|       |
 * +-----+-+-------+
 *        ^
 *        +--------- 4: Switch Sense:
 *                      Returns the status of the four sense switches as
 *                      selected by the Clock Select field of the
 *                      Miscellaneous Output Register (see write_b_3c2)
 * \endcode
 **/
u8 CVGACard::read_b_3c2() {
  u8 res = 0x60; // is VGA (bits 5-6 set)

  // Sense bit readback: select which of 4 sense switches based on clock select
  // MAME: const u8 sense_bit = (3 - (vga.miscellaneous_output >> 2)) & 3;
  //        if(BIT(m_input_sense->read(), sense_bit)) res |= 0x10;
  const u8 sense_bit = (3 - ((vga.miscellaneous_output >> 2) & 3)) & 3;
  if (BIT(0x0F, sense_bit)) // all sense pins active
    res |= 0x10;

  res |= vga.crtc.irq_latch << 7;
  return res;
}

/**
 * Read from the 0xa0000 window, one byte at a time.
 **/
u32 CVGACard::legacy_read(u32 address, int dsize) {
  u32 data = 0;
  switch (dsize) {
  case 32:
    data |= (u32)mem_r(address + 3) << 24;
    data |= (u32)mem_r(address + 2) << 16;
    [[fallthrough]];
  case 16:
    data |= (u32)mem_r(address + 1) << 8;
    [[fallthrough]];
  case 8:
    data |= (u32)mem_r(address + 0);
    break;
  default:
    FAILURE(InvalidArgument, "Unsupported dsize");
  }

  return data;
}

/**
 * Write to the 0xa0000 window, one byte at a time.
 **/
void CVGACard::legacy_write(u32 address, int dsize, u32 data) {
  switch (dsize) {
  case 8:
    mem_w(address, (u8)data);
    break;

  case 16:
    mem_w(address, (u8)data);
    mem_w(address + 1, (u8)(data >> 8));
    break;

  case 32:
    mem_w(address, (u8)data);
    mem_w(address + 1, (u8)(data >> 8));
    mem_w(address + 2, (u8)(data >> 16));
    mem_w(address + 3, (u8)(data >> 24));
    break;

  default:
    FAILURE(InvalidArgument, "Unsupported dsize");
  }
}

/**
 * Thread entry point.
 *
 * The thread first initializes the GUI, and then starts looping the
 * following actions until interrupted (by StopThread being set to true)
 *   - Handle any GUI events (mouse moves, keypresses)
 *   - Update the GUI to match the screen buffer
 *   - Flush the updated GUI content to the screen
 *   .
 **/
void CVGACard::run() {
  try {
    // Initialize the GUI once (and let it know our tilesize). The serial
    // BREAK menu stops and restarts the device threads around every
    // interaction, so this runs again on "continue" -- and a second
    // SDL_Init/window creation is not what the GUI expects.
    if (!gui_initialized) {
      bx_gui->init(state.x_tilesize, state.y_tilesize);
      gui_initialized = true;
    }
    if (m_restored) { // a resumed machine: the GUI never saw this text font
      m_restored = false;
      bx_gui->lock();
      bx_gui->set_text_charmap(&vga.memory[vga.sequencer.char_sel.base[0]]);
      bx_gui->unlock();
    }
    bool was_paused = false;
    PauseAck.store(false, std::memory_order_release);
    for (;;) {
      // Terminate thread if StopThread is set to true
      if (StopThread)
        return;
      // Handle GUI events (50 times per second)
      bx_gui->lock();
      bx_gui->handle_events();
      bx_gui->unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // During firmware reset: keep pumping events (window stays alive),
      // but do NOT touch emulated VGA state.
      if (PauseThread.load(std::memory_order_acquire)) {
        if (!was_paused) {
          bx_gui->lock();
          bx_gui->clear_screen();
          bx_gui->unlock();
          was_paused = true;
        }
        PauseAck.store(true, std::memory_order_release);
        continue;
      }
      PauseAck.store(false, std::memory_order_release);
      was_paused = false;

      // Update the screen (50 times per second)
      bx_gui->lock();
      update();
      bx_gui->flush();
      bx_gui->unlock();
    }
  }

  catch (CException &e) {
    printf("Exception in %s thread: %s.\n", card_name(),
           e.displayText().c_str());
    myThreadDead.store(true);

    // Let the thread die...
  }
}

/**
 * Create and start thread.
 **/
void CVGACard::start_threads() {
  // Resume after reset if the thread already exists
  PauseThread.store(false, std::memory_order_release);

  if (!myThread) {
    printf("%s", thread_tag());
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

/**
 * Stop and destroy thread.
 **/
void CVGACard::stop_threads() {
  // During firmware reset, do NOT kill the render thread (it owns the SDL
  // window). Just pause it so the window stays alive.
  if (cSystem && cSystem->IsResetInProgress()) {
    PauseThread.store(true, std::memory_order_release);

    // Wait briefly until the thread acknowledges the pause
    if (myThread) {
      for (int spin = 0; spin < 600; spin++) // up to ~600ms
      {
        if (PauseAck.load(std::memory_order_acquire))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // Make it visible in the log whether we paused or stopped
      printf("%s(pause)", thread_tag());
    }
    return;
  }

  // Normal shutdown: actually stop the thread.
  StopThread = true;
  if (myThread) {
    printf("%s", thread_tag());
    myThread->join();
    myThread = nullptr;
  }
}

/**
 * Check if threads are still running.
 **/
void CVGACard::check_state() {
  if (myThreadDead.load())
    FAILURE_1(Thread, "%s thread has died", card_name());
}

/**
 * Save state to a Virtual Machine State file.
 **/
static constexpr u32 kCoreMagic = 0x56474131; // 'VGA1': the core block

int CVGACard::SaveState(FILE *f) {
  long ss = sizeof(state);
  u32 magic1 = state_magic1();
  u32 magic2 = state_magic2();
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  // state.memory and state.memsize are vestigial: nothing assigns or reads
  // them, as the real VRAM is vga.memory (allocated in init, and not part of
  // the savefile). Write them as zero so state files are deterministic
  // instead of carrying stray heap bytes.
  SVGACard_state saved = state;
  saved.memory = nullptr;
  saved.memsize = 0;

  fwrite(&magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&saved, sizeof(saved), 1, f);
  fwrite(&magic2, sizeof(u32), 1, f);

  // The VGA core and its VRAM, which the block above never carried -- a
  // resumed machine drew nothing and took no visible input. The registers
  // and latches verbatim with the VRAM pointer zeroed, then the VRAM bytes,
  // then whatever the card adds (save_card_state).
  vga_t core = vga;
  core.memory = nullptr;
  const u32 core_magic = kCoreMagic;
  const u64 vram = vga.svga_intf.vram_size;
  long cs = sizeof(core);
  fwrite(&core_magic, sizeof(u32), 1, f);
  fwrite(&cs, sizeof(long), 1, f);
  fwrite(&core, sizeof(core), 1, f);
  // The SVGA mode decode (bank registers, the rgb8/15/16/24/32 selects) is
  // derived from the registers by the write handlers, not from vga_t: a
  // resume without it drew a black 1280x480 desktop. Saved verbatim.
  long ms = sizeof(svga);
  fwrite(&ms, sizeof(long), 1, f);
  fwrite(&svga, sizeof(svga), 1, f);
  fwrite(&vram, sizeof(u64), 1, f);
  fwrite(vga.memory, 1, (size_t)vram, f);
  if ((res = save_card_state(f)))
    return res;
  fwrite(&core_magic, sizeof(u32), 1, f);
  printf("%s: %d bytes saved (+ core %d, VRAM %llu).\n", devid_string, (int)ss,
         (int)cs, (unsigned long long)vram);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CVGACard::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  int res;
  size_t r;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != state_magic1()) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  // Never let a pointer out of the file reach this process, even though
  // nothing reads these two today (see SaveState).
  state.memory = nullptr;
  state.memsize = 0;

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != state_magic2()) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  // The VGA core, the VRAM and the card's own block (see SaveState).
  u32 cm;
  long cs;
  if (fread(&cm, sizeof(u32), 1, f) != 1 || cm != kCoreMagic) {
    printf("%s: no VGA core block (a state file from before it existed)!\n",
           devid_string);
    return -1;
  }
  if (fread(&cs, sizeof(long), 1, f) != 1 || cs != (long)sizeof(vga_t)) {
    printf("%s: VGA core STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }
  vga_t core;
  if (fread(&core, sizeof(core), 1, f) != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }
  long ms;
  if (fread(&ms, sizeof(long), 1, f) != 1 || ms != (long)sizeof(svga) ||
      fread(&svga, sizeof(svga), 1, f) != 1) {
    printf("%s: SVGA mode block does not match!\n", devid_string);
    return -1;
  }
  u64 vram;
  if (fread(&vram, sizeof(u64), 1, f) != 1 ||
      vram != vga.svga_intf.vram_size) {
    printf("%s: VRAM size does not match (%llu in the file, %zu here)!\n",
           devid_string, (unsigned long long)vram, vga.svga_intf.vram_size);
    return -1;
  }
  u8 *mem = vga.memory; // this process's allocation, never the file's pointer
  vga = core;
  vga.memory = mem;
  if (fread(vga.memory, 1, (size_t)vram, f) != (size_t)vram) {
    printf("%s: unexpected end of file in VRAM!\n", devid_string);
    return -1;
  }
  if ((res = restore_card_state(f)))
    return res;
  if (fread(&cm, sizeof(u32), 1, f) != 1 || cm != kCoreMagic) {
    printf("%s: VGA core end MAGIC does not match!\n", devid_string);
    return -1;
  }
  // Everything derived from the registers is stale: the card recomputes its
  // decodes, and the render thread redraws from scratch (dimensions, the
  // 32bpp handshake, the text font, the cursor signature).
  post_restore();
  state.vga_mem_updated = 1;
  old_iWidth = old_iHeight = 0;
  state.last_bpp = 0;
  m_last_cursor_sig = ~(uint64_t)0;
  m_frames_since_render = 0;
  m_restored = true;

  printf("%s: %d bytes restored (+ core %d, VRAM %llu).\n", devid_string,
         (int)ss, (int)cs, (unsigned long long)vram);
  return 0;
}

/**
 * Load the option ROM named by the "rom" config value.
 **/
void CVGACard::load_option_rom(const char *default_name) {
  const char *name = myCfg->get_text_value("rom", default_name);
  FILE *rom = fopen(name, "rb");
  if (!rom) {
    FAILURE_2(FileNotFound, "%s rom file %s not found", card_name(), name);
  }

  rom_max = (unsigned)fread(option_rom, 1, sizeof(option_rom), rom);
  fclose(rom);

  // The console looks for a video BIOS at C0000, where firmware leaves a
  // copy of the card's option ROM. Answering there is the machine's part,
  // not the card's: a card behind a PCI-PCI bridge is reached through the
  // bridge's windows, and no window covers C0000.
  add_hose_mem(LEGACY_MEM_ROM, 0xc0000, rom_max);
}

/**
 * Read from the option ROM.
 **/
u32 CVGACard::rom_read(u32 address, int dsize) {
  u32 data = 0x00;
  u8 *x = (u8 *)option_rom;
  if (address <= rom_max) {
    x += address;
    switch (dsize) {
    case 8:
      data = (u32)endian_8((*((u8 *)x)) & 0xff);
      break;
    case 16:
      data = (u32)endian_16((*((u16 *)x)) & 0xffff);
      break;
    case 32:
      data = (u32)endian_32((*((u32 *)x)) & 0xffffffff);
      break;
    }
  }

  return data;
}

u8 CVGACard::get_actl_palette_idx(u8 index) { return atc_palette(index); }

void CVGACard::redraw_area(unsigned x0, unsigned y0, unsigned width,
                           unsigned height) {
  if ((width == 0) || (height == 0))
    return;

  state.vga_mem_updated = 1;
}

void CVGACard::update() {
  unsigned iWidth = 0, iHeight = 0;

  /* no screen update necessary
     Gate on the card's own enable, ATC video enable and SR1 "Screen Off" */
  if (!display_enabled() || !atc_video_enabled())
    return;

  const bool screen_off = (vga.sequencer.data[1] & 0x20) != 0; // SR1 bit5
  if (screen_off)
    return;

  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_last_refresh_time)
                        .count();

  if (elapsed_ms < (long long)timing.refresh_interval_ms)
    return;
  m_last_refresh_time = now;

  const uint8_t cur_mode = pc_vga_choosevideomode();

  if (cur_mode == SCREEN_OFF) {
    state.vga_mem_updated = 0;
    return;
  }

  // Dirty-gate: re-rasterize + re-upload only when something visible changed.
  // vga_mem_updated covers VRAM + CRTC text-cursor + palette + mode writes; a
  // hardware cursor is not flagged, so the card folds it into a signature.
  // Force a refresh every few frames so the cursor / blinking text still
  // animate on an otherwise static screen; tick_frame() keeps the blink
  // counter advancing on the skip path so blink timing stays correct.
  const int kBlinkRefreshFrames =
      8; // >= 2x the ~1.9 Hz VGA blink toggle at a 60 Hz refresh
  const uint64_t cursor_sig = hw_cursor_signature();
  if (direct_framebuffer_active())
    state.vga_mem_updated = 1; // the CPUs write VRAM behind our back
  if (!state.vga_mem_updated && cursor_sig == m_last_cursor_sig &&
      ++m_frames_since_render < kBlinkRefreshFrames) {
    screen().tick_frame(); // keep cursor/text-blink timing alive while skipping
                           // the render
    return;
  }
  m_frames_since_render = 0;
  m_last_cursor_sig = cursor_sig;

  vga.crtc.start_addr =
      vga.crtc.start_addr_latch; // FIXME: Figure out proper handling, but makes
                                 // BSD happy again....
  vga.attribute.pel_shift = vga.attribute.pel_shift_latch;

  determine_screen_dimensions(&iHeight, &iWidth);

  if (iWidth == 0 || iHeight == 0)
    return;

  // Update screen shim's visible area
  screen().set_visible_area(iWidth, iHeight);

  // Ensure bitmap is large enough
  m_render_bitmap.allocate(iWidth, iHeight);

  // Render via MAME's screen_update pipeline
  rectangle clip = m_render_bitmap.cliprect();
  screen_update(m_render_bitmap, clip);

  // Tick the frame counter (for cursor blink)
  screen().tick_frame();

  // MAME always produces ARGB32 — tell SDL we're in 32bpp mode.
  if (state.last_bpp != 32 || iWidth != old_iWidth || iHeight != old_iHeight) {
    bx_gui->dimension_update(iWidth, iHeight, 0, 0, 32);
    old_iWidth = iWidth;
    old_iHeight = iHeight;
    state.last_bpp = 32;
  }

  bx_gui->graphics_frame_update(m_render_bitmap.raw(), iWidth, iHeight);

  state.vga_mem_updated = 0;
}

void CVGACard::determine_screen_dimensions(unsigned *piHeight,
                                           unsigned *piWidth) {
  int ai[0x20];
  int i;
  int h;
  int v;
  for (i = 0; i < 0x20; i++)
    ai[i] = m_crtc_map.read_byte(i);

  h = (ai[1] + 1) * (seq_dotperchar() ? 8 : 9) / timing.divisor;
  v = (ai[18] | ((ai[7] & 0x02) << 7) | ((ai[7] & 0x40) << 3)) + 1;
  apply_extended_timing(h, v);
  v *= (get_interlace_mode() + 1); // interlaced mode

  if (vga.gc.shift256) {
    // was shift_reg == 2 mode 13h / 256-color byte mode
    // chain_four vs modeX
    *piWidth = h;
    *piHeight = v;
  } else if (vga.gc.shift_reg) {
    // was shift_reg == 1 CGA 4-color interleave
    if (x_dotclockdiv2())
      h <<= 1;
    *piWidth = h;
    *piHeight = v;
  } else {
    // was shift_reg == 0 standard VGA planar / EGA
    *piWidth = 640;
    *piHeight = 480;
    if (m_crtc_map.read_byte(0x06) == 0xBF) {
      if (m_crtc_map.read_byte(0x17) == 0xA3 &&
          m_crtc_map.read_byte(0x14) == 0x40 &&
          m_crtc_map.read_byte(0x09) == 0x41) {
        *piWidth = 320;
        *piHeight = 240;
      } else {
        if (x_dotclockdiv2())
          h <<= 1;
        *piWidth = h;
        *piHeight = v;
      }
    } else if ((h >= 640) && (v >= 480)) {
      *piWidth = h;
      *piHeight = v;
    }
  }
}

void CVGACard::palette_update() {
  CVGA::palette_update();

  for (int i = 0; i < 256; i++) {
    // pal6bit: expand 6-bit color to 8-bit
    u8 r = (vga.dac.color[3 * (i & vga.dac.mask) + 0] & 0x3f);
    u8 g = (vga.dac.color[3 * (i & vga.dac.mask) + 1] & 0x3f);
    u8 b = (vga.dac.color[3 * (i & vga.dac.mask) + 2] & 0x3f);
    // Expand 6-bit to 8-bit: (val << 2) | (val >> 4)
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    bx_gui->palette_change((unsigned)i, (unsigned)r, (unsigned)g, (unsigned)b);
  }
}
