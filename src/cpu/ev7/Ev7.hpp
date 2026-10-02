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
 * What the EV7 (21364) changes in the Alpha core: its physical address map
 * and the state a processor is in when the console first runs on it.
 *
 * The core is the EV68's -- same IPRs, same PALcode instructions -- so the
 * processor class is the same (docs/platforms.md, "Source layout"). What
 * differs is that the system logic sits on the chip: every processor has
 * its own memory, its own 4 MB block of registers (Rbox, Cbox, Zbox, GIO,
 * chipsets/marvel/) and its own I/O port, and each is addressed by the
 * processor's PID.
 *
 * Sources: Linux core_marvel.h (EV7_IPE, EV7_CSR_PHYS, IO7_IPE) and v4.19
 * marvel_node_mem_start; the console's own code for the rest
 * (docs/platforms/marvel.md).
 **/
#if !defined(INCLUDED_EV7_H_)
#define INCLUDED_EV7_H_

#include "StdAfx.hpp"

class CAlphaCPU;

namespace ev7 {

/// The EV7 decodes 44 physical address bits; PA<63:44> are ignored (the
/// console's PALcode forms CSR addresses sign-extended, its C code does not).
constexpr u64 kPhysMask = U64(0xFFFFFFFFFFF);

/// The processor-ID field: PA<43:35> holds the PE number inverted
/// (EV7_IPE). Its top bit is therefore the I/O bit: PEs 0-255 are I/O space,
/// and memory, whose PA<43> is 0, carries its PID elsewhere.
constexpr u64 ipe(u32 pe) { return (u64)(~pe & 0x1ff) << 35; }

/// The I/O bit.
constexpr bool is_io(u64 pa) { return (pa >> 43) & 1; }

/// Where PID `pid`'s memory starts: PID<1:0> in PA<35:34> and PID<6:2> in
/// PA<41:37>, 16 GB apart for the first four (v4.19 marvel_node_mem_start;
/// real listings put PID 1's memory at 0x4_0000_0000).
constexpr u64 memory_base(u32 pid) {
  return (u64)((pid & 3) | ((pid & 0x7c) << 1)) << 34;
}

/// The largest memory one PID can own before it runs into the next one's.
constexpr u64 kMemorySlot = U64(1) << 34;

/// The PID whose memory holds `pa` (memory space only).
constexpr u32 memory_pid(u64 pa) {
  return (u32)(((pa >> 34) & 3) | (((pa >> 37) & 0x1f) << 2));
}

/// Each processor's register block: 4 MB at PA<34:22> all ones in its own
/// I/O space (EV7_CSR_PHYS). PE 0's is 0xFFF_FFC0_0000.
constexpr u64 kCsrSize = U64(0x400000);
constexpr u64 csr_base(u32 pe) { return ipe(pe) | (U64(0x7ffc) << 20); }

/// Is `pa` in some processor's register block? Gives the PE and the offset.
inline bool csr_decode(u64 pa, u32 *pe, u32 *off) {
  if (!is_io(pa) || ((pa >> 22) & 0x1fff) != 0x1fff)
    return false;
  *pe = (u32)(~(pa >> 35) & 0xff);
  *off = (u32)(pa & (kCsrSize - 1));
  return true;
}

/// An IO7 port's space: PA<34:32> holds the port inverted (IO7_IPE in
/// core_marvel.h); port 7 is the IO7's own registers.
constexpr u64 io7_base(u32 pe, u32 port) {
  return ipe(pe) | ((u64)(~port & 7) << 32);
}

/**
 * Leave `cpu` as the XSROM leaves it when it jumps to the console: the
 * registers the console's decompressor and PALcode read at entry. `pid` is
 * the processor's PID; `decompressor_base`, when not 0, is where the
 * console's self-decompressor was entered, which is what it passes on in
 * r19 (see Ev7Reset.cpp for what each register means and where that was
 * found).
 */
void xsrom_handoff(CAlphaCPU *cpu, u32 pid, u64 decompressor_base);

} // namespace ev7

#endif // !defined(INCLUDED_EV7_H_)
