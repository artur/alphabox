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
 * S3 ViRGE MMIO register window.
 *
 * 64 KB, reached at BAR0 + 16 MB or (old MMIO) at 0xa0000:
 *   0x0000-0x7fff  host data for the engine (image transfer), write only
 *   0x8180-0x81ff  the streams processor
 *   0x8200-0x821f  the memory port controller (FIFO and timeout tuning)
 *   0x83b0-0x83df  the VGA ports 0x3b0-0x3df
 *   0x8504         subsystem status (read) / control (write)
 *   0x850c         advanced function control
 *   0x8580-0x859c  bus-master DMA (command DMA at 0x8590-0x859c)
 *   0xa000-0xa1ff  the engine's colour pattern
 *   0xa4d4-0xad7c  the 2D register sets (BitBLT/fill, line, polygon)
 *   0xb0d4-0xb57c  the 3D register sets (line, triangle)
 *   0xff00-0xff5c  the local peripheral bus; 0xff20 the serial port (DDC)
 *
 * The engine runs each command to completion on the write that starts it
 * (or, for a BitBLT from host data, on the write that completes it), so
 * the status register always reads an idle engine with an empty FIFO.
 **/

#include "MonitorEdid.hpp"
#include "S3Virge.hpp"
#include "System.hpp"

#include <chrono>

using namespace virge;

static constexpr long long FRAME_US = 16667;

static long long clock_us() {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return duration_cast<microseconds>(steady_clock::now() - t0).count();
}

/**
 * The register sets' shared registers -- bases, clipping, strides,
 * patterns and colours, and the command -- are one register behind three
 * names in 2D (0xa4xx, 0xa8xx, 0xacxx) and two in 3D (0xb0xx, 0xb4xx):
 * this names each by its BitBLT (or triangle) copy.
 **/
static u32 canonical_engine_register(u32 offset) {
  const u32 in_set = offset & 0x3ff;
  if (in_set < 0xd4 || in_set > 0x100)
    return offset;
  if (offset >= 0xa400 && offset < 0xb000)
    return 0xa400 + in_set;
  if (offset >= 0xb000 && offset < 0xb800)
    return 0xb400 + in_set;
  return offset;
}

u32 CS3Virge::mmio_read(u32 offset, int dsize) {
  u32 v;
  if (offset < IMAGE_WINDOW_BYTES) {
    v = 0xffffffffu; // host data is write only
  } else if (offset >= MMIO_VGA_FIRST && offset <= MMIO_VGA_LAST) {
    v = 0;
    for (int i = 0; i < dsize / 8; i++)
      v |= u32(io_read_b(0x3b0 + (offset - MMIO_VGA_FIRST) + i)) << (8 * i);
    return v;
  } else {
    v = mmio_read_dword(offset & ~3u) >> (8 * (offset & 3));
  }
  if (dsize < 32)
    v &= (1u << dsize) - 1;
  if (m_trace)
    printf("%s: mmio read  %04x/%d = %0*x\n", devid_string, offset, dsize / 8,
           dsize / 4, v);
  return v;
}

void CS3Virge::mmio_write(u32 offset, int dsize, u32 data) {
  if (m_trace && offset >= IMAGE_WINDOW_BYTES)
    printf("%s: mmio write %04x/%d = %0*x\n", devid_string, offset, dsize / 8,
           dsize / 4, data);
  if (offset < IMAGE_WINDOW_BYTES) {
    s2d_host_data(data, dsize / 8);
    return;
  }
  if (offset >= MMIO_VGA_FIRST && offset <= MMIO_VGA_LAST) {
    for (int i = 0; i < dsize / 8; i++)
      io_write_b(0x3b0 + (offset - MMIO_VGA_FIRST) + i, u8(data >> (8 * i)));
    return;
  }
  if ((offset & ~3u) == SERIAL_PORT) {
    if ((offset & 3) == 0)
      serial_port_write(u8(data));
    return;
  }
  if (dsize < 32) {
    // A narrow write lands in the register's dword; the register then acts
    // as if the whole dword had been written.
    const u32 shift = 8 * (offset & 3);
    const u32 mask = ((1u << dsize) - 1) << shift;
    // Subsystem control keeps its enables; its status byte clears only
    // the bits written.
    const u32 cur = (offset & ~3u) == SUBSYS_STATUS
                        ? r.subsys_enable << 8
                        : M(canonical_engine_register(offset & ~3u));
    data = (cur & ~mask) | ((data << shift) & mask);
  }
  mmio_write_dword(offset & ~3u, data);
}

u32 CS3Virge::mmio_read_dword(u32 offset) {
  switch (offset) {
  case SUBSYS_STATUS:
    return STATUS_IDLE | r.subsys_status;
  case ADV_FUNC_CTL:
    // Bits 9..6: the command FIFO's free slots (8 on the DX).
    return (M(offset) & 0x3f) | (8u << 6);
  case DMA_READ_PTR:
    return M(DMA_WRITE_PTR) & 0xffff; // everything fetched
  case DMA_WRITE_PTR:
    return M(offset) & 0xffff;
  case SERIAL_PORT:
    return serial_port_read();
  }
  if (offset < 0x8000)
    return 0xffffffffu;
  return M(canonical_engine_register(offset));
}

void CS3Virge::mmio_write_dword(u32 offset, u32 data) {
  if (offset < 0x8000)
    return;
  switch (offset) {
  case SUBSYS_STATUS: {
    // Low byte: interrupt status bits to clear; bits 15..8: the enables.
    std::lock_guard<std::mutex> lock(m_int_lock);
    r.subsys_status &= ~(data & 0xff);
    r.subsys_enable = (data >> 8) & 0xff;
    update_int_line();
    return;
  }
  case ADV_FUNC_CTL:
    M(offset) = data;
    refresh_direct_lfb();
    return;
  case DMA_WRITE_PTR:
    M(offset) = data;
    if ((data & 0x10000) && (M(DMA_ENABLE) & 1))
      command_dma_run();
    return;
  case DMA_ENABLE:
    M(offset) = data;
    M(DMA_WRITE_PTR) &= ~0xffffu;
    M(DMA_READ_PTR) = 0;
    return;
  }

  if (offset >= 0x8180 && offset < 0x8200) {
    // The streams processor: the display reads these each frame.
    M(offset) = data;
    state.vga_mem_updated = 1;
    return;
  }
  if (offset >= PATTERN_RAM && offset < 0xc000) {
    const u32 canon = canonical_engine_register(offset);
    M(canon) = data;
    engine_register_written(offset, data);
    return;
  }
  M(offset) = data;
}

/**
 * Command DMA: the driver fills a ring in system memory with register
 * writes -- a header dword (bits 15..0 the dwords that follow, bits 31..16
 * the first register's MMIO offset / 4; bit 31 set: host data) and the
 * values -- and moves the write pointer; the chip fetches up to it.
 **/
void CS3Virge::command_dma_run() {
  const u32 base_reg = M(DMA_BASE);
  const u32 size = (base_reg & 2) ? 0x10000 : 0x1000;
  const u32 base = base_reg & ~(size - 1);
  const u32 write_ptr = M(DMA_WRITE_PTR) & (size - 1) & ~3u;
  u32 read_ptr = M(DMA_READ_PTR) & (size - 1) & ~3u;
  u32 left = 0, reg = 0;
  bool host = false;
  while (read_ptr != write_ptr) {
    u32 w = 0;
    do_pci_read(base + read_ptr, &w, 4, 1);
    read_ptr = (read_ptr + 4) & (size - 1);
    if (!left) {
      left = w & 0xffff;
      host = (w & 0x80000000u) != 0;
      reg = ((w >> 16) << 2) & 0xfffc;
      continue;
    }
    if (host)
      s2d_host_data(w, 4);
    else {
      mmio_write(reg, 32, w);
      reg = (reg + 4) & 0xfffc;
    }
    left--;
  }
  M(DMA_READ_PTR) = read_ptr;
  std::lock_guard<std::mutex> lock(m_int_lock);
  r.subsys_status |= INT_CMD_DMA_DONE;
  update_int_line();
}

// --- the serial port: DDC to the monitor --------------------------------

u8 CS3Virge::serial_port_read() const {
  u8 v = r.serial_port & ~(SP_SCR | SP_SDR);
  if (r.serial_port & SP_ENABLE) {
    if (m_ddc.scl())
      v |= SP_SCR;
    if (m_ddc.sda())
      v |= SP_SDR;
  }
  return v;
}

void CS3Virge::serial_port_write(u8 data) {
  r.serial_port = data & ~(SP_SCR | SP_SDR);
  if (data & SP_ENABLE)
    m_ddc.drive_from_host((data & SP_SCW) != 0, (data & SP_SDW) != 0);
  else
    m_ddc.drive_from_host(true, true);
}

void CS3Virge::ddc_attach_monitor() {
  m_ddc.attach(std::make_shared<Eeprom24C02>(
      0x50,
      std::vector<u8>(kMonitorEdid, kMonitorEdid + sizeof(kMonitorEdid))));
  m_ddc.drive_from_host(true, true);
}

// --- interrupts ------------------------------------------------------------

/**
 * Every ~10 ms from the render thread: flag the vertical retrace of each
 * new frame, whether or not anyone reads the status.
 **/
void CS3Virge::card_tick() {
  if (!m_trace_file.empty() && ++m_trace_poll >= 50) {
    m_trace_poll = 0;
    FILE *f = fopen(m_trace_file.c_str(), "r");
    if (f)
      fclose(f);
    if (bool(f) != m_trace.load()) {
      m_trace = bool(f);
      printf("%s: trace %s\n", devid_string, f ? "on" : "off");
    }
  }
  const u32 frame = u32(clock_us() / FRAME_US);
  std::lock_guard<std::mutex> lock(m_int_lock);
  if (frame != r.vblank_seen) {
    r.vblank_seen = frame;
    r.subsys_status |= INT_VSYNC;
  }
  update_int_line();
}

/// INTA: what the enables let through, while CR32 lets anything out.
/// Call with m_int_lock.
void CS3Virge::update_int_line() {
  const bool want = (cr(CR_BKWD_1) & CR32_INT_ENABLE) &&
                    (r.subsys_status & r.subsys_enable & 0xff) != 0;
  if (want == m_int_asserted)
    return;
  m_int_asserted = want;
  do_pci_interrupt(0, want);
}
