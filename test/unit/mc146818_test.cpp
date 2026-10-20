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

/* Unit test of the MC146818 time-of-year clock (src/devices/isa/
 * Mc146818.cpp) on clocks of its own: the test sets the host's time, reads
 * and sets the guest's through ports 70h-73h as the consoles and operating
 * systems do, and checks the registers against the Motorola MC146818A data
 * sheet. Built with every lane as `mc146818_test`; exit status 0 when every
 * check passes.
 */

#include "Mc146818.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

static int checks = 0, failures = 0;

#define CHECK_EQ(got, want)                                                    \
  do {                                                                         \
    const unsigned g_ = (unsigned)(got), w_ = (unsigned)(want);                \
    checks++;                                                                  \
    if (g_ != w_) {                                                            \
      failures++;                                                              \
      printf("FAIL %s:%d: %s = 0x%02x, want 0x%02x\n", __FILE__, __LINE__,     \
             #got, g_, w_);                                                    \
    }                                                                          \
  } while (0)

static s64 utc(int y, int mo, int d, int h, int mi, int s) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = s;
#ifdef _WIN32
  return (s64)_mkgmtime(&t);
#else
  return (s64)timegm(&t);
#endif
}

/// A clock with its RAM, on host clocks the test sets.
struct Rig {
  u8 ram[256];
  u8 ports[4];
  long offset = 0;
  s64 real = 0;               // ns since the epoch
  u64 steady = 5000000000000; // ns
  CMc146818 rtc;
  Rig()
      : rtc(ram, ports, offset,
            {[this] { return real; }, [this] { return steady; }}) {
    memset(ram, 0, sizeof(ram));
    memset(ports, 0, sizeof(ports));
    ram[0x0a] = 0x26; // as at power-on here: 32.768 kHz, 1024 Hz
    ram[0x0b] = 0x06; // binary, 24 hour
    at(2026, 10, 20, 12, 34, 56);
  }
  void at(int y, int mo, int d, int h, int mi, int s, u64 ns = 0) {
    real = utc(y, mo, d, h, mi, s) * 1000000000 + (s64)ns;
  }
  void ns(u64 n) {
    real += (s64)n;
    steady += n;
  }
  u8 rd(u8 reg) {
    rtc.write(0, reg);
    return rtc.read(1);
  }
  void wr(u8 reg, u8 v) {
    rtc.write(0, reg);
    rtc.write(1, v);
  }
};

/// Time and date in binary and BCD, 24 hour (data sheet Table 3, p.12).
static void test_read_time() {
  Rig r;
  CHECK_EQ(r.rd(0x00), 56);
  CHECK_EQ(r.rd(0x02), 34);
  CHECK_EQ(r.rd(0x04), 12);
  CHECK_EQ(r.rd(0x06), 3); // Tuesday; Sunday = 1
  CHECK_EQ(r.rd(0x07), 20);
  CHECK_EQ(r.rd(0x08), 10);
  CHECK_EQ(r.rd(0x09), 26);
  CHECK_EQ(r.rd(0x0d), 0x80); // VRT
  r.wr(0x0b, 0x02);           // BCD
  CHECK_EQ(r.rd(0x00), 0x56);
  CHECK_EQ(r.rd(0x07), 0x20);
  CHECK_EQ(r.rd(0x09), 0x26);
}

/// Setting the clock: SET, the registers, SET clear. The clock then runs on
/// from the time written.
static void test_set_time() {
  Rig r;
  r.wr(0x0b, 0x86); // SET, binary, 24 hour
  r.wr(0x00, 0);
  r.wr(0x02, 0);
  r.wr(0x04, 8);
  r.wr(0x07, 1);
  r.wr(0x08, 1);
  r.wr(0x09, 30);
  r.wr(0x0b, 0x06);
  CHECK_EQ(r.rd(0x09), 30);
  CHECK_EQ(r.rd(0x04), 8);
  CHECK_EQ(r.rd(0x00), 0);
  r.ns(61000000000ull); // 61 s on
  CHECK_EQ(r.rd(0x02), 1);
  CHECK_EQ(r.rd(0x00), 1);
}

/// The ARC console's years count from 1980.
static void test_arc_year() {
  Rig r;
  r.rtc.set_arc_year_compat(true);
  CHECK_EQ(r.rd(0x09), 46);
}

/// Register C's PF sets once per period of the rate in register A and is
/// cleared by reading register C (data sheet p.15-16).
static void test_periodic_flag() {
  Rig r;
  r.rd(0x0c);   // first use: pacing starts
  r.ns(500000); // half a period at 1024 Hz
  CHECK_EQ(r.rd(0x0c) & 0x40, 0);
  r.ns(600000);
  CHECK_EQ(r.rd(0x0c) & 0x40, 0x40);
  CHECK_EQ(r.rd(0x0c) & 0x40, 0);
  CHECK_EQ(r.rtc.interval_period_ns(), 976562);
}

/// Ports 72h/73h reach the upper 128 bytes of RAM.
static void test_upper_ram() {
  Rig r;
  r.rtc.write(2, 0x10);
  r.rtc.write(3, 0xa5);
  r.rtc.write(2, 0x10);
  CHECK_EQ(r.rtc.read(3), 0xa5);
  CHECK_EQ(r.ram[0x90], 0xa5);
}

/// 12-hour mode: hours 1 to 12, PM in bit 7 (data sheet Table 3, p.12):
/// 00:30 is 12 AM, 12:30 is 12 PM, in binary and BCD; and the time written
/// back that way sets the clock to the same hour.
static void test_12_hour() {
  Rig r;
  r.wr(0x0b, 0x04); // binary, 12 hour
  r.at(2026, 10, 20, 0, 30, 0);
  CHECK_EQ(r.rd(0x04), 0x0c);
  r.at(2026, 10, 20, 12, 30, 0);
  CHECK_EQ(r.rd(0x04), 0x8c);
  r.at(2026, 10, 20, 13, 30, 0);
  CHECK_EQ(r.rd(0x04), 0x81);
  r.wr(0x0b, 0x00); // BCD, 12 hour
  r.at(2026, 10, 20, 0, 30, 0);
  CHECK_EQ(r.rd(0x04), 0x12);
  r.at(2026, 10, 20, 23, 30, 0);
  CHECK_EQ(r.rd(0x04), 0x91);
  // read 12 AM and write it back: still half past midnight
  r.at(2026, 10, 20, 0, 30, 0);
  const u8 h = r.rd(0x04);
  r.wr(0x0b, 0x80);
  r.wr(0x04, h);
  r.wr(0x0b, 0x00);
  r.wr(0x0b, 0x06); // binary, 24 hour
  CHECK_EQ(r.rd(0x04), 0);
  CHECK_EQ(r.rd(0x02), 30);
}

int main() {
  test_read_time();
  test_set_time();
  test_arc_year();
  test_periodic_flag();
  test_upper_ram();
  test_12_hour();
  printf("mc146818_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
