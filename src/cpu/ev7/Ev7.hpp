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
 * The map is a value chosen per machine (CMarvel::map): the EV7's, or the
 * one a Marvel machine of 21464 processors has in 21464 mode
 * (docs/platforms/ev8-native.md, section 2.1).
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

/**
 * A Marvel machine's physical address map: where each processor's I/O
 * space, register block, IO7 ports and memory are. Two exist:
 *
 *  - kEv7Map, the EV7's: 44 physical address bits (PA<63:44> are ignored:
 *    the console's PALcode forms CSR addresses sign-extended, its C code
 *    does not); the PE number inverted in PA<43:35> (EV7_IPE), whose top
 *    bit is therefore the I/O bit: PEs 0-255 are I/O space, and memory,
 *    whose PA<43> is 0, carries its PID elsewhere: PID<1:0> in PA<35:34>
 *    and PID<6:2> in PA<41:37>, 16 GB apart for the first four (v4.19
 *    marvel_node_mem_start; real listings put PID 1's memory at
 *    0x4_0000_0000);
 *  - k21464Map, the 21464 mode's (docs/platforms/ev8-native.md, 2.1,
 *    decision N18): the same map with the inverted field widened to the
 *    top of the 48-bit PA [21464 IDS Rev 1.1k, pp. 43, 298]: ipe8(n) = (~n
 *    & 0x1fff) << 35, so PA<47> is the I/O bit, PA<46:45> are 11 for every
 *    10-bit PID and PA<44:35> hold ~PID; the memory of PID n at ((n & 3) |
 *    (n & 0x3fc) << 1) << 34, the EV7's for PIDs below 128. For PIDs 0-31
 *    the low 41 bits of every address are the EV7's and PA<47:41> all ones
 *    for I/O, all zeros for memory: what the 21464's SPE43 superpage
 *    reaches from Linux's KSEG arithmetic unchanged [IDS pp. 688-689].
 *
 * In both, a processor's register block is the 4 MB at PA<34:22> all ones
 * in its own I/O space (EV7_CSR_PHYS), and an IO7 port's space has the port
 * inverted in PA<34:32> (IO7_IPE in core_marvel.h); port 7 is the IO7's
 * own registers.
 */
struct address_map {
  unsigned pa_bits;  ///< physical address bits decoded: 44 or 48
  unsigned io_bit;   ///< the I/O bit: 43 or 47
  unsigned pe_bits;  ///< the inverted PE field, PA<io_bit:35>: 9 or 13
  unsigned pid_bits; ///< the PIDs that exist: 8 (the console's) or 10
  /// The PID bits above <1:0> that place memory, from PA<37> up: <6:2>
  /// (0x7c) on the EV7, <9:2> (0x3fc) in 21464 mode.
  u32 memory_pid_mask;

  static constexpr u64 kCsrSize = U64(0x400000); ///< a register block

  /// The physical address bits decoded.
  constexpr u64 phys_mask() const { return (U64(1) << pa_bits) - 1; }
  /// PE `pe`'s I/O space: the PE inverted in PA<io_bit:35>.
  constexpr u64 ipe(u32 pe) const {
    return (u64)(~pe & ((1u << pe_bits) - 1)) << 35;
  }
  /// The I/O bit.
  constexpr bool is_io(u64 pa) const { return (pa >> io_bit) & 1; }
  /// Where PID `pid`'s memory starts.
  constexpr u64 memory_base(u32 pid) const {
    return (u64)((pid & 3) | ((pid & memory_pid_mask) << 1)) << 34;
  }
  /// The PID whose memory holds `pa` (memory space only).
  constexpr u32 memory_pid(u64 pa) const {
    return (u32)(((pa >> 34) & 3) |
                 (((pa >> 37) & (memory_pid_mask >> 2)) << 2));
  }
  /// PE `pe`'s register block: PE 0's is 0xFFF_FFC0_0000 on the EV7,
  /// 0xFFFF_FFC0_0000 in 21464 mode.
  constexpr u64 csr_base(u32 pe) const { return ipe(pe) | (U64(0x7ffc) << 20); }
  /// The PE whose I/O space `pa` is in; false when `pa` is not I/O space or
  /// its inverted field names no PID (in 21464 mode: PA<46:45> not 11).
  bool io_pe(u64 pa, u32 *pe) const {
    if (!is_io(pa))
      return false;
    *pe = (u32)(~(pa >> 35) & ((1u << pe_bits) - 1));
    return (*pe >> pid_bits) == 0;
  }
  /// Is `pa` in some processor's register block? Gives the PE and the
  /// offset.
  bool csr_decode(u64 pa, u32 *pe, u32 *off) const {
    if (((pa >> 22) & 0x1fff) != 0x1fff || !io_pe(pa, pe))
      return false;
    *off = (u32)(pa & (kCsrSize - 1));
    return true;
  }
  /// An IO7 port's space.
  constexpr u64 io7_base(u32 pe, u32 port) const {
    return ipe(pe) | ((u64)(~port & 7) << 32);
  }
  /// Is `pa` in some PE's I/O space? Gives the PE, the port (PA<34:32>
  /// inverted: 0-3 the PCI ports, 7 the IO7's registers) and the offset in
  /// the port's 4 GB.
  bool io7_decode(u64 pa, u32 *pe, u32 *port, u32 *off) const {
    if (!io_pe(pa, pe))
      return false;
    *port = (u32)(~(pa >> 32) & 7);
    *off = (u32)pa;
    return true;
  }
};

/// The EV7's map.
constexpr address_map kEv7Map = {44, 43, 9, 8, 0x7c};
/// The 21464 mode's map (N18).
constexpr address_map k21464Map = {48, 47, 13, 10, 0x3fc};

/// The largest memory one PID can own before it runs into the next one's,
/// in either map: 16 GB (memory.bits 34). N18's second 16 GB of a 21464
/// PID, at + 2^36, is not modelled [decision, ev8-native.md S11b].
constexpr u64 kMemorySlot = U64(1) << 34;

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
