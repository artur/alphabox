/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2026 gdwnldsKSC and the ES40-Emu contributors
 * Website: https://github.com/ES40-Emu/es40
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
 * Contains the instruction dispatch macros used by AlphaCPU.cpp
 **/

#ifdef ES40_JIT
#define ES40_EXECUTE_END() return
#else
#define ES40_EXECUTE_END() goto _next_instruction
#endif

#if defined(DEBUG_INSTALL_VMFAULT)
#define DEBUG_INSTALL_PAL_TRAP(offset) debug_trace_user_pal_trap(offset)
#else
#define DEBUG_INSTALL_PAL_TRAP(offset) ((void)0)
#endif

#define GO_PAL(offset)                                                         \
  {                                                                            \
    DEBUG_INSTALL_PAL_TRAP(offset);                                            \
    state.exc_addr = state.current_pc;                                         \
    set_pc(state.pal_base | offset | 1);                                       \
    /* HRM 4.2.4: clear lock_flag on taken exception/interrupt (fail a         \
       pending STx_C). Exclude transparent TB-miss fills; offset is a          \
       compile-time constant so this condition folds to nothing per site. */   \
    if constexpr ((offset) != DTBM_SINGLE && (offset) != DTBM_DOUBLE_3 &&      \
                  (offset) != DTBM_DOUBLE_4 && (offset) != ITB_MISS)           \
      cSystem->cpu_clear_lock(state.iProcNum);                                 \
  }

/* HRM 6.8.2 Table 6-8: OPCDEC fault is raised for opcodes 1-7, the PALRES
 * opcodes (0x19/0x1B/0x1D/0x1E/0x1F) when not in PALmode and HWE is clear,
 * for unimplemented function-field encodings of opcodes 0x14 and 0x1C, and
 * for invalid CALL_PAL function values. brokenpipe (alphafix) routes all of
 * these through the same `unallocated_encoding()` -> EXCP_OPCDEC path.
 *
 * The warning is intentional diagnostic noise: if a truly missing opcode case
 * exists in es40's dispatch, it surfaces in the output even though the
 * architectural OPCDEC trap also fires. It is printed once per opcode and
 * function (report_opcdec), so a guest that takes the trap on purpose does
 * not flood the output. */
#define UNKNOWN1                                                               \
  report_opcdec(opcode, -1);                                                   \
  GO_PAL(OPCDEC);                                                              \
  ES40_EXECUTE_END();

#define UNKNOWN2                                                               \
  report_opcdec(opcode, function);                                             \
  GO_PAL(OPCDEC);                                                              \
  ES40_EXECUTE_END();

// Execute the DO_<mnemonic> macro for an instruction. The format names the
// operand layout and generates nothing.
#define OP(mnemonic, format)                                                   \
  DO_##mnemonic;                                                               \
  ES40_EXECUTE_END();

// Execute a function rather than a DO_<mnemonic> macro for an instruction
#define OP_FNC(mnemonic, format)                                               \
  mnemonic();                                                                  \
  ES40_EXECUTE_END();
