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

#if !defined(INCLUDED_IRQSTATS_H)
#define INCLUDED_IRQSTATS_H

#include <cstdint> // datatypes.hpp needs the fixed-width types

#include "datatypes.hpp"
#include <atomic>

// Interrupt-rate counters: bumped where interrupts are raised and taken,
// printed and reset every 5 s by the Ali thread under ALPHABOX_IRQSTATS=1.
struct SIrqStats {
  std::atomic<u64> cpu_int{0};      // CPU interrupt entries (PAL INTERRUPT)
  std::atomic<u64> cpu_eir[6]{};    // ...by EIR & EIEN bit pending at entry
  std::atomic<u64> cpu_sw{0};       // ...with a software interrupt pending
  std::atomic<u64> cpu_ast{0};      // ...with an AST pending
  std::atomic<u64> cchip_timer{0};  // Cchip interval-timer ticks
  std::atomic<u64> drir_rise[64]{}; // Cchip DRIR bits going 0 -> 1
  std::atomic<u64> isa_edge[16]{};  // 8259 input edges (PIC0 0-7, PIC1 8-15)
  std::atomic<u64> isa_ack[16]{};   // 8259 IRQs acknowledged by the guest
};
inline SIrqStats g_irqstats;

#endif // !defined(INCLUDED_IRQSTATS_H)
