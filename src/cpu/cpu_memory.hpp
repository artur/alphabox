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
 * Contains code macros for the processor memory load/store instructions.
 * Based on ARM chapter 4.2.
 **/
#define DO_LDA state.r[REG_1] = state.r[REG_2] + DISP_16;

#define DO_LDAH state.r[REG_1] = state.r[REG_2] + (DISP_16 << 16);

/* A load to R31 is a prefetch (HRM 2.6): it is translated, and it takes the
   DTB miss, access violation or fault-on-read the load would take ("load
   operations to R31 and F31 may generate exceptions. These exceptions must
   be dismissed by PALcode"), but nothing is read. A prefetch to I/O space
   is turned into one to memory space (HRM 4.2, p. 4-5), so no device sees
   it. LDQ_U to R31 is UNOP, which the 21264 retires at decode without
   executing it (HRM 2.5, Table 2-6): no translation, no fault. Compiled
   code follows the same rule (jit_read, bit 9 of its size argument).

   LLSC_MEMOP: the memory operations HRM D.19 (Restriction 23) lists make
   the next STx_C fail when they run between it and its LDx_L: LDL, LDQ,
   LDF, LDG, LDS, LDT, the same stores, LDQ_U not to R31 and STQ_U. The
   byte and word forms are not in the list, and neither are HW_LD, HW_ST,
   ECB and WH64 (the HRM names those four as not failing it). */
#define LLSC_MEMOP m_llsc_clean = 0;

#define DO_LDBU                                                                \
  if ((REG_1 & 0x1f) != 31) {                                                  \
    READ_VIRT_F(state.r[REG_2] + DISP_16, 8, state.r[REG_1], zext_u64_8);      \
  } else {                                                                     \
    PREFETCH_VIRT(state.r[REG_2] + DISP_16, 8);                                \
  }

#define DO_LDL                                                                 \
  LLSC_MEMOP                                                                   \
  if ((REG_1 & 0x1f) != 31) {                                                  \
    READ_VIRT_F(state.r[REG_2] + DISP_16, 32, state.r[REG_1], sext_u64_32);    \
  } else {                                                                     \
    PREFETCH_VIRT(state.r[REG_2] + DISP_16, 32);                               \
  }

/* LDx_L: always perform the load and establish the lock, regardless of Ra.
   The state.r[31] auto-clear after each instruction makes writing to R31
   harmless, but the cpu_lock side-effect must occur for correct LL/SC pairing.
*/
#define DO_LDL_L                                                               \
  READ_VIRT_LOCK_F(state.r[REG_2] + DISP_16, 32, state.r[REG_1], sext_u64_32);

#define DO_LDQ                                                                 \
  LLSC_MEMOP                                                                   \
  if ((REG_1 & 0x1f) != 31) {                                                  \
    READ_VIRT(state.r[REG_2] + DISP_16, 64, state.r[REG_1]);                   \
  } else {                                                                     \
    PREFETCH_VIRT(state.r[REG_2] + DISP_16, 64);                               \
  }

#define DO_LDQ_L READ_VIRT_LOCK(state.r[REG_2] + DISP_16, 64, state.r[REG_1]);

/* LDL_ARM, LDQ_ARM (21464, CPU_EXT_EV8; docs/platforms/ev8-isa.md 3.1): a
   load from Rbv, with no displacement (the low 16 bits are the function),
   faulting as LDQ does [21464 IDS Rev 1.1k, PDF p. 669] -- here as this
   core's LDQ, so unaligned only across a page (docs/cpu-fidelity.md) -- and
   then, only when it did not fault [p. 670], the watch on the line it read
   (watch_arm). As LDx_L, always performed, whatever Ra [inference, "as
   similar as possible to the LDx_L instructions", p. 669]; a memory
   operation for a pending STx_C as LDQ is [inference]. The arm comes before
   the read and both under the LL/SC reader guard, as LDx_L's lock: a store
   or a device write the read does not see clears the flag. */
#define READ_VIRT_ARM(va, size, dest, f)                                       \
  {                                                                            \
    LLSC_MEMOP                                                                 \
    pbc = false;                                                               \
    DATA_PHYS(va, ACCESS_READ, (size / 8) - 1);                                \
    CSystem::CLLSCDRAMGuard _llsc_guard(cSystem, phys_address < dram_size);    \
    watch_arm(phys_address);                                                   \
    dest =                                                                     \
        f((phys_address < dram_size ? dram_read(dram_ptr, phys_address, size)  \
                                    : sys_read(phys_address, size)));          \
  }
#define DO_LDL_ARM                                                             \
  READ_VIRT_ARM(state.r[REG_2], 32, state.r[REG_1], sext_u64_32);
#define DO_LDQ_ARM READ_VIRT_ARM(state.r[REG_2], 64, state.r[REG_1], (u64));

#define DO_LDQ_U                                                               \
  if ((REG_1 & 0x1f) != 31) {                                                  \
    LLSC_MEMOP                                                                 \
    READ_VIRT((state.r[REG_2] + DISP_16) & ~U64(0x7), 64, state.r[REG_1]);     \
  }

#define DO_LDWU                                                                \
  if ((REG_1 & 0x1f) != 31) {                                                  \
    READ_VIRT_F(state.r[REG_2] + DISP_16, 16, state.r[REG_1], zext_u64_16);    \
  } else {                                                                     \
    PREFETCH_VIRT(state.r[REG_2] + DISP_16, 16);                               \
  }

#define DO_STB WRITE_VIRT(state.r[REG_2] + DISP_16, 8, state.r[REG_1]);

#define DO_STL                                                                 \
  LLSC_MEMOP                                                                   \
  WRITE_VIRT(state.r[REG_2] + DISP_16, 32, state.r[REG_1]);

#define DO_STL_C                                                               \
  WRITE_VIRT_COND(state.r[REG_2] + DISP_16, 32, state.r[REG_1], state.r[REG_1]);

#define DO_STQ                                                                 \
  LLSC_MEMOP                                                                   \
  WRITE_VIRT(state.r[REG_2] + DISP_16, 64, state.r[REG_1]);

#define DO_STQ_C                                                               \
  WRITE_VIRT_COND(state.r[REG_2] + DISP_16, 64, state.r[REG_1], state.r[REG_1]);

#define DO_STQ_U                                                               \
  LLSC_MEMOP                                                                   \
  WRITE_VIRT((state.r[REG_2] + DISP_16) & ~U64(0x07), 64, state.r[REG_1]);

#define DO_STW WRITE_VIRT(state.r[REG_2] + DISP_16, 16, state.r[REG_1]);
