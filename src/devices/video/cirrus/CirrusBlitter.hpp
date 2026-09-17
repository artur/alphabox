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

/* Cirrus Logic GD54xx BitBLT engine.
 *
 * Deliberately independent of the device and the emulator: it works on a
 * framebuffer and the chip's graphics controller register array, nothing
 * else, so it can be exercised on its own (see CirrusBlitter.cpp for the
 * operations it implements).
 */

#if !defined(INCLUDED_CIRRUS_BLITTER_H)
#define INCLUDED_CIRRUS_BLITTER_H

#include <cstddef>
#include <cstdint>

#include "datatypes.hpp"

class CCirrusBlitter {
public:
  /// \param gr   the chip's GR register array: GR00..GR39 are read when a
  ///             blit starts, and GR31's status bits are kept up to date.
  /// \param name prefix for the few messages the engine prints.
  CCirrusBlitter(u8 *gr, const char *name) : m_gr(gr), m_name(name) {}

  /// The framebuffer; its size must be a power of two.
  void set_vram(u8 *vram, u32 vram_bytes) {
    m_vram = vram;
    m_vram_bytes = vram_bytes;
    m_mask = vram_bytes - 1;
  }

  /// A write to GR31 (start/status).
  void status_write(u8 data);
  /// Start the blit the registers describe (GR31 start, or GR2A autostart).
  void start();
  /// Abandon any blit in progress and clear the status bits.
  void reset();

  /// A system-to-screen blit is waiting for source bytes: aperture writes
  /// go to host_write() instead of VRAM.
  bool host_active() const { return m_host_active; }
  void host_write(u8 data);

  /// True if VRAM was drawn since the previous call.
  bool take_drawn() {
    const bool drawn = m_drawn;
    m_drawn = false;
    return drawn;
  }

  static constexpr size_t BUFFER_SIZE = 2048 * 4;

private:
  enum kind_t {
    FILL,
    COPY_FWD,
    COPY_BKWD,
    COPY_TRANSPARENT_FWD,
    COPY_TRANSPARENT_BKWD,
    EXPAND,
    EXPAND_TRANSPARENT,
    PATTERN,
    EXPAND_PATTERN,
    EXPAND_PATTERN_TRANSPARENT,
  };

  bool start_host();
  void run(u32 dst, u32 src, int dst_pitch, int src_pitch, int width,
           int height);
  u8 src_byte(u32 addr) const;
  u32 src_pixel(u32 addr, int bytes) const;
  void store(u32 addr, u8 src);
  void put(u32 addr, u32 color);
  void put_transparent(u32 addr, u32 color, u32 transparent);
  u32 color(bool foreground) const;

  u8 *m_gr;
  const char *m_name;
  u8 *m_vram = nullptr;
  u32 m_vram_bytes = 0;
  u32 m_mask = 0;
  bool m_drawn = false;
  bool m_reported_host_dst = false;

  // The operation in progress.
  kind_t m_kind = FILL;
  int m_rop = 0;   ///< index into the raster-operation table
  int m_bytes = 1; ///< bytes per pixel
  int m_width = 0, m_height = 0;
  int m_dst_pitch = 0, m_src_pitch = 0;
  u32 m_dst = 0, m_src = 0;
  u8 m_mode = 0, m_mode_ext = 0;
  u32 m_fg = 0, m_bg = 0;

  // System-to-screen transfer.
  bool m_host_active = false;
  bool m_host_source = false; ///< src_byte reads the buffer, not VRAM
  int m_host_remaining = 0;   ///< source bytes still expected
  size_t m_host_fill = 0;     ///< bytes buffered for the current line
  size_t m_host_line = 0;     ///< bytes per source line
  u8 m_buffer[BUFFER_SIZE] = {};
};

#endif // !defined(INCLUDED_CIRRUS_BLITTER_H)
