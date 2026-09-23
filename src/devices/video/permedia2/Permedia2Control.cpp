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
 * Permedia 2 region 0: the control status, memory control and video
 * timing registers, the RAMDAC, the SVGA's ports, the monitor's DDC lines.
 *
 * Every register is a dword at an 8-byte stride. The RAMDAC's are eight
 * bits wide in the low byte of theirs; the SVGA's ports appear at
 * 0x6000 + port, one byte a port, the whole 1 KB of I/O space repeated
 * over the 4 KB block. The graphics processor's registers (0x8000 up) and
 * its FIFO (0x2000) are kept but do nothing yet.
 **/

#include "MonitorEdid.hpp"
#include "Permedia2.hpp"

#include <chrono>

using namespace permedia2;

/// A 60 Hz frame on the card's own clock: the timing generator's line
/// counter and its vertical retrace interrupt run on it.
static constexpr long long FRAME_US = 16667;

static long long clock_us() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() -
                                                               t0)
      .count();
}

u32 CPermedia2::control_read(u32 offset, int bytes) {
  // The SVGA's ports are bytes of their own: a wider access reads several.
  if ((offset & 0xf000) == R0_SVGA) {
    u32 v = 0;
    for (int i = 0; i < bytes; i++)
      v |= u32(io_read_b((offset + i) & 0x3ff)) << (8 * i);
    return v;
  }
  const u32 v = control_read_dword(offset & ~3u);
  if (bytes == 4)
    return v;
  return (v >> (8 * (offset & 3))) & ((1u << (8 * bytes)) - 1);
}

void CPermedia2::control_write(u32 offset, int bytes, u32 data) {
  if ((offset & 0xf000) == R0_SVGA) {
    for (int i = 0; i < bytes; i++)
      io_write_b((offset + i) & 0x3ff, u8(data >> (8 * i)));
    return;
  }
  if (bytes != 4) {
    // Merge a narrow write into the register's current value. The RAMDAC
    // takes its byte as it is: a read there has side effects.
    const u32 base = offset & ~3u;
    const int shift = 8 * (offset & 3);
    const u32 mask = ((1u << (8 * bytes)) - 1) << shift;
    if ((base & 0xf000) == R0_RAMDAC) {
      if (shift == 0)
        control_write_dword(base, data);
      return;
    }
    const u32 old = base < R0_GP ? r.ctl[base >> 2] : r.gp[(base - R0_GP) >> 2];
    control_write_dword(base, (old & ~mask) | ((data << shift) & mask));
    return;
  }
  control_write_dword(offset, data);
}

u32 CPermedia2::control_read_dword(u32 offset) {
  if (offset >= R0_GP)
    return r.gp[(offset - R0_GP) >> 2];
  switch (offset & 0xf000) {
  case R0_RAMDAC:
    return ramdac_read(int((offset >> 3) & 0xf));
  case R0_GP_FIFO:
    return 0;
  }
  switch (offset) {
  case RESET_STATUS:
  case REBOOT:
    return 0;
  case INT_FLAGS: {
    std::lock_guard<std::mutex> lock(m_int_lock);
    return r.ctl[INT_FLAGS >> 2];
  }
  case IN_FIFO_SPACE:
    return 0x20; // the graphics processor takes everything at once
  case OUT_FIFO_WORDS:
    return 0;
  case COUNT: // MClk, 50 MHz at power-on
    return u32(clock_us() * 50);
  case LINE_COUNT:
    return line_count();
  case VIDEO_CONTROL:
    return r.ctl[offset >> 2] &
           ~(VC_BYPASS_PENDING | VC_GP_PENDING | (3u << 13) | (1u << 15));
  case DISPLAY_DATA:
    return display_data_read();
  }
  return r.ctl[offset >> 2];
}

void CPermedia2::control_write_dword(u32 offset, u32 data) {
  if (offset >= R0_GP) {
    r.gp[(offset - R0_GP) >> 2] = data;
    return;
  }
  switch (offset & 0xf000) {
  case R0_RAMDAC:
    ramdac_write(int((offset >> 3) & 0xf), u8(data));
    return;
  case R0_GP_FIFO:
    return;
  }
  u32 &reg = r.ctl[offset >> 2];
  switch (offset) {
  case RESET_STATUS: // a software reset of the graphics processor: it
  case REBOOT:       // has no state yet; nor has the SGRAM's mode register
  case IN_FIFO_SPACE:
  case OUT_FIFO_WORDS:
  case COUNT:
  case FB_WRITE_MASK:
    return;
  case INT_ENABLE: {
    std::lock_guard<std::mutex> lock(m_int_lock);
    reg = data & 0x1fff;
    update_int_line();
    return;
  }
  case INT_FLAGS: { // write one to clear
    std::lock_guard<std::mutex> lock(m_int_lock);
    reg &= ~data;
    update_int_line();
    return;
  }
  case ERROR_FLAGS:
    reg &= ~data;
    return;
  case APERTURE_ONE:
  case APERTURE_TWO:
    reg = data & AP_WRITABLE;
    refresh_direct_aperture();
    return;
  case CHIP_CONFIG:
    reg = data & 0x1fff;
    return;
  case MEM_CONFIG:
  case VIDEO_CONTROL:
  case SCREEN_BASE:
  case SCREEN_STRIDE:
  case H_TOTAL:
  case HB_END:
  case V_TOTAL:
  case VB_END:
    reg = data;
    state.vga_mem_updated = 1;
    return;
  case DISPLAY_DATA:
    // Bits 5-7 are cleared by writing ones; the drivers are the two
    // open-drain outputs, released when their bit is one.
    reg = (reg & (0xe0 & ~data)) | (data & 0xff1c);
    m_ddc.drive_from_host((data & DD_CLK_OUT) != 0, (data & DD_DATA_OUT) != 0);
    return;
  case FIFO_CONTROL:
    reg = data & 0x1f1f; // the video FIFO never underflows here
    return;
  }
  reg = data;
}

/**
 * The timing generator's current line, from the card's clock. While the
 * graphics processor has no timing programmed, the SVGA's 525 lines.
 **/
u32 CPermedia2::line_count() const {
  u32 lines = (reg(V_TOTAL) & 0x7ff) + 1;
  if (lines < 2)
    lines = 525;
  return u32((clock_us() % FRAME_US) * lines / FRAME_US);
}

/**
 * DisplayData read: the lines as they are on the bus (the monitor may
 * hold SDA low), the drivers and the latched state as written.
 **/
u32 CPermedia2::display_data_read() const {
  u32 v = reg(DISPLAY_DATA) & ~(DD_DATA_IN | DD_CLK_IN);
  if (m_ddc.sda())
    v |= DD_DATA_IN;
  if (m_ddc.scl())
    v |= DD_CLK_IN;
  return v;
}

void CPermedia2::ddc_attach_monitor() {
  m_ddc.attach(std::make_shared<Eeprom24C02>(
      0x50,
      std::vector<u8>(kMonitorEdid, kMonitorEdid + sizeof(kMonitorEdid))));
  m_ddc.drive_from_host(true, true);
}

/**
 * Every ~10 ms from the render thread: flag the vertical retrace of each
 * frame that has begun since the last tick.
 **/
void CPermedia2::card_tick() {
  const u32 frame = u32(clock_us() / FRAME_US);
  std::lock_guard<std::mutex> lock(m_int_lock);
  if (frame != r.vblank_seen) {
    r.vblank_seen = frame;
    r.ctl[INT_FLAGS >> 2] |= INT_VRETRACE;
  }
  update_int_line();
}

/// INTA follows the flags the enable lets through. Call with m_int_lock.
void CPermedia2::update_int_line() {
  const bool want = (r.ctl[INT_FLAGS >> 2] & r.ctl[INT_ENABLE >> 2]) != 0;
  if (want == m_int_asserted)
    return;
  m_int_asserted = want;
  do_pci_interrupt(0, want);
}

// --- the RAMDAC ------------------------------------------------------------

/**
 * A direct register. 0-3 are the VGA DAC's own four -- the palette's
 * write address (also the indirect registers' index and the cursor RAM's
 * address), data, pixel mask and read address -- and go through the VGA
 * core, whose palette this is.
 **/
u8 CPermedia2::ramdac_read(int reg_no) {
  switch (reg_no) {
  case RD_PALETTE_WRITE_ADDRESS:
    return r.rd_index;
  case RD_PALETTE_DATA:
    return CVGACard::io_read_b(0x3c9);
  case RD_PIXEL_MASK:
    return CVGACard::io_read_b(0x3c6);
  case RD_PALETTE_READ_ADDRESS:
    return vga.dac.read_index;
  case RD_CURSOR_COLOR_ADDRESS:
    return r.cursor_color_addr;
  case RD_CURSOR_COLOR_DATA: {
    const u8 v = r.cursor_color[r.cursor_color_addr & 3][r.cursor_color_state];
    if (++r.cursor_color_state == 3) {
      r.cursor_color_state = 0;
      r.cursor_color_addr = (r.cursor_color_addr + 1) & 3;
    }
    return v;
  }
  case RD_INDEXED_DATA:
    return ramdac_indexed_read(r.rd_index);
  case RD_CURSOR_RAM_DATA: {
    u8 &cr = r.rd_indexed[RDI_CURSOR_CONTROL];
    const u32 a = (u32((cr >> CUR_RAM_ADDR_SHIFT) & 3) << 8) | r.rd_index;
    const u8 v = r.cursor_ram[a];
    const u32 next = (a + 1) & 0x3ff;
    r.rd_index = u8(next);
    cr = u8((cr & ~(3u << CUR_RAM_ADDR_SHIFT)) |
            ((next >> 8) << CUR_RAM_ADDR_SHIFT));
    return v;
  }
  case RD_CURSOR_X_LOW:
    return r.cursor_x_lo;
  case RD_CURSOR_X_HIGH:
    return r.cursor_x_hi;
  case RD_CURSOR_Y_LOW:
    return r.cursor_y_lo;
  case RD_CURSOR_Y_HIGH:
    return r.cursor_y_hi;
  }
  return 0;
}

void CPermedia2::ramdac_write(int reg_no, u8 data) {
  switch (reg_no) {
  case RD_PALETTE_WRITE_ADDRESS:
    r.rd_index = data;
    CVGACard::io_write_b(0x3c8, data);
    return;
  case RD_PALETTE_DATA:
    CVGACard::io_write_b(0x3c9, data);
    return;
  case RD_PIXEL_MASK:
    CVGACard::io_write_b(0x3c6, data);
    return;
  case RD_PALETTE_READ_ADDRESS:
    CVGACard::io_write_b(0x3c7, data);
    return;
  case RD_CURSOR_COLOR_ADDRESS:
    r.cursor_color_addr = data & 3;
    r.cursor_color_state = 0;
    return;
  case RD_CURSOR_COLOR_DATA:
    r.cursor_color[r.cursor_color_addr & 3][r.cursor_color_state] = data;
    if (++r.cursor_color_state == 3) {
      r.cursor_color_state = 0;
      r.cursor_color_addr = (r.cursor_color_addr + 1) & 3;
    }
    state.vga_mem_updated = 1;
    return;
  case RD_INDEXED_DATA:
    ramdac_indexed_write(r.rd_index, data);
    return;
  case RD_CURSOR_RAM_DATA: {
    u8 &cr = r.rd_indexed[RDI_CURSOR_CONTROL];
    const u32 a = (u32((cr >> CUR_RAM_ADDR_SHIFT) & 3) << 8) | r.rd_index;
    r.cursor_ram[a] = data;
    const u32 next = (a + 1) & 0x3ff;
    r.rd_index = u8(next);
    cr = u8((cr & ~(3u << CUR_RAM_ADDR_SHIFT)) |
            ((next >> 8) << CUR_RAM_ADDR_SHIFT));
    state.vga_mem_updated = 1;
    return;
  }
  case RD_CURSOR_X_LOW:
    r.cursor_x_lo = data;
    return;
  case RD_CURSOR_X_HIGH:
    r.cursor_x_hi = data & 0x0f;
    return;
  case RD_CURSOR_Y_LOW:
    r.cursor_y_lo = data;
    return;
  case RD_CURSOR_Y_HIGH:
    r.cursor_y_hi = data & 0x0f;
    return;
  }
}

/**
 * The indirect registers. The clock synthesizers lock at once; the pixel
 * clock's M/N/P are write-only, as are the memory clock's.
 **/
u8 CPermedia2::ramdac_indexed_read(u8 index) const {
  switch (index) {
  case RDI_PIXEL_CLOCK_STATUS:
  case RDI_MEMORY_CLOCK_STATUS:
    return PLL_LOCKED;
  }
  return r.rd_indexed[index];
}

void CPermedia2::ramdac_indexed_write(u8 index, u8 data) {
  switch (index) {
  case RDI_PIXEL_CLOCK_STATUS:
  case RDI_MEMORY_CLOCK_STATUS:
    return;
  case RDI_COLOR_MODE:
  case RDI_MISC_CONTROL:
    vga.dac.dirty = 1;
    break;
  }
  r.rd_indexed[index] = data;
  state.vga_mem_updated = 1;
}
