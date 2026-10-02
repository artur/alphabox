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
 * The Radeon's register aperture.
 *
 * One 64 KB file of dword registers, reached through BAR2, through the
 * I/O BAR (its first 256 bytes) and indirectly through MM_INDEX/MM_DATA
 * (bit 31 of MM_INDEX turns the pair into a window on the framebuffer).
 * A register without a meaning here reads back what was written. Those
 * with one:
 *
 *   - CLOCK_CNTL_INDEX/DATA: the PLL registers. A divider change through
 *     PPLL_REF_DIV's atomic-update bit is taken at once, so the bit reads
 *     back clear;
 *   - the CRTC's live status: the current line, the vertical blank (live
 *     and latched), the frame count, the vertical-blank interrupt;
 *   - the DAC: PALETTE_INDEX/DATA are the VGA DAC's own index and colours
 *     at eight bits a channel (PALETTE_30_DATA at ten);
 *   - the DDC lines (GPIO_VGA_DDC), with a monitor answering on them;
 *   - the memory controller's idle bit and the engine's FIFO and busy bit;
 *   - the configuration mirrors: CONFIG_APER_*_BASE from the BARs, and
 *     PCI configuration space at 0xf00;
 *   - the VGA's I/O ports, mirrored at their own numbers (0x3b0-0x3df).
 **/

#include "MonitorEdid.hpp"
#include "Radeon.hpp"

using namespace radeon;

void CRadeon::trace_access(bool write, u32 reg, int bytes, u32 data) {
  if (m_trace_new) {
    u8 &seen = m_seen[write ? 1 : 0][(reg & (REG_APERTURE_BYTES - 1)) >> 2];
    if (seen)
      return;
    seen = 1;
  } else if (m_trace_budget <= 0) {
    return;
  } else {
    m_trace_budget--;
  }
  printf("%s: %-5s %s %04x/%d %s %08x\n", devid_string, m_trace_path,
         write ? "write" : "read ", reg, bytes, write ? "<-" : "->", data);
}

u32 CRadeon::reg_read(u32 offset, int bytes) {
  offset &= REG_APERTURE_BYTES - 1;
  if (offset >= VGA_MIRROR_FIRST && offset <= VGA_MIRROR_LAST) {
    u32 v = 0;
    for (int i = 0; i < bytes; i++)
      v |= u32(io_read_b(offset + i)) << (8 * i);
    return v;
  }
  const u32 shift = (offset & 3) * 8;
  if ((offset & 3) + bytes > 4) { // straddles two registers
    u32 v = 0;
    for (int i = 0; i < bytes; i++)
      v |= reg_read(offset + i, 1) << (8 * i);
    return v;
  }
  const u32 reg = offset & ~3u;
  u32 v;
  if (reg == MM_DATA) {
    const u32 idx = R(MM_INDEX);
    if (idx & 0x80000000u)
      v = vram_read((idx & 0x7fffffffu) + (offset & 3), bytes);
    else if ((idx & ~3u) > MM_DATA)
      v = reg_read((idx & ~3u) + (offset & 3), bytes);
    else
      v = 0;
    return v;
  }
  v = reg_read32(reg) >> shift;
  if (bytes < 4)
    v &= (1u << (8 * bytes)) - 1;
  if (m_trace || m_trace_new)
    trace_access(false, offset, bytes, v);
  return v;
}

void CRadeon::reg_write(u32 offset, int bytes, u32 data) {
  offset &= REG_APERTURE_BYTES - 1;
  if (offset >= VGA_MIRROR_FIRST && offset <= VGA_MIRROR_LAST) {
    for (int i = 0; i < bytes; i++)
      io_write_b(offset + i, u8(data >> (8 * i)));
    return;
  }
  if ((offset & 3) + bytes > 4) {
    for (int i = 0; i < bytes; i++)
      reg_write(offset + i, 1, data >> (8 * i));
    return;
  }
  const u32 reg = offset & ~3u;
  if (reg == MM_DATA) {
    const u32 idx = R(MM_INDEX);
    if (idx & 0x80000000u)
      vram_write((idx & 0x7fffffffu) + (offset & 3), bytes, data);
    else if ((idx & ~3u) > MM_DATA)
      reg_write((idx & ~3u) + (offset & 3), bytes, data);
    return;
  }
  if (m_trace || m_trace_new)
    trace_access(true, offset, bytes, data);
  const u32 shift = (offset & 3) * 8;
  const u32 mask = (bytes >= 4 ? 0xffffffffu : (1u << (8 * bytes)) - 1)
                   << shift;
  // A PLL register is merged with its own value, not with the data port's.
  const u32 old = reg == CLOCK_CNTL_DATA
                      ? m_pll[R(CLOCK_CNTL_INDEX) & PLL_INDEX_MASK]
                      : R(reg);
  const u32 merged = (old & ~mask) | ((data << shift) & mask);
  reg_write32(reg, merged, old, mask);
}

/**
 * The CRTC's vertical position, from the card's clock: the current mode's
 * total lines swept 60 times a second. With no mode set (the totals zero)
 * a 525-line VGA frame.
 **/
u32 CRadeon::current_vline(bool *in_vblank) const {
  u32 total, disp;
  if (native_crtc_active()) {
    total = (R(CRTC_V_TOTAL_DISP) & 0xfff) + 1;
    disp = ((R(CRTC_V_TOTAL_DISP) >> 16) & 0xfff) + 1;
  } else {
    total = 525;
    disp = 480;
  }
  if (total < disp + 1)
    total = disp + 1;
  const long long us = radeon_clock_us();
  const long long frame_us = 1000000 / 60;
  const u32 line = u32(((us % frame_us) * total) / frame_us);
  if (in_vblank)
    *in_vblank = line >= disp;
  return line;
}

u32 CRadeon::reg_read32(u32 reg) {
  switch (reg) {
  case CLOCK_CNTL_DATA:
    return pll_read(u8(R(CLOCK_CNTL_INDEX) & PLL_INDEX_MASK));

  case CRTC_STATUS: {
    bool vb;
    current_vline(&vb);
    if (vb)
      R(CRTC_STATUS) |= CRTC_VBLANK_SAVE;
    return (R(CRTC_STATUS) & CRTC_VBLANK_SAVE) | (vb ? CRTC_VBLANK_CUR : 0);
  }
  case CRTC2_STATUS:
    return 0;

  case CRTC_VLINE_CRNT_VLINE:
    return (current_vline(nullptr) << 16) | (R(reg) & 0xfff);

  case CRTC_CRNT_FRAME:
    return u32(radeon_clock_us() / (1000000 / 60)) & 0x1fffff;

  case GEN_INT_STATUS: {
    std::lock_guard<std::mutex> l(m_int_lock);
    return R(GEN_INT_STATUS);
  }

  case GPIO_VGA_DDC:
  case GPIO_DVI_DDC:
  case GPIO_MONID:
  case GPIO_CRT2_DDC:
    return gpio_read(reg);

  case PALETTE_INDEX:
    return (u32(vga.dac.read_index) << 16) | vga.dac.write_index;
  case PALETTE_DATA:
    return palette_read_data();
  case PALETTE_30_DATA: {
    const u32 v = palette_read_data();
    return (((v >> 16) & 0xff) << 22) | (((v >> 8) & 0xff) << 12) |
           ((v & 0xff) << 2);
  }

  case MC_STATUS:
    // <1:0> both channels' SDRAM powered up (the BIOS waits for both after
    // each mode-register write: the loop at 0x6c94), <2> the controller
    // idle (Linux's MC_IDLE).
    return R(MC_STATUS) | 7u;

  case RBBM_STATUS:
  case RBBM_STATUS_ALT:
    return 64 | (engine_busy() ? RBBM_ACTIVE : 0);

  case CONFIG_APER_0_BASE:
    return config_read(0, 0x10, 32) & 0xfffffff0u;
  case CONFIG_APER_1_BASE:
    return (config_read(0, 0x10, 32) & 0xfffffff0u) + FB_APERTURE_HALF;
  case CONFIG_REG_1_BASE:
    return config_read(0, 0x18, 32) & 0xfffffff0u;
  }
  if (reg >= PCI_CONFIG_MIRROR && reg < PCI_CONFIG_MIRROR + 0x100)
    return config_read(0, reg - PCI_CONFIG_MIRROR, 32);
  if (is_engine_reg(reg))
    return engine_read(reg);
  return R(reg);
}

void CRadeon::reg_write32(u32 reg, u32 data, u32 old, u32 byte_mask) {
  switch (reg) {
  case CLOCK_CNTL_DATA:
    if (R(CLOCK_CNTL_INDEX) & PLL_WR_EN)
      pll_write(u8(R(CLOCK_CNTL_INDEX) & PLL_INDEX_MASK), data);
    return;

  case CRTC_STATUS:
    // VBLANK_SAVE is cleared by writing it.
    if (data & CRTC_VBLANK_SAVE)
      R(CRTC_STATUS) &= ~CRTC_VBLANK_SAVE;
    return;
  case CRTC2_STATUS:
    return;

  case GEN_INT_STATUS: {
    // Writing a one acknowledges that interrupt; SW_INT_FIRE raises the
    // software interrupt (the DRM's fence).
    std::lock_guard<std::mutex> l(m_int_lock);
    const u32 w = data & byte_mask;
    R(GEN_INT_STATUS) &= ~(w & ~INT_SW_FIRE);
    if (w & INT_SW_FIRE)
      R(GEN_INT_STATUS) |= INT_SW;
    update_int_line();
    return;
  }
  case GEN_INT_CNTL: {
    std::lock_guard<std::mutex> l(m_int_lock);
    R(GEN_INT_CNTL) = data;
    update_int_line();
    return;
  }

  case GPIO_VGA_DDC:
    R(reg) = data & ~(GPIO_Y_0 | GPIO_Y_1);
    ddc_drive();
    return;

  case PALETTE_INDEX:
    if (byte_mask & 0x000000ffu) {
      vga.dac.write_index = u8(data);
      vga.dac.state = 0;
      vga.dac.read = 0;
    }
    if (byte_mask & 0x00ff0000u) {
      vga.dac.read_index = u8(data >> 16);
      vga.dac.state = 0;
      vga.dac.read = 1;
    }
    R(reg) = data;
    return;
  case PALETTE_DATA:
    palette_write_data(data & 0xffffff);
    return;
  case PALETTE_30_DATA:
    palette_write_data((((data >> 22) & 0xff) << 16) |
                       (((data >> 12) & 0xff) << 8) | ((data >> 2) & 0xff));
    return;

  case DAC_CNTL:
    R(reg) = data;
    vga.dac.dirty = 1;
    return;

  case RBBM_SOFT_RESET:
    R(reg) = data;
    if (data & 0xff) // CP, HI, SE, RE, PP, E2, RB: the engine
      engine_reset();
    return;

  case CRTC_GEN_CNTL:
  case CRTC_EXT_CNTL:
  case CRTC_H_TOTAL_DISP:
  case CRTC_V_TOTAL_DISP:
  case CRTC_OFFSET:
  case CRTC_OFFSET_CNTL:
  case CRTC_PITCH:
    R(reg) = data;
    state.vga_mem_updated = 1;
    return;

  case SURFACE_CNTL:
  case MC_FB_LOCATION:
    R(reg) = data;
    refresh_direct_aperture();
    return;

  case CONFIG_APER_0_BASE:
  case CONFIG_APER_1_BASE:
  case CONFIG_APER_SIZE:
  case CONFIG_REG_1_BASE:
  case CONFIG_REG_APER_SIZE:
  case MC_STATUS:
  case RBBM_STATUS:
  case RBBM_STATUS_ALT:
    return; // read-only
  }
  if (reg >= PCI_CONFIG_MIRROR && reg < PCI_CONFIG_MIRROR + 0x100)
    return; // a read-only copy of configuration space
  if (is_engine_reg(reg)) {
    R(reg) = data;
    engine_write(reg, data);
    return;
  }
  R(reg) = data;
}

/**
 * The PLLs. Nothing here generates a clock; the registers keep what the
 * BIOS writes so that it reads back, and the atomic divider update
 * completes at once.
 **/
u32 CRadeon::pll_read(u8 index) {
  u32 v = m_pll[index & PLL_INDEX_MASK];
  if (index == PLL_PPLL_REF_DIV || index == PLL_P2PLL_REF_DIV)
    v &= ~PPLL_ATOMIC_UPDATE;
  if (index == PLL_TEST_CNTL) {
    // <31:24>: a counter the BIOS times its delays with (0x716b in the
    // RV200 BIOS): it clears the byte, then polls until the byte reaches a
    // count, a bounded number of times, and repeats. Under the console's
    // x86 emulator each poll costs tens of microseconds, so a real-rate
    // counter made the BIOS's thousands of delays take minutes; the
    // counter here has always run out.
    v |= 0xff000000u;
  }
  if (m_trace && m_trace_budget > 0) {
    m_trace_budget--;
    printf("%s: pll   read  %02x -> %08x\n", devid_string, index, v);
  }
  return v;
}

void CRadeon::pll_write(u8 index, u32 data) {
  if (m_trace && m_trace_budget > 0) {
    m_trace_budget--;
    printf("%s: pll   write %02x <- %08x\n", devid_string, index, data);
  }
  m_pll[index & PLL_INDEX_MASK] = data;
}

/**
 * The palette through the register aperture: PALETTE_DATA is one entry
 * as 0x00RRGGBB, after which the index moves on. The VGA DAC holds the
 * colours (six bits a channel unless DAC_8BIT_EN), so an 8-bit write is
 * scaled to what the DAC ports would have stored.
 **/
void CRadeon::palette_write_data(u32 data) {
  const bool eight = (R(DAC_CNTL) & DAC_8BIT_EN) != 0;
  u8 *c = &vga.dac.color[3 * vga.dac.write_index];
  for (int i = 0; i < 3; i++) {
    const u8 v = u8(data >> (16 - 8 * i));
    c[i] = eight ? v : u8(v >> 2);
  }
  vga.dac.write_index++;
  vga.dac.state = 0;
  vga.dac.dirty = 1;
}

u32 CRadeon::palette_read_data() {
  const bool eight = (R(DAC_CNTL) & DAC_8BIT_EN) != 0;
  const u8 *c = &vga.dac.color[3 * vga.dac.read_index];
  u32 v = 0;
  for (int i = 0; i < 3; i++) {
    u32 ch = c[i];
    if (!eight)
      ch = (ch << 2) | (ch >> 4);
    v |= (ch & 0xff) << (16 - 8 * i);
  }
  vga.dac.read_index++;
  return v;
}

/**
 * The DDC lines. A GPIO register drives line 0 (data) and line 1 (clock)
 * low while its enable bit is set and its output bit clear, and reads the
 * lines back in <8> and <9>. The VGA connector's lines, GPIO_VGA_DDC, have
 * the monitor (MonitorEdid.hpp) on them; the DVI and second CRT
 * connectors are empty and read high.
 **/
void CRadeon::ddc_attach_monitor() {
  m_ddc.attach(std::make_shared<Eeprom24C02>(
      0x50,
      std::vector<u8>(kMonitorEdid, kMonitorEdid + sizeof(kMonitorEdid))));
  ddc_drive();
}

void CRadeon::ddc_drive() {
  const u32 g = R(GPIO_VGA_DDC);
  const bool sda_release = !(g & GPIO_EN_0) || (g & GPIO_A_0);
  const bool scl_release = !(g & GPIO_EN_1) || (g & GPIO_A_1);
  m_ddc.drive_from_host(scl_release, sda_release);
}

u32 CRadeon::gpio_read(u32 reg) const {
  u32 v = R(reg) & ~(GPIO_Y_0 | GPIO_Y_1);
  if (reg == GPIO_VGA_DDC) {
    if (m_ddc.sda())
      v |= GPIO_Y_0;
    if (m_ddc.scl())
      v |= GPIO_Y_1;
    return v;
  }
  // Nothing on the line: it reads what the chip drives, else high.
  if (!(v & GPIO_EN_0) || (v & GPIO_A_0))
    v |= GPIO_Y_0;
  if (!(v & GPIO_EN_1) || (v & GPIO_A_1))
    v |= GPIO_Y_1;
  return v;
}

/**
 * About every 10 ms from the render thread: latch a vertical blank once a
 * frame (GEN_INT_STATUS <0>) and drive INTA if it is enabled.
 **/
void CRadeon::card_tick() {
  const long long frame = radeon_clock_us() / (1000000 / 60);
  std::lock_guard<std::mutex> l(m_int_lock);
  if (frame != m_last_vblank_frame) {
    m_last_vblank_frame = frame;
    R(GEN_INT_STATUS) |= INT_CRTC_VBLANK;
    R(CRTC_STATUS) |= CRTC_VBLANK_SAVE;
  }
  update_int_line();
}

/// Call with m_int_lock held.
void CRadeon::update_int_line() {
  const bool want = (R(GEN_INT_STATUS) & R(GEN_INT_CNTL) &
                     (INT_CRTC_VBLANK | INT_GUI_IDLE | INT_SW)) != 0;
  if (want == m_int_asserted)
    return;
  m_int_asserted = want;
  do_pci_interrupt(0, want);
}
