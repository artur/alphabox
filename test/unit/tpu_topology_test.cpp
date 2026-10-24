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

/* Unit test of the 21464's TPUs as processors on the Marvel boards
 * (src/chipsets/marvel/Topology.cpp; docs/platforms/ev8.md, S4), without
 * the console:
 *
 *  - the numbering the owner chose on 2026-10-25: TPU 0 keeps its chip's
 *    PID, TPU t is PID | t << s with s the lowest bit pair no configured
 *    chip's PID uses; on the ES47, ES80 and GS1280 layouts (the board rows'
 *    own coordinates, platforms/es80/ and gs1280/), every number distinct,
 *    below 32 within the 32-processor limit, and a chip of one TPU keeping
 *    today's PID; a layout that leaves no pair free refused;
 *  - memory and the IO7 with TPU 0 only (D4), the chips the management
 *    side describes, and the route between TPUs of one chip (local);
 *  - interrupt steering (D5): each TPU's PID names a register block of its
 *    own (RBOX_IREQ, RBOX_INT, RBOX_IMASK are in it), and an IO7 interrupt
 *    control register's target field carries the TPU's PID unchanged.
 *
 * Built with every lane as `tpu_topology_test`; exit status 0 when every
 * check passes.
 */

#include "Boards.hpp"
#include "Ev7.hpp"
#include "Io7.hpp"
#include "Topology.hpp"

#include <cstdio>
#include <set>
#include <vector>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

// The board files' board_devices call the shared Marvel one (Es47.cpp),
// which the layouts tested here do not need.
void marvel_board_devices(CConfigurator *, CSystem *) {}

static std::vector<u32> pids(const CMarvelTopology &t) {
  std::vector<u32> v;
  for (int i = 0; i < t.count(); i++)
    v.push_back(t.node(i).pid);
  return v;
}

/// Every processor's number distinct and below `below`, TPU 0 its chip's
/// PID, and the other TPUs without memory or IO7.
static void check_layout(const char *what, const marvel_layout *layout,
                         int chips, int tpus, int shift, u32 below) {
  CMarvelTopology t;
  CHECK(t.build(layout, chips, tpus));
  CHECK(t.count() == chips * tpus);
  CHECK(t.shift() == shift);
  std::set<u32> seen, blocks;
  for (int i = 0; i < t.count(); i++) {
    const ev7_node &n = t.node(i);
    CHECK((int)n.chip == i / tpus && (int)n.tpu == i % tpus);
    CHECK(n.pid == (n.chip_pid | n.tpu << shift));
    CHECK(n.chip_pid == CMarvelTopology::chip_pid(layout, (int)n.chip));
    if (n.tpu == 0)
      CHECK(n.pid == n.chip_pid);
    CHECK(n.pid < below);
    CHECK(seen.insert(n.pid).second);
    CHECK(t.has_memory(n.pid, t.count()) == (n.tpu == 0));
    if (n.tpu)
      CHECK(!t.has_io7(n.pid));
    CHECK(t.chip_by_pid(n.chip_pid, t.count()) == &t.node(i - (int)n.tpu));
    // D5: a register block per TPU. Its RBOX_IREQ (another processor's
    // interprocessor request) decodes to this TPU's PID and no other's.
    u32 pe = 0, off = 0;
    CHECK(ev7::csr_decode(ev7::csr_base(n.pid) | 0x70, &pe, &off));
    CHECK(pe == n.pid && off == 0x70);
    CHECK(blocks.insert((u32)(ev7::csr_base(n.pid) >> 22)).second);
    // An IO7's LSI_CTL names its target in <22:14>: the TPU's PID.
    CHECK(io7::ctl_target((u64)n.pid << 14 | (U64(1) << 24)) == n.pid);
    // The route to another TPU of the same chip ends in the shared Cbox
    // (bit 8, as the route to itself); to another chip's it does not.
    for (int j = 0; j < t.count(); j++) {
      const u32 r = t.route(n.pid, t.node(j).pid, t.count());
      CHECK((r & 0x510) == 0x410 || (r & 0x510) == 0x510);
      CHECK(((r & 0x100) != 0) == (t.node(j).chip == n.chip));
    }
  }
  printf("%-28s %2d chips x %d TPUs: shift %d, PIDs", what, chips, tpus,
         t.shift());
  for (u32 p : pids(t))
    printf(" %u", p);
  printf("\n");
}

/// A chip of one TPU keeps its PID: the layout's own numbers.
static void check_one_tpu(const marvel_layout *layout, int chips,
                          const std::vector<u32> &want) {
  CMarvelTopology t;
  CHECK(t.build(layout, chips, 1));
  CHECK(pids(t) == want);
  CHECK(t.shift() == 0);
  for (int i = 0; i < t.count(); i++)
    CHECK(t.has_memory(t.node(i).pid, t.count()));
}

// A chip whose PID uses every pair of bits: 0xAA, bits 1, 3, 5 and 7, at
// NS 6, EW 9 by the GS1280's rule (coord2id).
static void odd_coordinates(int, u8 *ns, u8 *ew) {
  *ns = 6;
  *ew = 9;
}
static bool no_io7(u32) { return false; }
static u8 drawer0(u32) { return 0; }

int main() {
  // The ES47 (the topology's default layout): chips at PIDs 0 and 1.
  check_one_tpu(nullptr, 2, {0, 1});
  check_layout("es47", nullptr, 2, 4, 1, 8);
  check_layout("es47", nullptr, 2, 2, 1, 4);
  {
    CMarvelTopology t;
    t.build(nullptr, 2, 4);
    CHECK((pids(t) == std::vector<u32>{0, 2, 4, 6, 1, 3, 5, 7}));
    CHECK(t.has_io7(0) && !t.has_io7(2) && !t.has_io7(1));
  }
  // The ES80: chips at PIDs 0, 1, 8, 9, 16, 17, 24, 25; TPUs in the module
  // field, as on the ES47.
  check_one_tpu(&es80_layout, 8, {0, 1, 8, 9, 16, 17, 24, 25});
  check_layout("es80, PIDs 0 1 8 9", &es80_layout, 4, 4, 1, 16);
  check_layout("es80", &es80_layout, 8, 4, 1, 32);
  {
    CMarvelTopology t;
    t.build(&es80_layout, 4, 4);
    CHECK(t.has_io7(0) && t.has_io7(8) && !t.has_io7(10) && !t.has_io7(1));
  }
  // The GS1280: chips at PIDs 0-15, every PID of a drawer; the TPUs above
  // the configured chips' bits.
  check_one_tpu(&gs1280_layout, 4, {0, 1, 2, 3});
  check_layout("gs1280", &gs1280_layout, 4, 4, 2, 16);
  check_layout("gs1280", &gs1280_layout, 8, 4, 3, 32);
  check_layout("gs1280", &gs1280_layout, 16, 2, 4, 32);
  check_layout("gs1280", &gs1280_layout, 5, 4, 3, 32);
  // No free pair: refused, with no processors laid out.
  const marvel_layout odd = {0x1, odd_coordinates, no_io7, 0, 0, drawer0};
  CHECK(CMarvelTopology::chip_pid(&odd, 0) == 0xaa);
  CHECK(CMarvelTopology::tpu_shift(&odd, 1) == -1);
  {
    CMarvelTopology t;
    CHECK(!t.build(&odd, 1, 2));
    CHECK(t.count() == 0);
    CHECK(t.build(&odd, 1, 1) && t.count() == 1 && t.node(0).pid == 0xaa);
  }
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
