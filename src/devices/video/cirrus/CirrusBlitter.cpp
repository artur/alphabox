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
 * GD54xx BitBLT engine.
 *
 * A blit is programmed through GR20..GR39 (or their memory-mapped aliases)
 * and started by GR31 bit 1, or by a write to GR2A with autostart on. The
 * operations:
 *   - solid fill with the foreground colour;
 *   - screen-to-screen copy, forwards or backwards, optionally skipping
 *     pixels equal to a transparent colour (8/16 bpp);
 *   - colour expansion of a 1 bpp source, two-colour or transparent;
 *   - 8x8 pattern fill, from a colour pattern or a 1 bpp one.
 * Each applies one of 16 raster operations between source and destination.
 * The source is VRAM, or -- for system-to-screen blits -- bytes the guest
 * writes to the aperture afterwards, consumed a line at a time. A screen-to-
 * system blit is the same thing the other way round: the destination is the
 * host, which reads the rectangle back out of the aperture a line at a time.
 *
 * Behaviour follows what QEMU's GD54xx model documents, except where a
 * comment below says otherwise. Every VRAM access is masked, so no
 * register value can reach outside the framebuffer.
 **/

#include "CirrusBlitter.hpp"
#include "CirrusRegs.hpp"

#include <cstring>

using namespace cirrus;

namespace {

/// GR32 value -> raster operation (see rop_apply). Unknown codes leave the
/// destination alone.
constexpr int ROP_INDEX_NOP = 2;

int rop_index(u8 rop) {
  switch (rop) {
  case ROP_0:
    return 0;
  case ROP_S_AND_D:
    return 1;
  case ROP_NOP:
    return 2;
  case ROP_S_AND_ND:
    return 3;
  case ROP_ND:
    return 4;
  case ROP_S:
    return 5;
  case ROP_1:
    return 6;
  case ROP_NS_AND_D:
    return 7;
  case ROP_S_XOR_D:
    return 8;
  case ROP_S_OR_D:
    return 9;
  case ROP_NS_OR_ND:
    return 10;
  case ROP_S_XNOR_D:
    return 11;
  case ROP_S_OR_ND:
    return 12;
  case ROP_NS:
    return 13;
  case ROP_NS_OR_D:
    return 14;
  case ROP_NS_AND_ND:
    return 15;
  default:
    return ROP_INDEX_NOP;
  }
}

/// The operations are bitwise, so applying them a byte at a time gives the
/// same result for any pixel width.
inline u8 rop_apply(int index, u8 d, u8 s) {
  switch (index) {
  case 0:
    return 0;
  case 1:
    return s & d;
  case 3:
    return s & ~d;
  case 4:
    return ~d;
  case 5:
    return s;
  case 6:
    return 0xff;
  case 7:
    return ~s & d;
  case 8:
    return s ^ d;
  case 9:
    return s | d;
  case 10:
    return ~s | ~d;
  case 11:
    return ~(s ^ d);
  case 12:
    return s | ~d;
  case 13:
    return ~s;
  case 14:
    return ~s | d;
  case 15:
    return ~s & ~d;
  case ROP_INDEX_NOP:
  default:
    return d;
  }
}

} // namespace

/**
 * GR31 write: a falling reset bit aborts the blit, a rising start bit (or
 * start written while releasing reset) starts one.
 **/
void CCirrusBlitter::status_write(u8 data) {
  const u8 old = m_gr[GC_BLT_STATUS];
  m_gr[GC_BLT_STATUS] = data;
  // One write can release reset and set start; the start must not be lost
  // (the Windows NT Alpha driver writes 0x04 then 0x02). QEMU treats the
  // two as exclusive and drops that start.
  const bool released = (old & BLT_RESET) && !(data & BLT_RESET);
  if (released)
    reset();
  if (!(data & BLT_RESET) && (data & BLT_START) &&
      (released || !(old & BLT_START)))
    start();
}

void CCirrusBlitter::reset() {
  m_gr[GC_BLT_STATUS] &= ~(BLT_START | BLT_STATUS_BUSY | BLT_FIFO_USED);
  m_host_active = false;
  m_host_source = false;
  m_host_dest = false;
  m_host_remaining = 0;
  m_host_fill = 0;
  m_host_line = 0;
}

/// Foreground (GR01/GR11/GR13/GR15) or background (GR00/GR10/GR12/GR14)
/// colour, as many bytes as the blit's pixel width.
u32 CCirrusBlitter::color(bool foreground) const {
  const int f = foreground ? 1 : 0;
  const u8 b[4] = {m_gr[f], m_gr[0x10 + f], m_gr[0x12 + f], m_gr[0x14 + f]};
  u32 color = 0;
  for (int i = 0; i < m_bytes; i++)
    color |= u32(b[i]) << (8 * i);
  return color;
}

u8 CCirrusBlitter::src_byte(u32 addr) const {
  if (m_host_source)
    return m_buffer[addr & (BUFFER_SIZE - 1)];
  return m_vram[addr & m_mask];
}

/// A little-endian source pixel; 16 and 32 bpp pixels are word aligned.
u32 CCirrusBlitter::src_pixel(u32 addr, int bytes) const {
  if (bytes == 2)
    addr &= ~1u;
  else if (bytes == 4)
    addr &= ~3u;
  u32 value = 0;
  for (int i = 0; i < bytes; i++)
    value |= u32(src_byte(addr + i)) << (8 * i);
  return value;
}

/**
 * The destination byte at an address. On a screen-to-system blit the
 * destination is the line the host is about to read, not VRAM, so the whole
 * of run() draws into the buffer and the guest takes the result away
 * through the aperture.
 **/
u8 &CCirrusBlitter::dst_byte(u32 addr) {
  if (m_host_dest)
    return m_buffer[addr & (BUFFER_SIZE - 1)];
  return m_vram[addr & m_mask];
}

void CCirrusBlitter::store(u32 addr, u8 src) {
  u8 &d = dst_byte(addr);
  d = rop_apply(m_rop, d, src);
}

void CCirrusBlitter::put(u32 addr, u32 color) {
  const int n = m_bytes;
  if (n == 2)
    addr &= ~1u;
  else if (n == 4)
    addr &= ~3u;
  for (int i = 0; i < n; i++)
    store(addr + i, u8(color >> (8 * i)));
}

/// Store a pixel unless the raster operation's result is the transparent
/// colour.
void CCirrusBlitter::put_transparent(u32 addr, u32 color, u32 transparent) {
  const int n = m_bytes;
  if (n == 2)
    addr &= ~1u;
  u8 result[4];
  u32 pixel = 0;
  for (int i = 0; i < n; i++) {
    const u8 d = dst_byte(addr + i);
    result[i] = rop_apply(m_rop, d, u8(color >> (8 * i)));
    pixel |= u32(result[i]) << (8 * i);
  }
  if (pixel == transparent)
    return;
  for (int i = 0; i < n; i++)
    dst_byte(addr + i) = result[i];
}

/**
 * Execute the current operation over a rectangle. Pitches are signed:
 * backward copies run with negated pitches from the bottom-right corner.
 **/
void CCirrusBlitter::run(u32 dst, u32 src, int dst_pitch, int src_pitch,
                         int width, int height) {
  const int n = m_bytes;

  // GR2F: pixels to leave untouched at the left edge of every line. In
  // 24 bpp it counts bytes (five bits), otherwise pixels (three bits).
  // QEMU applies the 24 bpp rule only to its transparent and colour-pattern
  // operations; the register means the same for all of them, so it is
  // applied uniformly here.
  const u8 skip = m_gr[GC_BLT_SKIP];
  const int skip_dst = (n == 3) ? (skip & 0x1f) : (skip & 0x07) * n;
  const int skip_pixels = (n == 3) ? (skip & 0x1f) / 3 : (skip & 0x07);
  const int skip_bits = skip_pixels & 7;
  const int skip_src_bytes = skip_pixels >> 3;

  switch (m_kind) {
  case FILL:
    for (int y = 0; y < height; y++, dst += dst_pitch) {
      u32 a = dst;
      for (int x = 0; x < width; x += n, a += n)
        put(a, m_fg);
    }
    break;

  case COPY_FWD: {
    const int dp = dst_pitch - width;
    const int sp = src_pitch - width;
    if (height > 1 && (dp < 0 || sp < 0))
      return;
    for (int y = 0; y < height; y++, dst += dp, src += sp)
      for (int x = 0; x < width; x++)
        store(dst++, src_byte(src++));
    break;
  }

  case COPY_BKWD: {
    const int dp = dst_pitch + width;
    const int sp = src_pitch + width;
    for (int y = 0; y < height; y++, dst += dp, src += sp)
      for (int x = 0; x < width; x++)
        store(dst--, src_byte(src--));
    break;
  }

  case COPY_TRANSPARENT_FWD:
  case COPY_TRANSPARENT_BKWD: {
    const u32 transparent = (n == 1) ? m_gr[GC_BLT_TRANSP]
                                     : u32(m_gr[GC_BLT_TRANSP]) |
                                           (u32(m_gr[GC_BLT_TRANSP + 1]) << 8);
    if (m_kind == COPY_TRANSPARENT_FWD) {
      const int dp = dst_pitch - width;
      const int sp = src_pitch - width;
      if (height > 1 && (dp < 0 || sp < 0))
        return;
      for (int y = 0; y < height; y++, dst += dp, src += sp)
        for (int x = 0; x < width; x += n, dst += n, src += n)
          put_transparent(dst, src_pixel(src, n), transparent);
    } else {
      // The addresses name the last byte of the pixel.
      const int dp = dst_pitch + width;
      const int sp = src_pitch + width;
      for (int y = 0; y < height; y++, dst += dp, src += sp)
        for (int x = 0; x < width; x += n, dst -= n, src -= n)
          put_transparent(dst - (n - 1), src_pixel(src - (n - 1), n),
                          transparent);
    }
    break;
  }

  case EXPAND:
  case EXPAND_TRANSPARENT: {
    // A 1 bpp source, one bit per pixel, MSB first; each line starts on a
    // fresh source byte. The source pitch is not used.
    const bool transparent = m_kind == EXPAND_TRANSPARENT;
    const bool invert = transparent && (m_mode_ext & BLT_EXT_EXPAND_INVERT);
    const u8 flip = invert ? 0xff : 0x00;
    const u32 ink = invert ? m_bg : m_fg;
    for (int y = 0; y < height; y++, dst += dst_pitch) {
      src += skip_src_bytes;
      unsigned mask = 0x80u >> skip_bits;
      unsigned bits = src_byte(src++) ^ flip;
      u32 a = dst + skip_dst;
      for (int x = skip_dst; x < width; x += n, a += n, mask >>= 1) {
        if (!(mask & 0xff)) {
          mask = 0x80;
          bits = src_byte(src++) ^ flip;
        }
        const bool set = (bits & mask) != 0;
        if (!transparent)
          put(a, set ? m_fg : m_bg);
        else if (set)
          put(a, ink);
      }
    }
    break;
  }

  case PATTERN: {
    // An 8x8 colour pattern in rows of 8, 16 or 32 bytes (24 bpp uses
    // 32-byte rows too).
    const int row_bytes = (n == 1) ? 8 : (n == 2) ? 16 : 32;
    int py = m_src & 7;
    for (int y = 0; y < height; y++, dst += dst_pitch, py = (py + 1) & 7) {
      const u32 row = src + py * row_bytes;
      int px = (n == 3) ? skip_bits : skip_dst;
      u32 a = dst + skip_dst;
      for (int x = skip_dst; x < width; x += n, a += n) {
        u32 color;
        switch (n) {
        case 1:
          color = src_byte(row + px);
          px = (px + 1) & 7;
          break;
        case 2:
          color = src_pixel(row + px, 2);
          px = (px + 2) & 15;
          break;
        case 3:
          color = src_pixel(row + px * 3, 3);
          px = (px + 1) & 7;
          break;
        default:
          color = src_pixel(row + px, 4);
          px = (px + 4) & 31;
          break;
        }
        put(a, color);
      }
    }
    break;
  }

  case EXPAND_PATTERN:
  case EXPAND_PATTERN_TRANSPARENT: {
    // An 8x8 1 bpp pattern, one byte per row.
    const bool transparent = m_kind == EXPAND_PATTERN_TRANSPARENT;
    const bool invert = transparent && (m_mode_ext & BLT_EXT_EXPAND_INVERT);
    const u8 flip = invert ? 0xff : 0x00;
    const u32 ink = invert ? m_bg : m_fg;
    int py = m_src & 7;
    for (int y = 0; y < height; y++, dst += dst_pitch, py = (py + 1) & 7) {
      const unsigned bits = src_byte(src + py) ^ flip;
      int bit = 7 - skip_bits;
      u32 a = dst + skip_dst;
      for (int x = skip_dst; x < width; x += n, a += n, bit = (bit - 1) & 7) {
        const bool set = ((bits >> bit) & 1) != 0;
        if (!transparent)
          put(a, set ? m_fg : m_bg);
        else if (set)
          put(a, ink);
      }
    }
    break;
  }
  }
}

/**
 * Prepare a system-to-screen blit: work out how many source bytes make a
 * line and wait for them (host_write).
 **/
bool CCirrusBlitter::start_host_src() {
  const int pattern_row = (m_bytes == 1) ? 8 : (m_bytes == 2) ? 16 : 32;

  if (m_mode & BLT_MODE_PATTERN) {
    // The whole pattern arrives at once. QEMU expects 8 * 8 * bytes for a
    // colour pattern, which disagrees with its own 32-byte rows at 24 bpp;
    // the pattern size used for VRAM sources is taken here.
    m_src_pitch = (m_mode & BLT_MODE_EXPAND) ? 8 : 8 * pattern_row;
    m_host_remaining = m_src_pitch;
  } else {
    if (m_mode & BLT_MODE_EXPAND) {
      const int pixels = m_width / m_bytes;
      // With double-word granularity each line is padded to 32 bits, i.e.
      // four bytes per word. (QEMU counts the words, not their bytes.)
      m_src_pitch = (m_mode_ext & BLT_EXT_DWORD_ALIGN)
                        ? ((pixels + 31) >> 5) * 4
                        : (pixels + 7) >> 3;
    } else {
      m_src_pitch = (m_width + 3) & ~3; // lines are 32-bit aligned
    }
    m_host_remaining = m_src_pitch * m_height;
  }

  if (m_src_pitch <= 0 || size_t(m_src_pitch) > BUFFER_SIZE)
    return false;

  m_host_line = size_t(m_src_pitch);
  m_host_fill = 0;
  m_host_source = true;
  m_host_active = true;
  return true;
}

/**
 * One source byte of a system-to-screen blit, written by the guest to the
 * aperture. Each complete line (or the whole pattern) is drawn at once.
 **/
void CCirrusBlitter::host_write(u8 data) {
  m_buffer[m_host_fill++] = data;
  if (m_host_fill < m_host_line)
    return;

  m_drawn = true;
  if (m_mode & BLT_MODE_PATTERN) {
    run(m_dst, 0, m_dst_pitch, 0, m_width, m_height);
    reset();
    return;
  }

  run(m_dst, 0, 0, 0, m_width, 1);
  m_dst += m_dst_pitch;
  m_host_fill = 0;
  m_host_remaining -= m_src_pitch;
  if (m_host_remaining <= 0)
    reset();
}

/**
 * Prepare a screen-to-system blit. The engine still does the work -- it
 * reads the source rectangle and applies the raster operation -- but the
 * result goes to the host instead of VRAM, one line at a time, so only the
 * line the guest is reading has to exist.
 *
 * The chip hands back a whole number of doublewords per line, which is how
 * it takes them in the other direction; the padding at the end of a short
 * line is whatever the engine leaves there, and here that is zero.
 **/
bool CCirrusBlitter::start_host_dst() {
  // Only a plain rectangle read makes sense this way round. Colour
  // expansion and the pattern fills invent pixels rather than read a
  // rectangle back, and no driver asks the engine to hand those over.
  if (m_kind != COPY_FWD && m_kind != COPY_TRANSPARENT_FWD)
    return false;

  m_host_line = size_t((m_width + 3) & ~3);
  if (m_host_line > BUFFER_SIZE)
    return false;

  m_host_remaining = int(m_host_line) * m_height;
  m_host_dest = true;
  m_host_active = true;
  // The engine holds a line the guest has not taken yet, which is what the
  // FIFO-used bit says.
  m_gr[GC_BLT_STATUS] |= BLT_FIFO_USED;
  fill_host_line();
  return true;
}

/**
 * Draw the next line of a screen-to-system blit into the buffer. The line
 * starts out blank, so a raster operation that wants a destination sees
 * zero: the engine never fetches the host's memory, and a driver reading the
 * screen back asks for the source anyway.
 **/
void CCirrusBlitter::fill_host_line() {
  memset(m_buffer, 0, m_host_line);
  run(0, m_src, 0, 0, m_width, 1);
  m_src += m_src_pitch;
  m_host_fill = 0;
}

/**
 * One byte of a screen-to-system blit, read by the guest from the aperture.
 * The next line is drawn as soon as the previous one has been taken, and the
 * blit ends with the last byte of the last line.
 **/
u8 CCirrusBlitter::host_read() {
  const u8 data = m_buffer[m_host_fill++];
  if (m_host_fill < m_host_line)
    return data;

  m_host_remaining -= int(m_host_line);
  if (m_host_remaining <= 0)
    reset();
  else
    fill_host_line();
  return data;
}

void CCirrusBlitter::start() {
  m_gr[GC_BLT_STATUS] |= BLT_STATUS_BUSY;

  m_width = (m_gr[0x20] | (m_gr[0x21] << 8)) + 1;
  m_height = (m_gr[0x22] | (m_gr[0x23] << 8)) + 1;
  m_dst_pitch = m_gr[0x24] | (m_gr[0x25] << 8);
  m_src_pitch = m_gr[0x26] | (m_gr[0x27] << 8);
  m_dst = (m_gr[0x28] | (m_gr[0x29] << 8) | (m_gr[0x2a] << 16)) & m_mask;
  m_src = (m_gr[0x2c] | (m_gr[0x2d] << 8) | (m_gr[0x2e] << 16)) & m_mask;
  m_mode = m_gr[GC_BLT_MODE];
  m_mode_ext = m_gr[GC_BLT_MODE_EXT];
  m_rop = rop_index(m_gr[GC_BLT_ROP]);
  m_bytes = ((m_mode & BLT_MODE_PIXEL_WIDTH) >> 4) + 1;
  m_mode &= ~BLT_MODE_PIXEL_WIDTH;
  m_host_source = false;
  m_host_dest = false;
  m_fg = color(true);
  m_bg = color(false);

  const bool transparent = (m_mode & BLT_MODE_TRANSPARENT) != 0;

  if ((m_mode & (BLT_MODE_HOST_SRC | BLT_MODE_HOST_DST)) ==
          (BLT_MODE_HOST_SRC | BLT_MODE_HOST_DST) ||
      size_t(m_width) > BUFFER_SIZE) {
    reset();
    return;
  }

  if ((m_mode_ext & BLT_EXT_SOLID_FILL) &&
      (m_mode & (BLT_MODE_HOST_DST | BLT_MODE_TRANSPARENT | BLT_MODE_PATTERN |
                 BLT_MODE_EXPAND)) == (BLT_MODE_PATTERN | BLT_MODE_EXPAND)) {
    m_kind = FILL;
    run(m_dst, 0, m_dst_pitch, 0, m_width, m_height);
    m_drawn = true;
    reset();
    return;
  }

  switch (m_mode & (BLT_MODE_EXPAND | BLT_MODE_PATTERN)) {
  case BLT_MODE_EXPAND:
    m_kind = transparent ? EXPAND_TRANSPARENT : EXPAND;
    break;
  case BLT_MODE_EXPAND | BLT_MODE_PATTERN:
    m_kind = transparent ? EXPAND_PATTERN_TRANSPARENT : EXPAND_PATTERN;
    break;
  case BLT_MODE_PATTERN:
    m_kind = PATTERN;
    break;
  default:
    if (transparent && m_bytes > 2) {
      // Transparent copies exist only for 8 and 16 bpp.
      reset();
      return;
    }
    // Backwards is there so that an overlapping screen-to-screen copy does
    // not overwrite what it has yet to read. A readback has no such overlap
    // -- the destination is the host's own memory -- and the guest expects
    // the stream in address order, so the direction bit is left alone.
    if ((m_mode & BLT_MODE_BACKWARDS) && !(m_mode & BLT_MODE_HOST_DST)) {
      m_dst_pitch = -m_dst_pitch;
      m_src_pitch = -m_src_pitch;
      m_kind = transparent ? COPY_TRANSPARENT_BKWD : COPY_BKWD;
    } else {
      m_kind = transparent ? COPY_TRANSPARENT_FWD : COPY_FWD;
    }
    break;
  }

  if (m_mode & BLT_MODE_HOST_SRC) {
    if (!start_host_src())
      reset();
    return; // runs as the source bytes arrive
  }

  if (m_mode & BLT_MODE_HOST_DST) {
    if (!start_host_dst())
      reset();
    return; // runs as the guest reads the bytes back
  }

  if (m_mode & BLT_MODE_PATTERN) {
    // A VRAM pattern starts on a pattern-size boundary.
    const u32 size = (m_bytes == 1) ? 64 : (m_bytes == 2) ? 128 : 256;
    m_src &= ~(size - 1);
    if (m_src + size > m_vram_bytes) {
      reset();
      return;
    }
    run(m_dst, m_src, m_dst_pitch, 0, m_width, m_height);
  } else {
    run(m_dst, m_src, m_dst_pitch, m_src_pitch, m_width, m_height);
  }
  m_drawn = true;
  reset();
}
