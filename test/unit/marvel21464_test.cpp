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

/* Unit test of the Marvel model's 21464 mode where it has no image path
 * (docs/platforms/ev8-native.md, section 2 and S11b), written from the
 * packet's tables:
 *
 *  - the map (2.1, N18): ipe8(n) = (~n & 0x1fff) << 35, the register
 *    window at | 0x7FFC << 20 (PID 0's at 0xFFFF_FFC0_0000), the IO7 ports
 *    at | (~p & 7) << 32, memory at ((n & 3) | (n & 0x3fc) << 1) << 34;
 *    every one of the 1024 PIDs decoded back, no other inverted field; for
 *    PIDs 0-31 the low 41 bits the EV7's and PA<47:41> all ones, memory
 *    below 2^45 and never I/O; the EV7's map as it was;
 *  - the topology's dense PIDs (2.2, N19): chip c is PID c on the ES47,
 *    ES80 and GS1280 layouts, its TPUs sharing it; the IO7s still where the
 *    layout cables them (by the console's number for the place); R_WHOAMI's
 *    layout and R_ROUT's entries in the IDS's (pp. 526-527).
 *
 * Built with every lane as `marvel21464_test`; exit status 0 when every
 * check passes.
 */

#include "Boards.hpp"
#include "Ev7.hpp"
#include "Topology.hpp"

#include <cstdio>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

// The board files' board_devices call the shared Marvel one (Es47.cpp).
void marvel_board_devices(CConfigurator *, CSystem *) {}

static void check_map() {
  const ev7::address_map &m = ev7::k21464Map, &e = ev7::kEv7Map;
  const u64 low41 = (U64(1) << 41) - 1;
  CHECK(m.phys_mask() == U64(0xFFFFFFFFFFFF));
  CHECK(e.phys_mask() == U64(0xFFFFFFFFFFF));
  CHECK(m.ipe(0) == U64(0xFFF800000000));
  CHECK(m.csr_base(0) == U64(0xFFFFFFC00000));
  CHECK(m.csr_base(1) == U64(0xFFF7FFC00000));
  CHECK(m.io7_base(0, 0) == U64(0xFFFF00000000));
  CHECK(m.io7_base(0, 7) == U64(0xFFF800000000));
  for (u32 n = 0; n < 1024; n++) {
    u32 pe = ~0u, off = ~0u, port = ~0u;
    CHECK(m.ipe(n) == (u64)(~n & 0x1fff) << 35);
    CHECK(m.is_io(m.csr_base(n)) && ((m.csr_base(n) >> 45) & 3) == 3);
    CHECK(m.csr_decode(m.csr_base(n) | 0x3F0040, &pe, &off));
    CHECK(pe == n && off == 0x3F0040);
    for (u32 p = 0; p < 8; p++) {
      CHECK(m.io7_decode(m.io7_base(n, p) | 0xFF800000, &pe, &port, &off));
      CHECK(pe == n && port == p && off == 0xFF800000);
    }
    // Memory: the formula, its inverse, below 2^45, never I/O.
    const u64 base = m.memory_base(n);
    CHECK(base == (u64)((n & 3) | (n & 0x3fc) << 1) << 34);
    CHECK(m.memory_pid(base) == n);
    CHECK(m.memory_pid(base + ev7::kMemorySlot - 1) == n);
    CHECK(base + ev7::kMemorySlot <= U64(1) << 45);
    CHECK(!m.is_io(base + ev7::kMemorySlot - 1));
    if (n < 128)
      CHECK(base == e.memory_base(n));
    // For PIDs 0-31 the EV7's low 41 bits, PA<47:41> all ones: what SPE43
    // reaches from Linux's KSEG arithmetic.
    if (n < 32) {
      CHECK((m.csr_base(n) & low41) == (e.csr_base(n) & low41));
      CHECK(m.csr_base(n) >> 41 == 0x7f);
      CHECK((m.io7_base(n, 3) & low41) == (e.io7_base(n, 3) & low41));
      CHECK(m.io7_base(n, 3) >> 41 == 0x7f);
      CHECK(base >> 41 == 0);
    }
  }
  // An inverted field that is no 10-bit PID: PA<47> set, PA<46:45> not 11.
  for (u64 top : {U64(0), U64(1), U64(2)}) {
    u32 pe, off, port;
    const u64 pa = U64(1) << 47 | top << 45 | U64(0x7FFC) << 20;
    CHECK(!m.csr_decode(pa, &pe, &off));
    CHECK(!m.io7_decode(pa, &pe, &port, &off));
  }
  // Not I/O: memory.
  {
    u32 pe, off;
    CHECK(!m.csr_decode(U64(0x7FFFFFC00000), &pe, &off));
  }
  // The EV7's map, unchanged.
  for (u32 n = 0; n < 256; n++) {
    u32 pe, off;
    CHECK(e.ipe(n) == (u64)(~n & 0x1ff) << 35);
    CHECK(e.csr_decode(e.csr_base(n) | 0x70, &pe, &off) && pe == n &&
          off == 0x70);
    if (n < 128)
      CHECK(e.memory_pid(e.memory_base(n)) == n);
  }
  CHECK(e.csr_base(0) == U64(0xFFFFFC00000));
  CHECK(e.memory_base(1) == U64(0x400000000));
}

/// Dense PIDs: chip c is PID c, its TPUs share it, TPU 0 first.
static void check_dense(const char *what, const marvel_layout *layout,
                        int chips, int tpus) {
  CMarvelTopology t;
  CHECK(t.build(layout, chips, tpus, true));
  CHECK(t.dense());
  CHECK(t.count() == chips * tpus);
  for (int i = 0; i < t.count(); i++) {
    const ev7_node &n = t.node(i);
    CHECK((int)n.pid == i / tpus && (int)n.chip == i / tpus);
    CHECK(n.chip_pid == n.pid && (int)n.tpu == i % tpus);
    CHECK(n.site == CMarvelTopology::chip_pid(layout, (int)n.chip));
    CHECK(t.by_pid(n.pid, t.count()) == &t.node(i - (int)n.tpu));
    CHECK(t.has_memory(n.pid, t.count()));
  }
  printf("%-8s %2d chips x %d TPUs: dense PIDs 0-%d\n", what, chips, tpus,
         chips - 1);
}

static void check_routes() {
  // The GS1280's first eight: NS 0-1 by EW 0-3 (Gs1280.cpp).
  CMarvelTopology g;
  CHECK(g.build(&gs1280_layout, 8, 1, true));
  const u32 kAccess = 0xff; // VAL, IIP, IIO, IME, PIP, PIO, PME, CAD
  // Itself: no hops, its own coordinates.
  CHECK(g.route21464(0, 0, 8) == kAccess);
  // PID 1, NS 1 EW 0: North, the first hop North.
  CHECK(g.route21464(0, 1, 8) == (kAccess | 1u << 16 | 0u << 20));
  // PID 2, NS 0 EW 1: East, the first hop East (IND 2).
  CHECK(g.route21464(0, 2, 8) == (kAccess | 1u << 12 | 2u << 20));
  // PID 6, NS 0 EW 3: one hop West on the ring of four (REW, IND 3).
  CHECK(g.route21464(0, 6, 8) == (kAccess | 3u << 12 | 1u << 11 | 3u << 20));
  // PID 7, NS 1 EW 3: North first, then West.
  CHECK(g.route21464(0, 7, 8) ==
        (kAccess | 1u << 16 | 3u << 12 | 1u << 11 | 0u << 20));
  // Not there: 0.
  CHECK(g.route21464(0, 9, 8) == 0 && g.route21464(0, 3, 2) == 0);
  // The ES80's ring of eight: PID 7 is one hop South of PID 0.
  CMarvelTopology r;
  CHECK(r.build(&es80_layout, 8, 4, true));
  CHECK(r.route21464(0, 7, 32) == (kAccess | 7u << 16 | 1u << 10 | 1u << 20));
  // TPUs of one chip share its route: to itself.
  CHECK(r.route21464(3, 3, 32) == (kAccess | 3u << 16));
  // R_WHOAMI: EW <9:5>, NS <4:0> [IDS p. 527].
  CHECK(CMarvelTopology::whoami21464(1, 0) == 1);
  CHECK(CMarvelTopology::whoami21464(3, 2) == (2u << 5 | 3u));
  CHECK(CMarvelTopology::whoami21464(31, 31) == 0x3ff);
}

int main() {
  check_map();
  check_dense("es47", nullptr, 2, 4);
  check_dense("es80", &es80_layout, 8, 4);
  check_dense("gs1280", &gs1280_layout, 16, 2);
  check_dense("gs1280", &gs1280_layout, 8, 4);
  {
    // The IO7s where the layout cables them, by the place's console number:
    // the ES80's on each 2P drawer's first chip (dense 0, 2, 4, 6), the
    // GS1280's on PID 0, the ES47's on PID 0.
    CMarvelTopology t;
    t.build(&es80_layout, 8, 4, true);
    for (u32 p = 0; p < 8; p++)
      CHECK(t.has_io7(p) == (p % 2 == 0));
    t.build(&gs1280_layout, 16, 2, true);
    for (u32 p = 0; p < 16; p++)
      CHECK(t.has_io7(p) == (p == 0));
    t.build(nullptr, 2, 4, true);
    CHECK(t.has_io7(0) && !t.has_io7(1));
    // No TPU shift with dense PIDs: a layout whose PIDs use every bit pair.
    CHECK(t.shift() == 0);
  }
  check_routes();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
