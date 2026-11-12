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
 * The Mach64's self-test (ALPHABOX_VIDEO_SELFTEST, VideoSelfTest.hpp): the
 * engine's host data register written as a driver writes it.
 **/
#include "StdAfx.hpp"
#include "Mach64.hpp"
#include "VideoSelfTest.hpp"

#include <vector>

using namespace mach64;

void CMach64::selftest() {
  VideoSelfTest t("MACH64");
  const regs_t saved = r;
  const accel_t saved_accel = accel;
  const u32 keep = std::min(m_vram_bytes, 64u * 1024);
  std::vector<u8> mem(vga.memory, vga.memory + keep);
  memset(vga.memory, 0, keep);

  // A 4 x 2 rectangle of 16-bit pixels from the host, at 0, 64 pixels a row.
  r.dp_pix_width = BPP_16 | (BPP_16 << 8) | (BPP_16 << 16);
  r.dp_src = SRC_HOST | (SRC_HOST << 8) | (MONO_SRC_1 << 16);
  r.dp_mix = 0x70007; // source
  r.dst_off_pitch = 8u << 22;
  r.src_off_pitch = 8u << 22;
  r.dst_y_x = 0;
  r.dst_height_width = (4u << 16) | 2;
  r.dst_cntl = DST_X_DIR | DST_Y_DIR;
  r.src_cntl = 0;
  r.pat_cntl = 0;
  r.host_cntl = 0;
  r.clr_cmp_cntl = 0;
  r.sc_left_right = 0x1fffu << 16;
  r.sc_top_bottom = 0x7fffu << 16;
  r.write_mask = 0xffffffffu;
  engine_start_rect();
  auto px = [&](int x, int y) {
    const u32 a = u32(y * 64 + x) * 2;
    return u32(vga.memory[a]) | (u32(vga.memory[a + 1]) << 8);
  };
  auto none_from = [&](int n) {
    for (int i = n; i < 8; i++)
      if (px(i & 3, i >> 2))
        return false;
    return true;
  };
  t.say("a rectangle from the host waits for its data", accel.busy);

  // A byte is no 16-bit pixel. Taken as one it left the count of bits
  // below zero, and the whole rectangle was drawn from that one write.
  engine_write8(HOST_DATA0, 0xab);
  t.say("a byte of host data draws nothing and the rectangle waits",
        accel.busy && none_from(0));
  // A word at the register's low half is one pixel (it was two bytes).
  engine_write16(HOST_DATA0, 0x1111);
  t.say("a word at the low half is one pixel",
        accel.busy && px(0, 0) == 0x1111 && none_from(1));
  engine_write32(HOST_DATA0, 0x33332222u);
  t.say("a longword is two", accel.busy && px(1, 0) == 0x2222 &&
                                 px(2, 0) == 0x3333 && none_from(3));
  engine_write32(HOST_DATA0, 0x55554444u);
  engine_write32(HOST_DATA0, 0x77776666u);
  t.say("the rectangle wraps to its second row",
        accel.busy && px(3, 0) == 0x4444 && px(0, 1) == 0x5555 &&
            px(1, 1) == 0x6666 && px(2, 1) == 0x7777 && !px(3, 1));
  engine_write16(HOST_DATA0 + 2, 0x8888);
  t.say("the last pixel ends it", !accel.busy && px(3, 1) == 0x8888 &&
                                      !px(4, 0) && !px(0, 2));

  r = saved;
  accel = saved_accel;
  memcpy(vga.memory, mem.data(), keep);
  t.rom_end((const u8 *)option_rom, rom_max,
            [this](u32 a, int bits) { return rom_read(a, bits); });
  t.finish();
}
