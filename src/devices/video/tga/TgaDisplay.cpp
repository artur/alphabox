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
 * DECchip 21030 (TGA): the screen. The video timing registers (4.5) give
 * the visible size and where in the frame buffer it starts; the pixels go
 * through the RAMDAC's palette; the Bt485's cursor (or the 21030's own,
 * which the 24-plane boards use) is laid over them. The render thread
 * also pumps the GUI's events and raises the end-of-frame interrupt.
 **/

#include "StdAfx.hpp"
#include "System.hpp"
#include "Tga.hpp"
#include "VGA.hpp"
#include "gui/gui.hpp"
#include "gui/vga.hpp"

using namespace tga;

/// The refresh the screen is drawn and the end-of-frame interrupt raised
/// at, whatever the dot clock: the ICS1562 is not decoded.
static constexpr int kFrameMs = 16;

void CTga::start_threads() {
  PauseThread.store(false, std::memory_order_release);
  if (!myThread) {
    printf(" tga");
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

/**
 * During a firmware reset the render thread, which owns the SDL window,
 * pauses rather than stops (as the VGA cards' does, CVGACard).
 **/
void CTga::stop_threads() {
  if (cSystem && cSystem->IsResetInProgress()) {
    PauseThread.store(true, std::memory_order_release);
    if (myThread) {
      for (int spin = 0; spin < 600; spin++) {
        if (PauseAck.load(std::memory_order_acquire))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      printf(" tga(pause)");
    }
    return;
  }
  StopThread = true;
  if (myThread) {
    printf(" tga");
    myThread->join();
    myThread = nullptr;
  }
}

void CTga::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "TGA thread has died");
}

void CTga::run() {
  try {
    m_owns_gui = theTGA == this && !theVGA;
    m_dump_prefix = getenv("ALPHABOX_TGA_DUMP");
    if (!m_owns_gui)
      printf("%s: the display belongs to another card; this one is drawn "
             "%s\n",
             devid_string,
             m_dump_prefix ? "into ALPHABOX_TGA_DUMP files" : "nowhere");
    if (m_owns_gui && !gui_initialized) {
      bx_gui->init(X_TILESIZE, Y_TILESIZE);
      gui_initialized = true;
    }
    bool was_paused = false;
    PauseAck.store(false, std::memory_order_release);
    for (;;) {
      if (StopThread)
        return;
      if (m_owns_gui) {
        bx_gui->lock();
        bx_gui->handle_events();
        bx_gui->unlock();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      if (PauseThread.load(std::memory_order_acquire)) {
        if (!was_paused && m_owns_gui) {
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

      const auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                m_last_frame)
              .count() < kFrameMs)
        continue;
      m_last_frame = now;
      if (!m_trace_file.empty()) {
        FILE *t = fopen(m_trace_file.c_str(), "r");
        m_trace = t != nullptr;
        if (t)
          fclose(t);
      }
      frame_tick();
      if (!m_owns_gui) {
        update_screen();
        continue;
      }
      bx_gui->lock();
      update_screen();
      bx_gui->flush();
      bx_gui->unlock();
    }
  } catch (CException &e) {
    printf("Exception in TGA thread: %s.\n", e.displayText().c_str());
    myThreadDead.store(true);
  }
}

/**
 * INTA follows the pending interrupts the interrupt status register
 * enables (4.7.2). Called with m_irq_lock held.
 **/
void CTga::update_irq() {
  const u32 pending = r[SISR] & SISR_PENDING;
  const u32 enabled = (r[SISR] & SISR_ENABLES) >> 16;
  const bool assert_it = (pending & enabled) != 0;
  if (assert_it != m_irq_asserted) {
    m_irq_asserted = assert_it;
    do_pci_interrupt(0, assert_it);
  }
}

/**
 * The start of vertical sync: end-of-frame becomes pending whether or not
 * it is enabled (Linux's tgafb polls for it with interrupts off). The
 * timer interrupt is posted 256 frame buffer clocks after it was last
 * cleared, which is within any frame.
 **/
void CTga::frame_tick() {
  std::lock_guard<std::mutex> lock(m_irq_lock);
  if (!(r[VVVR] & VVVR_VIDEO_VALID))
    return;
  r[SISR] |= SISR_EOFIP | SISR_TIP;
  update_irq();
}

/**
 * The visible screen: VHCR's active field counts 4-pixel units (its MSBs
 * in <29:28>), the last unit hidden when the odd bit is set and the count
 * is odd; VVCR the lines. A scan line in memory is the whole active
 * count, so a 1284-pixel "turbo" line shows 1280 (7.2.3.3). The screen
 * starts at VVBR's row of 2K or 4K pixels, by the VRAM's column size
 * (GDER<9>, Table 4-58).
 **/
bool CTga::screen_geometry(unsigned &w, unsigned &h, unsigned &stride_px,
                           u32 &start) const {
  if ((r[VVVR] & (VVVR_VIDEO_VALID | VVVR_BLANK)) != VVVR_VIDEO_VALID)
    return false;
  if (m_model.tga2() && (r[VVVR] & 0x30))
    return false; // the monitor powered down
  const u32 active = (((r[VHCR] >> 28) & 3) << 9) | (r[VHCR] & 0x1ff);
  if (active < 2)
    return false;
  stride_px = active * 4;
  w = stride_px;
  if ((r[VHCR] & VHCR_ODD) && (active & 1))
    w -= 4;
  h = r[VVCR] & VVCR_ACTIVE;
  if (!h || w > 4096 || h > 4096)
    return false;
  const u32 row_px = (r[GDER] & GDER_CS) ? 2048 : 4096;
  start = r[VVBR] * row_px * (deep() ? 4 : 1);
  return true;
}

void CTga::update_screen() {
  unsigned w, h, stride;
  u32 start;
  if (!screen_geometry(w, h, stride, start))
    return;
  const u64 gen = m_generation.load(std::memory_order_acquire);
  // Redraw when anything changed; a static screen still gets one every
  // half second, which keeps the frame dump going.
  if (gen == m_drawn_generation && w == m_last_w && h == m_last_h &&
      ++m_frames_since_render < 30)
    return;
  m_frames_since_render = 0;
  m_drawn_generation = gen;
  render(w, h, stride, start);
}

void CTga::render(unsigned w, unsigned h, unsigned stride, u32 start) {
  m_frame.resize(size_t(w) * h);
  if (!deep()) {
    u32 pal[256];
    for (unsigned i = 0; i < 256; i++)
      pal[i] = ramdac_color(u8(i));
    for (unsigned y = 0; y < h; y++) {
      const u32 row = start + y * stride;
      u32 *out = &m_frame[size_t(y) * w];
      for (unsigned x = 0; x < w; x++)
        out[x] = pal[vram8(row + x)];
    }
  } else if (m_model.ramdac == TgaRamdac::Rgb561) {
    // A 32-bpp frame buffer through the RGB561's window types.
    for (unsigned y = 0; y < h; y++) {
      const u32 row = start + y * stride * 4;
      u32 *out = &m_frame[size_t(y) * w];
      for (unsigned x = 0; x < w; x++)
        out[x] = rgb561_pixel(vram32(row + 4 * x));
    }
  } else {
    // A 32-bpp frame buffer as true colour (no Bt463 window types yet).
    for (unsigned y = 0; y < h; y++) {
      const u32 row = start + y * stride * 4;
      u32 *out = &m_frame[size_t(y) * w];
      for (unsigned x = 0; x < w; x++)
        out[x] = 0xff000000u | (vram32(row + 4 * x) & 0x00ffffff);
    }
  }
  if (m_model.ramdac == TgaRamdac::Rgb561)
    draw_rgb561_cursor(w, h);
  else if (dac.cmd[2] & 3)
    draw_bt485_cursor(w, h);
  if (r[VVVR] & VVVR_CURSOR)
    draw_tga_cursor(w, h);

  if (w != m_last_w || h != m_last_h)
    printf("%s: display %ux%u, %u bpp (VHCR %08x VVCR %08x)\n", devid_string, w,
           h, deep() ? 32 : 8, r[VHCR], r[VVCR]);
  if (!m_owns_gui) {
    m_last_w = w;
    m_last_h = h;
    dump_frame(w, h);
    return;
  }
  if (w != m_last_w || h != m_last_h) {
    bx_gui->dimension_update(w, h, 0, 0, 32);
    m_last_w = w;
    m_last_h = h;
  }
  bx_gui->graphics_frame_update(m_frame.data(), w, h);
}

/// A cursor colour (overscan is 0) as 0xffRRGGBB, 6- or 8-bit like the
/// palette.
static u32 coc_color(const u8 *c, bool eight) {
  u32 rr = c[0], gg = c[1], bb = c[2];
  if (!eight) {
    rr = ((rr & 0x3f) << 2) | ((rr & 0x3f) >> 4);
    gg = ((gg & 0x3f) << 2) | ((gg & 0x3f) >> 4);
    bb = ((bb & 0x3f) << 2) | ((bb & 0x3f) >> 4);
  }
  return 0xff000000u | (rr << 16) | (gg << 8) | bb;
}

/**
 * The Bt485 cursor: 64x64 (command register 3 bit 2) or 32x32, two planes
 * of 1 bit a pixel, the leftmost pixel in a byte's MSB; plane 0 first,
 * plane 1 at half the cursor RAM (512 bytes for 64x64, 128 for 32x32).
 * Command register 2 <1:0> chooses how the pair (plane 1, plane 0) is
 * shown: 1 three colours (00 transparent, then colours 1-3); 2 XGA (00
 * colour 1, 01 colour 2, 10 transparent, 11 the screen inverted); 3 X
 * Windows (0x transparent, 10 colour 1, 11 colour 2). The position
 * registers hold the cursor's bottom right corner plus one: the left edge
 * is X less the cursor size, as NetBSD's bt485 programs it.
 **/
void CTga::draw_bt485_cursor(unsigned w, unsigned h) {
  const unsigned mode = dac.cmd[2] & 3;
  const unsigned size = (dac.cmd[3] & 0x04) ? 64 : 32;
  const unsigned plane1 = size == 64 ? 512 : 128;
  const unsigned row_bytes = size / 8;
  const int left = int(dac.cur_x) - int(size);
  const int top = int(dac.cur_y) - int(size);
  const bool eight = (dac.cmd[0] & 0x02) != 0;
  const u32 c1 = coc_color(dac.coc[1], eight);
  const u32 c2 = coc_color(dac.coc[2], eight);
  const u32 c3 = coc_color(dac.coc[3], eight);
  for (unsigned cy = 0; cy < size; cy++) {
    const int y = top + int(cy);
    if (y < 0 || y >= int(h))
      continue;
    for (unsigned cx = 0; cx < size; cx++) {
      const int x = left + int(cx);
      if (x < 0 || x >= int(w))
        continue;
      const unsigned byte = cy * row_bytes + cx / 8;
      const unsigned bit = 7 - (cx & 7);
      const unsigned p0 = (dac.cursor[byte] >> bit) & 1;
      const unsigned p1 = (dac.cursor[plane1 + byte] >> bit) & 1;
      const unsigned v = (p1 << 1) | p0;
      u32 &px = m_frame[size_t(y) * w + unsigned(x)];
      switch (mode) {
      case 1:
        if (v)
          px = v == 1 ? c1 : v == 2 ? c2 : c3;
        break;
      case 2:
        if (v == 0)
          px = c1;
        else if (v == 1)
          px = c2;
        else if (v == 3)
          px = px ^ 0x00ffffffu;
        break;
      case 3:
        if (v & 2)
          px = (v & 1) ? c2 : c1;
        break;
      }
    }
  }
}

/**
 * The 21030's own 64x64x2 cursor (4.6): rows of 16 bytes from CCBR's base
 * in the first kilobyte of VRAM, CCBR<15:10> + 1 of them; its position in
 * CXYR is counted from the start of sync, so the screen's left edge is at
 * the horizontal sync plus back porch (Table 4-63). Its two bits a pixel
 * drive the RAMDAC's overlay inputs; drawn here with the Bt485's cursor
 * colours. Unverified: no driver for an 8-plane board turns it on.
 **/
void CTga::draw_tga_cursor(unsigned w, unsigned h) {
  const u32 base = r[CCBR] & 0x3f0;
  const unsigned rows = ((r[CCBR] >> 10) & 0x3f) + 1;
  const int hsync = int((r[VHCR] >> 14) & 0x7f) * 4;
  const int hbp = int((r[VHCR] >> 21) & 0x7f) * 4;
  const int vsync = int((r[VVCR] >> 16) & 0x3f);
  const int vbp = int((r[VVCR] >> 22) & 0x3f);
  const int left = int(r[CXYR] & 0xfff) - (hsync + hbp);
  const int top = int((r[CXYR] >> 12) & 0xfff) - (vsync + vbp);
  const bool eight = (dac.cmd[0] & 0x02) != 0;
  for (unsigned cy = 0; cy < rows; cy++) {
    const int y = top + int(cy);
    if (y < 0 || y >= int(h))
      continue;
    for (unsigned cx = 0; cx < 64; cx++) {
      const int x = left + int(cx);
      if (x < 0 || x >= int(w))
        continue;
      const u32 a = (base + cy * 16 + cx / 4) & 0x3ff;
      const unsigned v = (vram8(a) >> (2 * (cx & 3))) & 3;
      if (v)
        m_frame[size_t(y) * w + unsigned(x)] = coc_color(dac.coc[v], eight);
    }
  }
}

/// ALPHABOX_TGA_DUMP=<prefix>: a card without the GUI writes its screen
/// as <prefix>-NNN-WxH.ppm every two seconds, like ALPHABOX_DUMP_FB.
void CTga::dump_frame(unsigned w, unsigned h) {
  if (!m_dump_prefix)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (m_dump_seq &&
      std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_dump)
              .count() < 2000)
    return;
  m_last_dump = now;
  char path[512];
  snprintf(path, sizeof(path), "%s-%03u-%ux%u.ppm", m_dump_prefix, m_dump_seq++,
           w, h);
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  for (size_t i = 0; i < size_t(w) * h; i++) {
    const u8 rgb[3] = {u8(m_frame[i] >> 16), u8(m_frame[i] >> 8),
                       u8(m_frame[i])};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}
