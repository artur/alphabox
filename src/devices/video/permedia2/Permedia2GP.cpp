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
 * The Permedia 2's graphics processor: its input (the register file, the
 * FIFO port and the input DMA, with their hold, increment and indexed tag
 * formats), its output FIFO, the rasterizer and the per-fragment units a
 * 2D driver uses -- bit masks, scissors, area stipple, colour DDA, colour
 * format, framebuffer read and write, logic ops, writemasks, block fills,
 * image upload and download (Programmer's Reference chapters 2, 4 and 7).
 *
 * Everything runs inside the write that starts it: a DMA buffer is read
 * and executed when DMACount is written, so the count reads back zero; a
 * primitive runs to its end when its Render command arrives, or, when it
 * waits for host data or bit masks, one fragment per word as they come.
 **/

#include "Permedia2.hpp"
#include "System.hpp"

#include <vector>

using namespace permedia2;

static inline s32 sext(u32 v, int bits) {
  return s32(v << (32 - bits)) >> (32 - bits);
}

/// Once per feature: the first time a driver asks for something this model
/// does not do, say so.
void CPermedia2::gp_unimplemented(const char *what) {
  if (!m_unimplemented_seen.insert(what).second)
    return;
  printf("%s: graphics processor: %s is not implemented\n", devid_string, what);
}

void CPermedia2::gp_reset() {
  memset(&r.g, 0, sizeof(r.g));
  r.g.want_tag = true;
}

// --- input ---------------------------------------------------------------

/**
 * The FIFO port's words (2.3.4.1). A tag description is the count or mask
 * in bits 31..16, the mode in bits 15..14 and the tag in bits 9..0 -- ten
 * bits, not the nine the manual says: the delta unit's tags run to 0x265,
 * and perm2's Direct3D sends its vertices there; hold
 * sends every following word to the tag, increment to successive tags,
 * indexed to the tags of the group whose bits the mask sets, in order.
 **/
void CPermedia2::gp_fifo_word(u32 w) {
  auto &g = r.g;
  if (g.want_tag) {
    g.in_mode = u8((w >> 14) & 3);
    if (g.in_mode == 2) {
      g.in_tag = w & 0x3f0;
      g.in_mask = w >> 16;
      g.want_tag = g.in_mask == 0;
    } else {
      if (g.in_mode == 3)
        gp_unimplemented("tag mode 3");
      g.in_tag = w & 0x3ff;
      g.in_left = (w >> 16) + 1;
      g.want_tag = false;
    }
    return;
  }
  if (g.in_mode == 2) {
    int b = 0;
    while (!(g.in_mask & (1u << b)))
      b++;
    g.in_mask &= ~(1u << b);
    if (g.in_mask == 0)
      g.want_tag = true;
    gp_write(g.in_tag | u32(b), w);
    return;
  }
  const u32 tag = g.in_tag;
  if (g.in_mode == 1)
    g.in_tag++;
  if (--g.in_left == 0)
    g.want_tag = true;
  gp_write(tag, w);
}

/**
 * The input DMA: `count` words from DMAAddress (a bus address) into the
 * FIFO port. It completes here, raising the DMA interrupt flag as the
 * count reaches zero.
 **/
void CPermedia2::gp_dma(u32 count) {
  r.ctl[DMA_COUNT >> 2] = 0;
  if (!count)
    return;
  const u32 addr = r.ctl[DMA_ADDRESS >> 2];
  std::vector<u32> buf(count);
  do_pci_read(addr, buf.data(), 4, count);
  if (m_trace_gp)
    printf("%s: dma %u words from %08x\n", devid_string, count, addr);
  for (u32 w : buf)
    gp_fifo_word(w);
  std::lock_guard<std::mutex> lock(m_int_lock);
  r.ctl[INT_FLAGS >> 2] |= INT_DMA;
  update_int_line();
}

void CPermedia2::gp_output(u32 w) {
  auto &g = r.g;
  if (g.out_count == sizeof(g.out) / sizeof(g.out[0])) {
    gp_unimplemented("an output FIFO the host does not drain");
    return;
  }
  g.out[(g.out_head + g.out_count) % 256] = w;
  g.out_count++;
}

u32 CPermedia2::gp_output_read() {
  auto &g = r.g;
  if (!g.out_count)
    return 0;
  const u32 w = g.out[g.out_head];
  g.out_head = (g.out_head + 1) % 256;
  g.out_count--;
  return w;
}

u32 CPermedia2::gp_read(u32 tag) {
  if (tag >= GP_TAGS)
    return 0;
  return G(tag);
}

/**
 * A register loaded from the input FIFO, and what it sets off.
 **/
void CPermedia2::gp_write(u32 tag, u32 data) {
  if (tag >= GP_TAGS)
    return;
  if (m_trace_gp)
    printf("%s: gp %03x = %08x\n", devid_string, tag, data);
  auto &g = r.g;
  G(tag) = data;
  switch (tag) {
  case T_RENDER:
    raster_begin(data);
    return;
  case T_CONTINUE_NEW_LINE:
  case T_CONTINUE_NEW_DOM:
  case T_CONTINUE_NEW_SUB:
  case T_CONTINUE:
    raster_continue(int(tag), data & 0xfff);
    return;
  case T_BIT_MASK_PATTERN: {
    // Byte swapped as RasterizerMode's BitMaskByteSwapMode says.
    u32 m = data;
    switch ((G(T_RASTERIZER_MODE) >> 7) & 3) {
    case 1:
      m = ((m & 0x00ff00ff) << 8) | ((m >> 8) & 0x00ff00ff);
      break;
    case 2:
      m = (m << 16) | (m >> 16);
      break;
    case 3:
      m = (m << 24) | ((m & 0xff00) << 8) | ((m >> 8) & 0xff00) | (m >> 24);
      break;
    }
    g.mask = m;
    g.mask_pos = s32((G(T_RASTERIZER_MODE) >> 10) & 31);
    g.mask_bits = 32 - g.mask_pos;
    if (g.active)
      raster_run();
    return;
  }
  case T_COLOR:
  case T_FB_DATA:
  case T_FB_SOURCE_DATA:
  case T_TEXEL0:
  case T_DEPTH:
  case T_STENCIL:
    if (g.active && (g.render & RENDER_SYNC_ON_HOST_DATA)) {
      u32 d = data;
      switch ((G(T_RASTERIZER_MODE) >> 15) & 3) { // HostDataByteSwapMode
      case 1:
        d = ((d & 0x00ff00ff) << 8) | ((d >> 8) & 0x00ff00ff);
        break;
      case 2:
        d = (d << 16) | (d >> 16);
        break;
      case 3:
        d = (d << 24) | ((d & 0xff00) << 8) | ((d >> 8) & 0xff00) | (d >> 24);
        break;
      }
      g.have_host = true;
      g.host_tag = tag;
      g.host_data = d;
      raster_run();
    }
    return;
  case T_TEXEL_LUT_INDEX:
    g.lut_index = data & 0xff;
    return;
  case T_TEXEL_LUT_DATA:
    g.lut[g.lut_index & 0xff] = data;
    g.lut_index = (g.lut_index + 1) & 0xff;
    return;
  case T_TEXEL_LUT_TRANSFER:
    gp_unimplemented("texel LUT loads from memory");
    return;
  case T_FB_READ_MODE:
    G(T_FB_WRITE_CONFIG) = data;
    g.relative_offset = sext(data >> FBRM_RELATIVE_OFFSET_SHIFT, 3);
    return;
  case T_PACKED_DATA_LIMITS:
    g.relative_offset = sext(data >> 29, 3);
    return;
  case T_FB_WINDOW_BASE:
    G(T_FB_SOURCE_BASE) = data;
    return;
  case T_FB_SOURCE_DELTA: {
    const s32 dx = sext(data, 12), dy = sext(data >> 16, 12);
    const s32 w = s32(fb_width(G(T_FB_READ_MODE)));
    const bool up = (G(T_FB_READ_MODE) & FBRM_BOTTOM_LEFT) != 0;
    G(T_FB_SOURCE_OFFSET) = u32((up ? -dy : dy) * w + dx) & 0xffffff;
    return;
  }
  case T_FB_BLOCK_COLOR:
    G(T_FB_BLOCK_COLOR_U) = G(T_FB_BLOCK_COLOR_L) = data;
    return;
  case T_CONFIG: {
    // The fields of five registers in one.
    const u32 rd =
        ((data & CONFIG_READ_SOURCE) ? FBRM_READ_SOURCE : 0) |
        ((data & CONFIG_READ_DESTINATION) ? FBRM_READ_DESTINATION : 0) |
        ((data & CONFIG_PACKED_DATA) ? FBRM_PACKED_DATA : 0);
    const u32 rmask =
        FBRM_READ_SOURCE | FBRM_READ_DESTINATION | FBRM_PACKED_DATA;
    G(T_FB_READ_MODE) = (G(T_FB_READ_MODE) & ~rmask) | rd;
    G(T_FB_WRITE_CONFIG) = (G(T_FB_WRITE_CONFIG) & ~rmask) | rd;
    G(T_FB_WRITE_MODE) =
        (G(T_FB_WRITE_MODE) & ~1u) | ((data & CONFIG_FB_WRITE) ? 1 : 0);
    G(T_COLOR_DDA_MODE) =
        (G(T_COLOR_DDA_MODE) & ~1u) | ((data & CONFIG_COLOR_DDA) ? 1 : 0);
    G(T_LOGICAL_OP_MODE) = (G(T_LOGICAL_OP_MODE) & ~0x1fu) |
                           ((data & CONFIG_LOGIC_OP_ENABLE) ? 1 : 0) |
                           (((data >> CONFIG_LOGIC_OP_SHIFT) & 15) << 1);
    return;
  }
  case T_SYNC:
    // Everything before it is done; the host out unit passes its tag
    // and data as FilterMode lets it, and an interrupt when bit 31 asks.
    if (G(T_FILTER_MODE) & (1u << 10))
      gp_output(T_SYNC);
    if (G(T_FILTER_MODE) & (1u << 11))
      gp_output(data);
    if ((data & 0x80000000u) && (G(T_FILTER_MODE) & (3u << 10))) {
      std::lock_guard<std::mutex> lock(m_int_lock);
      r.ctl[INT_FLAGS >> 2] |= INT_SYNC;
      update_int_line();
    }
    return;
  case T_RESET_PICK_RESULT:
    g.pick = false;
    return;
  case T_PICK_RESULT:
  case T_MIN_HIT_REGION:
  case T_MAX_HIT_REGION: {
    const u32 v = tag == T_PICK_RESULT      ? (g.pick ? 1 : 0)
                  : tag == T_MIN_HIT_REGION ? G(T_MIN_REGION)
                                            : G(T_MAX_REGION);
    if (G(T_FILTER_MODE) & (1u << 12))
      gp_output(tag);
    if (G(T_FILTER_MODE) & (1u << 13))
      gp_output(v);
    return;
  }
  case T_SUSPEND_UNTIL_FRAME_BLANK:
    // Flip at the next frame blank: this model has no frame in flight.
    r.ctl[SCREEN_BASE >> 2] = data & 0xfffff;
    state.vga_mem_updated = 1;
    return;
  case T_TEXTURE_DATA: {
    // Fast texture download: raw 32-bit words at successive addresses.
    const u32 off = G(T_TEXTURE_DOWNLOAD_OFFSET);
    vram_write((off * 4) & vram_mask(), 4, data);
    G(T_TEXTURE_DOWNLOAD_OFFSET) = off + 1;
    G(T_TEXTURE_DATA) = data;
    state.vga_mem_updated = 1;
    return;
  }
  default:
    if (tag >= T_TEXEL_LUT0 && tag < T_TEXEL_LUT0 + 16)
      g.lut[tag - T_TEXEL_LUT0] = data;
    else if (tag >= 0x200 && tag < 0x260)
      delta_vertex(tag, data);
    break;
  case T_DRAW_TRIANGLE:
    delta_triangle(data);
    return;
  case T_REPEAT_TRIANGLE:
    delta_triangle(g.last_draw);
    return;
  case T_DRAW_LINE01:
    delta_line(data, 0, 1);
    return;
  case T_DRAW_LINE10:
    delta_line(data, 1, 0);
    return;
  case T_REPEAT_LINE:
    delta_line(g.last_draw, 0, 1);
    return;
  }
}

// --- the rasterizer ------------------------------------------------------

/// RasterizerMode's BiasCoordinates: what is added to the start values.
static s32 coordinate_bias(u32 mode) {
  switch ((mode >> 4) & 3) {
  case 1:
    return 0x8000;
  case 2:
    return 0x7fff;
  }
  return 0;
}

void CPermedia2::raster_begin(u32 render) {
  auto &g = r.g;
  if (g.active)
    g.active = false; // a primitive left waiting for data is abandoned
  g.render = render;
  g.prim = int((render >> RENDER_PRIMITIVE_SHIFT) & 3);
  const s32 bias = coordinate_bias(G(T_RASTERIZER_MODE));
  g.xdom = s64(s32(G(T_START_X_DOM)) + bias) << 16;
  g.xsub = s64(s32(G(T_START_X_SUB)) + bias) << 16;
  g.dxdom = s64(s32(G(T_DX_DOM))) << 16;
  g.dxsub = s64(s32(G(T_DX_SUB))) << 16;
  g.y = s64(s32(G(T_START_Y)) + bias) << 16;
  g.dy = s64(s32(G(T_DY))) << 16;
  if (g.exact) {
    g.xdom = g.exact_xdom;
    g.dxdom = g.exact_dxdom;
    g.xsub = g.exact_xsub;
    g.dxsub = g.exact_dxsub;
  }
  g.count = s32(G(T_COUNT) & 0xfff);
  g.in_span = false;
  g.have_host = false;
  if (!(render & RENDER_REUSE_BIT_MASK))
    g.mask_bits = 0;
  switch (g.prim) {
  case PRIM_POINT:
    g.count = 1;
    break;
  case PRIM_RECTANGLE: {
    // Always the rectangle from the origin; the Increase bits only choose
    // the order its pixels are visited in (a copy must not overrun its
    // own source).
    const s32 ox = sext(G(T_RECTANGLE_ORIGIN), 12);
    const s32 oy = sext(G(T_RECTANGLE_ORIGIN) >> 16, 12);
    const s32 w = s32(G(T_RECTANGLE_SIZE) & 0xfff);
    const s32 h = s32((G(T_RECTANGLE_SIZE) >> 16) & 0xfff);
    const bool incx = render & RENDER_INCREASE_X;
    const bool incy = render & RENDER_INCREASE_Y;
    g.xdom = incx ? ox : ox + w - 1; // integers here, not 16.16
    g.xsub = incx ? ox + w : ox - 1;
    g.y = incy ? oy : oy + h - 1;
    g.dy = incy ? 1 : -1;
    g.count = w > 0 ? h : 0;
    break;
  }
  }
  // The colour DDA starts on the dominant edge.
  g.er = s32(G(T_R_START));
  g.eg = s32(G(T_G_START));
  g.eb = s32(G(T_B_START));
  g.ea = s32(G(T_A_START));
  g.ez = (s64(s32(G(T_Z_START_U))) << 11) | (G(T_Z_START_L) >> 21);
  g.es = s32(G(T_S_START));
  g.et = s32(G(T_T_START));
  g.eq = s32(G(T_Q_START));
  g.ef = s32(G(T_F_START));
  g.tex_plane = g.tex_plane_next;
  g.tex_plane_next = false;
  g.active = true;
  raster_run();
}

/**
 * The continue commands (4.4.14): the scan carries on from where the last
 * primitive left it with `count` more scanlines (or line pixels), after
 * reloading the dominant edge, the subordinate edge, neither, or -- for a
 * polyline -- the line's deltas with its fractions adjusted.
 **/
void CPermedia2::raster_continue(int kind, u32 count) {
  auto &g = r.g;
  const s32 bias = coordinate_bias(G(T_RASTERIZER_MODE));
  switch (kind) {
  case T_CONTINUE_NEW_DOM:
    g.xdom = s64(s32(G(T_START_X_DOM)) + bias) << 16;
    g.dxdom = s64(s32(G(T_DX_DOM))) << 16;
    break;
  case T_CONTINUE_NEW_SUB:
    g.xsub = s64(s32(G(T_START_X_SUB)) + bias) << 16;
    g.dxsub = s64(s32(G(T_DX_SUB))) << 16;
    if (g.exact) {
      g.xsub = g.exact_xsub;
      g.dxsub = g.exact_dxsub;
    }
    break;
  case T_CONTINUE_NEW_LINE: {
    g.dxdom = s64(s32(G(T_DX_DOM))) << 16;
    g.dy = s64(s32(G(T_DY))) << 16;
    static const s64 frac[4] = {-1, 0, 0x80000000ll, 0x7fff0000ll};
    const s64 f = frac[(G(T_RASTERIZER_MODE) >> 2) & 3];
    if (f >= 0) {
      g.xdom = (g.xdom & ~0xffffffffll) | f;
      g.y = (g.y & ~0xffffffffll) | f;
    }
    break;
  }
  default:
    g.dxdom = s64(s32(G(T_DX_DOM))) << 16;
    g.dxsub = s64(s32(G(T_DX_SUB))) << 16;
    g.dy = s64(s32(G(T_DY))) << 16;
    break;
  }
  g.count = s32(count);
  g.in_span = false;
  g.active = true;
  raster_run();
}

/**
 * The next span: a trapezoid's scanline from its dominant edge towards the
 * subordinate one, the right-hand pixel left out; a line's or a point's
 * single pixel; a rectangle's row. False when the primitive is done.
 **/
bool CPermedia2::raster_next_span() {
  auto &g = r.g;
  if (g.count <= 0)
    return false;
  const u32 rm = G(T_RASTERIZER_MODE);
  const bool limits =
      !(rm & (1u << 18)) &&
      !(g.render & (RENDER_SYNC_ON_HOST_DATA | RENDER_SYNC_ON_BIT_MASK));
  switch (g.prim) {
  case PRIM_LINE:
  case PRIM_POINT:
    g.x = s32(g.xdom >> 32);
    g.yi = s32(g.y >> 32);
    g.xend = g.x + 1;
    g.xstep = 1;
    g.xdom += g.dxdom;
    g.y += g.dy;
    break;
  case PRIM_RECTANGLE:
    g.x = s32(g.xdom);
    g.xend = s32(g.xsub);
    g.xstep = g.xsub > g.xdom ? 1 : -1;
    g.yi = s32(g.y);
    g.y += g.dy;
    break;
  default: {
    const s32 a = s32(g.xdom >> 32), b = s32(g.xsub >> 32);
    g.span_xdom = g.xdom;
    g.yi = s32(g.y >> 32);
    if (b >= a) {
      g.x = a;
      g.xend = b;
      g.xstep = 1;
    } else {
      g.x = a - 1;
      g.xend = b - 1;
      g.xstep = -1;
    }
    g.xdom += g.dxdom;
    g.xsub += g.dxsub;
    g.y += g.dy;
    break;
  }
  }
  g.count--;
  if (limits) {
    const s32 ymin = sext(G(T_Y_LIMITS), 16),
              ymax = sext(G(T_Y_LIMITS) >> 16, 16);
    if (g.yi < ymin || g.yi >= ymax)
      g.xend = g.x; // the whole scanline rejected
  }
  g.span_x0 = g.x;
  dda_span_start();
  g.in_span = true;
  return true;
}

/**
 * Run the primitive until it ends or waits for the host: for a bit mask
 * when the current one is used up (SyncOnBitMask), or for the next data
 * word (SyncOnHostData), one fragment per word.
 **/
void CPermedia2::raster_run() {
  auto &g = r.g;
  const u32 rm = G(T_RASTERIZER_MODE);
  const bool bm = (g.render & RENDER_SYNC_ON_BIT_MASK) != 0;
  const bool hd = (g.render & RENDER_SYNC_ON_HOST_DATA) != 0;
  const bool reuse = (g.render & RENDER_REUSE_BIT_MASK) != 0;
  // Bits 9 and 18 are active low: a mask runs on across scanlines, and
  // the X and Y limits apply, unless they are set (the manual's "0 =
  // Enabled"; perm2 packs glyph rows into consecutive mask bits).
  const bool packing = (rm & (1u << 9)) == 0;
  const bool relative = (rm & (1u << 19)) != 0;
  const bool fast = (g.render & RENDER_FAST_FILL) != 0;
  const bool xlimits = !(rm & (1u << 18)) && !bm && !hd;
  const s32 xmin = sext(G(T_X_LIMITS), 16),
            xmax = sext(G(T_X_LIMITS) >> 16, 16);

  while (g.active) {
    if (!g.in_span) {
      if (!raster_next_span()) {
        g.active = false;
        break;
      }
      continue;
    }
    if (g.x == g.xend) {
      g.in_span = false;
      if (bm && !packing && !fast)
        g.mask_bits = 0; // a mask is not carried onto the next scanline
      g.er += s32(G(T_DR_DY_DOM));
      g.eg += s32(G(T_DG_DY_DOM));
      g.eb += s32(G(T_DB_DY_DOM));
      g.ez += (s64(s32(G(T_DZ_DY_DOM_U))) << 11) |
              (G(T_DZ_DY_DOM_L) >> 21);
      g.es += s32(G(T_DS_DY_DOM));
      g.et += s32(G(T_DT_DY_DOM));
      g.eq += s32(G(T_DQ_DY_DOM));
      g.ef += s32(G(T_DF_DY_DOM));
      continue;
    }
    bool pass = true;
    if (bm) {
      if (g.mask_bits <= 0) {
        if (!reuse)
          return; // wait for BitMaskPattern
        g.mask = G(T_BIT_MASK_PATTERN);
        g.mask_pos = s32((rm >> 10) & 31);
        g.mask_bits = 32 - g.mask_pos;
      }
      if (hd && !g.have_host)
        return;
      int bit = relative ? (g.x & 31) : g.mask_pos;
      if (rm & 1) // MirrorBitMask
        bit = 31 - bit;
      pass = ((g.mask >> bit) & 1) != 0;
      if (rm & 2) // InvertBitMask
        pass = !pass;
      if (!relative) {
        g.mask_pos++;
        g.mask_bits--;
      }
    } else if (hd && !g.have_host) {
      return;
    }
    if (!xlimits || (g.x >= xmin && g.x < xmax))
      fragment(g.x, g.yi, pass);
    g.have_host = false;
    g.x += g.xstep;
    dda_step_x();
  }
}

// --- the colour DDA ------------------------------------------------------

/// Colour DDA values: 9.15 fixed point, the integer part clamped to 0..255.
static inline u32 dda_component(s32 v) {
  const s32 i = v >> 15;
  return u32(i < 0 ? 0 : i > 255 ? 255 : i);
}

void CPermedia2::dda_span_start() {
  auto &g = r.g;
  g.cr = g.er;
  g.cg = g.eg;
  g.cb = g.eb;
  g.ca = g.ea;
  g.z = g.ez;
  g.s = g.es;
  g.t = g.et;
  g.q = g.eq;
  g.f = g.ef;
  dda_correct_span();
}

void CPermedia2::dda_step_x() {
  auto &g = r.g;
  const s32 k = g.xstep;
  g.cr += s32(G(T_DR_DX)) * k;
  g.cg += s32(G(T_DG_DX)) * k;
  g.cb += s32(G(T_DB_DX)) * k;
  g.z += ((s64(s32(G(T_DZ_DX_U))) << 11) | (G(T_DZ_DX_L) >> 21)) * k;
  g.s += s64(s32(G(T_DS_DX))) * k;
  g.t += s64(s32(G(T_DT_DX))) * k;
  g.q += s64(s32(G(T_DQ_DX))) * k;
  g.f += s32(G(T_DF_DX)) * k;
}

void CPermedia2::dda_step_y() {}

u32 CPermedia2::fragment_color() {
  auto &g = r.g;
  const u32 mode = G(T_COLOR_DDA_MODE);
  if (!(mode & 2))
    return G(T_CONSTANT_COLOR);
  return dda_component(g.cr) | (dda_component(g.cg) << 8) |
         (dda_component(g.cb) << 16) | (dda_component(g.ca) << 24);
}

// --- per fragment --------------------------------------------------------

bool CPermedia2::scissor_pass(s32 x, s32 y) const {
  const u32 mode = r.gp[T_SCISSOR_MODE];
  if (mode & 1) { // user scissor
    const u32 mn = r.gp[T_SCISSOR_MIN_XY], mx = r.gp[T_SCISSOR_MAX_XY];
    if (x < sext(mn, 12) || y < sext(mn >> 16, 12) || x >= sext(mx, 12) ||
        y >= sext(mx >> 16, 12))
      return false;
  }
  if (mode & 2) { // screen scissor, after the window origin
    const u32 wo = r.gp[T_WINDOW_ORIGIN], ss = r.gp[T_SCREEN_SIZE];
    const s32 sx = x + sext(wo, 12), sy = y + sext(wo >> 16, 12);
    if (sx < 0 || sy < 0 || sx >= s32(ss & 0x7ff) ||
        sy >= s32((ss >> 16) & 0x7ff))
      return false;
  }
  return true;
}

bool CPermedia2::stipple_pass(s32 x, s32 y) const {
  const u32 mode = r.gp[T_AREA_STIPPLE_MODE];
  const int xo = int((mode >> 7) & 7), yo = int((mode >> 12) & 7);
  int col = (x + xo) & 7, row = (y + yo) & 7;
  if (mode & (1u << 18)) // MirrorX
    col = 7 - col;
  if (mode & (1u << 19)) // MirrorY
    row = 7 - row;
  bool pass = (r.gp[T_AREA_STIPPLE_PATTERN0 + row] >> col) & 1;
  if (mode & (1u << 17))
    pass = !pass;
  return pass;
}

/// The framebuffer width a mode's partial products give (Appendix B).
u32 CPermedia2::fb_width(u32 mode) const {
  u32 w = 0;
  for (int i = 0; i < 3; i++) {
    const u32 pp = (mode >> (3 * i)) & 7;
    if (pp)
      w += 16u << pp;
  }
  return w;
}

u32 CPermedia2::fb_pixel_bytes() const {
  switch (r.gp[T_FB_READ_PIXEL] & 7) {
  case 0:
    return 1;
  case 1:
    return 2;
  case 4:
    return 3;
  }
  return 4;
}

/**
 * A fragment's framebuffer address in pixels (3.3.1): the window base, the
 * row by the width the partial products give (upwards from a bottom-left
 * origin), the pixel offset, and for a source read the source offset. The
 * destination is addressed with FBWriteConfig's width, the source with
 * FBReadMode's.
 **/
s64 CPermedia2::fb_address(s32 x, s32 y, u32 mode, bool source) const {
  const s64 w = fb_width(mode);
  const s64 base = source ? r.gp[T_FB_SOURCE_BASE] & 0xffffff
                          : r.gp[T_FB_WINDOW_BASE] & 0xffffff;
  s64 a = (r.gp[T_FB_READ_MODE] & FBRM_BOTTOM_LEFT) ? base - y * w + x
                                                    : base + y * w + x;
  a += sext(r.gp[T_FB_PIXEL_OFFSET], 24);
  if (source)
    a += sext(r.gp[T_FB_SOURCE_OFFSET], 24);
  return a;
}

u32 CPermedia2::fb_read_pixel(s64 pixel_addr) const {
  const u32 n = fb_pixel_bytes();
  return vram_read(u32(pixel_addr * n) & vram_mask(), int(n));
}

void CPermedia2::fb_write_pixel(s64 pixel_addr, u32 value) {
  const u32 n = fb_pixel_bytes();
  const u32 a = u32(pixel_addr * n) & vram_mask();
  const u32 hw = r.gp[T_FB_HARDWARE_WRITE_MASK];
  if (hw != 0xffffffffu) {
    const u32 old = vram_read(a, int(n));
    value = (value & hw) | (old & ~hw);
  }
  vram_write(a, int(n), value);
}

/**
 * The colour format unit (4.14, DitherMode): the internal colour -- red in
 * bits 7..0, green, blue, alpha in 31..24 -- to the framebuffer's format,
 * each component its top bits, repeated to fill the word as the chip
 * does for the front and back halves of a double-buffered pixel.
 **/
u32 CPermedia2::format_color(u32 c) const {
  const u32 dm = r.gp[T_DITHER_MODE];
  if (!(dm & 1))
    return c;
  u32 rr = c & 0xff, gg = (c >> 8) & 0xff, bb = (c >> 16) & 0xff, aa = c >> 24;
  switch ((dm >> 12) & 3) {
  case 1:
    aa = 0;
    break;
  case 2:
    aa = 0xf8;
    break;
  }
  const bool rgb = (dm & (1u << 10)) != 0;
  const u32 fmt = ((dm >> 2) & 15) | (((dm >> 16) & 1) << 4);
  auto pack = [&](int rb, int gb, int bbits, int ab, int rs, int gs, int bs,
                  int as) {
    u32 v = ((rr >> (8 - rb)) << rs) | ((gg >> (8 - gb)) << gs) |
            ((bb >> (8 - bbits)) << bs);
    if (ab)
      v |= (aa >> (8 - ab)) << as;
    return v;
  };
  u32 v;
  switch (fmt) {
  case 0: // 8:8:8:8
    return rgb ? (bb | (gg << 8) | (rr << 16) | (aa << 24))
               : (rr | (gg << 8) | (bb << 16) | (aa << 24));
  case 1: // 5:5:5:1
  case 13:
    v = rgb ? pack(5, 5, 5, 1, 10, 5, 0, 15) : pack(5, 5, 5, 1, 0, 5, 10, 15);
    return v | (v << 16);
  case 2: // 4:4:4:4
    v = rgb ? pack(4, 4, 4, 4, 8, 4, 0, 12) : pack(4, 4, 4, 4, 0, 4, 8, 12);
    return v | (v << 16);
  case 5: // 3:3:2
  case 6:
    v = rgb ? pack(3, 3, 2, 0, 5, 2, 0, 0) : pack(3, 3, 2, 0, 0, 3, 6, 0);
    break;
  case 9: // 2:3:2:1
  case 10:
    v = rgb ? pack(2, 3, 2, 1, 5, 2, 0, 7) : pack(2, 3, 2, 1, 0, 2, 5, 7);
    break;
  case 11: // 2:3:2 offset: a 7-bit value, plus 64
  case 12:
    v = (rgb ? pack(2, 3, 2, 0, 5, 2, 0, 0) : pack(2, 3, 2, 0, 0, 2, 5, 0)) +
        64;
    break;
  case 14: // CI8
    v = rr;
    break;
  case 16: // 5:6:5
  case 17:
    v = rgb ? pack(5, 6, 5, 0, 11, 5, 0, 0) : pack(5, 6, 5, 0, 0, 5, 11, 0);
    return v | (v << 16);
  default:
    return c;
  }
  v &= 0xff;
  return v | (v << 8) | (v << 16) | (v << 24);
}

static inline u32 logic_op(u32 op, u32 s, u32 d) {
  switch (op & 15) {
  case 0:
    return 0;
  case 1:
    return s & d;
  case 2:
    return s & ~d;
  case 3:
    return s;
  case 4:
    return ~s & d;
  case 5:
    return d;
  case 6:
    return s ^ d;
  case 7:
    return s | d;
  case 8:
    return ~(s | d);
  case 9:
    return ~(s ^ d);
  case 10:
    return ~d;
  case 11:
    return s | ~d;
  case 12:
    return ~s;
  case 13:
    return ~s | d;
  case 14:
    return ~(s & d);
  }
  return 0xffffffffu;
}

/**
 * One fragment through the units after the rasterizer. `mask_pass` is the
 * bit mask test's verdict: a failing fragment is dropped, or drawn in
 * Texel0's colour when RasterizerMode forces the background colour.
 **/
void CPermedia2::fragment(s32 x, s32 y, bool mask_pass) {
  auto &g = r.g;
  const u32 render = g.render;
  bool background = false;
  if (!mask_pass) {
    if (!(G(T_RASTERIZER_MODE) & (1u << 6)))
      return;
    background = true;
  }
  if (!scissor_pass(x, y))
    return;
  if ((render & RENDER_AREA_STIPPLE) && (G(T_AREA_STIPPLE_MODE) & 1) &&
      !stipple_pass(x, y)) {
    if (!(G(T_AREA_STIPPLE_MODE) & (1u << 20)))
      return;
    background = true;
  }
  if (render & RENDER_FAST_FILL) {
    // Block fill: the raw block colour, nothing else applies.
    fb_write_pixel(fb_address(x, y, G(T_FB_WRITE_CONFIG), false),
                   G(T_FB_BLOCK_COLOR_L));
    state.vga_mem_updated = 1;
    return;
  }
  const u32 rmode = G(T_FB_READ_MODE);
  if (!(rmode & FBRM_PACKED_DATA)) {
    fragment_pixel(x, y, -1, background);
    return;
  }
  // Packed data (4.11.4): the fragment is a 32-bit word of pixels, each
  // plotted if it lies within PackedDataLimits, in pixels. The manual puts
  // XEnd in bits 11..0 and XStart in 27..16; perm2 loads the pair both
  // ways round -- its packed copies with (start, end) = (2, 18) as
  // 0x00120002, its icon downloads as 0x00020012 -- so the limits are
  // taken as the span between the two.
  const u32 bytes = fb_pixel_bytes();
  if (bytes == 3) {
    gp_unimplemented("packed 24-bit pixels");
    return;
  }
  const int ppw = int(4 / bytes);
  const u32 pdl = G(T_PACKED_DATA_LIMITS);
  const s32 x0 = sext(pdl, 12), x1 = sext(pdl >> 16, 12);
  const s32 start = std::min(x0, x1), end = std::max(x0, x1);
  if ((rmode & FBRM_DATA_TYPE_COLOR) && (G(T_FILTER_MODE) & (3u << 8))) {
    // A packed upload returns whole words.
    u32 word = 0;
    for (int i = 0; i < ppw; i++)
      word |=
          fb_read_pixel(fb_address(x * ppw + i, y, G(T_FB_WRITE_CONFIG), false))
          << (8 * bytes * u32(i));
    if (G(T_FILTER_MODE) & (1u << 8))
      gp_output(T_FB_COLOR);
    if (G(T_FILTER_MODE) & (1u << 9))
      gp_output(word);
  }
  for (int i = 0; i < ppw; i++) {
    const s32 p = x * ppw + i;
    if (p < start || p >= end || !scissor_pass(p, y))
      continue;
    fragment_pixel(p, y, i, background);
  }
}

/**
 * One pixel of a fragment: its source colour, then the logic op, the
 * software and hardware writemasks and the write. `lane` is the pixel's
 * place in a packed fragment's word (-1 when not packed): host data then
 * carries a pixel a lane, and a source read is shifted by the relative
 * offset.
 **/
void CPermedia2::fragment_pixel(s32 p, s32 y, int lane, bool background) {
  auto &g = r.g;
  const u32 rmode = G(T_FB_READ_MODE);
  const u32 bytes = fb_pixel_bytes();
  const s64 dest = fb_address(p, y, G(T_FB_WRITE_CONFIG), false);
  auto pick_lane = [&](u32 v) {
    if (lane < 0)
      return v;
    v >>= 8 * bytes * u32(lane);
    return bytes == 4 ? v : v & ((1u << (8 * bytes)) - 1);
  };

  // The depth and stencil tests, against the localbuffer.
  if (lane < 0 && ((G(T_DEPTH_MODE) & 1) || (G(T_STENCIL_MODE) & 1) ||
                   (G(T_LB_WRITE_MODE) & 1)) &&
      !depth_stencil(p, y))
    return;

  // The source colour, raw framebuffer format.
  u32 s;
  if (background) {
    s = format_color(G(T_TEXEL0));
  } else if (g.have_host && g.host_tag != T_COLOR && g.host_tag != T_TEXEL0) {
    switch (g.host_tag) {
    case T_FB_DATA:
    case T_FB_SOURCE_DATA:
      s = g.host_data;
      break;
    default:
      gp_unimplemented("depth or stencil download");
      return;
    }
  } else if (rmode & FBRM_READ_SOURCE) {
    const s32 shift = lane < 0 ? 0 : g.relative_offset;
    s = fb_read_pixel(fb_address(p - shift, y, rmode, true));
    lane = -1; // already this pixel's
  } else if (G(T_LOGICAL_OP_MODE) & (1u << 5)) {
    s = G(T_FB_WRITE_DATA);
  } else {
    // A colour through the 3D units: the colour DDA's or the host's, then
    // texture, fog, alpha blend, dither, and the colour format.
    u32 c = 0;
    if (g.have_host)
      c = g.host_data;
    else if (G(T_COLOR_DDA_MODE) & 1)
      c = fragment_color();
    if ((g.render & RENDER_TEXTURE) &&
        ((G(T_TEXTURE_READ_MODE) & 1) || (G(T_YUV_MODE) & 7))) {
      u32 t = 0xffffffffu;
      bool applied = true;
      const u32 before = t;
      if (!texture_color(t, p, y))
        return; // the chroma test rejected the fragment
      if (t == before && !(G(T_TEXTURE_READ_MODE) & 1))
        applied = false;
      if (applied && (G(T_TEXTURE_COLOR_MODE) & 1))
        c = apply_texture(c, t);
    }
    if ((g.render & RENDER_FOG) && (G(T_FOG_MODE) & 1))
      c = apply_fog(c);
    if (G(T_ALPHA_BLEND_MODE) & 1)
      c = alpha_blend(c, fb_read_pixel(dest));
    s = format_color(dither(c, p, y));
  }
  s = pick_lane(s);

  // An image upload: the destination's pixels to the host.
  if ((rmode & FBRM_DATA_TYPE_COLOR) && !(rmode & FBRM_PACKED_DATA)) {
    const u32 d = fb_read_pixel(dest);
    if (G(T_FILTER_MODE) & (1u << 8))
      gp_output(T_FB_COLOR);
    if (G(T_FILTER_MODE) & (1u << 9))
      gp_output(d);
  }
  if (!(G(T_FB_WRITE_MODE) & 1))
    return;

  const u32 lop = G(T_LOGICAL_OP_MODE);
  const u32 swm = G(T_FB_SOFTWARE_WRITE_MASK);
  u32 v = s;
  if ((lop & 1) || swm != 0xffffffffu) {
    const u32 d = fb_read_pixel(dest);
    if (lop & 1)
      v = logic_op(lop >> 1, s, d);
    v = (v & swm) | (d & ~swm);
  }
  fb_write_pixel(dest, v);
  state.vga_mem_updated = 1;
}
