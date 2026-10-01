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
 * DECchip 21030 (TGA): the graphics modes of chapter 6 of the 21030
 * manual. A write to frame buffer space (or to GCTR, with the address from
 * GADR) is interpreted by the mode in GMOR<6:0>; the slope registers start
 * lines; the copy-64 registers move 64-byte spans. Every operation
 * completes before the write returns, so the chip is never busy.
 *
 * Pixels: in an 8-bpp frame buffer a pixel is a byte. In a 32-bpp frame
 * buffer GOPR's destination bitmap decides: packed 8-bpp (a byte),
 * unpacked 8-bpp (one byte, GOPR's destination byte, of each Dword) or
 * 12/24-bpp (the Dword). A 32-bit colour register (foreground, background,
 * plane mask) applies to an 8-bit pixel through the byte lane the pixel
 * sits in, which is why software replicates the colour across the Dword
 * (Figure 4-40).
 **/

#include "StdAfx.hpp"
#include "System.hpp"
#include "Tga.hpp"

using namespace tga;

/// The raster operation, X style (Table 4-21): bit 0 is the result for
/// src = 1, dst = 1; bit 1 for 1, 0; bit 2 for 0, 1; bit 3 for 0, 0.
static inline u32 apply_rop(unsigned rop, u32 s, u32 d) {
  u32 v = 0;
  if (rop & 1)
    v |= s & d;
  if (rop & 2)
    v |= s & ~d;
  if (rop & 4)
    v |= ~s & d;
  if (rop & 8)
    v |= ~s & ~d;
  return v;
}

unsigned CTga::dst_step() const {
  if (!deep())
    return 1;
  return ((r[GOPR] >> GOPR_DBM_SHIFT) & 3) == BM_PB8 ? 1 : 4;
}

unsigned CTga::dst_width() const {
  if (!deep())
    return 1;
  const u32 dbm = (r[GOPR] >> GOPR_DBM_SHIFT) & 3;
  return (dbm == BM_PB8 || dbm == BM_UB8) ? 1 : 4;
}

/// The byte of each Dword an unpacked 8-bpp destination writes.
u32 CTga::dst_byte_offset() const {
  if (!deep())
    return 0;
  const u32 dbm = (r[GOPR] >> GOPR_DBM_SHIFT) & 3;
  return dbm == BM_UB8 ? (r[GOPR] >> GOPR_DBY_SHIFT) & 3 : 0;
}

/// A colour register's value for the pixel at byte address `a`.
static inline u32 lane_value(u32 color, u32 a, unsigned width) {
  return width == 4 ? color : (color >> (8 * (a & 3))) & 0xff;
}

/**
 * Write one pixel: `value` is the pixel (8 bits, or the Dword), combined
 * with the destination by the raster op (or simply stored, for the block
 * modes) and through the plane mask, whose byte lane follows the address.
 **/
void CTga::pixel_write(u32 a, unsigned width, u32 value, bool rop_on) {
  const unsigned rop = rop_on ? (r[GOPR] & GOPR_ROP) : 3;
  if (width == 1) {
    u8 &d = m_vram[a & m_vram_mask];
    const u32 pm = (r[GPMR] >> (8 * (a & 3))) & 0xff;
    const u32 v = apply_rop(rop, value, d);
    d = u8((d & ~pm) | (v & pm));
  } else {
    const u32 d = vram32(a);
    const u32 pm = r[GPMR];
    const u32 v = apply_rop(rop, value, d);
    vram32_set(a, (d & ~pm) | (v & pm));
  }
}

/// After every operation: a one-shot pixel mask returns to all ones
/// (4.4.21.3), and GADR is no longer new.
void CTga::op_done() {
  if (!e.gpxr_persistent)
    e.gpxr = 0xffffffff;
  e.addr_new = false;
  m_generation++;
}

void CTga::fb_write(u32 fbaddr, u32 data, u32 bytemask, bool via_gctr) {
  const u32 mode = r[GMOR] & GMOR_MODE;
  if (m_trace_first)
    first_use("mode %02x rop %x gopr %03x%s", mode, r[GOPR] & 0xf,
              r[GOPR] & 0xff0, via_gctr ? " via GCTR" : "");
  if (mode_is_line(mode)) {
    // A line segment: the write's address (with its two LSBs in data
    // <17:16> for packed 8-bpp) starts it; through GCTR, GADR does if it
    // was written, else the line continues where the last one stopped.
    if (!via_gctr) {
      e.cur_addr = deep() ? fbaddr : fbaddr | ((data >> 16) & 3);
      e.addr_new = false;
    }
    line_draw(data & 0xffff, false);
    return;
  }

  u32 a = fbaddr;
  if (via_gctr)
    a = e.addr_new ? r[GADR] : e.cur_addr;
  e.cur_addr = a;
  switch (mode) {
  case MODE_SIMPLE:
    op_simple(a & ~3u, data, bytemask);
    break;
  case MODE_SIMPLE_Z:
    unimplemented("simple-Z mode (written as simple)");
    op_simple(a & ~3u, data, bytemask);
    break;
  case MODE_OPAQUE_STIPPLE:
    op_stipple(a, data, true);
    break;
  case MODE_TRANSPARENT_STIPPLE:
    op_stipple(a, data, false);
    break;
  case MODE_BLOCK_STIPPLE:
    op_block_stipple(a, data);
    break;
  case MODE_BLOCK_FILL:
    op_fill(a, data, 0);
    break;
  case MODE_OPAQUE_FILL:
    op_fill(a, data, 1);
    break;
  case MODE_TRANSPARENT_FILL:
    op_fill(a, data, 2);
    break;
  case MODE_COPY:
    op_copy(a, data);
    break;
  case MODE_DMA_READ:
  case MODE_DMA_READ_DITHER:
    op_dma_read(a, data);
    break;
  case MODE_DMA_WRITE:
    op_dma_write(a, data);
    break;
  default: {
    char buf[64];
    snprintf(buf, sizeof(buf), "graphics mode %02x", mode);
    unimplemented(buf);
    break;
  }
  }
  op_done();
}

/**
 * Simple mode (6.2.1): the write's bytes, masked by the PCI byte enables
 * and the pixel mask's low nibble, through the raster op and plane mask.
 * An unpacked 8-bpp destination takes the four bytes as four pixels, one
 * per Dword; a 12/24-bpp one is a pixel whose write GPXR<0> enables.
 **/
void CTga::op_simple(u32 a, u32 data, u32 bytemask) {
  u32 m = bytemask & e.gpxr & 0xf;
  if (!deep() || dst_step() == 1) {
    for (unsigned b = 0; b < 4; b++)
      if (m & (1u << b))
        pixel_write(a + b, 1, (data >> (8 * b)) & 0xff, true);
    return;
  }
  if (dst_width() == 1) { // unpacked 8-bpp: address aligned to 16 bytes
    const u32 dby = dst_byte_offset();
    a &= ~15u;
    for (unsigned b = 0; b < 4; b++)
      if (m & (1u << b))
        pixel_write(a + 4 * b + dby, 1, (data >> (8 * b)) & 0xff, true);
    return;
  }
  if (!(e.gpxr & 1))
    return;
  for (unsigned b = 0; b < 4; b++)
    if (bytemask & (1u << b))
      pixel_write(a + b, 1, (data >> (8 * b)) & 0xff, true);
}

/**
 * Opaque and transparent stipple (6.2.3, 6.2.4): 32 pixels from the
 * 4-pixel-aligned address, bit n of the mask for pixel n. Opaque writes
 * foreground for ones and background for zeros where the pixel mask
 * allows; transparent writes foreground for ones only.
 **/
void CTga::op_stipple(u32 a, u32 mask, bool opaque) {
  const unsigned step = dst_step();
  const unsigned width = dst_width();
  const u32 dby = dst_byte_offset();
  // The manual's alignments (6.2.3): 4 bytes for packed 8-bpp, 16 for
  // unpacked, 8 for 12/24-bpp.
  a &= step == 1 ? ~3u : width == 1 ? ~15u : ~7u;
  for (unsigned n = 0; n < 32; n++) {
    const u32 bit = 1u << n;
    const u32 pa = a + n * step + dby;
    if (opaque) {
      if (!(e.gpxr & bit))
        continue;
      const u32 c = (mask & bit) ? r[GFGR] : r[GBGR];
      pixel_write(pa, width, lane_value(c, pa, width), true);
    } else if (mask & bit) {
      pixel_write(pa, width, lane_value(r[GFGR], pa, width), true);
    }
  }
}

/// Block colour j of the 8-pixel pattern (Figure 4-21): bytes of
/// GBCR1:GBCR0 for 8-bit pixels, one register each for 12/24-bpp.
static inline u32 block_color(const u32 *gbcr, unsigned j, unsigned width) {
  static const unsigned order[8] = {0, 2, 4, 6, 1, 3, 5, 7};
  if (width == 4)
    return gbcr[order[j & 7]];
  const u64 p = (u64(gbcr[1]) << 32) | gbcr[0];
  return u32(p >> (8 * (j & 7))) & 0xff;
}

/**
 * Block stipple (6.2.5): 32 pixels from a 1-pixel-aligned address, the
 * block colour pattern written where the mask is one, no raster op. The
 * mask is aligned to 4 pixels (6.2.5.2): bit n is the pixel n places
 * after the 4-pixel boundary at or below the address, so pixel p of the
 * span takes bit (p + address mod 4) mod 32 -- the software's rotate and
 * the chip's. The pattern is aligned to 8 pixels in the frame buffer.
 **/
void CTga::op_block_stipple(u32 a, u32 mask) {
  const unsigned step = dst_step();
  const unsigned width = dst_width();
  const u32 dby = dst_byte_offset();
  if (step == 4)
    a &= ~3u;
  const unsigned rot = (a / step) & 3;
  for (unsigned p = 0; p < 32; p++) {
    if (!(mask & (1u << ((p + rot) & 31))))
      continue;
    const u32 pa = a + p * step;
    pixel_write(pa + dby, width, block_color(&r[GBCR0], pa / step, width),
                false);
  }
}

/**
 * The fill modes (6.2.6-6.2.8): a span of up to 2K pixels, its length
 * less one in the data's <10:0>, the address LSBs for packed 8-bpp in
 * <17:16>; GDAR's 32-bit fill mask repeats every 32 pixels, aligned like
 * block stipple's. kind 0 block fill (the block colours, no raster op),
 * 1 opaque fill (foreground and background), 2 transparent fill
 * (foreground where the mask is one).
 *
 * The manual's opaque-fill parameter table lists the pixel mask, its text
 * says pixels cannot be masked; it is not applied.
 **/
void CTga::op_fill(u32 a, u32 data, int kind) {
  const unsigned step = dst_step();
  const unsigned width = dst_width();
  const u32 dby = dst_byte_offset();
  if (step == 1)
    a |= (data >> 16) & 3;
  else
    a &= ~3u;
  const unsigned count = (data & 0x7ff) + 1;
  const unsigned rot = (a / step) & 3;
  const u32 fill = r[GDAR];
  for (unsigned p = 0; p < count; p++) {
    const bool on = (fill >> ((p + rot) & 31)) & 1;
    const u32 pa = a + p * step;
    switch (kind) {
    case 0:
      if (on)
        pixel_write(pa + dby, width, block_color(&r[GBCR0], pa / step, width),
                    false);
      break;
    case 1:
      pixel_write(pa + dby, width,
                  lane_value(on ? r[GFGR] : r[GBGR], pa + dby, width), true);
      break;
    default:
      if (on)
        pixel_write(pa + dby, width, lane_value(r[GFGR], pa + dby, width),
                    true);
      break;
    }
  }
}

/**
 * The TGA2's rectangle engine, as tga2.dll drives it (no TGA2 manual was
 * found; this is read off the driver's code and register traffic). The
 * TGA2's register file is 1 KB, twice the 21030's: its lower half holds
 * the 21030 registers, its upper half the rectangle engine.
 *
 *   0x200-0x2fc  rectangle ports, starting at GADR when GADR is new
 *   0x300        the rectangle address (a frame buffer byte address)
 *   0x304-0x358  rectangle ports, starting at the rectangle address
 *   0x35c        the row pitch, in pixels
 *
 * A write to a port fills (data <31:16> + 1) rows of (data <15:0> + 1)
 * pixels in the fill mode in force -- block, opaque or transparent fill,
 * GDAR the fill mask of each span as for a 21030 fill -- one span a row,
 * and leaves the address below the last row: the driver clears a 640x480
 * screen as bands of 167, 167 and 146 rows after one address write, and
 * draws a 1-pixel-wide line down 144 rows as 144 one-pixel writes. The
 * ports are many so that consecutive writes do not merge in the CPU's
 * write buffer; the driver steps through them.
 **/
void CTga::tga2_port_write(u32 offset, u32 data) {
  if (offset == 0x35c) {
    e.rect_pitch = data & 0xffff;
    return;
  }
  if (offset == 0x300) {
    e.rect_addr = data;
    e.addr_new = false;
    return;
  }
  if (offset < 0x300 && e.addr_new) {
    e.rect_addr = r[GADR];
    e.addr_new = false;
  }
  if (offset < 0x35c) {
    tga2_rect(data);
    return;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "TGA2 register %03x", offset);
  unimplemented(buf);
}

void CTga::tga2_rect(u32 size) {
  const u32 mode = r[GMOR] & GMOR_MODE;
  int kind;
  switch (mode) {
  case MODE_BLOCK_FILL:
    kind = 0;
    break;
  case MODE_OPAQUE_FILL:
    kind = 1;
    break;
  case MODE_TRANSPARENT_FILL:
    kind = 2;
    break;
  default: {
    char buf[64];
    snprintf(buf, sizeof(buf), "TGA2 rectangle in mode %02x", mode);
    unimplemented(buf);
    return;
  }
  }
  if (m_trace_first)
    first_use("TGA2 rectangle, mode %02x", mode);
  const unsigned rows = (size >> 16) + 1;
  const unsigned cols = (size & 0xffff) + 1;
  const u32 pitch = e.rect_pitch * dst_step();
  for (unsigned y = 0; y < rows; y++) {
    for (unsigned x = 0; x < cols; x += 2048) {
      const unsigned n = std::min(2048u, cols - x);
      op_fill(e.rect_addr + x * dst_step(), u32(n - 1), kind);
    }
    e.rect_addr += pitch;
  }
  op_done();
}

/**
 * The byte shifter (6.2.9.1): each quadword read joins the residue
 * register, the previous one read, and is rotated by the signed pixel
 * shift. A forward copy (shift 0..7) moves bytes up by the shift, the
 * residue's high bytes filling the bottom; a backward copy (-8..-1)
 * reads downwards and moves bytes down, the residue's low bytes filling
 * the top. The first quadword of an alignment that needs it only primes
 * the residue, which software compensates for (6.2.9.3).
 **/
u64 CTga::byte_shift(u64 in, int quad_bytes) {
  (void)quad_bytes;
  int s = int(r[GPSR] & 0xf);
  if (s & 8)
    s -= 16;
  u64 out;
  if (s >= 0) {
    out = s ? (in << (8 * s)) | (e.residue >> (64 - 8 * s)) : in;
  } else {
    const int k = -s;
    out = k == 8 ? e.residue : (in >> (8 * k)) | (e.residue << (64 - 8 * k));
  }
  e.residue = in;
  return out;
}

static inline u64 vram64(const std::vector<u8> &vram, u32 mask, u32 a) {
  u64 v;
  memcpy(&v, &vram[(a & mask) & ~7u], 8);
  return v;
}

/**
 * Copy mode (6.2.9): writes alternate between a source read, which fills
 * the copy buffer from up to four quadwords (8 in a 32-bpp frame buffer)
 * through the byte shifter, and a destination write, which empties it
 * through the destination mask, the raster op and the plane mask. The
 * mask has a bit per byte in an 8-bpp frame buffer, one per Dword pixel
 * (16 pixels) in a 32-bpp one. A negative pixel shift copies backwards:
 * the quadwords step down from the address.
 **/
void CTga::op_copy(u32 a, u32 mask) {
  int s = int(r[GPSR] & 0xf);
  if (s & 8)
    s -= 16;
  const int dir = s < 0 ? -1 : 1;
  const u32 qa = a & ~7u;
  const bool d = deep();
  if (d && ((r[GOPR] >> GOPR_DBM_SHIFT) & 3) < BM_DC12)
    unimplemented("copy mode with 8-bpp bitmaps in a 32-bpp frame buffer");
  const int quads = d ? 8 : 4;
  if (!e.copy_drain) {
    if (mask)
      for (int i = 0; i < quads; i++)
        e.copybuf[i] =
            byte_shift(vram64(m_vram, m_vram_mask, qa + u32(8 * dir * i)), 8);
    e.copy_drain = true;
    e.cbr_fill = 0;
    return;
  }
  for (int i = 0; i < quads; i++) {
    const u32 q = qa + u32(8 * dir * i);
    const u64 v = e.copybuf[i];
    if (!d) {
      const u32 bm = (mask >> (8 * i)) & 0xff;
      for (unsigned b = 0; b < 8; b++)
        if (bm & (1u << b))
          pixel_write(q + b, 1, u32(v >> (8 * b)) & 0xff, true);
    } else {
      for (unsigned p = 0; p < 2; p++)
        if (mask & (1u << (2 * i + p)))
          pixel_write(q + 4 * p, 4, u32(v >> (32 * p)), true);
    }
  }
  e.copy_drain = false;
  e.cbr_fill = 0;
}

/**
 * Which way a register copy steps: the 21030's copy 64 always forward
 * (4.3.4); the TGA2's copy 128 by the pixel shift's sign, as copy mode
 * does -- tga2.dll moves an overlapping span right to left with a pixel
 * shift of -8, its 128-byte pairs stepping down from the copy-mode edge.
 **/
int CTga::copy_dir(unsigned quads) const {
  if (quads <= 8)
    return 1;
  return (r[GPSR] & 8) ? -1 : 1;
}

/**
 * The copy-64 registers (4.3.4): GCSR fills all eight entries of the copy
 * buffer from an 8-byte-aligned frame buffer address, through the byte
 * shifter; GCDR empties them, unmasked, through the raster op and plane
 * mask. Always forward.
 **/
void CTga::copy64_load(u32 a, unsigned quads) {
  const u32 qa = a & ~7u;
  const int dir = copy_dir(quads);
  for (unsigned i = 0; i < quads; i++)
    e.copybuf[i] =
        byte_shift(vram64(m_vram, m_vram_mask, qa + u32(8 * dir * int(i))), 8);
  e.cbr_fill = 0;
  m_generation++;
}

void CTga::copy64_store(u32 a, unsigned quads) {
  const u32 qa0 = a & ~7u;
  const int dir = copy_dir(quads);
  for (unsigned i = 0; i < quads; i++) {
    const u64 v = e.copybuf[i];
    const u32 qa = qa0 + u32(8 * dir * int(i));
    if (!deep()) {
      for (unsigned b = 0; b < 8; b++)
        pixel_write(qa + b, 1, u32(v >> (8 * b)) & 0xff, true);
    } else {
      pixel_write(qa, 4, u32(v), true);
      pixel_write(qa + 4, 4, u32(v >> 32), true);
    }
  }
  e.cbr_fill = 0;
  m_generation++;
}

/**
 * DMA-read copy (6.2.10): the chip reads (count) Dwords of PCI memory from
 * GDBR and writes them, through a 32-bit byte shifter, to the frame buffer
 * from the write's address; edge masks for the first two and the last
 * Dword, and a final write of the residue (the flush) through Mask Right 1
 * (Tables 6-23, 6-24).
 **/
void CTga::op_dma_read(u32 a, u32 data) {
  if ((r[GMOR] & GMOR_MODE) == MODE_DMA_READ_DITHER)
    unimplemented("dithered DMA-read copy (copied undithered)");
  const unsigned count = ((data >> 16) & 0x7ff) + 1;
  const u32 left0 = (data >> 8) & 0xf, left1 = (data >> 12) & 0xf;
  const u32 right0 = data & 0xf, right1 = (data >> 4) & 0xf;
  const unsigned s = r[GPSR] & 3;
  if (r[GPSR] & 0xc)
    unimplemented("DMA-read copy pixel shift above 3");
  std::vector<u32> buf(count);
  do_pci_read(r[GDBR] & ~3u, buf.data(), 4, count);
  a &= ~3u;
  u32 residue = 0;
  auto put = [&](u32 da, u32 v, u32 m) {
    for (unsigned b = 0; b < 4; b++)
      if (m & (1u << b))
        pixel_write(da + b, 1, (v >> (8 * b)) & 0xff, true);
  };
  for (unsigned i = 0; i < count; i++) {
    const u32 in = buf[i];
    const u32 out = s ? (in << (8 * s)) | (residue >> (32 - 8 * s)) : in;
    residue = in;
    u32 m = 0xf;
    if (i == count - 1)
      m = right0;
    else if (i == 0)
      m = left0;
    else if (i == 1)
      m = left1;
    put(a + 4 * i, out, m);
  }
  const u32 flush = s ? residue >> (32 - 8 * s) : 0;
  put(a + 4 * count, flush, right1);
  trace("DMA read %u Dwords from %08x to fb %06x", count, r[GDBR], a);
}

/**
 * DMA-write copy (6.2.11): (count) quadwords read from the frame buffer
 * through the byte shifter, the first only priming the residue; the
 * results go to PCI memory from GDBR a Dword at a time, ANDed with GDAR,
 * the first through Mask Left and the last through Mask Right (a byte
 * enable a bit), a Dword whose four enables are all clear not sent.
 **/
void CTga::op_dma_write(u32 a, u32 data) {
  const unsigned count = ((data >> 16) & 0x7ff) + 1;
  const u32 mleft = (data >> 8) & 0xff, mright = data & 0xff;
  u32 qa = a & ~7u;
  u32 p = r[GDBR] & ~3u;
  auto send = [&](u32 v, u32 m) {
    if (!m)
      return;
    v &= r[GDAR];
    if (m != 0xf) {
      u32 old;
      do_pci_read(p, &old, 4, 1);
      for (unsigned b = 0; b < 4; b++)
        if (!(m & (1u << b)))
          v = (v & ~(0xffu << (8 * b))) | (old & (0xffu << (8 * b)));
    }
    do_pci_write(p, &v, 4, 1);
    p += 4;
  };
  byte_shift(vram64(m_vram, m_vram_mask, qa), 8); // primes the residue
  qa += 8;
  for (unsigned j = 1; j < count; j++, qa += 8) {
    const u64 out = byte_shift(vram64(m_vram, m_vram_mask, qa), 8);
    u32 m = 0xff;
    if (j == 1)
      m = mleft;
    else if (j == count - 1)
      m = mright;
    send(u32(out), m & 0xf);
    send(u32(out >> 32), m >> 4);
  }
  trace("DMA write %u quadwords from fb %06x to %08x", count, a & ~7u, r[GDBR]);
}

/**
 * Line setup on a write to a slope (or slope-no-go, or span width)
 * register (6.2.12.1): the octant is the register's number (Figure 4-10:
 * bit 0 dy >= 0, i.e. downwards; bit 1 dx >= 0), the data the absolute
 * dy and dx. The terms go to GB1R-GB3R, the hardware's pseudo-code
 * followed term for term, the initial error depending on the graphics
 * environment (Win32 or X).
 **/
void CTga::line_setup(unsigned octant, u32 slope) {
  const int adx = int(slope & 0xffff);
  const int ady = int(slope >> 16);
  const bool dxge0 = (octant & 2) != 0;
  const bool dyge0 = (octant & 1) != 0;
  const bool dxgedy = adx >= ady;
  const int pixel_bytes = deep() ? 4 : 1;
  const int width = int(r[GBWR] & 0xffff);
  const int dmajor = dxgedy ? adx : ady;
  const int dminor = dxgedy ? ady : adx;
  const bool majorge0 = dxgedy ? dxge0 : dyge0;
  const bool minorge0 = dxgedy ? dyge0 : dxge0;
  const int amajor = dxgedy ? pixel_bytes : width;
  const int aminor = dxgedy ? width : pixel_bytes;
  int errinc;
  if (r[GMOR] & GMOR_GE)
    errinc = dxgedy ? (dyge0 ? 1 : 0) : (dxge0 ? 0 : 1);
  else
    errinc = majorge0 ? 1 : 0;
  const int cap = (r[GMOR] & GMOR_CE) ? 1 : 0;
  const u32 length = u32(dmajor + cap) & 0xf;
  const int einc1 = dminor;
  const int einc2 = dmajor - dminor;
  const int ierr = (2 * dminor - dmajor - 1 + errinc) >> 1;
  const int ainc1 = majorge0 ? amajor : -amajor;
  const int ainc2 = (minorge0 ? aminor : -aminor) + ainc1;
  r[GB1R] = (u32(ainc1) << 16) | (u32(einc1) & 0xffff);
  r[GB2R] = (u32(ainc2) << 16) | (u32(einc2) & 0xffff);
  r[GB3R] = (u32(ierr) << 15) | length;
  e.bres_new = true;
  // GSWR reads back the slope flags (Table 4-11).
  r[GSWR] = (dxgedy ? 4 : 0) | (dxge0 ? 2 : 0) | (dyge0 ? 1 : 0);
}

/**
 * Draw one line segment (6.2.12): up to 16 pixels from the working
 * address, Bresenham-stepped, bit i of the line mask for the i-th pixel.
 * The working length and error come from GB3R only if it is new, the
 * address from GADR only if that is (6.2.12.3); afterwards the engine is
 * left at the next pixel with a length of 16, ready for GCTR to continue.
 * Opaque lines write foreground for ones and background for zeros,
 * transparent ones foreground for ones.
 **/
void CTga::line_draw(u32 mask, bool via_register) {
  (void)via_register;
  const u32 mode = r[GMOR] & GMOR_MODE;
  if (!mode_is_line(mode)) {
    // A slope register outside a line mode only sets up the engine.
    return;
  }
  if (mode != MODE_OPAQUE_LINE && mode != MODE_TRANSPARENT_LINE) {
    char buf[64];
    snprintf(buf, sizeof(buf), "line mode %02x (drawn as a 2D line)", mode);
    unimplemented(buf);
  }
  if (e.addr_new)
    e.cur_addr = r[GADR];
  if (e.bres_new) {
    e.cur_err = int32_t(r[GB3R]) >> 15;
    e.cur_len = r[GB3R] & 0xf;
  }
  const int32_t ainc1 = int16_t(r[GB1R] >> 16);
  const int32_t einc1 = int32_t(r[GB1R] & 0xffff);
  const int32_t ainc2 = int16_t(r[GB2R] >> 16);
  const int32_t einc2 = int32_t(r[GB2R] & 0xffff);
  const unsigned n = e.cur_len ? e.cur_len : 16;
  const unsigned width = dst_width();
  const u32 dby = dst_byte_offset();
  const bool transparent = line_is_transparent(mode);
  for (unsigned i = 0; i < n; i++) {
    const bool on = (mask >> i) & 1;
    const u32 pa = (width == 4 ? e.cur_addr & ~3u : e.cur_addr) + dby;
    if (on || !transparent)
      pixel_write(pa, width, lane_value(on ? r[GFGR] : r[GBGR], pa, width),
                  true);
    if (e.cur_err < 0) {
      e.cur_addr += u32(ainc1);
      e.cur_err += einc1;
    } else {
      e.cur_addr += u32(ainc2);
      e.cur_err -= einc2;
    }
  }
  e.cur_len = 0;
  e.bres_new = false;
  op_done();
}
