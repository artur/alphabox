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

/**
 * \file
 * Tarantula's section of the state file (Tarantula.hpp). No version step:
 * the section is optional, recognised by its magic word, so a state file
 * written before it existed (main's, before S7) restores with the vector
 * state at its reset values, and rows without Tarantula write none
 * [decision, docs/platforms/ev8.md S7].
 **/
#include "Tarantula.hpp"

namespace tarantula {

namespace {
constexpr u32 kMagic1 = 0x54415231; // "TAR1"
constexpr u32 kMagic2 = 0x31524154;

bool put(FILE *f, const void *p, size_t n) { return fwrite(p, n, 1, f) == 1; }
bool get(FILE *f, void *p, size_t n) { return fread(p, n, 1, f) == 1; }
} // namespace

int save(FILE *f, const State &s, u64 vctl) {
  const u32 size = kPayload;
  bool ok = put(f, &kMagic1, sizeof(kMagic1)) && put(f, &size, sizeof(size)) &&
            put(f, &vctl, sizeof(vctl)) && put(f, &s.vl, sizeof(s.vl)) &&
            put(f, &s.vs, sizeof(s.vs)) && put(f, s.vm, sizeof(s.vm));
  for (int n = 0; ok && n < kRegs; n++)
    ok = put(f, s.vr[n], sizeof(s.vr[n]));
  ok = ok && put(f, &kMagic2, sizeof(kMagic2));
  return ok ? 0 : -1;
}

int restore(FILE *f, State &s, u64 *vctl) {
  const long at = ftell(f);
  u32 m1 = 0;
  if (!get(f, &m1, sizeof(m1)) || m1 != kMagic1) {
    // Not ours: the next component's first word, or the end of the file.
    clearerr(f);
    if (at < 0 || fseek(f, at, SEEK_SET) != 0)
      return -1;
    return 0;
  }
  u32 size = 0, m2 = 0;
  State t;
  u64 v = 0;
  if (!get(f, &size, sizeof(size)) || size != kPayload ||
      !get(f, &v, sizeof(v)) || !get(f, &t.vl, sizeof(t.vl)) ||
      !get(f, &t.vs, sizeof(t.vs)) || !get(f, t.vm, sizeof(t.vm)))
    return -1;
  for (int n = 0; n < kRegs; n++)
    if (!get(f, t.vr[n], sizeof(t.vr[n])))
      return -1;
  if (!get(f, &m2, sizeof(m2)) || m2 != kMagic2 || t.vl > kMaxVl)
    return -1;
  memset(t.vr[kRegs - 1], 0, sizeof(t.vr[kRegs - 1])); // $vr31 reads zero
  s = t;
  *vctl = v & kVen; // <63:1> RAZ
  return 1;
}

} // namespace tarantula
