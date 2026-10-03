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

/* The Radeon self-test (ALPHABOX_RADEON_SELFTEST, RadeonSelfTest.cpp):
 * what a generation's 3D scenes get from the common part -- the way to
 * drive the card as a driver does, the report, the frames' directory --
 * and the reference helpers they share.
 *
 * The common checks run first (the FIFO, the CP, the GART, the clocks,
 * the 2D engine, the cursor), then the generation's scenes
 * (CRadeonEngine3D::selftest_scenes; the R100's in
 * r100/RadeonR100SelfTest.cpp), then the last common checks. A
 * generation's scenes draw through the CP ring in VRAM (`cp`), wait for
 * the engine before they read the memory it draws in (`vr32` waits;
 * `sync` alone), and write their frames to `dir` with write_png.
 */

#if !defined(INCLUDED_RADEON_SELFTEST_H)
#define INCLUDED_RADEON_SELFTEST_H

#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "datatypes.hpp"

namespace radeon {

/// The common self-test's means, handed to a generation's scenes.
struct SelfTest {
  /// One check's result: "%RADEON-I-SELFTEST: <what> ok|FAILED <detail>".
  std::function<void(const std::string &what, bool ok,
                     const std::string &detail)>
      report;
  /// A register write through the MMIO BAR (after the CP's stream, when
  /// the CP was used last).
  std::function<void(u32 reg, u32 value)> wr;
  /// Wait for the engine: every FIFO entry free, then GUI_ACTIVE clear.
  std::function<void()> sync;
  /// VRAM by offset, after the engine is idle.
  std::function<u32(u32 addr, int bytes)> vrd;
  std::function<void(u32 addr, int bytes, u32 value)> vwr;
  std::function<u32(u32 addr)> vr32;
  std::function<void(u32 addr, u32 value)> vw32;
  /// Dwords onto the CP's ring, and the write pointer past them.
  std::function<void(const std::vector<u32> &dwords)> cp;
  /// Where the frames go.
  std::string dir;
};

namespace selftest {

/// The PNG writer's CRC table (once, before write_png).
void crc_init();
/// An RGB image (0x00RRGGBB pixels) as a PNG file.
bool write_png(const std::string &path, int w, int h,
               const std::vector<u32> &px);

// --- small helpers -----------------------------------------------------------
inline u32 fbits(float f) {
  u32 v;
  memcpy(&v, &f, 4);
  return v;
}
/// The self-test's pseudo-random sequence (one for the whole run).
u32 rnd();
inline float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline u32 to8(float v) { return u32(std::lround(clampf(v) * 255.0f)); }
inline u32 argbf(const float c[4]) {
  return (to8(c[3]) << 24) | (to8(c[0]) << 16) | (to8(c[1]) << 8) | to8(c[2]);
}
inline void unargb(u32 v, float c[4]) {
  c[0] = float((v >> 16) & 0xff) / 255;
  c[1] = float((v >> 8) & 0xff) / 255;
  c[2] = float(v & 0xff) / 255;
  c[3] = float(v >> 24) / 255;
}

/// Packet headers.
inline u32 pkt0(u32 reg, u32 count) { return ((count - 1) << 16) | (reg >> 2); }
inline u32 pkt0_one(u32 reg, u32 count) {
  return ((count - 1) << 16) | 0x8000 | (reg >> 2);
}
inline u32 pkt3(u32 op, u32 count) {
  return 0xc0000000u | ((count - 1) << 16) | (op << 8);
}

/// The reference rasteriser: every pixel whose centre a triangle covers
/// (top-left rule), with its barycentric weights.
struct RefVtx {
  double x, y;
};
void ref_triangle(
    RefVtx a, RefVtx b, RefVtx c, int w, int h,
    const std::function<void(int, int, double, double, double)> &shade);

} // namespace selftest
} // namespace radeon

#endif // !defined(INCLUDED_RADEON_SELFTEST_H)
