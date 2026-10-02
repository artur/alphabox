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
 * The registers an EV7 holds when the console first runs on it.
 *
 * On a real machine the processor's CMM loads the SROM into it, the SROM
 * loads the XSROM (MVXSROM_V1_0_31.BIN, Alpha code behind a 0x40-byte
 * header), and the XSROM runs commands the CMM sends it until command 0x50
 * (its dispatcher at 0xfbf4) makes it wait on RBOX_SCRATCH for an address
 * and jump there (0x6650-0x671c, in the image less its header):
 *
 *   - it polls RBOX_SCRATCH for a word 0xf1xxxxxx (the address's upper 24
 *     bits), echoes it as 0xf2xxxxxx, then waits for 0xf3yyyyyy (the lower
 *     24 bits), and enters `xxxxxx << 24 | yyyyyy` in PALmode (hw_ret with
 *     bit 0 set);
 *   - r28 is the PID throughout: the XSROM never writes it, it only reads
 *     it (`sll at, 35, t8` makes its CSR base mask), so the SROM set it;
 *   - r19 points at this processor's block in the CMM's memory, 0x40008 or
 *     0x46008 by bit 6 of a GIO status word (0x498-0x4d8): the same two
 *     addresses the console's PALcode later reads through GIO;
 *   - r1 and r2 are whatever its last CSR access left there.
 *
 * The primary is sent to the console's self-decompressor (at 0x900000 in
 * its memory: the decompressor checks that it runs at 0x30000, 0x30240 or
 * 0x900000). The decompressor saves r1, r2, r16-r21, sp and r28 at its
 * first instructions and restores them before it jumps into the inflated
 * console, except that r19 comes back as its own base + 0x10 (it stores t1,
 * its own address, in r19's slot; 0x9007c4-0x9007ac in SRM_V7_3.EXE).
 *
 * What the console's PALcode then does with them (its reset entry, 0x3e780
 * in the decompressed SRM V7.3-1):
 *
 *   - r28 is stored as the processor's PID, and every CSR address it forms
 *     from then on is that PID's;
 *   - if r19 is not zero, or r18 is negative, it takes the cold path: its
 *     per-processor scratch area is found from PAL_BASE and the PID. With
 *     r19 = 0 and r18 >= 0 it takes a restart path that indexes a table with
 *     r18 and reads r21 as a pointer -- which zeroed registers send into
 *     garbage, as the first probe's did;
 *   - r1 and r2 are dead: r1 is overwritten by its first instruction, r2 at
 *     0x3e6b0, before either is read.
 *
 * Secondaries are started the same way later, by the console: it copies
 * its PALcode into the secondary's memory and writes the address (PID 1:
 * 0x4_0003_0001) through that processor's RBOX_SCRATCH, which the
 * emulator's Rbox serves (chipsets/marvel/Ev7Csr.cpp). They enter the
 * PALcode directly, with r19 still the XSROM's.
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "Ev7.hpp"

void ev7::xsrom_handoff(CAlphaCPU *cpu, u32 pid, u64 decompressor_base) {
  cpu->set_r(28, pid);
  // Which of the CMM's two processor blocks: the CMM serves both processors
  // of a module, and bit 6 of its status word picks the block [inference:
  // processor 0 and 1 of the module, so the PID's low bit].
  cpu->set_r(19, decompressor_base ? decompressor_base + 0x10
                 : (pid & 1)       ? U64(0x46008)
                                   : U64(0x40008));
  // r18: the XSROM's own (0x5718, 0x8b38 and others write it); the console
  // only tests its sign when r19 is 0, which it never is here.
  cpu->set_r(18, 0);
  // r1/r2: the XSROM's scratch, dead at the console's entry (see above).
  cpu->set_r(1, 0);
  cpu->set_r(2, 0);
}
