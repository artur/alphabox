/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

/**
 * The Permedia 2's self-test (ALPHABOX_VIDEO_SELFTEST, VideoSelfTest.hpp):
 * the rasterizer called with the registers a driver would have loaded.
 **/
#include "StdAfx.hpp"
#include "Permedia2.hpp"
#include "VideoSelfTest.hpp"

#include <vector>

using namespace permedia2;

void CPermedia2::selftest() {
  VideoSelfTest t("PERMEDIA2");
  const auto saved = r;
  const u32 keep = std::min(m_vram_bytes, 256u * 1024);
  std::vector<u8> mem(vga.memory, vga.memory + keep);
  auto &g = r.g;

  // A 1024-pixel-wide frame buffer of 32-bit pixels at 0, and the user
  // scissor x 10..19, y 10..13.
  auto setup = [&] {
    memset(&r.gp, 0, sizeof(r.gp));
    memset(&g, 0, sizeof(g));
    memset(vga.memory, 0, keep);
    r.gp[T_FB_READ_PIXEL] = 2;
    r.gp[T_FB_WRITE_CONFIG] = 6; // one partial product of 1024
    r.gp[T_FB_HARDWARE_WRITE_MASK] = 0xffffffffu;
    r.gp[T_FB_SOFTWARE_WRITE_MASK] = 0xffffffffu;
    r.gp[T_FB_WRITE_MODE] = 1;
    r.gp[T_SCISSOR_MODE] = 1;
    r.gp[T_SCISSOR_MIN_XY] = (10u << 16) | 10;
    r.gp[T_SCISSOR_MAX_XY] = (14u << 16) | 20;
    r.gp[T_DY] = 1u << 16;
  };
  auto px = [&](int x, int y) { return vram_read(u32(y * 1024 + x) * 4, 4); };
  const u32 trapezoid = u32(PRIM_TRAPEZOID) << RENDER_PRIMITIVE_SHIFT;

  // A trapezoid whose edges are two thousand million pixels to either side
  // (the delta unit hands such edges over from vertices that are anywhere):
  // only the scissor's columns are walked. A pixel a step, each of its six
  // rows took minutes.
  for (int dir = 0; dir < 2; dir++) {
    setup();
    r.gp[T_FB_BLOCK_COLOR_L] = 0x11223344u;
    r.gp[T_START_Y] = 9u << 16;
    r.gp[T_COUNT] = 6;
    const s64 far = s64(2000000000) << 32;
    g.exact = true;
    g.exact_xdom = dir ? far : -far;
    g.exact_xsub = dir ? -far : far;
    g.exact_dxdom = g.exact_dxsub = 0;
    t.start();
    raster_begin(trapezoid | RENDER_FAST_FILL);
    g.exact = false;
    const long ms = t.ms();
    bool in = true;
    for (int y = 10; y < 14; y++)
      for (int x = 10; x < 20; x++)
        in = in && px(x, y) == 0x11223344u;
    const bool out =
        !px(9, 10) && !px(20, 10) && !px(10, 9) && !px(10, 14) && !px(0, 11);
    t.say(dir ? "a span from far right to far left: under 2 s"
              : "a span from far left to far right: under 2 s",
          ms < 2000);
    t.say("... the scissor's 40 pixels filled", in);
    t.say("... and none beside them", out && !g.active);
  }

  // The colour DDA arrives at the scissor with the value a pixel a step
  // gives it: red rising by one a pixel from the span's first.
  for (int dir = 0; dir < 2; dir++) {
    setup();
    r.gp[T_COLOR_DDA_MODE] = 3; // on, Gouraud
    r.gp[T_DR_DX] = 1u << 15;
    r.gp[T_R_START] = dir ? 100u << 15 : 0;
    r.gp[T_START_X_DOM] = u32(s32(dir ? 40 : -5) << 16);
    r.gp[T_START_X_SUB] = u32(s32(dir ? -5 : 40) << 16);
    r.gp[T_START_Y] = 10u << 16;
    r.gp[T_COUNT] = 2;
    raster_begin(trapezoid);
    // left to right: x = -5 is 0, so x is x + 5; right to left: x = 39 is
    // 100 and each pixel leftwards one less
    bool ok = true;
    for (int x = 10; x < 20; x++)
      ok = ok && (px(x, 11) & 0xff) == u32(dir ? 100 - (39 - x) : x + 5);
    t.say(dir ? "Gouraud right to left: the colour at the scissor"
              : "Gouraud left to right: the colour at the scissor",
          ok && !px(9, 11) && !px(20, 11));
  }

  r = saved;
  memcpy(vga.memory, mem.data(), keep);
  t.rom_end((const u8 *)option_rom, rom_max,
            [this](u32 a, int bits) { return rom_read(a, bits); });
  t.finish();
}
