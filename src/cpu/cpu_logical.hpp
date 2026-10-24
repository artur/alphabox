/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

/**
 * \file
 * Contains code macros for the processor logical instructions.
 * Based on ARM chapter 4.5.
 **/
#define DO_AND state.r[REG_3] = state.r[REG_1] & V_2;
#define DO_BIC state.r[REG_3] = state.r[REG_1] & ~V_2;
#define DO_BIS state.r[REG_3] = state.r[REG_1] | V_2;
#define DO_EQV state.r[REG_3] = state.r[REG_1] ^ ~V_2;
#define DO_ORNOT state.r[REG_3] = state.r[REG_1] | ~V_2;
#define DO_XOR state.r[REG_3] = state.r[REG_1] ^ V_2;

#define DO_CMOVEQ                                                              \
  if (!state.r[REG_1])                                                         \
    state.r[REG_3] = V_2;
#define DO_CMOVGE                                                              \
  if ((s64)state.r[REG_1] >= 0)                                                \
    state.r[REG_3] = V_2;
#define DO_CMOVGT                                                              \
  if ((s64)state.r[REG_1] > 0)                                                 \
    state.r[REG_3] = V_2;
#define DO_CMOVLBC                                                             \
  if (!(state.r[REG_1] & U64(0x1)))                                            \
    state.r[REG_3] = V_2;
#define DO_CMOVLBS                                                             \
  if (state.r[REG_1] & U64(0x1))                                               \
    state.r[REG_3] = V_2;
#define DO_CMOVLE                                                              \
  if ((s64)state.r[REG_1] <= 0)                                                \
    state.r[REG_3] = V_2;
#define DO_CMOVLT                                                              \
  if ((s64)state.r[REG_1] < 0)                                                 \
    state.r[REG_3] = V_2;
#define DO_CMOVNE                                                              \
  if (state.r[REG_1])                                                          \
    state.r[REG_3] = V_2;

// CMOV2 (21464, CPU_EXT_EV8): the second half of a CMOVxx, which the 21464
// splits at Icache fill time. Its first operand's predicate bit says
// whether to take Rbv, and is set only between the two halves; executed
// from memory it is clear, so Rc <- Rav, and the CMOV2 Rc,Rb,Rc the chip
// makes copies Rc to Rc [21464 IDS Rev 1.1k, PDF pp. 73-74;
// docs/platforms/ev8-isa.md 3.2].
#define DO_CMOV2 state.r[REG_3] = state.r[REG_1];

#define DO_SLL state.r[REG_3] = state.r[REG_1] << (V_2 & 63);
#define DO_SRA                                                                 \
  state.r[REG_3] =                                                             \
      (V_2 & 63)                                                               \
          ? ((state.r[REG_1] >> (V_2 & 63)) |                                  \
             ((state.r[REG_1] >> 63) ? (X64_QUAD << (64 - (V_2 & 63))) : 0))   \
          : state.r[REG_1];
#define DO_SRL state.r[REG_3] = state.r[REG_1] >> (V_2 & 63);
