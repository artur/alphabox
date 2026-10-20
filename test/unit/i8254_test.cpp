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

/* Unit test of the 8254 interval timer (src/devices/isa/I8254.cpp) on a
 * clock of its own: the test sets the time, programs the counters through
 * their ports as a guest does, and checks OUT, the counts and the IRQ0 edges
 * against the Intel 8254 data sheet (231164-005). Built with every lane as
 * `i8254_test`; exit status 0 when every check passes.
 */

#include "I8254.hpp"

#include <cstdio>
#include <cstring>

static int checks = 0, failures = 0;

#define CHECK_EQ(got, want)                                                    \
  do {                                                                         \
    const unsigned long long g_ = (unsigned long long)(got),                   \
                             w_ = (unsigned long long)(want);                  \
    checks++;                                                                  \
    if (g_ != w_) {                                                            \
      failures++;                                                              \
      printf("FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__,     \
             #got, g_, w_);                                                    \
    }                                                                          \
  } while (0)

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

/// A timer on a clock the test moves.
struct Rig {
  SPit_state st;
  u64 t = 1000000000000ull; // ns; far from zero, like a host's uptime
  CI8254 pit;
  Rig() : pit(st, [this] { return t; }) {
    memset(&st, 0, sizeof(st));
    pit.reset();
  }
  /// Advance by a whole number of input clocks (rounded up to the ns).
  void clocks(u64 n) {
    t += (n * 1000000000ull + PIT_CLOCK_HZ - 1) / PIT_CLOCK_HZ;
  }
  void ns(u64 n) { t += n; }
  u8 in(u32 port) { return pit.read(port); }
  void outb(u32 port, u8 v) { pit.write(port, v); }
  /// Control word, then a two-byte count.
  void program(int c, int mode, u16 count, bool bcd = false) {
    outb(3, (u8)((c << 6) | 0x30 | (mode << 1) | (bcd ? 1 : 0)));
    outb((u32)c, (u8)count);
    outb((u32)c, (u8)(count >> 8));
  }
  u16 latch_count(int c) {
    outb(3, (u8)(c << 6)); // counter latch command
    const u8 lo = in((u32)c);
    return (u16)(lo | (in((u32)c) << 8));
  }
  u8 status(int c) {
    outb(3, (u8)(0xe0 | (2 << c))); // read-back: status only
    return in((u32)c);
  }
};

/// Mode 3 square wave on counter 2: OUT high for the first half of each
/// period of N clocks, low for the second (data sheet p.12).
static void test_mode3_wave() {
  Rig r;
  r.pit.load_gate(2, true);
  r.program(2, 3, 1000);
  CHECK(r.pit.out(2));
  r.clocks(400);
  CHECK(r.pit.out(2));
  r.clocks(200); // 600: second half
  CHECK(!r.pit.out(2));
  r.clocks(450); // 1050: next period
  CHECK(r.pit.out(2));
}

/// Counter 0 in mode 3 raises IRQ0 once per period: SRM's 65536 count gives
/// 18.2 interrupts a second.
static void test_mode3_irq0() {
  Rig r;
  r.program(0, 3, 0); // 0 = 65536
  int edges = 0;
  for (int ms = 0; ms < 1000; ms++) {
    r.ns(1000000);
    edges += r.pit.clock();
  }
  CHECK(edges >= 18 && edges <= 19);
}

/// Mode 0: OUT low after the control word and while counting, high at
/// terminal count (data sheet p.10).
static void test_mode0_out() {
  Rig r;
  r.pit.load_gate(2, true);
  r.outb(3, 0xb0); // counter 2, LSB then MSB, mode 0
  CHECK(!r.pit.out(2));
  r.outb(2, 100);
  r.outb(2, 0);
  CHECK(!r.pit.out(2));
  r.clocks(50);
  CHECK(!r.pit.out(2));
  r.clocks(60);
  CHECK(r.pit.out(2));
}

/// A counter latch command holds the count until both bytes are read; a
/// second latch before that is ignored (data sheet p.8).
static void test_latch() {
  Rig r;
  r.pit.load_gate(2, true);
  r.program(2, 2, 10000);
  r.clocks(1000);
  r.outb(3, 0x80); // latch counter 2
  r.clocks(500);
  r.outb(3, 0x80); // ignored: the first latch is still unread
  const u8 lo = r.in(2);
  r.clocks(500);
  const u16 v = (u16)(lo | (r.in(2) << 8));
  CHECK(v >= 8999 && v <= 9000);
  // unlatched now: the next latch sees the count move on
  const u16 w = r.latch_count(2);
  CHECK(w >= 7999 && w <= 8000);
}

/// Read-back status: OUT, NULL COUNT, the access, mode and BCD bits as
/// written (data sheet p.8-9).
static void test_readback_status() {
  Rig r;
  r.pit.load_gate(2, true);
  r.outb(3, 0xb6); // counter 2, LSB/MSB, mode 3
  CHECK_EQ(r.status(2), 0x80 | 0x40 | 0x36);
  r.outb(2, 0x00);
  r.outb(2, 0x10);
  CHECK_EQ(r.status(2), 0x80 | 0x36);
}

/// GATE2 low holds counter 2; a rising GATE restarts a mode 3 count from
/// its initial count with OUT high.
static void test_gate() {
  Rig r;
  r.pit.load_gate(2, false);
  r.program(2, 3, 1000);
  r.clocks(700);
  CHECK(r.pit.out(2)); // gated off: OUT high
  r.pit.set_gate(2, true);
  CHECK(r.pit.out(2));
  r.clocks(600);
  CHECK(!r.pit.out(2));
}

int main() {
  test_mode3_wave();
  test_mode3_irq0();
  test_mode0_out();
  test_latch();
  test_readback_status();
  test_gate();
  printf("i8254_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
