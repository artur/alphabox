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
 * The PCI-to-system DMA windows of the 21272 family: four windows per hose,
 * each a base (WSBAn), a mask (WSMn) and a translated base (TBAn), direct
 * mapped or scatter-gather through 8 KB page table entries in memory. The
 * Tsunami/Typhoon Pchip and the Titan PA-chip ports translate the same way
 * (Tsunami HRM 10.1.4; Titan: Linux core_titan.c sets them up identically),
 * so both use these. The registers stay in each chipset's own state.
 **/
#if !defined(INCLUDED_PCI_WINDOWS_H_)
#define INCLUDED_PCI_WINDOWS_H_

#include "StdAfx.hpp"

namespace pci_window {

constexpr u64 WSM_MASK = U64(0x00000000fff00000);     ///< WSMn <31:20>
constexpr u64 ADD_MASK = U64(0x00000000000fffff);     ///< <19:0>
constexpr u64 TBA_MASK = U64(0x00000007fff00000);     ///< <34:20>
constexpr u64 PTE_ADD_MASK = U64(0x00000000000fe000); ///< <19:13>
constexpr int PTE_ADD_SHIFT = 10;
constexpr u64 PTE_TBA_MASK = U64(0x00000007fffffc00); ///< <34:10>
constexpr u64 PTE_MASK = U64(0x00000007ffffe000);     ///< <34:13>
constexpr int PTE_SHIFT = 12;
constexpr u64 PTE_ADD2_MASK = U64(0x0000000000001fff); ///< <12:0>
constexpr u64 PTE_PEER_BIT = U64(0x0000000090000000);  ///< <31,28>
constexpr u64 PIO_ACCESS = U64(0x0000080000000000);    ///< <43>

/// Whether PCI address `address` falls in the enabled window `wsba`/`wsm`.
inline bool hit(u64 wsba, u64 wsm, u32 address) {
  return (wsba & 1) && !((address ^ wsba) & 0xfff00000 & ~wsm);
}

/// Direct-mapped translation (Tsunami HRM 10.1.4.2).
inline u64 direct(u32 address, u64 wsm, u64 tba) {
  wsm &= WSM_MASK;
  return (address & (wsm | ADD_MASK)) | (tba & ~wsm & TBA_MASK);
}

/// Where the page table entry for `address` is (HRM 10.1.4.3).
inline u64 pte_address(u32 address, u64 wsm, u64 tba) {
  wsm &= WSM_MASK;
  return ((address & (wsm | PTE_ADD_MASK)) >> PTE_ADD_SHIFT) |
         (tba & PTE_TBA_MASK & ~(wsm >> PTE_ADD_SHIFT));
}

/// The system address a valid PTE gives `address`; a PTE with bit 31 or 28
/// set points peer to peer, into PIO space.
inline u64 from_pte(u64 pte, u32 address) {
  u64 a = ((pte << PTE_SHIFT) & PTE_MASK) | (address & PTE_ADD2_MASK);
  if (pte & PTE_PEER_BIT)
    a |= PIO_ACCESS;
  return a;
}

} // namespace pci_window

#endif // !defined(INCLUDED_PCI_WINDOWS_H_)
