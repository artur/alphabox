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

/* Unit test of Tarantula's section of the state file (src/cpu/Tarantula.cpp;
 * docs/platforms/ev8.md, S7), the one piece of the vector state no
 * instruction reaches: a section written and read back gives every
 * register, element, vl, vs, the mask and VCTL<VEN>; a file with no section
 * (one written before S7, or by a row without Tarantula) restores nothing
 * and leaves the file where it was, so the next component reads its own
 * data; a section that is truncated or damaged is refused. Built with every
 * lane as `tarantula_state_test`; exit status 0 when every check passes.
 */

#include "Tarantula.hpp"

#include <cstdio>
#include <memory>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

using tarantula::State;

static u64 pattern(int n, int i) {
  return (U64(0x9E3779B97F4A7C15) * (u64)(n * 128 + i + 1)) ^ (u64)n << 56;
}

static void fill(State &s) {
  for (int n = 0; n < tarantula::kRegs - 1; n++)
    for (int i = 0; i < tarantula::kElems; i++)
      s.vr[n][i] = pattern(n, i);
  for (int i = 0; i < tarantula::kElems; i++)
    s.vr[tarantula::kRegs - 1][i] = 0;
  s.vl = 77;
  s.vs = U64(0xFFFFFFFFFFFFFFF8); // -8: a signed byte stride
  s.vm[0] = U64(0x0123456789ABCDEF);
  s.vm[1] = U64(0xF0E1D2C3B4A59687);
}

static bool same(const State &a, const State &b) {
  return memcmp(&a, &b, sizeof(State)) == 0;
}

int main() {
  std::unique_ptr<State> a(new State), b(new State);
  a->clear();
  fill(*a);

  // A section, then another component's data (a word that is not the
  // magic), read back in order.
  {
    FILE *f = tmpfile();
    CHECK(f != nullptr);
    if (!f)
      return 1;
    CHECK(tarantula::save(f, *a, 1) == 0);
    const long end = ftell(f);
    CHECK(end == 4 + 4 + (long)tarantula::kPayload + 4);
    const unsigned next = 0xC0FFEE11;
    fwrite(&next, sizeof(next), 1, f);
    rewind(f);
    b->clear();
    u64 vctl = 0;
    CHECK(tarantula::restore(f, *b, &vctl) == 1);
    CHECK(same(*a, *b));
    CHECK(vctl == 1);
    CHECK(ftell(f) == end);
    unsigned got = 0;
    CHECK(fread(&got, sizeof(got), 1, f) == 1 && got == next);
    // VCTL<63:1> are RAZ: a value with them set comes back as VEN alone.
    rewind(f);
    CHECK(tarantula::save(f, *a, U64(0xFFFFFFFFFFFFFFFE)) == 0);
    rewind(f);
    vctl = 5;
    CHECK(tarantula::restore(f, *b, &vctl) == 1 && vctl == 0);
    fclose(f);
  }

  // No section: the next component's data where the section would be, and
  // the end of the file. Nothing is restored, the file stays where it was.
  {
    FILE *f = tmpfile();
    const unsigned other[2] = {0x2126468C, 0x12345678}; // a CPU block's magic
    fwrite(other, sizeof(other), 1, f);
    rewind(f);
    b->clear();
    u64 vctl = 7;
    CHECK(tarantula::restore(f, *b, &vctl) == 0);
    CHECK(ftell(f) == 0);
    CHECK(vctl == 7);
    State zero;
    zero.clear();
    CHECK(same(*b, zero));
    unsigned got[2] = {0, 0};
    CHECK(fread(got, sizeof(got), 1, f) == 1 && got[0] == other[0] &&
          got[1] == other[1]);
    // At the end of the file (the processor is the last component).
    CHECK(tarantula::restore(f, *b, &vctl) == 0);
    CHECK(ftell(f) == (long)sizeof(other));
    fclose(f);
  }

  // Truncated, and damaged: refused.
  {
    FILE *f = tmpfile();
    CHECK(tarantula::save(f, *a, 1) == 0);
    const long whole = ftell(f);
    fclose(f);
    for (long cut : {8L, 100L, whole - 4, whole - 1}) {
      FILE *g = tmpfile();
      tarantula::save(g, *a, 1);
      fflush(g);
      // Copy the first `cut` bytes into a file of their own.
      rewind(g);
      std::unique_ptr<char[]> buf(new char[whole]);
      CHECK(fread(buf.get(), 1, (size_t)whole, g) == (size_t)whole);
      fclose(g);
      FILE *h = tmpfile();
      fwrite(buf.get(), 1, (size_t)cut, h);
      rewind(h);
      u64 vctl = 0;
      CHECK(tarantula::restore(h, *b, &vctl) == -1);
      fclose(h);
    }
    // A vl above 128.
    State bad = *a;
    bad.vl = 129;
    FILE *g = tmpfile();
    tarantula::save(g, bad, 1);
    rewind(g);
    u64 vctl = 0;
    CHECK(tarantula::restore(g, *b, &vctl) == -1);
    fclose(g);
  }

  // $vr31 reads as zero after a restore, whatever the file holds.
  {
    State odd = *a;
    for (int i = 0; i < tarantula::kElems; i++)
      odd.vr[tarantula::kRegs - 1][i] = ~U64(0);
    FILE *f = tmpfile();
    tarantula::save(f, odd, 0);
    rewind(f);
    u64 vctl = 1;
    CHECK(tarantula::restore(f, *b, &vctl) == 1);
    bool zero = true;
    for (int i = 0; i < tarantula::kElems; i++)
      zero = zero && b->vr[tarantula::kRegs - 1][i] == 0;
    CHECK(zero);
    CHECK(b->vr[5][9] == pattern(5, 9) && vctl == 0);
    fclose(f);
  }

  // The mask helpers.
  {
    State s;
    s.clear();
    s.set_mask(0, true);
    s.set_mask(63, true);
    s.set_mask(64, true);
    s.set_mask(127, true);
    CHECK(s.vm[0] == (U64(1) | U64(1) << 63));
    CHECK(s.vm[1] == (U64(1) | U64(1) << 63));
    CHECK(s.mask(127) && s.mask(64) && !s.mask(65) && !s.mask(1));
    s.set_mask(63, false);
    CHECK(!s.mask(63) && s.vm[0] == 1);
  }

  printf("tarantula_state_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
