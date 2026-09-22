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
 * Mach64 register block: block 0's CRTC, cursor, clock synthesizer,
 * memory banks, DAC, EEPROM, configuration and status registers, and the
 * block 1 overlay registers (stored only). The GUI engine's registers
 * (0x100 upward in block 0) are in Mach64Engine.cpp.
 *
 * Every path to the registers -- aperture page, VGA aperture, block I/O,
 * sparse I/O -- arrives here with a byte offset into the 2 KB block.
 * Registers are stored whole and accessed a byte lane at a time, as the
 * chip allows; a register's side effects run once per access, after its
 * bytes are in.
 *
 * Modelled on 86Box's mach64_ext_readb/writeb (see Mach64.hpp).
 **/

#include "Mach64.hpp"

#include <chrono>

using namespace mach64;

/// The frame clock the CRTC status reports against: a nominal 60 Hz on
/// the wall clock, with one time origin for every register that uses it --
/// mach64_clock_us(), which the engine's timing shares.
static constexpr long long FRAME_US = 16667;
static long long clock_us() { return mach64_clock_us(); }

/// Byte `lane` of `v`.
static inline u8 lane_get(u32 v, u32 lane) { return u8(v >> (8 * (lane & 3))); }
static inline void lane_set(u32 &v, u32 lane, u8 b) {
  const u32 sh = 8 * (lane & 3);
  v = (v & ~(0xffu << sh)) | (u32(b) << sh);
}

/**
 * The register at an offset, or nullptr for a byte no register holds
 * (reads 0, writes are dropped).
 **/
static u32 *block0_reg(CMach64::regs_t &r, u32 reg) {
  switch (reg & 0x3fc) {
  case CRTC_H_TOTAL_DISP:
    return &r.crtc_h_total_disp;
  case CRTC_H_SYNC_STRT_WID:
    return &r.crtc_h_sync_strt_wid;
  case CRTC_V_TOTAL_DISP:
    return &r.crtc_v_total_disp;
  case CRTC_V_SYNC_STRT_WID:
    return &r.crtc_v_sync_strt_wid;
  case CRTC_VLINE_CRNT_VLINE:
    return &r.crtc_vline;
  case CRTC_OFF_PITCH:
    return &r.crtc_off_pitch;
  case CRTC_GEN_CNTL:
    return &r.crtc_gen_cntl;
  case DSP_CONFIG:
    return &r.dsp_config;
  case DSP_ON_OFF:
    return &r.dsp_on_off;
  case OVR_CLR:
    return &r.ovr_clr;
  case OVR_WID_LEFT_RIGHT:
    return &r.ovr_wid_left_right;
  case OVR_WID_TOP_BOTTOM:
    return &r.ovr_wid_top_bottom;
  case VGA_DSP_CONFIG:
    return &r.vga_dsp_config;
  case VGA_DSP_ON_OFF:
    return &r.vga_dsp_on_off;
  case CUR_CLR0:
    return &r.cur_clr0;
  case CUR_CLR1:
    return &r.cur_clr1;
  case CUR_OFFSET:
    return &r.cur_offset;
  case CUR_HORZ_VERT_POSN:
    return &r.cur_horz_vert_posn;
  case CUR_HORZ_VERT_OFF:
    return &r.cur_horz_vert_off;
  case GP_IO:
    return &r.gp_io;
  case SCRATCH_REG0:
    return &r.scratch_reg0;
  case SCRATCH_REG1:
    return &r.scratch_reg1;
  case CLOCK_CNTL:
    return &r.clock_cntl;
  case BUS_CNTL:
    return &r.bus_cntl;
  case MEM_CNTL:
    return &r.mem_cntl;
  case MEM_VGA_WP_SEL:
    return &r.mem_vga_wp_sel;
  case MEM_VGA_RP_SEL:
    return &r.mem_vga_rp_sel;
  case DAC_CNTL:
    return &r.dac_cntl;
  case GEN_TEST_CNTL:
    return &r.gen_test_cntl;
  case CONFIG_CNTL:
    return &r.config_cntl;
  case CONFIG_CHIP_ID:
    return &r.config_chip_id;
  case CONFIG_STAT0:
    return &r.config_stat0;
  case CONFIG_STAT1:
    return &r.config_stat1;
  }
  return nullptr;
}

/**
 * The GUI engine's registers as the guest reads them back.
 **/
static u32 *engine_reg(CMach64::regs_t &r, u32 reg) {
  switch (reg & 0x3fc) {
  case DST_OFF_PITCH:
    return &r.dst_off_pitch;
  case DST_Y_X:
    return &r.dst_y_x;
  case DST_HEIGHT_WIDTH:
    return &r.dst_height_width;
  case DST_BRES_LNTH:
    return &r.dst_bres_lnth;
  case DST_BRES_ERR:
    return &r.dst_bres_err;
  case DST_BRES_INC:
    return &r.dst_bres_inc;
  case DST_BRES_DEC:
    return &r.dst_bres_dec;
  case DST_CNTL:
    return &r.dst_cntl;
  case SRC_OFF_PITCH:
    return &r.src_off_pitch;
  case SRC_Y_X:
    return &r.src_y_x;
  case SRC_HEIGHT1_WIDTH1:
    return &r.src_height1_width1;
  case SRC_Y_X_START:
    return &r.src_y_x_start;
  case SRC_HEIGHT2_WIDTH2:
    return &r.src_height2_width2;
  case SRC_CNTL:
    return &r.src_cntl;
  case HOST_CNTL:
    return &r.host_cntl;
  case PAT_REG0:
    return &r.pat_reg0;
  case PAT_REG1:
    return &r.pat_reg1;
  case PAT_CNTL:
    return &r.pat_cntl;
  case SC_LEFT_RIGHT:
    return &r.sc_left_right;
  case SC_TOP_BOTTOM:
    return &r.sc_top_bottom;
  case DP_BKGD_CLR:
    return &r.dp_bkgd_clr;
  case DP_FRGD_CLR:
    return &r.dp_frgd_clr;
  case DP_WRITE_MASK:
    return &r.write_mask;
  case DP_CHAIN_MASK:
    return &r.chain_mask;
  case DP_PIX_WIDTH:
    return &r.dp_pix_width;
  case DP_MIX:
    return &r.dp_mix;
  case DP_SRC:
    return &r.dp_src;
  case DP_SET_GUI_ENGINE:
    return &r.dp_set_gui_engine;
  case CLR_CMP_CLR:
    return &r.clr_cmp_clr;
  case CLR_CMP_MASK:
    return &r.clr_cmp_mask;
  case CLR_CMP_CNTL:
    return &r.clr_cmp_cntl;
  case CONTEXT_MASK:
    return &r.context_mask;
  case CONTEXT_LOAD_CNTL:
    return &r.context_load_cntl;
  }
  return nullptr;
}

/**
 * Multi-byte access, little-endian, one lane at a time; a block-0
 * register's side effects run once, and the engine's registers take the
 * access at its own width (a host-data write is one transfer, whatever
 * its size).
 **/
/**
 * Count an access for trace "new": registers are keys 0..0x3ff (write
 * flag 0x200, block-0 flag 0x100), VGA port reads 0x400 upward. Every
 * million accesses the three most frequent are printed -- a driver
 * polling one of them shows up as the top line.
 **/
void CMach64::trace_hit(u32 key) {
  m_hits[key]++;
  if (++m_hits_total < (1u << 20))
    return;
  u32 top[3] = {0, 0, 0};
  for (u32 k = 0; k < 0x440; k++)
    for (int t = 0; t < 3; t++)
      if (m_hits[k] > m_hits[top[t]]) {
        for (int u = 2; u > t; u--)
          top[u] = top[u - 1];
        top[t] = k;
        break;
      }
  printf("%s: hot:", devid_string);
  for (u32 k : top) {
    if (!m_hits[k])
      continue;
    if (k >= 0x400)
      printf(" port %03x x%u", 0x3c0 + (k - 0x400), m_hits[k]);
    else
      printf(" %s %c+%03x x%u", (k & 0x200) ? "write" : "read",
             (k & 0x100) ? '0' : '1', (k & 0xff) << 2, m_hits[k]);
  }
  printf("\n");
  memset(m_hits, 0, sizeof(m_hits));
  m_hits_total = 0;
}

void CMach64::trace_first(u32 offset, bool write, u32 data) {
  const u32 key = (write ? 0x200u : 0u) | ((offset & (REG_BLOCK_BYTES - 1)) >> 2);
  trace_hit(key);
  if (m_seen[key])
    return;
  m_seen[key] = 1;
  printf("%s: first %s %c+%03x = %08x (%s)\n", devid_string,
         write ? "write" : "read ", (offset & REG_BLOCK0) ? '0' : '1',
         offset & 0x3fc, data, m_trace_path);
}

u32 CMach64::reg_read(u32 offset, int bytes) {
  u32 data = 0;
  for (int i = 0; i < bytes; i++)
    data |= u32(reg_read8(offset + i)) << (8 * i);
  if (m_trace_new)
    trace_first(offset, false, data);
  if (m_trace)
    printf("%s: reg read  %c+%03x/%d = %0*x (%s)\n", devid_string,
           (offset & REG_BLOCK0) ? '0' : '1', offset & 0x3ff, bytes,
           bytes * 2, data, m_trace_path);
  return data;
}

void CMach64::reg_write(u32 offset, int bytes, u32 data) {
  if (m_trace_new)
    trace_first(offset, true, data);
  if (m_trace)
    printf("%s: reg write %c+%03x/%d = %0*x (%s)\n", devid_string,
           (offset & REG_BLOCK0) ? '0' : '1', offset & 0x3ff, bytes,
           bytes * 2, data, m_trace_path);
  if ((offset & REG_BLOCK0) && (offset & 0x3ff) >= GUI_FIRST) {
    const u32 reg = offset & 0x3ff;
    switch (bytes) {
    case 1:
      engine_write8(reg, u8(data));
      break;
    case 2:
      engine_write16(reg & ~1u, u16(data));
      break;
    default:
      engine_write32(reg & ~3u, data);
      break;
    }
    state.vga_mem_updated = 1;
    return;
  }
  for (int i = 0; i < bytes; i++)
    reg_write8(offset + i, u8(data >> (8 * i)));
  if (offset & REG_BLOCK0)
    reg_written(offset & 0x3fc);
}

u8 CMach64::reg_read8(u32 offset) {
  if (!(offset & REG_BLOCK0)) // block 1: overlay and scaler
    return lane_get(r.block1[(offset & 0x3ff) >> 2], offset);

  const u32 reg = offset & 0x3ff;
  const u32 lane = reg & 3;

  if (reg >= GUI_FIRST) {
    switch (reg & 0x3fc) {
    case DST_X: // the halves of DST_Y_X and DST_HEIGHT_WIDTH, separately
      return lane < 2 ? lane_get(r.dst_y_x, lane + 2) : 0;
    case DST_X_WIDTH:
      return lane < 2 ? lane_get(r.dst_y_x, lane + 2)
                      : lane_get(r.dst_height_width, lane);
    case DST_Y:
      return lane < 2 ? lane_get(r.dst_y_x, lane) : 0;
    case DST_WIDTH:
      return lane < 2 ? lane_get(r.dst_height_width, lane + 2) : 0;
    case DST_HEIGHT:
      return lane < 2 ? lane_get(r.dst_height_width, lane) : 0;
    case DST_X_Y:
      return lane_get(r.dst_y_x, lane ^ 2);
    case DST_WIDTH_HEIGHT:
      return lane_get(r.dst_height_width, lane ^ 2);
    case SRC_X:
      return lane < 2 ? lane_get(r.src_y_x, lane + 2) : 0;
    case SRC_Y:
      return lane < 2 ? lane_get(r.src_y_x, lane) : 0;
    case SRC_WIDTH1:
      return lane < 2 ? lane_get(r.src_height1_width1, lane + 2) : 0;
    case SRC_HEIGHT1:
      return lane < 2 ? lane_get(r.src_height1_width1, lane) : 0;
    case SRC_X_START:
      return lane < 2 ? lane_get(r.src_y_x_start, lane + 2) : 0;
    case SRC_Y_START:
      return lane < 2 ? lane_get(r.src_y_x_start, lane) : 0;
    case SRC_WIDTH2:
      return lane < 2 ? lane_get(r.src_height2_width2, lane + 2) : 0;
    case SRC_HEIGHT2:
      return lane < 2 ? lane_get(r.src_height2_width2, lane) : 0;
    case SC_LEFT:
      return lane < 2 ? lane_get(r.sc_left_right, lane) : 0;
    case SC_RIGHT:
      return lane < 2 ? lane_get(r.sc_left_right, lane + 2) : 0;
    case SC_TOP:
      return lane < 2 ? lane_get(r.sc_top_bottom, lane) : 0;
    case SC_BOTTOM:
      return lane < 2 ? lane_get(r.sc_top_bottom, lane + 2) : 0;
    case FIFO_STAT: {
      // One bit per slot, set while that slot still holds work. The engine
      // draws instantly, so what is reported is the queue the part would
      // still have at this instant (see engine_charge).
      if (!engine_busy())
        return 0;
      const long long left_us = m_engine_busy_until_us - mach64_clock_us();
      int slots = int(left_us / 4) + 1; // ~4 us of work to a slot
      if (slots > 16)
        slots = 16;
      const u32 bits = (slots >= 16) ? 0xffffu : ((1u << slots) - 1u);
      return lane < 2 ? lane_get(bits, lane) : 0;
    }
    case GUI_TRAJ_CNTL:
      switch (lane) {
      case 0:
      case 1:
        return lane_get(r.dst_cntl, lane);
      case 2:
        return lane_get(r.src_cntl, 0);
      default:
        return lane_get(r.pat_cntl, 0);
      }
    case GUI_STAT: {
      // Bit 0 says the engine is drawing; byte 2 how many of the 32 command
      // slots are free. Both come from the same charge.
      if (!engine_busy())
        return lane == 2 ? 32 : 0;
      if (lane == 0)
        return 1; // GUI_ACTIVE
      if (lane == 2) {
        const long long left_us = m_engine_busy_until_us - mach64_clock_us();
        long long used = left_us / 4 + 1;
        if (used > 32)
          used = 32;
        return u32(32 - used);
      }
      return 0;
    }
    default: {
      const u32 creg = is_gt() ? gt_canonical(reg & 0x3fc) : (reg & 0x3fc);
      if (u32 *p = engine_reg(r, creg))
        return lane_get(*p, lane);
      if (is_gt())
        return lane_get(r.gt[creg >> 2], lane);
      return 0;
    }
    }
  }

  switch (reg & 0x3fc) {
  case CRTC_VLINE_CRNT_VLINE: // the current scan line, from the clock
    if (lane >= 2) {
      const u32 vtotal = (r.crtc_v_total_disp & 0x7ff) + 1;
      const u32 line = u32((clock_us() % FRAME_US) * vtotal / FRAME_US) & 0x7ff;
      return lane_get(line << 16, lane);
    }
    return lane_get(r.crtc_vline, lane);
  case CRTC_INT_CNTL:
    return lane == 0 ? crtc_int_cntl_read() : 0;
  case CLOCK_CNTL:
    return pll_read(int(lane));
  case GP_IO: // lines with nothing on them read high
    return lane == 1 ? 0x30 : lane_get(r.gp_io, lane);
  case DAC_REGS: { // the VGA DAC ports, in the order 3c8 3c9 3c6 3c7
    static const u16 ports[4] = {0x3c8, 0x3c9, 0x3c6, 0x3c7};
    return CVGACard::io_read_b(ports[lane]);
  }
  case DAC_CNTL:
    if (lane == 3)
      return dac_gio_read(lane_get(r.dac_cntl, 3));
    return lane_get(r.dac_cntl, lane);
  case CONFIG_CNTL:
    return lane_get(config_cntl_read(), lane);
  default:
    if (u32 *p = block0_reg(r, reg))
      return lane_get(*p, lane);
    if (is_gt())
      return lane_get(r.gt[(reg & 0x3fc) >> 2], lane);
    return 0;
  }
}

void CMach64::reg_write8(u32 offset, u8 data) {
  if (!(offset & REG_BLOCK0)) {
    lane_set(r.block1[(offset & 0x3ff) >> 2], offset, data);
    return;
  }

  const u32 reg = offset & 0x3ff;
  const u32 lane = reg & 3;

  switch (reg & 0x3fc) {
  case CRTC_INT_CNTL:
    if (lane == 0) {
      // Status bits stay; writing 1 to VBLANK_INT acknowledges it.
      std::lock_guard<std::mutex> lock(m_int_lock);
      r.crtc_int_cntl = (r.crtc_int_cntl & CRTC_INT_STATUS_BITS) |
                        (data & ~CRTC_INT_STATUS_BITS);
      if (data & CRTC_VBLANK_INT)
        r.crtc_int_cntl &= ~CRTC_VBLANK_INT;
      update_int_line(); // the acknowledgement, or a change of the enable
    }
    return;
  case MEM_VGA_WP_SEL: // a byte write sets a whole 32 KB bank number
  case MEM_VGA_RP_SEL: {
    u32 &sel =
        (reg & 0x3fc) == MEM_VGA_WP_SEL ? r.mem_vga_wp_sel : r.mem_vga_rp_sel;
    if (lane & 1)
      return;
    lane_set(sel, lane, data);
    lane_set(sel, lane + 1, 0);
    return;
  }
  case DAC_REGS: {
    static const u16 ports[4] = {0x3c8, 0x3c9, 0x3c6, 0x3c7};
    CVGACard::io_write_b(ports[lane], data);
    return;
  }
  case CLOCK_CNTL:
    lane_set(r.clock_cntl, lane, data);
    pll_write(int(lane), data);
    return;
  case CONFIG_CHIP_ID: // read only
    return;
  default:
    if (u32 *p = block0_reg(r, reg))
      lane_set(*p, lane, data);
    else if (is_gt())
      lane_set(r.gt[(reg & 0x3fc) >> 2], lane, data);
    return;
  }
}

/**
 * What a block-0 register does once written.
 **/
void CMach64::reg_written(u32 reg) {
  switch (reg) {
  case CRTC_H_TOTAL_DISP:
  case CRTC_H_SYNC_STRT_WID:
  case CRTC_V_TOTAL_DISP:
  case CRTC_V_SYNC_STRT_WID:
  case CRTC_OFF_PITCH:
  case CRTC_GEN_CNTL:
  case CUR_CLR0:
  case CUR_CLR1:
  case CUR_OFFSET:
  case CUR_HORZ_VERT_POSN:
  case CUR_HORZ_VERT_OFF:
    state.vga_mem_updated = 1;
    break;
  case MEM_VGA_WP_SEL:
  case MEM_VGA_RP_SEL:
    update_banks();
    break;
  case DAC_CNTL:
    ddc_drive();
    vga.dac.dirty = 1; // 8-bit DAC on or off changes every colour
    state.vga_mem_updated = 1;
    break;
  case GEN_TEST_CNTL:
    eeprom_clock();
    // Clearing GUI_ENGINE_ENABLE is how a driver resets the engine: the
    // command in flight is abandoned and the FIFO emptied. Without this a
    // host-data command cut short by the reset would take the next
    // command's HOST_DATA writes as its own.
    if (!(r.gen_test_cntl & GEN_GUI_EN)) {
      accel.busy = false;
      m_engine_busy_until_us = 0;
    }
    state.vga_mem_updated = 1; // the cursor enable lives here
    break;
  case CONFIG_CNTL:
    state.vga_mem_updated = 1;
    break;
  }
}

/**
 * CRTC_INT_CNTL: bit 0 is the vertical blank as it happens, bit 2 latches
 * that one has happened since it was acknowledged. Both come from the
 * wall clock at a nominal 60 Hz, as the VGA status port does.
 **/
u32 CMach64::vblank_frame() const { return u32(clock_us() / FRAME_US); }

/// A vertical blank happens whether or not anyone is looking: the card's
/// own thread latches it and drives the line, so a driver that waits for
/// the interrupt rather than polling the status gets one.
void CMach64::card_tick() {
  const u32 frame = vblank_frame();
  std::lock_guard<std::mutex> lock(m_int_lock);
  if (frame != r.vblank_seen) {
    r.vblank_seen = frame;
    r.crtc_int_cntl |= CRTC_VBLANK_INT;
  }
  update_int_line();
}

void CMach64::update_int_line() {
  const bool want = (r.crtc_int_cntl & CRTC_VBLANK_INT) &&
                    (r.crtc_int_cntl & CRTC_VBLANK_INT_EN);
  if (want == m_int_asserted)
    return;
  m_int_asserted = want;
  do_pci_interrupt(0, want);
}

u8 CMach64::crtc_int_cntl_read() {
  const u32 frame = vblank_frame();
  std::unique_lock<std::mutex> lock(m_int_lock);
  if (frame != r.vblank_seen) {
    r.vblank_seen = frame;
    r.crtc_int_cntl |= CRTC_VBLANK_INT;
    update_int_line();
  }
  u8 v = r.crtc_int_cntl & ~CRTC_VBLANK;
  lock.unlock();
  if ((clock_us() % FRAME_US) >= FRAME_US - 1000) // the last ~1 ms of a frame
    v |= CRTC_VBLANK;
  return v;
}

/**
 * CONFIG_CNTL reads back the aperture's address, 4 MB units in bits
 * 13..4, from wherever the PCI BAR was put.
 **/
u32 CMach64::config_cntl_read() {
  const u32 base = config_read(0, 0x10, 32) & 0xfff00000u;
  return (r.config_cntl & ~CFG_MEM_AP_LOC_MASK) |
         (((base >> 22) << CFG_MEM_AP_LOC_SHIFT) & CFG_MEM_AP_LOC_MASK);
}

/**
 * GEN_TEST_CNTL carries the EEPROM's pins: chip select, clock and data in
 * are written, data out is read back in bit 3.
 **/
void CMach64::eeprom_clock() {
  m_eeprom.set_pins((r.gen_test_cntl & GEN_EEPROM_CS) != 0,
                    (r.gen_test_cntl & GEN_EEPROM_CLK) != 0,
                    (r.gen_test_cntl & GEN_EEPROM_DATA_OUT) != 0);
  if (m_eeprom.data_out())
    r.gen_test_cntl |= GEN_EEPROM_DATA_IN;
  else
    r.gen_test_cntl &= ~GEN_EEPROM_DATA_IN;
}

/**
 * CLOCK_CNTL: byte 0 selects the clock, byte 1 addresses a synthesizer
 * register (bits 5..2), byte 2 is that register's data. The frequencies
 * programmed are not used -- the refresh runs on the wall clock -- but the
 * registers read back, which the BIOS relies on.
 **/
void CMach64::pll_write(int lane, u8 data) {
  switch (lane) {
  case 1:
    r.pll_addr = (data >> 2) & 0x0f;
    break;
  case 2:
    r.pll_regs[r.pll_addr] = data;
    break;
  }
}

u8 CMach64::pll_read(int lane) const {
  return lane == 2 ? r.pll_regs[r.pll_addr] : lane_get(r.clock_cntl, lane);
}

/**
 * The two 32 KB banks of the VGA aperture, in 32 KB units, one per
 * 16-bit half of the select registers.
 **/
void CMach64::update_banks() {
  r.bank_w[0] = (r.mem_vga_wp_sel & 0xffff) << 15;
  r.bank_w[1] = (r.mem_vga_wp_sel >> 16) << 15;
  r.bank_r[0] = (r.mem_vga_rp_sel & 0xffff) << 15;
  r.bank_r[1] = (r.mem_vga_rp_sel >> 16) << 15;
}
