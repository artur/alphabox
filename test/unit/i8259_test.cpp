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

/* Unit test of the ALi M1543C's cascaded 8259A pair
 * (src/devices/isa/I8259.cpp): initialised and driven through its ports as
 * the consoles and operating systems do, interrupts raised as the devices
 * raise them, the master's INT output recorded, and the answers checked
 * against the Intel 8259A data sheet (231468-003). Built with every lane as
 * `i8259_test`; exit status 0 when every check passes.
 */

#include "I8259.hpp"

#include <cstdio>
#include <cstring>

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

/// A pair with its INT output recorded.
struct Rig {
  SPic_state st;
  bool line = false;
  CI8259 pic;
  Rig() : pic(st, [this](bool high) { line = high; }) {
    memset(&st, 0, sizeof(st));
    pic.reset();
  }
  /// The usual initialisation: edge triggered, cascaded, 8086 mode, master
  /// vectors from 0x20, slave from 0x28, nothing masked.
  void init(u8 icw4 = 0x01) {
    pic.write(0, 0, 0x11);
    pic.write(0, 1, 0x20);
    pic.write(0, 1, 0x04);
    pic.write(0, 1, icw4);
    pic.write(1, 0, 0x11);
    pic.write(1, 1, 0x28);
    pic.write(1, 1, 0x02);
    pic.write(1, 1, icw4);
    pic.write(0, 1, 0x00);
    pic.write(1, 1, 0x00);
  }
  u8 isr(int index) {
    pic.write(index, 0, 0x0b); // OCW3: read ISR
    const u8 v = pic.read(index, 0);
    pic.write(index, 0, 0x0a); // back to IRR
    return v;
  }
  void eoi(int index, int irq) { pic.write(index, 0, (u8)(0x60 | irq)); }
};

/// An edge on IR1 raises INT; the acknowledge returns base + 1 and puts it
/// in service; a specific EOI ends it (data sheet p.7, p.14).
static void test_basic() {
  Rig r;
  r.init();
  CHECK_EQ(r.line, 0);
  r.pic.interrupt(0, 1);
  CHECK_EQ(r.line, 1);
  CHECK_EQ(r.pic.read(0, 0), 0x02); // IRR
  CHECK_EQ(r.pic.read_vector(), 0x21);
  CHECK_EQ(r.line, 0);
  CHECK_EQ(r.isr(0), 0x02);
  r.eoi(0, 1);
  CHECK_EQ(r.isr(0), 0x00);
}

/// A slave input reaches the CPU through the master's IR2: the vector comes
/// from the slave, and both have it in service until their EOIs.
static void test_cascade() {
  Rig r;
  r.init();
  r.pic.interrupt(1, 6); // IRQ14
  CHECK_EQ(r.line, 1);
  CHECK_EQ(r.pic.read_vector(), 0x2e);
  CHECK_EQ(r.isr(1), 0x40);
  CHECK_EQ(r.isr(0), 0x04);
  r.eoi(1, 6);
  r.eoi(0, 2);
  CHECK_EQ(r.isr(0), 0x00);
  CHECK_EQ(r.line, 0);
}

/// A lower-priority request waits while a higher one is in service and is
/// delivered at its EOI; a masked one waits for the unmask.
static void test_priority_and_mask() {
  Rig r;
  r.init();
  r.pic.interrupt(0, 3);
  CHECK_EQ(r.pic.read_vector(), 0x23);
  r.pic.interrupt(0, 5);
  CHECK_EQ(r.line, 0); // IR3 in service blocks IR5
  r.eoi(0, 3);
  CHECK_EQ(r.line, 1);
  CHECK_EQ(r.pic.read_vector(), 0x25);
  r.eoi(0, 5);
  r.pic.write(0, 1, 0x10); // mask IR4
  r.pic.interrupt(0, 4);
  CHECK_EQ(r.line, 0);
  r.pic.write(0, 1, 0x00);
  CHECK_EQ(r.line, 1);
}

/// A request that goes away before the acknowledge gives the default IR7,
/// which is not put in service (data sheet p.7, p.18).
static void test_spurious() {
  Rig r;
  r.init();
  r.pic.set_line(0, 4, true);
  CHECK_EQ(r.line, 1);
  r.pic.set_line(0, 4, false);
  CHECK_EQ(r.pic.read_vector(), 0x27);
  CHECK_EQ(r.isr(0), 0x00);
}

/// A level-triggered input (ELCR) keeps its request after the acknowledge
/// for as long as the device holds the line; all eight ELCR bits of both
/// controllers are read/write on the M1543C (data sheet v1.10, 4D0h-4D1h).
static void test_level() {
  Rig r;
  r.init();
  r.pic.write_elcr(0, 0xff);
  CHECK_EQ(r.pic.read_elcr(0), 0xff);
  r.pic.write_elcr(0, 0x00);
  r.pic.write_elcr(1, 0x08); // IRQ11 level
  r.pic.set_line(1, 3, true);
  CHECK_EQ(r.pic.read_vector(), 0x2b);
  CHECK_EQ(r.pic.read(1, 0) & 0x08, 0x08); // still requested
  r.pic.set_line(1, 3, false);
  CHECK_EQ(r.pic.read(1, 0) & 0x08, 0x00);
  r.eoi(1, 3);
  r.eoi(0, 2);
  CHECK_EQ(r.line, 0);
}

/// Linux's acknowledge: rotate on specific EOI (OCW2 0xE0 | irq) leaves the
/// level just served at the lowest priority (data sheet p.15-16).
static void test_rotate_specific_eoi() {
  Rig r;
  r.init();
  r.pic.interrupt(0, 1);
  CHECK_EQ(r.pic.read_vector(), 0x21);
  r.pic.write(0, 0, 0xe1);
  r.pic.interrupt(0, 1);
  r.pic.interrupt(0, 6);
  CHECK_EQ(r.pic.read_vector(), 0x26); // IR1 is now lowest
}

/// The poll command: the next read acknowledges the highest request and
/// returns 0x80 | level (data sheet p.16).
static void test_poll() {
  Rig r;
  r.init();
  r.pic.interrupt(0, 5);
  r.pic.write(0, 0, 0x0c); // OCW3: poll
  CHECK_EQ(r.pic.read(0, 0), 0x85);
  CHECK_EQ(r.isr(0), 0x20);
}

int main() {
  test_basic();
  test_cascade();
  test_priority_and_mask();
  test_spurious();
  test_level();
  test_rotate_specific_eoi();
  test_poll();
  printf("i8259_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
