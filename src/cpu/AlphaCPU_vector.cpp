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
 * Tarantula's instructions in the interpreter (stage S7 of
 * docs/platforms/ev8.md): VC (opcode 05), VV (01) and VS (02), and the
 * vector enable. The memory groups SM (03) and RM (04) are S8's; opcode 06 is
 * spare. Every encoding and rule is docs/platforms/ev8-isa.md's, section 4
 * (the contract with the nada toolchain), cited by section; the paper is
 * Espasa et al., ISCA 2002 [TAR p. n]. What the sheet leaves to the
 * implementation is marked [inference] or [decision] here, in ev8.md's
 * "S7" and in docs/cpu-fidelity.md.
 *
 * Compiled code never contains one of these words: classify() compiles
 * nothing of opcodes 01-06, so a block ends before each and the interpreter
 * runs it, on every row (S10 is the JIT's).
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "Ev7.hpp"
#include "cpu_debug.hpp"

using tarantula::kElems;

namespace {

/// The function codes each group assigns (ev8-isa.md 4.4, 4.7); every other
/// code is unassigned and raises OPCDEC (4.3, 4.9).
bool vv_assigned(u32 f) {
  return f <= 0x0D || (f >= 0x10 && f <= 0x19) || (f >= 0x20 && f <= 0x2E) ||
         (f >= 0x30 && f <= 0x32);
}
bool vs_assigned(u32 f) {
  return vv_assigned(f) && f != 0x24 && f != 0x2C && f != 0x2D && f != 0x2E;
}
bool vc_assigned(u32 f) { return f <= 0x02 || (f >= 0x04 && f <= 0x06); }

} // namespace

/**
 * At init(): the state of a processor with Tarantula, cleared, and on a
 * board without EV7 register windows -- the ES40 under the
 * ALPHABOX_CPU_ANY_BOARD test hook, where the CPU oracle runs every row --
 * the part of the processor's window Tarantula owns: VCTL (the `ev8` row
 * only) and, with ALPHABOX_TARANTULA_PEEK=1, the register window
 * (docs/headless.md). On the Marvel boards CEv7Csr answers VCTL.
 *
 * [decision] The window is answered on any board under the hook so that
 * the oracle's cases set and clear VEN as an operating system would, with a
 * quadword store to the address ev8-isa.md gives. The Tsunami and the Titan
 * decode 44 physical address bits and ignore PA<42:35> in their I/O space
 * (their "Bits <42:35> are don't cares if bit <43> is asserted"), so every
 * PID's window lands on one address there, in either map; each processor
 * that reaches it gets its own.
 **/
void CAlphaCPU::tarantula_init() {
  if (!(m_model->ext & CPU_EXT_TARANTULA))
    return;
  if (!m_vec)
    m_vec.reset(new tarantula::State);
  tarantula_reset();
  const cpu_model *board = find_cpu_model(cSystem->platform().cpu_model);
  if (!cpu_any_board() || !board || board->family == CPU_FAMILY_EV7)
    return;
  // The window by the processor's own map: the EV7's for the `ev8` row,
  // whose VCTL the sheet places there; on the native EV8 the 21464 mode's
  // (N18, PID 0's block at 0xFFFF_FFC0_0000), the map its Marvel boards
  // use, for the hook's register window alone [decision]. The board then
  // folds the address into its own space.
  const ev7::address_map &map = m_ev8n ? ev7::k21464Map : ev7::kEv7Map;
  const u64 win = map.csr_base(m_pid) & cSystem->chipset()->phys_mask();
  if (!m_ev8n)
    cSystem->RegisterMemory(this, 0, win + tarantula::kVctlOffset, 8);
  static const bool peek = [] {
    const char *e = getenv("ALPHABOX_TARANTULA_PEEK");
    return e && *e && strcmp(e, "0") != 0;
  }();
  if (peek) {
    cSystem->RegisterMemory(this, 1, win + tarantula::kPeekOffset,
                            tarantula::kPeekSize);
    if (state.iProcNum == 0)
      printf("%%CPU-W-TARPEEK: the vector registers are readable and "
             "writable at offset %x of the register window (the "
             "ALPHABOX_TARANTULA_PEEK test hook)\n",
             tarantula::kPeekOffset);
  }
}

void CAlphaCPU::tarantula_reset() {
  if (!m_vec)
    return;
  m_vec->clear();
  m_vctl.store(0, std::memory_order_relaxed); // VCTL resets to 0 (section 2)
  m_vec_defer = false;
  m_vec_trap = 0;
}

/**
 * The window of a board without EV7 register windows (tarantula_init): the
 * processor that accesses it, which on such a board can only mean its own.
 **/
u64 CAlphaCPU::ReadMem(int index, u64 address, int dsize) {
  CAlphaCPU *c = (t_running_cpu && t_running_cpu->m_vec) ? t_running_cpu : this;
  if (!c->m_vec)
    return 0;
  u64 v = 0;
  if (index == 0) {
    v = c->vctl_is_csr() ? c->vctl_csr_read() : 0;
  } else if (index == 1 && address < tarantula::kPeekSize) {
    const u32 n = (u32)(address / tarantula::kPeekStride);
    const u32 i = (u32)((address % tarantula::kPeekStride) / 8);
    v = c->m_vec->vr[n][i];
  }
  if (dsize == 32)
    v = (address & 4) ? (v >> 32) : (v & 0xffffffff);
  return v;
}

void CAlphaCPU::WriteMem(int index, u64 address, int dsize, u64 data) {
  CAlphaCPU *c = (t_running_cpu && t_running_cpu->m_vec) ? t_running_cpu : this;
  if (!c->m_vec)
    return;
  if (index == 0) {
    if (c->vctl_is_csr() && !(address & 4))
      c->vctl_csr_write(data);
    return;
  }
  if (index != 1 || address >= tarantula::kPeekSize || dsize != 64)
    return;
  const u32 n = (u32)(address / tarantula::kPeekStride);
  const u32 i = (u32)((address % tarantula::kPeekStride) / 8);
  if (n != tarantula::kRegs - 1) // $vr31 discards writes
    c->m_vec->vr[n][i] = data;
}

/**
 * Opcodes 01-06 on a row with Tarantula. The order of the checks: an
 * encoding the sheet does not assign -- an unassigned function code, an SBZ
 * field that is not zero, a mask or literal bit where the group has none --
 * raises OPCDEC whatever VEN is (4.3); then VEN = 0 raises OPCDEC (section
 * 5); then a floating-point function with the FPU disabled takes FEN.
 *
 * - SM (03) and RM (04) are S8's: until then they are no instructions of the
 *   row and raise OPCDEC, which AMASK bit 3, clear until S8, lets software
 *   expect [decision]. Opcode 06 is spare: all OPCDEC (4.2).
 * - VV's <12> "must be 0" (4.4): a field that must be zero, so OPCDEC
 *   [inference: the sheet does not say SBZ, and it is no register field,
 *   the only kind it lets Alphabox ignore].
 * - A literal (L, <12>) on a VC instruction other than SETVL and SETVS
 *   raises OPCDEC, as a literal on a VS code that reads Fb does (4.4)
 *   [decision: 4.7 gives L to those two only].
 * - FEN: functions 20-3F of VV and VS are the floating-point half of the
 *   map (VS reads Fb there, and the arithmetic ones use the FPCR), and with
 *   the FPU disabled (PCTX<FPE>) they take FEN as a scalar FP operate does
 *   [inference: the sheet computes each element "as the scalar T
 *   instruction"]; the integer ones and VC do not.
 * - In PALmode the instructions execute as in any other mode: they are
 *   UNPREDICTABLE there (section 5) [decision].
 **/
void CAlphaCPU::tarantula(u32 ins) {
  const u32 op = ins >> 26;
  const u32 fnc = (ins >> 5) & 0x3f;
  const bool lit = (ins >> 12) & 1;
  const bool m = (ins >> 11) & 1;
  const bool sbz =
      lit || ((ins >> 13) & 7) == 0; // <15:13> of the register form
  bool valid;
  switch (op) {
  case 0x01: // VV
    valid = !lit && sbz && vv_assigned(fnc) && !(fnc == 0x0D && m);
    break;
  case 0x02: // VS: a literal only with the codes that read Rb
    valid =
        sbz && vs_assigned(fnc) && !(lit && fnc >= 0x20) && !(fnc == 0x0D && m);
    break;
  case 0x05: // VC
    valid = !m && sbz && vc_assigned(fnc) && (!lit || fnc <= 0x01);
    break;
  default: // 03 SM and 04 RM (S8), 06 spare
    valid = false;
    break;
  }
  if (!valid) {
    report_opcdec(op, (int)((ins >> 5) & 0x7f));
    GO_PAL(OPCDEC);
    return;
  }
  // VEN = 0: the vector unit is off for this thread; an OS that knows
  // vectors enables it and returns to the instruction (section 5). Not
  // reported: it is how lazy switching works.
  if (!ven()) {
    GO_PAL(OPCDEC);
    return;
  }
  // The groups' instructions follow in their own commits: until then
  // every one raises OPCDEC with VEN = 1 too.
  (void)fnc;
  GO_PAL(OPCDEC);
}
