/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2025-2026 gdwnldsKSC and the ES40-Emu contributors
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
 * Contains code macros for miscellaneous processor instructions.
 * Based on ARM chapter 4.11.
 **/
#include <atomic>

#define DO_AMASK                                                               \
  do {                                                                         \
    /* RA must be R31 per AHB / QEMU */                                        \
    if (REG_1 != 31) {                                                         \
      GO_PAL(OPCDEC);                                                          \
    } else {                                                                   \
      state.r[REG_3] = V_2 & ~m_model->amask;                                  \
    }                                                                          \
  } while (0)

/* The linkage register is the PALshadow R23 whatever I_CTL[SDE] says (HRM
   6.8.1: "PALshadow register R23 is used as the linkage register"). */
#define SAVE_CALL_PAL_R23()                                                    \
  do {                                                                         \
    state.r[32 + 23] = state.pc & ~U64(0x2);                                   \
  } while (0)

/* CALL_PAL is not an exception: EXC_ADDR keeps the PC of the last exception
   or interrupt (HRM 5.2.7: it is updated "when it encounters an exception or
   interrupt"; HRM 6.8: CALL_PAL entries and exceptions are two classes). The
   return address goes to R23 alone (HRM 6.8.1). The JIT's OP_CALL_PAL does
   the same. */
#define ENTER_NATIVE_CALL_PAL()                                                \
  do {                                                                         \
    SAVE_CALL_PAL_R23();                                                       \
    set_pc(state.pal_base | (1 << 13) | ((function & 0x80) << 5) |             \
           ((function & 0x3f) << 6) | 1);                                      \
  } while (0)

#define DO_CALL_PAL                                                            \
  if (((function < 0x40) && ((state.cm != 0))) ||                              \
      ((function > 0x3f) && (function < 0x80)) || (function > 0xbf)) {         \
    UNKNOWN2                                                                   \
  } else {                                                                     \
    if ((function == 0x00) && cSystem->exit_on_pal_halt())                     \
      cSystem->RequestPalHaltExit(); /* main loop exits gracefully */          \
    ENTER_NATIVE_CALL_PAL();                                                   \
  }

#define DO_IMPLVER state.r[REG_3] = m_model->implver;

// state.cc is wall-clock in both engines (advanced at batch boundaries by real
// elapsed time * cpu_hz), so the live cycle count is simply state.cc.
// RPCC syncs the wall-clock counter at every read and never returns the same
// value twice (see rpcc_read).
#define DO_RPCC state.r[REG_1] = rpcc_read();

// The following ops have no function right now (at least, not until multiple
// CPU's are supported). well, let's set up to have that happen soon.... Alpha
// ordering approximations (conservative): TRAPB: serialize exceptions + memory;
// use full fence.
#define DO_TRAPB std::atomic_thread_fence(std::memory_order_seq_cst);
// EXCB/MB: full fence (data+io ordering).
#define DO_EXCB std::atomic_thread_fence(std::memory_order_seq_cst);
#define DO_MB std::atomic_thread_fence(std::memory_order_seq_cst);
// WMB: HRM 4.11.1 requires WMB to order all prior stores from this CPU
// before all subsequent stores, as observed by every other CPU/DMA agent.
// A standalone atomic_thread_fence(release) only synchronizes with a matching
// acquire fence/load on another thread; it does NOT impose store-store
// ordering visible to unsynchronized observers.
#define DO_WMB std::atomic_thread_fence(std::memory_order_seq_cst);
// FETCH/FETCH_M/ECB/WH64/WH64EN are hints — safe as no-ops.
#define DO_FETCH ;   /* hint: no effect */
#define DO_FETCH_M ; /* hint: no effect */
#define DO_ECB ;     /* cache hint: no effect */
#define DO_WH64 ;    /* write hint: no effect */
#define DO_WH64EN ;  /* write hint enable: no effect */

