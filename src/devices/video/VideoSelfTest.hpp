/* Alphabox - An AlphaServer ES40 emulator
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

/* ALPHABOX_VIDEO_SELFTEST=1 (or =exit): the Mach64, the Permedia 2 and the
 * ViRGE check a few things of their engines at the end of init(), before
 * any machine starts, driving the card as a driver does or calling the
 * engine with the registers a driver would have loaded (docs/headless.md).
 * Each check prints `%<CARD>-I-SELFTEST: <what> ok|FAILED`, the card PASS
 * or FAIL last; with "exit" the emulator ends with the result. The card's
 * registers and the memory the checks drew in are put back.
 *
 * The Radeon has its own, larger one (ALPHABOX_RADEON_SELFTEST).
 */
#if !defined(INCLUDED_VIDEO_SELFTEST_H)
#define INCLUDED_VIDEO_SELFTEST_H

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct VideoSelfTest {
  const char *card;
  int checks = 0, failed = 0;
  std::chrono::steady_clock::time_point t0;

  explicit VideoSelfTest(const char *name) : card(name) {}
  static bool wanted() { return getenv("ALPHABOX_VIDEO_SELFTEST") != nullptr; }

  void say(const char *what, bool ok) {
    printf("%%%s-I-SELFTEST: %-60s %s\n", card, what, ok ? "ok" : "FAILED");
    checks++;
    if (!ok)
      failed++;
  }
  /// A read that straddles the end of the card's option ROM (`rom`, of
  /// `size` bytes; `read` is the card's rom_read): the bytes inside, the
  /// rest zero. It read as zero whole.
  template <class Read> void rom_end(const u8 *rom, u32 size, Read read) {
    if (!rom || size < 4)
      return;
    const u32 last2 = u32(rom[size - 2]) | (u32(rom[size - 1]) << 8);
    say("a longword read across the ROM's end: its two bytes inside",
        read(size - 2, 32) == last2 && read(size - 1, 16) == (last2 >> 8) &&
            read(size, 8) == 0 && read(size - 4, 32) != 0);
  }
  /// A walk that must be short: start() before it, ms() after.
  void start() { t0 = std::chrono::steady_clock::now(); }
  long ms() const {
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  }
  /// PASS or FAIL; with ALPHABOX_VIDEO_SELFTEST=exit the emulator ends here.
  void finish() {
    printf("%%%s-I-SELFTEST: %s (%d checks, %d failed)\n", card,
           failed ? "FAIL" : "PASS", checks, failed);
    const char *e = getenv("ALPHABOX_VIDEO_SELFTEST");
    if (e && strcmp(e, "exit") == 0) {
      fflush(stdout);
      exit(failed ? 1 : 0);
    }
  }
};

#endif // !defined(INCLUDED_VIDEO_SELFTEST_H)
