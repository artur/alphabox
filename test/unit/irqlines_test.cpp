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

/* Unit test of the wire-OR of the chipset's device interrupt inputs
 * (src/system/InterruptLines.hpp): the sources drive their inputs as the PCI
 * devices and the ISA interrupt controller do, and the level the chipset is
 * told is checked against the wire it models. 21272 HRM Table 6-9: the
 * inputs are level sensitive; PCI 2.2 2.2.6: INTx# is open drain and shared;
 * PCI-to-PCI Bridge Architecture Specification 1.1 Table 9-1: behind a bridge
 * devices d and d + 4 reach the same primary pin. Built with every lane as
 * `irqlines_test`; exit status 0 when every check passes.
 */

#include "InterruptLines.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int checks = 0, failures = 0;

#define CHECK_EQ(got, want)                                                    \
  do {                                                                         \
    const long g_ = (long)(got), w_ = (long)(want);                            \
    checks++;                                                                  \
    if (g_ != w_) {                                                            \
      failures++;                                                              \
      printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #got, g_, \
             w_);                                                              \
    }                                                                          \
  } while (0)

/// The chipset end: the last level each input was told, and how often.
struct Chipset {
  int level[64] = {};
  int calls = 0;
  void operator()(int input, bool high) {
    level[input] = high ? 1 : 0;
    calls++;
  }
};

/// Two devices, as two PCI cards behind a bridge.
static int dev_a, dev_b;
static const void *const A = &dev_a;
static const void *const B = &dev_b;

/// One source: the line follows it, and the chipset hears every drive.
static void test_one_source() {
  CInterruptLines l;
  Chipset c;
  CHECK_EQ(l.drive(20, A, 0, true, c), 1);
  CHECK_EQ(c.level[20], 1);
  CHECK_EQ(l.level(20), 1);
  // A device that asserts again (its status changed, the line did not) is
  // passed on as before: the chipset re-drives the processors' lines.
  CHECK_EQ(l.drive(20, A, 0, true, c), 1);
  CHECK_EQ(c.calls, 2);
  CHECK_EQ(l.drive(20, A, 0, false, c), 0);
  CHECK_EQ(c.level[20], 0);
  CHECK_EQ(l.level(20), 0);
  // Letting go of a line that is not asserted is a no-op on the wire.
  CHECK_EQ(l.drive(20, A, 0, false, c), 0);
  CHECK_EQ(c.level[20], 0);
  CHECK_EQ(c.calls, 4);
}

/// Two devices on one input: the line is high while either asserts it,
/// whichever lets go first. With the last caller winning, B letting go
/// cleared the input while A still asserted it, and A's level interrupt was
/// lost until A changed state.
static void test_two_devices_share() {
  CInterruptLines l;
  Chipset c;
  l.drive(21, A, 0, true, c);
  l.drive(21, B, 0, true, c);
  CHECK_EQ(l.drive(21, A, 0, false, c), 1); // B still asserts
  CHECK_EQ(c.level[21], 1);
  CHECK_EQ(l.drive(21, B, 0, false, c), 0);
  CHECK_EQ(c.level[21], 0);

  // The other order.
  l.drive(21, A, 0, true, c);
  l.drive(21, B, 0, true, c);
  CHECK_EQ(l.drive(21, B, 0, false, c), 1);
  CHECK_EQ(c.level[21], 1);
  CHECK_EQ(l.drive(21, A, 0, false, c), 0);
  CHECK_EQ(c.level[21], 0);

  // The case that lost interrupts: B never asserted, and its driver's
  // interrupt service routine, run for A's interrupt on the shared vector,
  // reads B's status, which lets go of B's (deasserted) request.
  l.drive(21, A, 0, true, c);
  CHECK_EQ(l.drive(21, B, 0, false, c), 1);
  CHECK_EQ(c.level[21], 1);
  CHECK_EQ(l.level(21), 1);
  CHECK_EQ(l.drive(21, A, 0, false, c), 0);
}

/// The functions of one device are requests of their own (the ES47 wires
/// both of its embedded USB functions to one line).
static void test_functions_share() {
  CInterruptLines l;
  Chipset c;
  l.drive(30, A, 0, true, c);
  l.drive(30, A, 1, true, c);
  CHECK_EQ(l.drive(30, A, 0, false, c), 1);
  CHECK_EQ(l.drive(30, A, 1, false, c), 0);
  // A function that asserts twice is one request: one let-go clears it.
  l.drive(30, A, 1, true, c);
  l.drive(30, A, 1, true, c);
  CHECK_EQ(l.drive(30, A, 1, false, c), 0);
}

/// Inputs are separate wires; one source driving two of them holds each
/// on its own (a device moved between inputs keeps the old one until it
/// lets go of it there).
static void test_inputs_independent() {
  CInterruptLines l;
  Chipset c;
  l.drive(8, A, 0, true, c);
  l.drive(9, B, 0, true, c);
  l.drive(55, B, -1, true, c); // the ISA interrupt, a source of its own
  CHECK_EQ(l.drive(9, B, 0, false, c), 0);
  CHECK_EQ(l.level(8), 1);
  CHECK_EQ(l.level(55), 1);
  CHECK_EQ(c.level[8], 1);
  CHECK_EQ(c.level[9], 0);
  CHECK_EQ(c.level[55], 1);
  // The Marvel numbers its inputs (PID << 8) | LSI: past any DRIR bit.
  int marvel = -1;
  l.drive(0x305, A, 0, true, [&](int n, bool h) { marvel = h ? n : -n; });
  CHECK_EQ(marvel, 0x305);
  CHECK_EQ(l.level(0x305), 1);
  CHECK_EQ(l.level(5), 0);
}

/// A machine reset clears the chipset's inputs, and the sources with them.
static void test_reset() {
  CInterruptLines l;
  Chipset c;
  l.drive(12, A, 0, true, c);
  l.drive(12, B, 0, true, c);
  l.reset();
  CHECK_EQ(l.level(12), 0);
  CHECK_EQ(l.drive(12, A, 0, false, c), 0);
  CHECK_EQ(l.drive(12, B, 0, true, c), 1);
}

/// Devices on threads of their own drive one input at once. The chipset is
/// told under the lock, so it hears the levels in order: at every delivery
/// the level it is told is the OR of the requests the test knows are up,
/// and at the end it holds the level of the wire.
static void test_threads() {
  CInterruptLines l;
  const int kThreads = 4, kRounds = 20000;
  int sources[kThreads];
  std::atomic<int> up[kThreads];
  for (auto &u : up)
    u = 0;
  int last = -1;
  int wrong = 0;
  auto deliver = [&](int, bool high) {
    int any = 0;
    for (auto &u : up)
      any |= u.load();
    if (any != (high ? 1 : 0))
      wrong++;
    last = high ? 1 : 0;
  };
  std::vector<std::thread> t;
  for (int i = 0; i < kThreads; i++)
    t.emplace_back([&, i] {
      for (int r = 0; r < kRounds; r++) {
        const bool on = (r & 1) == 0;
        // Thread 0 ends asserted, the others let go in the end.
        const bool level = (i == 0 && r == kRounds - 1) ? true : on;
        // The test's own record of each request, kept under the same lock
        // as the wire's: at every delivery the two must agree.
        l.drive(40, &sources[i], 0, level, [&](int n, bool high) {
          up[i] = level ? 1 : 0;
          deliver(n, high);
        });
      }
    });
  for (auto &x : t)
    x.join();
  CHECK_EQ(wrong, 0);
  CHECK_EQ(last, 1);
  CHECK_EQ(l.level(40), 1);
  CHECK_EQ(l.drive(40, &sources[0], 0, false, [](int, bool) {}), 0);
}

int main() {
  test_one_source();
  test_two_devices_share();
  test_functions_share();
  test_inputs_independent();
  test_reset();
  test_threads();
  printf("irqlines_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
