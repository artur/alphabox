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
 * Mach64 display: the extended mode, the hardware cursor, the 8-bit DAC.
 *
 * With CRTC_GEN_CNTL's EXT_DISP_EN and CRTC_EN both set the Mach64's own
 * CRTC drives the screen: the size comes from CRTC_H_TOTAL_DISP and
 * CRTC_V_TOTAL_DISP, the framebuffer start and pitch from CRTC_OFF_PITCH,
 * the pixel format from CRTC_GEN_CNTL, all independent of the VGA
 * registers the BIOS left behind. Otherwise the card is a VGA and CVGA
 * draws it.
 *
 * The hardware cursor is 64x64, two bits a pixel from a 1 KB bitmap in
 * VRAM: 0 and 1 are the two cursor colours, 2 transparent, 3 inverts the
 * pixel under it. CUR_HORZ_VERT_OFF says where in the bitmap the screen
 * edge falls.
 **/

#include "Mach64.hpp"
#include "gui/gui.hpp"

using namespace mach64;

bool CMach64::native_crtc_active() const {
  return (r.crtc_gen_cntl & (CRTC_EXT_DISP_EN | CRTC_EN)) ==
         (CRTC_EXT_DISP_EN | CRTC_EN);
}

/// The extended CRTC's blanking is drawn as black, not gated, so the
/// screen goes dark rather than stale.
bool CMach64::display_enabled() const { return true; }

unsigned CMach64::native_width() const {
  return (((r.crtc_h_total_disp >> 16) & 0xff) + 1) * 8;
}

unsigned CMach64::native_height() const {
  return ((r.crtc_v_total_disp >> 16) & 0x7ff) + 1;
}

u8 CMach64::native_bpp_code() const {
  return u8((r.crtc_gen_cntl & CRTC_PIX_WIDTH_MASK) >> CRTC_PIX_WIDTH_SHIFT);
}

/// CRTC_OFF_PITCH's pitch is in units of eight pixels.
unsigned CMach64::native_pitch_bytes() const {
  const unsigned px = ((r.crtc_off_pitch >> 22) & 0x3ff) * 8;
  switch (native_bpp_code()) {
  case BPP_4:
    return px / 2;
  case BPP_8:
    return px;
  case BPP_15:
  case BPP_16:
    return px * 2;
  case BPP_24:
    return px * 3;
  case BPP_32:
    return px * 4;
  default:
    return px / 8;
  }
}

void CMach64::determine_screen_dimensions(unsigned *height, unsigned *width) {
  if (!native_crtc_active()) {
    CVGACard::determine_screen_dimensions(height, width);
    return;
  }
  unsigned w = native_width();
  unsigned h = native_height();
  if (w > 2048)
    w = 2048;
  if (h > 2048)
    h = 2048;
  *width = w;
  *height = h;
}

/**
 * The palette. With DAC_8BIT_EN the DAC takes its colours at eight bits;
 * otherwise it is a VGA's six-bit one.
 **/
void CMach64::palette_update() {
  if (!(r.dac_cntl & DAC_8BIT_EN)) {
    CVGACard::palette_update();
    return;
  }
  for (int i = 0; i < 256; i++) {
    const u8 *c = &vga.dac.color[3 * (i & vga.dac.mask)];
    set_pen_color(i, c[0], c[1], c[2]);
    bx_gui->palette_change((unsigned)i, c[0], c[1], c[2]);
  }
}

/**
 * What the extended mode reads: the frame from its start for as many lines
 * as are shown, and the cursor image, which lives in off-screen VRAM and
 * changes shape without any register moving. In a VGA mode the whole of
 * VRAM, as the base does.
 **/
uint64_t CMach64::direct_view_hash() const {
  if (!native_crtc_active())
    return CVGACard::direct_view_hash();
  const u32 start = (r.crtc_off_pitch & 0xfffff) * 8;
  const int dbl = (r.crtc_gen_cntl & CRTC_DBL_SCAN_EN) ? 1 : 0;
  const u32 lines = (native_height() >> dbl) + 1;
  uint64_t h = hash_vram(start, native_pitch_bytes() * lines);
  if (r.gen_test_cntl & GEN_CUR_EN)
    h = hash_vram((r.cur_offset & 0xfffff) << 3, 1024, h);
  if (overlay_active()) { // where it is, how it is scaled, and its source
    for (u32 i : {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x08u, 0x09u,
                  0x0au, 0x0du, 0x0fu, 0x12u})
      h = h * 1000003u ^ r.block1[i];
    const u32 lines = r.block1[0x0a] & 0x7ff;
    const u32 pitch = (r.block1[0x0f] & 0xfff) * 4; // up to 4 bytes a pixel
    h = hash_vram(r.block1[0x0d] & 0xffffff, pitch * lines, h);
    const u32 fmt = (r.block1[0x12] >> 16) & 0xf;
    if (fmt == 9 || fmt == 10) { // the chroma planes of planar YUV
      const u32 c = (r.block1[0x0f] & 0xfff) * lines / (fmt == 9 ? 16 : 4);
      h = hash_vram(r.block1[0x75] & 0xffffff, c, h);
      h = hash_vram(r.block1[0x76] & 0xffffff, c, h);
    }
  }
  return h;
}

uint64_t CMach64::hw_cursor_signature() const {
  if (!(r.gen_test_cntl & GEN_CUR_EN))
    return 0;
  uint64_t sig = 1;
  sig = sig * 1000003u ^ r.cur_offset;
  sig = sig * 1000003u ^ r.cur_horz_vert_posn;
  sig = sig * 1000003u ^ r.cur_horz_vert_off;
  sig = sig * 1000003u ^ r.cur_clr0;
  sig = sig * 1000003u ^ r.cur_clr1;
  return sig;
}

static inline u32 argb(u32 rr, u32 g, u32 b) {
  return 0xff000000u | ((rr & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
}

static inline u32 expand5(u32 v) {
  v &= 0x1f;
  return (v << 3) | (v >> 2);
}
static inline u32 expand6(u32 v) {
  v &= 0x3f;
  return (v << 2) | (v >> 4);
}

/**
 * One frame of the extended mode. Double scan repeats every line.
 **/
void CMach64::render_native(bitmap_rgb32 &bitmap) {
  const int width = bitmap.width();
  const int height = bitmap.height();
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;

  if (r.crtc_gen_cntl & (CRTC_DISPLAY_DIS | CRTC_HSYNC_DIS | CRTC_VSYNC_DIS)) {
    bitmap.fill(black_pen(), bitmap.cliprect());
    return;
  }

  const u32 start = (r.crtc_off_pitch & 0xfffff) * 8;
  const u32 pitch = native_pitch_bytes();
  const int dbl = (r.crtc_gen_cntl & CRTC_DBL_SCAN_EN) ? 1 : 0;
  const u8 bpp = native_bpp_code();

  for (int y = 0; y < height; y++) {
    uint32_t *line = &bitmap.pix(y);
    const u32 row = start + u32(y >> dbl) * pitch;
    switch (bpp) {
    case BPP_4:
      for (int x = 0; x < width; x += 2) {
        const u8 d = vram[(row + (x >> 1)) & mask];
        line[x] = pen(d & 0x0f);
        if (x + 1 < width)
          line[x + 1] = pen(d >> 4);
      }
      break;
    case BPP_8:
      for (int x = 0; x < width; x++)
        line[x] = pen(vram[(row + x) & mask]);
      break;
    case BPP_15:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 2) & mask;
        const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
        line[x] = argb(expand5(p >> 10), expand5(p >> 5), expand5(p));
      }
      break;
    case BPP_16:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 2) & mask;
        const u32 p = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
        line[x] = argb(expand5(p >> 11), expand6(p >> 5), expand5(p));
      }
      break;
    case BPP_24:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 3) & mask;
        line[x] = argb(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a]);
      }
      break;
    case BPP_32:
      for (int x = 0; x < width; x++) {
        const u32 a = (row + x * 4) & mask;
        line[x] = argb(vram[(a + 2) & mask], vram[(a + 1) & mask], vram[a]);
      }
      break;
    default: // 1 bpp: two pens
      for (int x = 0; x < width; x++) {
        const u8 d = vram[(row + (x >> 3)) & mask];
        line[x] = pen((d >> (7 - (x & 7))) & 1);
      }
      break;
    }
  }
}

void CMach64::draw_hw_cursor(bitmap_rgb32 &bitmap) {
  if (!(r.gen_test_cntl & GEN_CUR_EN))
    return;
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;
  const u32 col0 = argb(r.cur_clr0 >> 24, r.cur_clr0 >> 16, r.cur_clr0 >> 8);
  const u32 col1 = argb(r.cur_clr1 >> 24, r.cur_clr1 >> 16, r.cur_clr1 >> 8);
  const int x0 = int(r.cur_horz_vert_posn & 0x7ff);
  const int y0 = int((r.cur_horz_vert_posn >> 16) & 0x7ff);
  const int xoff = int(r.cur_horz_vert_off & 0x3f);
  const int yoff = int((r.cur_horz_vert_off >> 16) & 0x3f);
  u32 addr = ((r.cur_offset & 0xfffff) << 3) + u32(yoff) * 16;

  for (int row = yoff; row < 64; row++, addr += 16) {
    const int y = y0 + (row - yoff);
    if (y >= bitmap.height())
      break;
    uint32_t *line = &bitmap.pix(y);
    for (int col = xoff; col < 64; col++) {
      const int x = x0 + (col - xoff);
      if (x >= bitmap.width())
        break;
      const u32 a = (addr + (col >> 3) * 2) & mask;
      const u32 word = vram[a] | (u32(vram[(a + 1) & mask]) << 8);
      switch ((word >> ((col & 7) * 2)) & 3) {
      case 0:
        line[x] = col0;
        break;
      case 1:
        line[x] = col1;
        break;
      case 3:
        line[x] ^= 0x00ffffff;
        break;
      }
    }
  }
}

/**
 * The video overlay (block 1): a window of the display, OVERLAY_Y_X_START
 * to OVERLAY_Y_X_END inclusive (X in bits 26..16, Y in 10..0), filled from
 * SCALER_BUF0 -- SCALER_HEIGHT_WIDTH source pixels at SCALER_BUF_PITCH,
 * in VIDEO_FORMAT's SCALER_IN -- stepped by OVERLAY_SCALE_INC, wherever
 * the keyers of OVERLAY_KEY_CNTL say video rather than graphics. As
 * atidrab programs it: OVERLAY_SCALE_CNTL bits 31 and 30 on while shown,
 * bit 30 cleared to hide; horizontal step 4.12 in bits 15..0, vertical
 * 2.14 in bits 31..16 (0x800 and 0x2000 for twice the size). Source
 * pixels are replicated, not filtered: the driver leaves the filter bits
 * off. SCALER_IN: 3, 4, 6 RGB 1555, 565, 8888; 9, 10 planar YVU9, YVU12;
 * 11, 12 packed 4:2:2 (YUY2, UYVY). YUV becomes RGB by the equations of
 * ATI's programming guide (8.4.5).
 **/
bool CMach64::overlay_active() const {
  return (r.block1[0x09] & 0xc0000000u) == 0xc0000000u;
}

static inline u32 clamp_byte(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline u32 yuv_to_rgb(int y, int u, int v) {
  const int rr = 9 * y / 8 + 25 * v / 16 - 218;
  const int g = 9 * y / 8 - 13 * v / 16 - 25 * u / 64 + 136;
  const int b = 9 * y / 8 + 2 * u - 274;
  return argb(clamp_byte(rr), clamp_byte(g), clamp_byte(b));
}

void CMach64::draw_overlay(bitmap_rgb32 &bitmap) {
  if (!overlay_active())
    return;
  const u32 *b1 = r.block1;
  const int x0 = int(b1[0x00] >> 16) & 0x7ff, y0 = int(b1[0x00]) & 0x7ff;
  const int x1 = int(b1[0x01] >> 16) & 0x7ff, y1 = int(b1[0x01]) & 0x7ff;
  const u32 hinc = b1[0x08] & 0xffff, vinc = b1[0x08] >> 16;
  const int src_w = int(b1[0x0a] >> 16) & 0x7ff, src_h = int(b1[0x0a]) & 0x7ff;
  const u32 src_pitch = b1[0x0f] & 0xfff; // pixels
  const u32 src = b1[0x0d] & 0xffffff;
  const int fmt = int(b1[0x12] >> 16) & 0xf;
  if (!src_w || !src_h || !hinc || !vinc)
    return;
  // Planar YUV (9: YVU9, chroma a quarter of the size each way; 10: YVU12,
  // half): a Y plane at SCALER_BUF0_OFFSET, U and V planes at
  // SCALER_BUF0_OFFSET_U and _V (1_75, 1_76), their pitch the Y pitch
  // divided likewise.
  const bool planar = fmt == 9 || fmt == 10;
  const int sub = fmt == 9 ? 4 : 2;
  const u32 u_off = b1[0x75] & 0xffffff, v_off = b1[0x76] & 0xffffff;
  const u32 c_pitch = src_pitch / u32(sub);
  const int bpp_src = planar ? 1 : fmt == 6 ? 4 : 2;

  // The keyers: each compares a colour with its key under its mask; 0
  // false, 1 true, 4 not equal, 5 equal. Graphics is the frame the CRTC
  // shows, video the overlay's own pixel; bit 8 ANDs them, else ORs.
  const u32 key = b1[0x06];
  const u32 vfn = key & 7, gfn = (key >> 4) & 7;
  const bool and_mix = (key >> 8) & 1;
  auto keyer = [](u32 fn, u32 c, u32 clr, u32 msk) {
    switch (fn) {
    case 1:
      return true;
    case 4:
      return (c & msk) != (clr & msk);
    case 5:
      return (c & msk) == (clr & msk);
    }
    return false;
  };
  const u32 mask = vram_mask();
  const u8 *vram = vga.memory;
  const u32 start = (r.crtc_off_pitch & 0xfffff) * 8;
  const u32 pitch = native_pitch_bytes();
  const int gbytes = native_bpp_code() == BPP_32   ? 4
                     : native_bpp_code() == BPP_24 ? 3
                     : native_bpp_code() >= BPP_15 ? 2
                                                   : 1;

  for (int y = std::max(0, y0); y <= y1 && y < bitmap.height(); y++) {
    const int sy = std::min(src_h - 1, int((u64(y - y0) * vinc) >> 14));
    const u32 srow = src + u32(sy) * src_pitch * bpp_src;
    uint32_t *line = &bitmap.pix(y);
    for (int x = std::max(0, x0); x <= x1 && x < bitmap.width(); x++) {
      const int sx = std::min(src_w - 1, int((u64(x - x0) * hinc) >> 12));
      u32 vid, raw;
      if (planar) {
        const int yv = vram[(srow + u32(sx)) & mask];
        const u32 c = u32(sy / sub) * c_pitch + u32(sx / sub);
        const int u = vram[(u_off + c) & mask];
        const int v = vram[(v_off + c) & mask];
        raw = (u32(yv) << 16) | (u32(u) << 8) | u32(v);
        vid = yuv_to_rgb(yv, u, v);
      } else if (fmt == 11 || fmt == 12) { // packed 4:2:2, a pixel pair a dword
        const u32 a = srow + u32(sx & ~1) * 2;
        const u32 d = vram[a & mask] | (u32(vram[(a + 1) & mask]) << 8) |
                      (u32(vram[(a + 2) & mask]) << 16) |
                      (u32(vram[(a + 3) & mask]) << 24);
        // 11: Y0 U Y1 V from the lowest byte (YUY2); 12: U Y0 V Y1 (UYVY)
        const int yv = fmt == 11 ? int((d >> ((sx & 1) ? 16 : 0)) & 0xff)
                                 : int((d >> ((sx & 1) ? 24 : 8)) & 0xff);
        const int u = fmt == 11 ? int((d >> 8) & 0xff) : int(d & 0xff);
        const int v = fmt == 11 ? int(d >> 24) : int((d >> 16) & 0xff);
        raw = (u32(yv) << 16) | (u32(u) << 8) | u32(v);
        vid = yuv_to_rgb(yv, u, v);
      } else {
        const u32 a = srow + u32(sx) * bpp_src;
        const u32 p = vram[a & mask] | (u32(vram[(a + 1) & mask]) << 8) |
                      (bpp_src == 4 ? (u32(vram[(a + 2) & mask]) << 16) |
                                          (u32(vram[(a + 3) & mask]) << 24)
                                    : 0);
        raw = p;
        if (fmt == 6)
          vid = argb((p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
        else if (fmt == 3)
          vid = argb(expand5(p >> 10), expand5(p >> 5), expand5(p));
        else
          vid = argb(expand5(p >> 11), expand6(p >> 5), expand5(p));
      }
      const u32 ga = start + u32(y) * pitch + u32(x) * gbytes;
      u32 graphics = 0;
      for (int i = 0; i < gbytes; i++)
        graphics |= u32(vram[(ga + i) & mask]) << (8 * i);
      const bool kv = keyer(vfn, raw, b1[0x02], b1[0x03]);
      const bool kg = keyer(gfn, graphics, b1[0x04], b1[0x05]);
      if (and_mix ? (kv && kg) : (kv || kg))
        line[x] = vid;
    }
  }
}

uint32_t CMach64::screen_update(bitmap_rgb32 &bitmap,
                                const rectangle &cliprect) {
  if (native_crtc_active()) {
    render_native(bitmap);
    draw_overlay(bitmap);
    draw_hw_cursor(bitmap);
    return 0;
  }
  CVGA::screen_update(bitmap, cliprect);
  return 0;
}
