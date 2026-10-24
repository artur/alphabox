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

/* Unit test of the processor rows (src/cpu/CpuModels.cpp): the values
 * software reads to tell one part from another, against their sources.
 * IMPLVER is the Alpha Architecture Reference Manual's Table D-4 (4th ed.,
 * Appendix D.3): 2 for the 21264 family (EV6, EV67, EV68x), 3 for the
 * 21364 (EV7, EV79); 4 for the 21464 (EV8), which the ARM does not list,
 * is its Internal Design Specification's (Rev 1.1k, PDF p. 76). The EV8
 * row is of the EV7 family (its EV7-compatible mode), so the EV7 boards
 * take it, and has the EV8's extensions. Both engines execute IMPLVER from
 * the row
 * (cpu_misc.hpp, and the JIT's set_cpu_identity), so the row is what a
 * guest sees. Built with every lane as `cpumodels_test`; exit status 0 when
 * every check passes.
 */

#include "CpuModel.hpp"

#include <cstdio>

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

/// One row: present, of its family, with the IMPLVER of Table D-4 and the
/// extensions beyond its family's core.
static void check_row(const char *name, cpu_family family, int implver,
                      u32 ext = CPU_EXT_NONE) {
  const cpu_model *m = find_cpu_model(name);
  CHECK_EQ(m != nullptr, 1);
  if (!m)
    return;
  CHECK_EQ(m->family, family);
  CHECK_EQ(m->implver, implver);
  CHECK_EQ(m->ext, ext);
}

int main() {
  check_row("ev68cb", CPU_FAMILY_EV6, 2); // 21264/EV68x
  check_row("ev7", CPU_FAMILY_EV7, 3);    // 21364/EV7
  check_row("ev7z", CPU_FAMILY_EV7, 3);   // 21364/EV79
  check_row("ev8", CPU_FAMILY_EV7, 4, CPU_EXT_EV8); // 21464/EV8
  // The EV8 reports only what it implements of the IDS's 0x1F07: never
  // more, and the bits of every stage not yet done are clear (S3: bit 10,
  // S5-S6: bit 11, Tarantula: bit 3).
  if (const cpu_model *m = find_cpu_model("ev8")) {
    CHECK_EQ(m->amask & ~U64(0x1F07), 0);
    CHECK_EQ(m->amask & U64(0x0C08), 0);
  }
  CHECK_EQ(find_cpu_model("ev5") == nullptr, 1);
  printf("cpumodels: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
