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

/// Mode 0 after terminal count: OUT stays high until the counter is
/// written again (data sheet p.10), however late it is looked at and after
/// the timer thread has stepped past it; the count wraps to FFFF and counts
/// on (p.17), and NULL COUNT stays clear (p.9).
static void test_mode0_terminal() {
  Rig r;
  r.pit.load_gate(2, true);
  r.program(2, 0, 100);
  for (int ms = 0; ms < 5; ms++) { // 5 ms: the thread steps past terminal
    r.ns(1000000);
    r.pit.clock();
  }
  CHECK(r.pit.out(2));
  CHECK_EQ(r.status(2) & 0xc0, 0x80); // OUT high, NULL COUNT clear
  const u16 c = r.latch_count(2);     // 100 - 5965 clocks, wrapped: 0xe917
  CHECK(c >= 0xe915 && c <= 0xe918);
  r.ns(50000000);
  r.pit.clock();
  CHECK(r.pit.out(2));
  // a new count lowers OUT again until it expires
  r.outb(2, 50);
  r.outb(2, 0);
  CHECK(!r.pit.out(2));
  r.clocks(51);
  CHECK(r.pit.out(2));
}

/// GATE low suspends a mode 0 count and leaves OUT as it is; GATE high
/// resumes it from where it stopped (data sheet p.10, Figure 21).
static void test_mode0_gate() {
  Rig r;
  r.pit.load_gate(2, true);
  r.program(2, 0, 1000);
  r.clocks(400);
  r.pit.set_gate(2, false);
  r.ns(20000000); // 20 ms suspended
  r.pit.clock();
  CHECK(!r.pit.out(2));
  r.pit.set_gate(2, true);
  r.clocks(550);
  CHECK(!r.pit.out(2)); // 950 of 1000 counted
  r.clocks(60);
  CHECK(r.pit.out(2));
  // a gate drop right after terminal count, before the thread has looked:
  // OUT stays high
  r.pit.set_gate(2, false);
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

/// A state file carries the counts, not host times: a counter saved in the
/// middle of its count goes on from there when restored in another run,
/// whose clock reads something else entirely.
static void test_save_restore() {
  Rig a;
  a.pit.load_gate(2, true);
  a.program(2, 0, 10000); // mode 0 delay, 8.38 ms
  a.program(0, 3, 1000);  // square wave
  a.clocks(3000);
  SPit_state file = a.st;
  a.pit.save(file);

  Rig b;
  b.t = 5000000000ull; // another process: another clock
  b.st = file;
  b.pit.load_gate(2, true);
  b.pit.restored();
  CHECK(!b.pit.out(2));
  b.clocks(6990); // 9990 of 10000
  CHECK(!b.pit.out(2));
  b.clocks(20);
  CHECK(b.pit.out(2));

  Rig c; // counter 0: 3000 clocks in = the start of a period
  c.t = 7000000000ull;
  c.st = file;
  c.pit.restored();
  CHECK(c.pit.out(0));
  c.clocks(490);
  CHECK(c.pit.out(0));
  c.clocks(20); // 510: second half
  CHECK(!c.pit.out(0));
  // and the thread goes on raising IRQ0 at the period
  int edges = 0;
  for (int i = 0; i < 100; i++) { // 100 ms: about 119 periods of 1000 clocks
    c.ns(1000000);
    edges += c.pit.clock();
  }
  CHECK(edges >= 118 && edges <= 121);
}

/// A state file written before the timer saved its counts holds the count
/// clock() had reached: a restore goes on from that.
static void test_restore_old_file() {
  Rig a;
  a.pit.load_gate(2, true);
  a.program(2, 0, 10000);
  for (int ms = 0; ms < 3; ms++) {
    a.ns(1000000);
    a.pit.clock();
  }
  Rig b;
  b.t = 5000000000ull;
  b.st = a.st; // no save(): the thread's count, 3 ms in
  b.pit.load_gate(2, true);
  b.pit.restored();
  CHECK(!b.pit.out(2));
  b.ns(5300000); // 8.3 ms in all
  CHECK(!b.pit.out(2));
  b.ns(200000);
  CHECK(b.pit.out(2));
}

/// Reads and writes of one counter may interleave (data sheet p.8): read
/// LSB, write new LSB, read MSB, write new MSB. The MSB read belongs to the
/// count whose LSB was read; and a latched count survives the first byte of
/// a new count.
static void test_interleave() {
  Rig r;
  r.pit.load_gate(2, true);
  r.program(2, 2, 0x1234);
  const u8 lo = r.in(2); // count now, 0x1234 (no time passed)
  r.outb(2, 0x00);       // new count LSB
  const u8 hi = r.in(2);
  r.outb(2, 0x20); // new count MSB: 0x2000
  CHECK_EQ(lo | (hi << 8), 0x1234);

  Rig q;
  q.pit.load_gate(2, true);
  q.program(2, 0, 0x4000);
  q.clocks(0x100);
  q.outb(3, 0x80); // latch counter 2: 0x3f00
  q.outb(2, 0x55); // first byte of a new count
  const u8 l2 = q.in(2);
  CHECK_EQ(l2 | (q.in(2) << 8), 0x3f00);
  q.outb(2, 0x01); // the new count is 0x0155, loaded now (mode 0)
  CHECK_EQ(q.latch_count(2), 0x0155);
}

/// Mode 2: a count written while counting takes over at the end of the
/// current period; NULL COUNT shows until then (data sheet p.9-10).
static void test_mode2_new_count() {
  Rig r;
  r.pit.load_gate(2, true);
  r.program(2, 2, 1000);
  r.clocks(300);
  r.outb(2, 500 & 0xff);
  r.outb(2, 500 >> 8);
  CHECK_EQ(r.status(2) & 0x40, 0x40);
  r.clocks(600); // 900: still the old count
  const u16 a = r.latch_count(2);
  CHECK(a >= 99 && a <= 101);
  r.clocks(200); // 1100: the new count, 100 clocks in
  const u16 b = r.latch_count(2);
  CHECK(b >= 399 && b <= 401);
  CHECK_EQ(r.status(2) & 0x40, 0);
}

/// Mode 3: a count written while counting takes over at the end of the
/// current half-cycle (data sheet p.12-13).
static void test_mode3_new_count() {
  Rig r;
  r.pit.load_gate(2, true);
  r.program(2, 3, 1000);
  r.clocks(200); // high half of the old count
  r.outb(2, 600 & 0xff);
  r.outb(2, 600 >> 8);
  r.clocks(290); // 490
  CHECK(r.pit.out(2));
  r.clocks(20); // 510: the new count's low half (300 clocks) from 500
  CHECK(!r.pit.out(2));
  r.clocks(280); // 790
  CHECK(!r.pit.out(2));
  r.clocks(20); // 810: high half of the new count
  CHECK(r.pit.out(2));
  r.clocks(300); // 1110: low again (high 800-1100)
  CHECK(!r.pit.out(2));
}

/// A trigger on GATE loads a waiting count at once; a control word drops it.
static void test_new_count_trigger() {
  Rig r;
  r.pit.load_gate(2, true);
  r.program(2, 2, 1000);
  r.clocks(300);
  r.outb(2, 500 & 0xff);
  r.outb(2, 500 >> 8);
  r.pit.set_gate(2, false);
  r.pit.set_gate(2, true);
  r.clocks(100);
  const u16 a = r.latch_count(2);
  CHECK(a >= 399 && a <= 401);

  Rig q;
  q.pit.load_gate(2, true);
  q.program(2, 2, 1000);
  q.clocks(300);
  q.outb(2, 500 & 0xff);
  q.outb(2, 500 >> 8);
  q.outb(3, 0xb4); // control word, counter 2 mode 2: the waiting count goes
  CHECK_EQ(q.status(2) & 0x40, 0x40);
  q.outb(2, 0x00);
  q.outb(2, 0x08); // 0x800, loaded at once: no count was running
  q.clocks(0x100);
  const u16 b = q.latch_count(2);
  CHECK(b >= 0x6ff && b <= 0x701);
}

int main() {
  test_mode3_wave();
  test_mode3_irq0();
  test_mode0_out();
  test_mode0_terminal();
  test_mode0_gate();
  test_latch();
  test_readback_status();
  test_gate();
  test_save_restore();
  test_restore_old_file();
  test_interleave();
  test_mode2_new_count();
  test_mode3_new_count();
  test_new_count_trigger();
  printf("i8254_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
