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
 * Tarantula's instructions in the interpreter (stages S7 and S8 of
 * docs/platforms/ev8.md): VC (opcode 05), VV (01) and VS (02), the vector
 * enable, and the memory groups SM (03) and RM (04); DRAINM is decoded with
 * opcode 18 (AlphaCPU.cpp). Opcode 06 is spare. Every encoding and rule is
 * docs/platforms/ev8-isa.md's, section 4
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

/// The scalar IEEE instruction a vector FP element is computed as (4.3,
/// D13): its 11-bit function with /SUI and dynamic rounding (/SUID); the
/// T compares as CMPTxx/SU, the conversions as CVTQT/SUID and CVTTQ/SVID,
/// VVCVTTQC as CVTTQ/SVIC (4.4). Fa, Fb and Fc are 31, so that the trap
/// reports no scalar register (section 5). The IEEE routines read only the
/// qualifier bits and Fc of the word.
u32 scalar_word(u32 fnc) {
  u32 f;
  switch (fnc) {
  case 0x20: // ADDT/SUID
  case 0x21: // SUBT/SUID
  case 0x22: // MULT/SUID
  case 0x23: // DIVT/SUID
    f = 0x7E0 | (fnc & 3);
    break;
  case 0x24: // SQRTT/SUID (14.7EB)
    f = 0x7EB;
    break;
  case 0x28: // CMPTUN/SU
    f = 0x5A4;
    break;
  case 0x26: // CMPTLT/SU, for LT and GT
  case 0x2A:
    f = 0x5A6;
    break;
  case 0x27: // CMPTLE/SU, for LE and GE
  case 0x2B:
    f = 0x5A7;
    break;
  case 0x2C: // CVTQT/SUID
    f = 0x7EC;
    break;
  case 0x2D: // CVTTQ/SVID
    f = 0x7EF;
    break;
  case 0x2E: // CVTTQ/SVIC
    f = 0x72F;
    break;
  default: // CMPTEQ/SU, for EQ and NE; the CPYSx raise nothing
    f = 0x5A5;
    break;
  }
  return (0x16u << 26) | (31u << 21) | (31u << 16) | (f << 5) | 31u;
}

} // namespace

/**
 * At init(): the state of a processor with Tarantula, cleared, and on a
 * board without EV7 register windows -- the ES40, which takes the `ev8` row
 * as an Alphabox construction, or any board under the
 * ALPHABOX_CPU_ANY_BOARD test hook, where the CPU oracle runs every row --
 * the part of the processor's window Tarantula owns: VCTL (the `ev8` row
 * only) and, with ALPHABOX_TARANTULA_PEEK=1, the register window
 * (docs/headless.md). On the Marvel boards CEv7Csr answers VCTL.
 *
 * [decision] The window is answered on such a board so that software (the
 * oracle's cases, an operating system) sets and clears VEN with a quadword
 * store to the address ev8-isa.md gives. The Tsunami and the Titan
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
  if (!m_vec_rs)
    m_vec_rs.reset(new tarantula::Restart);
  tarantula_reset();
  // A Tarantula row is on a board of another family only as the ES40's
  // construction or through the test hook; either way the board has no
  // EV7 register windows.
  const cpu_model *board = find_cpu_model(cSystem->platform().cpu_model);
  if (!board || board->family == CPU_FAMILY_EV7)
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
  m_vec_rs->clear();
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
 * - SM (03) and RM (04), S8's: <15:12> SBZ, functions 00 and 01, and SM's
 *   <4:0> zero (4.5, 4.6, 4.9). They take no FEN: no T forms (4.5).
 *   Opcode 06 is spare: all OPCDEC (4.2).
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
  case 0x03: // SM: <15:12> SBZ, <4:0> zero
    valid = ((ins >> 12) & 0xf) == 0 && (ins & 0x1f) == 0 && fnc <= 0x01;
    break;
  case 0x04: // RM: <15:12> SBZ, Vx in <4:0>
    valid = ((ins >> 12) & 0xf) == 0 && fnc <= 0x01;
    break;
  default: // 06 spare
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
  if (op == 0x05) {
    tarantula_vc(ins);
    return;
  }
  if (op == 0x03 || op == 0x04) {
    tarantula_mem(ins);
    return;
  }
  if (fnc >= 0x20) {
    if (!state.fpen) {
      GO_PAL(FEN);
      return;
    }
    tarantula_fp(ins);
    return;
  }
  tarantula_int(ins);
}

/**
 * VC, vector control (4.7). Ra is not used (it must be 31, and is ignored,
 * 4.3); <20:16> and <4:0> name integer or vector registers as each
 * instruction says.
 **/
void CAlphaCPU::tarantula_vc(u32 ins) {
  tarantula::State &v = *m_vec;
  const u32 rb = I_GETRB(ins), rc = I_GETRC(ins);
  switch ((ins >> 5) & 0x3f) {
  case 0x00: { // SETVL: vl <- MIN(Rbv unsigned, 128); Rc <- vl
    const u64 b = RBV;
    v.vl = b > tarantula::kMaxVl ? tarantula::kMaxVl : b;
    if (rc != 31)
      state.r[REG_3] = v.vl;
    break;
  }
  case 0x01: // SETVS: vs <- Rbv, the literal zero-extended
    v.vs = RBV;
    break;
  case 0x02: // SETVM: vm[i] <- Vb[i]<0> below vl, 0 above (as BLBS tests)
    for (int i = 0; i < kElems; i++)
      v.set_mask(i, (u64)i < v.vl && (v.vr[rb][i] & 1));
    break;
  case 0x04: // GETVL
    if (rc != 31)
      state.r[REG_3] = v.vl;
    break;
  case 0x05: // GETVS
    if (rc != 31)
      state.r[REG_3] = v.vs;
    break;
  case 0x06: // GETVM: Vc[i] <- vm[i] below vl; the rest UNPREDICTABLE
    // (Alphabox: unchanged, 4.3)
    if (rc != 31)
      for (u64 i = 0; i < v.vl; i++)
        v.vr[rc][i] = v.mask((int)i) ? 1 : 0;
    break;
  }
}

/**
 * VV and VS, the integer half, functions 00-1F (4.4): Vc[i] <- Va[i] op B
 * for every active element, B = Vb[i] (VV) or the scalar (VS: Rbv or the
 * zero-extended literal). Active: i < vl, and vm[i] = 1 under the mask
 * (4.3). Inactive elements of Vc are left unchanged (the Alphabox choice of
 * 4.3: the sheet makes them UNPREDICTABLE). Arithmetic wraps and raises no
 * overflow; a compare writes the quadword 1 or 0 (4.3, points 7 and 8).
 * VVMRG and VSMRG read vm as a selector for every i < vl, and take no mask.
 **/
void CAlphaCPU::tarantula_int(u32 ins) {
  tarantula::State &v = *m_vec;
  const u32 fnc = (ins >> 5) & 0x3f;
  const bool vv = (ins >> 26) == 0x01;
  const bool m = (ins >> 11) & 1;
  const u32 va = I_GETRA(ins), vb = I_GETRB(ins), vc = I_GETRC(ins);
  const u64 s = vv ? 0 : RBV;
  for (u64 n = 0; n < v.vl; n++) {
    const int i = (int)n;
    if (m && !v.mask(i))
      continue;
    const u64 a = v.vr[va][i];
    const u64 b = vv ? v.vr[vb][i] : s;
    u64 r;
    switch (fnc) {
    case 0x00:
      r = a + b;
      break;
    case 0x01:
      r = a - b;
      break;
    case 0x02:
      r = a * b;
      break;
    case 0x03: // UMULH
      r = (u64)(((unsigned __int128)a * b) >> 64);
      break;
    case 0x04:
      r = a & b;
      break;
    case 0x05:
      r = a & ~b;
      break;
    case 0x06:
      r = a | b;
      break;
    case 0x07:
      r = a | ~b;
      break;
    case 0x08:
      r = a ^ b;
      break;
    case 0x09:
      r = a ^ ~b;
      break;
    case 0x0A:
      r = a << (b & 63);
      break;
    case 0x0B:
      r = a >> (b & 63);
      break;
    case 0x0C:
      r = (u64)((s64)a >> (b & 63));
      break;
    case 0x0D: // MRG
      r = v.mask(i) ? b : a;
      break;
    case 0x10:
      r = a == b;
      break;
    case 0x11:
      r = a != b;
      break;
    case 0x12:
      r = (s64)a < (s64)b;
      break;
    case 0x13:
      r = (s64)a <= (s64)b;
      break;
    case 0x14:
      r = (s64)a > (s64)b;
      break;
    case 0x15:
      r = (s64)a >= (s64)b;
      break;
    case 0x16:
      r = a < b;
      break;
    case 0x17:
      r = a <= b;
      break;
    case 0x18:
      r = a > b;
      break;
    default: // 0x19
      r = a >= b;
      break;
    }
    if (vc != 31)
      v.vr[vc][i] = r;
  }
}

/**
 * VV and VS, the floating-point half, functions 20-3F (4.4): T format, each
 * active element computed as the scalar instruction scalar_word() names
 * (D13), with the FPCR: rounding from FPCR<DYN> (or chopped for VVCVTTQC),
 * the trap disables and denormal controls as for a /S instruction, the
 * status bits updated as that instruction updates them, once per active
 * element and in element order. A compare writes the quadword 1 or 0, not
 * 2.0 (point 8). The unary VV ones read Vb only (Va is not used, 4.3).
 *
 * The trap (section 5, point 11): every active element is computed and
 * written, the correct value where no exception trapped and the IEEE
 * routines' default result where one did; then, if any element would have
 * trapped, the instruction takes one arithmetic trap, after it (trap
 * style): EXC_SUM the OR of what every element would have reported, with
 * SWC as for a /S instruction and REG 31. It is raised as the scalar
 * instruction raises its own, so on the `ev8` row EXC_ADDR is what this
 * core gives a scalar ARITH (the PALcode makes it the next PC) and on the
 * native EV8 the next instruction's PC [IDS p. 485] [inference]. What
 * "would trap" means is the scalar instruction's on this core, the
 * 21264's: an enabled exception the FPCR does not disable, or an FPCR
 * status bit the instruction sets that was clear (EXC_SUM<SET_*>, "PALcode
 * should set FPCR[...]", 21264 HRM 5.2.13).
 **/
void CAlphaCPU::tarantula_fp(u32 ins) {
  tarantula::State &v = *m_vec;
  const u32 fnc = (ins >> 5) & 0x3f;
  const bool vv = (ins >> 26) == 0x01;
  const bool m = (ins >> 11) & 1;
  const u32 va = I_GETRA(ins), vb = I_GETRB(ins), vc = I_GETRC(ins);
  const u64 s = vv ? 0 : state.f[FREG_2];
  const u32 sw = scalar_word(fnc);
  state.exc_sum = 0; // as FPSTART
  m_vec_defer = true;
  m_vec_trap = 0;
  for (u64 n = 0; n < v.vl; n++) {
    const int i = (int)n;
    if (m && !v.mask(i))
      continue;
    const u64 a = v.vr[va][i];
    const u64 b = vv ? v.vr[vb][i] : s;
    u64 r;
    switch (fnc) {
    case 0x20:
      r = ieee_fadd(a, b, sw, DT_T, false);
      break;
    case 0x21:
      r = ieee_fadd(a, b, sw, DT_T, true);
      break;
    case 0x22:
      r = ieee_fmul(a, b, sw, DT_T);
      break;
    case 0x23:
      r = ieee_fdiv(a, b, sw, DT_T);
      break;
    case 0x24: // SQRTT of Vb[i]
      r = ieee_sqrt(b, sw, DT_T);
      break;
    case 0x25:
      r = ieee_fcmp(a, b, sw, 0) == 0;
      break;
    case 0x26:
      r = ieee_fcmp(a, b, sw, 1) == -1;
      break;
    case 0x27: {
      const s32 c = ieee_fcmp(a, b, sw, 1);
      r = c == -1 || c == 0;
      break;
    }
    case 0x28:
      r = ieee_fcmp(a, b, sw, 0) == 2;
      break;
    case 0x29: // true when unordered
      r = ieee_fcmp(a, b, sw, 0) != 0;
      break;
    case 0x2A:
      r = ieee_fcmp(a, b, sw, 1) == 1;
      break;
    case 0x2B: {
      const s32 c = ieee_fcmp(a, b, sw, 1);
      r = c == 1 || c == 0;
      break;
    }
    case 0x2C: // CVTQT of Vb[i]
      r = ieee_cvtif(b, sw, DT_T);
      break;
    case 0x2D: // CVTTQ of Vb[i], dynamic rounding
    case 0x2E: // ... chopped
      r = ieee_cvtfi(b, sw);
      break;
    case 0x30: // CPYS
      r = (a & FPR_SIGN) | (b & ~FPR_SIGN);
      break;
    case 0x31: // CPYSN
      r = ((a & FPR_SIGN) ^ FPR_SIGN) | (b & ~FPR_SIGN);
      break;
    default: // 0x32 CPYSE
      r = (a & (FPR_SIGN | FPR_EXP)) | (b & ~(FPR_SIGN | FPR_EXP));
      break;
    }
    if (vc != 31)
      v.vr[vc][i] = r;
  }
  m_vec_defer = false;
  if (m_vec_trap)
    tarantula_trap(m_vec_trap | TRAP_SWC, sw);
}

/**
 * SM and RM (4.5, 4.6): VLOADQ and VSTOREQ, element i at Rbv + i x vs (vs a
 * signed byte stride, the product wrapping); VGATHQ and VSCATQ at Vx[i] +
 * Rbv. An element is active when i < vl and, with M, vm[i] = 1 (4.3); an
 * inactive one makes no access and raises nothing, and an inactive element
 * of a load's destination is left unchanged (4.3, the Alphabox choice).
 *
 * Faults (section 5). Every active element is checked, in increasing i,
 * before any is accessed: its address a multiple of 8, else the alignment
 * fault; then its translation for a read or a write (virt2phys, the core's
 * own: the TB, the superpages, the access and fault-on bits), else the TB
 * miss, access violation or fault on read or write the core raises for a
 * scalar access, VA that element's address and MM_STAT<OPCODE> 03 or 04
 * with WR for a store or scatter. So the fault reported is the
 * lowest-numbered active faulting element's, EXC_ADDR is the instruction's
 * own PC, and nothing has changed, neither memory nor a register; after the
 * handler the instruction restarts from its first element. The
 * translations already made are kept for that restart (Restart, in
 * Tarantula.hpp), which guarantees forward progress [decision].
 *
 * Then the accesses, in increasing i, each an aligned quadword as LDQ and
 * STQ make it: so when two active elements of a store or scatter write one
 * address, the higher one's value remains (4.3, the Alphabox choice), and
 * a gather computes every address before it writes an element, so Vx may
 * be Vt. A load or gather into $vr31 is a prefetch: nothing checked,
 * nothing accessed, no fault (4.3, [TAR p. 283]); a store or scatter of
 * $vr31 stores zeros. As a scalar LDQ or STQ is, the instruction is a
 * memory operation for a pending STx_C (HRM D.19) when an element is
 * accessed [inference: each element "an aligned quadword access, single-copy
 * atomic as LDQ's", 4.5].
 **/
void CAlphaCPU::tarantula_mem(u32 ins) {
  tarantula::State &v = *m_vec;
  tarantula::Restart &rs = *m_vec_rs;
  const bool rm = (ins >> 26) == 0x04;
  const bool st = ((ins >> 5) & 0x3f) == 0x01;
  const bool m = (ins >> 11) & 1;
  const u32 vt = I_GETRA(ins), vx = ins & 0x1f;
  const u64 base = state.r[REG_2];
  if (!st && vt == 31)
    return; // a prefetch
  const int flags = st ? ACCESS_WRITE : ACCESS_READ;
  // The kept translations serve this word's restart only.
  if (!(rs.valid && rs.pc == state.current_pc && rs.ins == ins &&
        rs.cm == state.cm && rs.asn == state.asn0 && rs.gen == m_vec_xlate_gen))
    rs.clear();
  rs.valid = false;
  int act[kElems];
  u64 pa[kElems];
  int n = 0;
  for (u64 k = 0; k < v.vl; k++) {
    const int i = (int)k;
    if (m && !v.mask(i))
      continue;
    const u64 ea = rm ? v.vr[vx][i] + base : base + k * v.vs;
    if (ea & 7) {
      tarantula_unalign(ea, st, ins);
      return;
    }
    if (rs.has(i) && rs.va[i] == ea) {
      pa[n] = rs.pa[i];
    } else {
      u64 p;
      if (virt2phys(ea, &p, flags, nullptr, ins)) {
        // The fault is taken (virt2phys set it up); keep what was
        // translated for the restart.
        rs.valid = true;
        rs.pc = state.current_pc;
        rs.ins = ins;
        rs.cm = state.cm;
        rs.asn = state.asn0;
        rs.gen = m_vec_xlate_gen;
        return;
      }
      rs.put(i, ea, p);
      pa[n] = p;
    }
    act[n++] = i;
  }
  if (!n)
    return;
  rs.clear();
  m_llsc_clean = 0; // as LLSC_MEMOP (cpu_memory.hpp)
  for (int k = 0; k < n; k++) {
    const u64 p = pa[k];
    if (st) {
      const u64 d = vt == 31 ? 0 : v.vr[vt][act[k]];
      if (p < dram_size) {
        dram_write(dram_ptr, p, 64, d);
        note_dram_write(p);
      } else {
        sys_write(p, 64, d);
      }
    } else {
      v.vr[vt][act[k]] =
          p < dram_size ? dram_read(dram_ptr, p, 64) : sys_read(p, 64);
    }
  }
}

/**
 * An element's alignment fault (4.5, section 5), as the core reports a
 * scalar access's (GO_PAL_UNALIGN, cpu_defs.hpp): VA the element's address,
 * EXC_SUM<REG> the Vt field, MM_STAT the opcode and WR; on the native EV8
 * its EXC_SUM and M_STAT. No operating system's fixup knows opcodes 03 and
 * 04, so the program gets the signal.
 **/
void CAlphaCPU::tarantula_unalign(u64 va, bool wr, u32 ins) {
  const u64 op = ins >> 26;
  state.fault_va = va;
  state.va_form_va = va;
  state.exc_sum = (u64)(I_GETRA(ins) & 0x1f) << 8;
  state.mm_stat = (op << 4) | (wr ? 1 : 0);
  if (m_ev8n)
    state.exc_sum = ev8_regs_of(ins);
  GO_PAL(UNALIGN);
}

void CAlphaCPU::tarantula_trap(u64 flags, u32 ins) {
  state.exc_sum = 0;
  ARITH_TRAP(flags, 31);
}
