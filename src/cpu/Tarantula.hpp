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
 * Tarantula, the vector extension proposed for the EV8: one processor's
 * (one TPU's) architectural state, and its section of the state file.
 *
 * The contract is docs/platforms/ev8-isa.md, section 4 (agreed with the
 * nada toolchain on 2026-10-25); the paper is Espasa et al., "Tarantula: A
 * Vector Extension to the Alpha Architecture", ISCA 2002 [TAR p. n]. The
 * instructions are AlphaCPU_vector.cpp's. Only the rows with
 * CPU_EXT_TARANTULA (`ev8`, `ev8n`) have this state; every other row's
 * processor and state file are as they were.
 **/
#if !defined(INCLUDED_TARANTULA_H_)
#define INCLUDED_TARANTULA_H_

#include "StdAfx.hpp"

#include <cstdio>
#include <cstring>

namespace tarantula {

constexpr int kRegs = 32;   ///< $vr0-$vr31 [TAR pp. 282-283]
constexpr int kElems = 128; ///< 64-bit elements per register [TAR p. 282]
constexpr u64 kMaxVl = 128; ///< vl runs from 0 to 128 (ev8-isa.md 4.1)

/// VCTL<VEN>: the Tarantula instructions execute (ev8-isa.md, section 2).
constexpr u64 kVen = 1;
/// VCTL's offset in each processor's own EV7 register window, the 4 MB block
/// of its PID [decision, the owner's, 2026-10-25; ev8-isa.md, section 2].
constexpr u32 kVctlOffset = 0x3F0000;

/// One processor's vector state (ev8-isa.md 4.1). Cleared at power-up and
/// by a reset: the paper leaves the values UNPREDICTABLE, Alphabox clears
/// them all.
struct State {
  /// The registers, untyped: an element is a quadword or a T float as the
  /// instruction reads it. vr[31] is never written, so $vr31 reads as zero.
  u64 vr[kRegs][kElems];
  u64 vl;    ///< the vector length, 0 to 128
  u64 vs;    ///< the stride, a signed byte distance
  u64 vm[2]; ///< the mask: element i's bit is vm[i / 64] bit i % 64

  bool mask(int i) const { return (vm[i >> 6] >> (i & 63)) & 1; }
  void set_mask(int i, bool on) {
    const u64 bit = U64(1) << (i & 63);
    vm[i >> 6] = on ? (vm[i >> 6] | bit) : (vm[i >> 6] & ~bit);
  }
  void clear() { memset(this, 0, sizeof(*this)); }
};

/// The translations a vector memory instruction (SM, RM) had made when one
/// of its elements faulted, kept for its restart: forward progress, which
/// ev8-isa.md section 5 leaves to the emulator. The instruction restarts
/// from its first element and translates every active element again; an
/// element whose address is the one kept takes the kept translation
/// instead of the TB, so each fault the handler resolves moves the
/// instruction on by at least one element, whatever the TB evicted
/// meanwhile -- a strided access may need 128 pages, and an EV7 PALcode's
/// miss handler takes TB entries of its own for the page tables.
/// [decision] Not a lane TLB: it serves only the re-execution of the same
/// word at the same PC, in the same mode and address space, and every TB
/// invalidation drops it (CAlphaCPU::m_vec_xlate_gen), as it does the TB
/// shadow's entries; so software sees the TB it filled, and no more
/// misses than one per element. Not architectural state: a restored state
/// starts without it, and the instruction translates again.
struct Restart {
  bool valid = false;
  u64 pc = 0;
  u32 ins = 0;
  int cm = 0;
  int asn = 0;
  u64 gen = 0;
  u64 have[2] = {0, 0}; ///< element i kept: bit i % 64 of have[i / 64]
  u64 va[kElems];
  u64 pa[kElems];

  bool has(int i) const { return (have[i >> 6] >> (i & 63)) & 1; }
  void put(int i, u64 v, u64 p) {
    va[i] = v;
    pa[i] = p;
    have[i >> 6] |= U64(1) << (i & 63);
  }
  void clear() {
    valid = false;
    have[0] = have[1] = 0;
  }
};

/// The state file's section, after the processor's own block (and, on the
/// native EV8, after its IPRs): a magic word, the payload's size, VCTL (the
/// CSR's value; the native EV8 keeps its VCTL among its IPRs), vl, vs, the
/// mask's two quadwords and the 32 registers, each field written alone, then
/// a second magic word. Returns 0, or -1 when a write failed.
int save(FILE *f, const State &s, u64 vctl);

/// Read the section where `f` stands. 1: read. 0: there is none here (a
/// state file written before Tarantula existed, or by a build without it):
/// the file is left where it was, the next component's data, and `s` and
/// `vctl` untouched. -1: a section that is damaged or truncated.
int restore(FILE *f, State &s, u64 *vctl);

/// The payload's size in bytes, as the section records it.
constexpr u32 kPayload = 5 * 8 + kRegs * kElems * 8;

} // namespace tarantula

#endif // !defined(INCLUDED_TARANTULA_H_)
