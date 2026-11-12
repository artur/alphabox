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
 * The ViRGE's self-test (ALPHABOX_VIDEO_SELFTEST, VideoSelfTest.hpp): the
 * S3d engine's triangle with the registers a driver would have loaded.
 **/
#include "StdAfx.hpp"
#include "S3Virge.hpp"
#include "VideoSelfTest.hpp"

#include <vector>

using namespace virge;

void CS3Virge::selftest() {
  VideoSelfTest t("VIRGE");
  const auto saved = r;
  std::vector<u8> mem(vga.memory, vga.memory + m_vram_bytes);
  const u32 stride = 2048;

  // 24-bit destination at 0, no Z buffer, clipping on: columns 100..109.
  auto setup = [&](u32 top, u32 bottom) {
    memset(&r.mmio, 0, sizeof(r.mmio));
    memset(vga.memory, 0, m_vram_bytes);
    M(S3D_CMD) = (2u << 2) | CMD_CLIP | (3u << 24);
    M(S3D_DEST_STRIDE) = stride << 16;
    M(S3D_CLIP_LR) = (100u << 16) | 109;
    M(S3D_CLIP_TB) = (top << 16) | bottom;
  };
  auto red = [&](int x, int y) {
    return int(vga.memory[(u32(y) * stride + u32(x) * 3 + 2) & vram_mask()]);
  };

  // A triangle from (105, 2047) whose sides run 2047 pixels a scanline to
  // either side, 2047 scanlines: the spans grow to four million pixels.
  // Only the clip's ten columns are walked; a pixel a step it was eight
  // thousand million steps.
  setup(0, 2047);
  M(0xb550) = 200u << 7; // red 200, S8.7
  M(0xb578) = 2047;
  M(0xb574) = M(0xb56c) = 105u << 20;
  M(0xb570) = u32(-(2047 << 20));
  M(0xb568) = u32(2047 << 20);
  M(0xb57c) = 0x80000000u | (2047u << 16);
  t.start();
  s3d_triangle();
  const long ms = t.ms();
  bool in = true;
  for (int y = 1; y <= 2046; y += 409)
    for (int x = 100; x <= 109; x++)
      in = in && red(x, y) == 200;
  t.say("a triangle with spans of millions of pixels: under 2 s", ms < 2000);
  t.say("... the clip's columns drawn on its scanlines", in);
  t.say("... and nothing beside them",
        !red(99, 2046) && !red(110, 2046) && !red(99, 1) && !red(110, 1) &&
            !red(105, 2047));

  // The colour arrives at the clip with the value a pixel a step gives it:
  // one scanline from -100 to 300, red rising by one a pixel.
  for (int dir = 0; dir < 2; dir++) {
    setup(10, 13);
    M(0xb540) = 128; // red's step a pixel
    M(0xb578) = 11;
    M(0xb574) = u32(s32(dir ? 300 : -100) << 20);
    M(0xb56c) = u32(s32(dir ? -100 : 300) << 20);
    M(0xb57c) = (dir ? 0 : 0x80000000u) | (1u << 16);
    s3d_triangle();
    // rightwards: x = -100 is 0; leftwards: the first pixel, 299, is 1
    bool ok = true;
    for (int x = 100; x <= 109; x++)
      ok = ok && red(x, 11) == (dir ? 300 - x : x + 100);
    t.say(dir ? "Gouraud right to left: the colour at the clip"
              : "Gouraud left to right: the colour at the clip",
          ok && !red(99, 11) && !red(110, 11));
  }

  r = saved;
  memcpy(vga.memory, mem.data(), m_vram_bytes);
  t.rom_end((const u8 *)option_rom, rom_max,
            [this](u32 a, int bits) { return rom_read(a, bits); });
  t.finish();
}
