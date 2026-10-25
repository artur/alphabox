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
 * The native EV8: the 21464's own privileged interface on the `ev8n` row
 * (CPU_FAMILY_EV8; docs/platforms/ev8-native.md, S11). Its modes and how a
 * jump changes them, the PALcode entries and the IPRs each writes, CALL_PAL,
 * the PALcode instructions HW_LD and HW_ST, translation (Appendix C's
 * superpages and sign check, the TB with TPU groups, 64 KB pages), and the
 * reset of every TPU. The IPRs are in cpu_pal_ev8.hpp.
 *
 * IDS is the 21464 Internal Design Specification, Rev 1.1k (January 2001),
 * cited by PDF page. What it leaves open is decided in ev8-native.md
 * (N1-N31) or here, marked [inference] or [decision] in both places and in
 * docs/cpu-fidelity.md.
 *
 * The rest of the core is the EV6 family's, shared: the user instructions
 * (those of the `ev8` row, CPU_EXT_EV8), the interrupt lines and the
 * interrupt check, the data page cache and the TB storage and lookup
 * (FindTBEntry). What makes the TB the 21464's is how this file fills it:
 * the 21464's masks, and `valid` meaning "present and of the current TPU
 * group" (ev8_set_group).
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#ifdef ES40_JIT
#include "jit/jitengine.hpp"
#endif
#include "cpu_debug.hpp"
#include "cpu_pal_ev8.hpp"

namespace {
/// PAL_BASE<47:15>: a physical address, <51:48> MBZ (N6) [IDS p. 499].
constexpr u64 kPalBaseMask = U64(0x0000ffffffff8000);
/// The 48-bit physical address [IDS p. 43].
constexpr u64 kPaMask = U64(0x0000ffffffffffff);
/// The placeholder after a native CMOV, `bis $31, $31, $31` [IDS pp. 72-74].
constexpr u32 kCmovPlaceholder = 0x47FF041F;
/// The entries Alphabox raises [IDS pp. 498-499, 633-634; N11].
constexpr u64 kDtbmDouble = 0x100, kDtbmDoubleAlt = 0x180, kDtbmSingle = 0x300,
              kDfault = 0x380, kIacv = 0x480, kItbMiss = 0x580,
              kDtbmSingleCons = 0x800, kItbMissCons = 0x880,
              kBadJumpIva = 0x900, kMildReset = 0xB00;

inline u64 sext_from(u64 v, int bit) {
  const u64 m = U64(1) << bit;
  v &= (m << 1) - 1;
  return (v ^ m) - m;
}

bool s_trace_ev8 = getenv("ALPHABOX_TRACE_EV8") != nullptr;
} // namespace

/// At init: the chip's shared IPRs (Table 16-1's per-chip ones). TPU 0 of a
/// chip makes them, its other TPUs, numbered right after it, share TPU 0's.
/// CPU_BASE<9:0> is the chip's number in the machine (N6, N19's dense PIDs:
/// the chips in the order of their sections).
void CAlphaCPU::ev8_init() {
  if (m_tpu == 0) {
    m_ev8_chip = std::make_shared<SEv8Chip>();
    const int tpus =
        cSystem->tpus_per_chip() > 0 ? cSystem->tpus_per_chip() : 1;
    m_ev8_chip->pid = (u32)(state.iProcNum / tpus);
  } else {
    m_ev8_chip = cSystem->get_cpu(state.iProcNum - m_tpu)->m_ev8_chip;
  }
  ev8_reset(0);
}

/**
 * Every reset class enters at a PALcode entry [IDS p. 631]; power-up is
 * MILD_RESET, 0xB00 (N11), on every TPU of every chip with its TPU_ID
 * (N17): PALcode parks the ones it does not want, with LDQ_ARM and QUIESCE.
 * The IPRs take their reset values: those the IDS gives (QUIESCE_TIMEOUT
 * 0x2800 [p. 511], TPU_ID [p. 488], CPU_BASE (N6), CHIP_ID 0 (N16)), and
 * zero for every other field [decision: Table 16-1 classes them as
 * initialized by hardware, with no value]. DC_CTL<SET_EN> is 11: "at least
 * one set must be enabled" [p. 501, inference]. PAL_BASE, "?" in the table,
 * is the loader's.
 **/
void CAlphaCPU::ev8_reset(u64 pal_base) {
  m_ev8 = SEv8();
  for (int i = 0; i < 64; i++)
    state.r[i] = state.f[i] = 0;
  state.pal_base = pal_base & kPalBaseMask;
  state.cm = state.alt_cm = 0;
  state.asn = state.asn0 = state.asn1 = 0;
  state.fpen = false;
  state.sde = state.hwe = false;
  state.eien = state.slen = state.cren = state.pcen = state.sien = 0;
  state.asten = state.aster = state.astrr = 0;
  state.sir = state.slr = state.crr = state.pcr = 0;
  state.i_ctl_spe = state.m_ctl_spe = 0;
  state.i_ctl_vptb = state.va_ctl_vptb = 0;
  state.i_ctl_va_mode = state.va_ctl_va_mode = 0;
  state.i_ctl_other = 0;
  state.exc_addr = state.exc_sum = state.fault_va = state.va_form_va = 0;
  state.mm_stat = state.dc_stat = state.i_stat = 0;
  state.cc = state.cc_offset = 0;
  cc_last_read = cc_borrow = cc_wall_remainder = 0; // the RPCC floor too
  state.cc_ena = false;
  state.last_tb_virt = 0;
  last_dtb_virt[0] = last_dtb_virt[1] = 0;
  if (m_tpu == 0 && m_ev8_chip) {
    m_ev8_chip->cpu_cnfg.store(0);
    m_ev8_chip->dc_ctl.store(U64(3) << 48);
  }
  ev8_tb_clear(TB_INDEX_DATA);
  ev8_tb_clear(TB_INDEX_ITB);
  flush_icache();
  dpc_context_changed();
  flush_data_page_cache();
  m_int_resignal = 0;
  cSystem->cpu_clear_lock(state.iProcNum);
  set_pc(state.pal_base | kMildReset | 1);
  stop_waiting();
}

/// EXC_SUM's register fields: the instruction's <25:21>, <20:16> and <4:0>
/// in <28:24>, <23:19> and <18:14>, "independent of the type of operation"
/// [IDS pp. 485-486].
u64 CAlphaCPU::ev8_regs_of(u32 ins) const {
  return ((u64)((ins >> 21) & 31) << 24) | ((u64)((ins >> 16) & 31) << 19) |
         ((u64)(ins & 31) << 14);
}

/// An entry, from the trapping instruction: PALmode, PAL_BASE + entry
/// [IDS pp. 498, 633].
void CAlphaCPU::ev8_trap(u64 entry, u32 ins) {
  (void)ins;
  set_pc(state.pal_base | entry | 1);
}

/**
 * GO_PAL on this row: the core's traps (OPCDEC, FEN, ARITH, UNALIGN,
 * INTERRUPT, MT_FPCR) at the 21464's entries, which for these are the
 * 21264's offsets [IDS p. 633]. EXC_ADDR is the PC of the trapping
 * instruction, with its mode in <1:0> [IDS pp. 484-485], "for all traps
 * except ARITH and MT_FPCR", where it is the next instruction's
 * [p. 485; Table 22-2's restart PC, pp. 630-631]; an interrupt's is the
 * instruction that would have run next, which is current_pc when execute()
 * takes one.
 **/
void CAlphaCPU::ev8_go_pal(u64 offset) {
  if (offset == ARITH || offset == MT_FPCR)
    state.exc_addr = state.current_pc + 4;
  else
    state.exc_addr = state.current_pc;
  set_pc(state.pal_base | offset | 1);
}

/// The 52-bit VA and 64 KB pages of a stream: VA_SIZE and PAGE_SIZE both
/// set. Every other combination behaves as 43-bit VA and 8 KB pages (N13):
/// the valid ones are those two [IDS p. 690, Table C-3].
bool CAlphaCPU::ev8_va52(bool istream) const {
  const u64 ctl = istream ? m_ev8.i_ctl : m_ev8.m_ctl;
  return (ctl & (U64(3) << 20)) == (U64(3) << 20);
}

/**
 * IVA_FORM and VA_FORM [IDS pp. 497-498, 512-513; Appendix C.4, pp.
 * 690-693]: the PTE's address in the virtual page table, from VPTE_BASE
 * and the address, in the stream's mode.
 **/
u64 CAlphaCPU::ev8_va_form(u64 va, bool istream) const {
  const u64 ctl = istream ? m_ev8.i_ctl : m_ev8.m_ctl;
  if (!ev8_va52(istream)) // 43-bit VA, 8 KB: VPTE_BASE<51:33>, VA<42:13>
    return (sext_from(ctl, 51) & U64(0xfffffffe00000000)) |
           ((va >> 10) & U64(0x00000001fffffff8));
  const u64 base = sext_from(ctl, 51) & U64(0xfffffc0000000000); // <51:42>
  // Reduced page tables, quadrant 1 (VA<51:50> = 01): <38:37> = 01 and
  // VA<49:29> in <23:3> [C.4.4, p. 693].
  if ((ctl & (U64(1) << 27)) && ((va >> 50) & 3) == 1)
    return base | (U64(1) << 37) | ((va >> 26) & U64(0x0000000000fffff8));
  // 52-bit VA, 64 KB: VA<51:16> in <38:3>, <41:39> the sign of VA<51>
  // [C.4.3, p. 692].
  return base | ((va >> 13) & U64(0x0000007ffffffff8)) |
         (((va >> 51) & 1) ? U64(0x0000038000000000) : 0);
}

/**
 * Appendix C's translation front end [IDS pp. 688-689, 695], N14: the
 * enabled superpages are tried first, each checking the address all the
 * way to bit 63, and only an address none of them takes is checked against
 * VA_SIZE. SPE<2> (SPE52) VA<63:50> = 0x3FFE: PA<47:0> = VA<47:0>; SPE<1>
 * (SPE43) VA<63:41> = 0x7FFFFE: PA = SEXT(VA<40:0>) to 48 bits; SPE<0>
 * (SPE32) VA<63:30> = 0x3FFFFFFFE: PA<29:0> = VA<29:0>. "Any non-kernel
 * mode access to an enabled superpage region must result in an access
 * violation" [pp. 490, 507]. The enables are per TPU (I_CTL, M_CTL; N14).
 **/
int CAlphaCPU::ev8_superpage(u64 va, bool istream, int cm, u64 *phys) const {
  const int spe = istream ? state.i_ctl_spe : state.m_ctl_spe;
  u64 pa;
  if ((spe & 4) && (va >> 50) == U64(0x3ffe))
    pa = va & kPaMask;
  else if ((spe & 2) && (va >> 41) == U64(0x7ffffe))
    pa = sext_from(va, 40) & kPaMask;
  else if ((spe & 1) && (va >> 30) == U64(0x3fffffffe))
    pa = va & U64(0x3fffffff);
  else {
    const int top = ev8_va52(istream) ? 51 : 42;
    return sext_from(va, top) == va ? 0 : -1;
  }
  if (cm)
    return -2;
  *phys = pa;
  return 1;
}

/**
 * virt2phys on this row (CAlphaCPU::virt2phys hands every translation
 * here). The entries and what they write are chapter 22's [IDS p. 633]:
 *   - a TB miss: ITB_MISS (EXC_ADDR) or ITB_MISS_CONS in console mode
 *     (I_MODE<CONSOLE>); DTBM_SINGLE or DTBM_SINGLE_CONS (M_MODE<CONSOLE>),
 *     which write DTBMS_RET_ADDR, not EXC_ADDR [p. 483], and EXC_SUM, VA,
 *     VA_FORM, M_STAT; a HW_LD/PTE's, the double miss: DTBM_DOUBLE, or
 *     DTBM_DOUBLE_ALT with M_CTL<DBL_ALT> (N12), EXC_ADDR only;
 *   - an access violation, a fault on read or write, a bad VA sign (N12:
 *     M_STAT<ACV>): DFAULT, or IACV for the I-stream (EXC_ADDR only);
 *   - a HW_LD/PTE writes no VA [p. 511].
 * M_STAT<OPCODE> is the instruction's opcode [p. 510]. A FAKE lookup (the
 * JIT's, the debugger's) only answers.
 **/
int CAlphaCPU::ev8_virt2phys(u64 virt, u64 *phys, int flags, bool *asm_bit,
                             u32 ins) {
  const bool exec = (flags & ACCESS_EXEC) != 0;
  const int t = exec ? TB_INDEX_ITB : TB_INDEX_DATA;
  const int cm = (flags & VPTE) ? 0 : (flags & ALT) ? state.alt_cm : state.cm;
  const bool forreal = !(flags & FAKE);
  const bool vpte = (flags & VPTE) != 0;
  const u64 wr = (flags & ACCESS_WRITE) ? 1 : 0;
  const u64 opcode = (u64)(ins >> 26);

  // A D-stream fault: DFAULT, with what it writes.
  auto dfault = [&](u64 mstat_bits) {
    state.exc_addr = state.current_pc;
    cSystem->cpu_clear_lock(state.iProcNum); // as GO_PAL does
    state.exc_sum = ev8_regs_of(ins);
    if (!vpte) {
      state.fault_va = virt;
      state.va_form_va = virt;
    }
    state.mm_stat = (opcode << 4) | wr | mstat_bits;
    ev8_trap(kDfault, ins);
    return -1;
  };
  auto iacv = [&]() {
    state.exc_addr = state.current_pc;
    cSystem->cpu_clear_lock(state.iProcNum);
    ev8_trap(kIacv, ins);
    return -1;
  };

  u64 pa = 0;
  const int sp = ev8_superpage(virt, exec, cm, &pa);
  if (sp == 1) {
    *phys = pa;
    // As on the other rows: a superpage matches every address space.
    if (asm_bit)
      *asm_bit = true;
    return 0;
  }
  if (sp < 0) {
    if (!forreal)
      return -1;
    return exec ? iacv() : dfault(2);
  }

  const int i = FindTBEntry(virt, flags);
  if (i < 0) {
    if (!forreal)
      return -1;
    if (vpte) {
      state.exc_addr = state.current_pc;
      ev8_trap((m_ev8.m_ctl & (U64(1) << 22)) ? kDtbmDoubleAlt : kDtbmDouble,
               ins);
    } else if (exec) {
      state.exc_addr = state.current_pc;
      ev8_trap(m_ev8.i_console ? kItbMissCons : kItbMiss, ins);
    } else {
      m_ev8.dtbms_ret_addr = state.current_pc;
      state.exc_sum = ev8_regs_of(ins);
      state.fault_va = virt;
      state.va_form_va = virt;
      state.mm_stat = (opcode << 4) | wr;
      ev8_trap(m_ev8.m_console ? kDtbmSingleCons : kDtbmSingle, ins);
    }
    return -1;
  }

  const STBEntry &e = state.tb[t][i];
  if (!(flags & NO_CHECK)) {
    if (!e.access[flags & ACCESS_WRITE][cm] ||
        ((flags & WRCHK) && !e.access[1][cm])) {
      if (!forreal)
        return -1;
      return exec ? iacv() : dfault(2);
    }
    if (e.fault[flags & ACCESS_MODE] || ((flags & WRCHK) && e.fault[1])) {
      if (!forreal)
        return -1;
      if (exec)
        return iacv();
      const u64 for_bit = (!(flags & ACCESS_WRITE) && e.fault[0]) ? 4 : 0;
      const u64 fow_bit =
          (((flags & ACCESS_WRITE) || (flags & WRCHK)) && e.fault[1]) ? 8 : 0;
      return dfault(for_bit | fow_bit);
    }
  }
  *phys = e.phys | (virt & e.keep_mask);
  if (asm_bit)
    *asm_bit = e.asm_bit != 0;
  return 0;
}

// --- The TB -----------------------------------------------------------------
//
// Storage and lookup are the core's (state.tb, the index, FindTBEntry); the
// entries are the 21464's [IDS pp. 305-307, 495-497, 503-506]: a tag of
// VA<51:13> (fewer bits with a granularity hint), the ASN or ASM, and the
// TPU group whose valid bit it carries, "at most one set": a fill sets the
// writer's group's [p. 306]. An entry is `valid` while it is present and of
// the current group, so FindTBEntry, the JIT's helpers and the data page
// cache see this TPU's group only.
//
// [decision] Each TPU keeps a TB of its own, with the group tags; the
// chip's TPUs do not share one. A shared TB would put a lock (or a
// cross-thread invalidation) on every TB lookup and fill of four host
// threads, and on the data page cache in front of them; with a TB per TPU
// the lookup path is the other rows'. Mode 2 (every TPU in a group of its
// own, what N25 has Linux use, "expected to be used by VMS" [p. 306]) is
// exact: no entry is ever another TPU's. Mode 3 (TPUs sharing a group) is
// not: a fill by one TPU is not seen by the others, and an invalidation by
// one reaches only its own TB. Software that invalidates what it changes on
// every TPU it runs on sees no difference but in the misses it takes.
// docs/cpu-fidelity.md records it.

/**
 * A fill: ITB_PTE [IDS pp. 495-496] or DTB_PTE0/1 [pp. 504-505], with the
 * pending tag, the stream's ASN and TPU group. With 8 KB pages the PFN is
 * PA<44:13>, with 64 KB PA<47:16>; a granularity hint of 1-3 is 8, 64 or
 * 512 pages [p. 306], "increased by one" with 64 KB pages [p. 307], which
 * the 64 KB base already is. A DTB PTE with VALID clear is dropped and
 * counted (N10). A fill of a page this group already holds replaces that
 * entry (N9: the second of the DTB writer block's two writes, which on the
 * chip go to the two copies of one entry).
 **/
void CAlphaCPU::ev8_add_tb(int t, u64 virt, u64 pte) {
  const bool itb = (t == TB_INDEX_ITB);
  if (!itb && !(pte & 1)) {
    ++m_ev8.invalid_ptes;
    if (s_trace_ev8 && m_ev8.invalid_ptes <= 50)
      printf("%%CPU-I-EV8: cpu%d DTB_PTE %016" PRIx64 " for %016" PRIx64
             " has VALID clear: no entry (the chip would hang, Restriction "
             "9), pc=%016" PRIx64 "\n",
             (int)state.iProcNum, pte, virt, state.current_pc);
    return;
  }
  const int base = ev8_va52(itb) ? 16 : 13;
  const int shift = base + 3 * (int)((pte >> 5) & 3);
  const u64 keep = (U64(1) << shift) - 1;
  const u64 match = U64(0x000fffffffffffff) & ~keep; // VA<51:shift>
  const u64 phys = ((pte >> 32) << base) & kPaMask & ~keep;
  const int grp = itb ? m_ev8.i_grp : m_ev8.m_grp;
  const int asn = itb ? state.asn : state.asn0;
  const bool asm_bit = (pte & 0x10) != 0;

  int i = -1;
  for (int j = 0; j < TB_ENTRIES; j++) {
    const STBEntry &o = state.tb[t][j];
    if (o.ev8_present && o.ev8_grp == grp &&
        !((o.virt ^ virt) & o.match_mask) &&
        (o.asm_bit || o.asn == asn || asm_bit)) {
      i = j;
      break;
    }
  }
#ifdef ES40_JIT
  const bool itb_remap = itb && i >= 0 && state.tb[t][i].phys != phys;
#endif
  if (i < 0) {
    i = state.next_tb[t];
    state.next_tb[t] = (i + 1 == TB_ENTRIES) ? 0 : i + 1;
  }
  STBEntry &e = state.tb[t][i];
  const STBEntry old = e;
  if (old.valid)
    tb_idx_drop(t, i);
  e.match_mask = match;
  e.keep_mask = keep;
  e.virt = virt & match;
  e.phys = phys;
  e.asn = asn;
  e.asm_bit = asm_bit ? 1 : 0;
  // Read enables KRE, ERE, SRE, URE <11:8>; the DTB's write enables KWE ..
  // UWE <15:12>, FOR <1>, FOW <2>. The ITB has no fault bit.
  for (int m = 0; m < 4; m++) {
    e.access[0][m] = (int)((pte >> (8 + m)) & 1);
    e.access[1][m] = itb ? 0 : (int)((pte >> (12 + m)) & 1);
  }
  e.fault[0] = itb ? 0 : (int)((pte >> 1) & 1);
  e.fault[1] = itb ? 0 : (int)((pte >> 2) & 1);
  e.fault[2] = 0;
  e.ev8_grp = (u8)grp;
  e.ev8_present = 1;
  e.valid = true;
  tb_idx_add(t, i);
  state.last_found_tb[t][0] = state.last_found_tb[t][1] = i;
  if (!itb) {
    if (old.valid)
      flush_data_page_cache_range(old.virt, old.match_mask);
    flush_data_page_cache_range(virt, match);
  }
#ifdef ES40_JIT
  if (itb_remap && m_jit)
    m_jit->note_itb_invalidate(CJitEngine::EPOCH_REMAP);
#endif
}

/// What caches a TB's translations, after entries left it.
void CAlphaCPU::ev8_tb_changed(int t) {
  if (t == TB_INDEX_DATA)
    flush_data_page_cache();
#ifdef ES40_JIT
  else if (m_jit)
    m_jit->note_itb_invalidate(CJitEngine::EPOCH_TBIA);
#endif
}

/**
 * ITB_IM and DTB_IM [IDS pp. 494, 502-503]. The mode's values are 00 all
 * groups, 01 this group, 03 its ASM entries, 05 its non-ASM entries, 0D its
 * non-ASM entries of this ASN, 10 all and reset the write pointer, 16 the
 * write pointer only. They read as bits [inference], which is how every
 * other value is taken: <0> this group only, <1> ASM entries only, <2>
 * non-ASM entries only (with <1>: none), <3> this ASN only, <4> reset the
 * write pointer. "This group" is the stream's TPU_GRP: ITB_IM's text says
 * M_PCTX's, ITB_IS's I_PCTX's [pp. 494-495, conflict]; I_PCTX's for the
 * ITB, which PALcode writes alike anyway [inference].
 **/
void CAlphaCPU::ev8_tb_im(int t, u64 mode) {
  const bool itb = (t == TB_INDEX_ITB);
  const int grp = itb ? m_ev8.i_grp : m_ev8.m_grp;
  const int asn = itb ? state.asn : state.asn0;
  for (int j = 0; j < TB_ENTRIES; j++) {
    STBEntry &e = state.tb[t][j];
    if (!e.ev8_present)
      continue;
    if ((mode & 1) && e.ev8_grp != grp)
      continue;
    if ((mode & 2) && !e.asm_bit)
      continue;
    if ((mode & 4) && e.asm_bit)
      continue;
    if ((mode & 8) && e.asn != asn)
      continue;
    if (e.valid)
      tb_idx_drop(t, j);
    e.ev8_present = 0;
    e.valid = false;
  }
  if (mode & 0x10)
    state.next_tb[t] = 0;
  ev8_tb_changed(t);
}

/// ITB_IS and DTB_IS: the entries of this group and ASN (or ASM) that map
/// VA<51:13> [IDS pp. 495, 503].
void CAlphaCPU::ev8_tb_is(int t, u64 va) {
  const bool itb = (t == TB_INDEX_ITB);
  const int grp = itb ? m_ev8.i_grp : m_ev8.m_grp;
  const int asn = itb ? state.asn : state.asn0;
  for (int j = 0; j < TB_ENTRIES; j++) {
    STBEntry &e = state.tb[t][j];
    if (!e.ev8_present || e.ev8_grp != grp || ((e.virt ^ va) & e.match_mask) ||
        !(e.asm_bit || e.asn == asn))
      continue;
    if (e.valid)
      tb_idx_drop(t, j);
    e.ev8_present = 0;
    e.valid = false;
    if (!itb)
      flush_data_page_cache_range(e.virt, e.match_mask);
  }
  if (!itb)
    flush_data_page_cache_range(va, U64(0x000fffffffffe000));
#ifdef ES40_JIT
  else if (m_jit)
    m_jit->note_itb_invalidate(CJitEngine::EPOCH_TBIS);
#endif
}

/// A TPU_GRP write (I_PCTX for the ITB, M_PCTX for the DTB [pp. 492, 509]):
/// the entries of the new group show, the others do not. The I-cache's
/// lines and compiled code from the old group's translations go too: the
/// chip tags Icache blocks with the group (IC_FLUSH flushes "all Icache
/// blocks that match the group number" [p. 493]).
void CAlphaCPU::ev8_set_group(int t, int grp) {
  int &cur = (t == TB_INDEX_ITB) ? m_ev8.i_grp : m_ev8.m_grp;
  if (cur == grp)
    return;
  cur = grp;
  for (int j = 0; j < TB_ENTRIES; j++) {
    STBEntry &e = state.tb[t][j];
    e.valid = e.ev8_present && e.ev8_grp == grp;
  }
  tb_idx_rebuild();
  if (t == TB_INDEX_ITB) {
    JIT_FLUSH_CAUSE(EPOCH_ICFLUSH);
    flush_icache();
  }
  ev8_tb_changed(t);
}

/// Every entry gone (reset): tbia's, and none present in any group.
void CAlphaCPU::ev8_tb_clear(int t) {
  tbia(t == TB_INDEX_ITB ? ACCESS_EXEC : ACCESS_READ);
  for (int j = 0; j < TB_ENTRIES; j++) {
    state.tb[t][j].ev8_present = 0;
    state.tb[t][j].ev8_grp = 0;
  }
}

// --- Jumps, CALL_PAL, CMOV ---------------------------------------------------

/**
 * JMP, JSR, RET and JSR_COROUTINE. The mode is PC<1:0>: 00 normal, 01
 * PALmode, 11 SuperPALmode [IDS pp. 484-485, 573]. In PALmode (either) a
 * jump takes the new mode from Rb<1:0> (N2, Table 17-5 [p. 574]): RET
 * leaves PALmode, enters SuperPALmode (N1: which behaves as PALmode, no
 * PNMI) or stays; 10 is no mode and is taken as normal [decision]. Ra gets
 * the next PC with its mode bits (N2, for all four, one instruction with
 * four hints [inference]). In normal mode Rb<1:0> are ignored and no RET
 * raises the mode [p. 573]. A jump into normal mode whose target is outside
 * the virtual address space, no enabled superpage's and not sign-extended
 * for I_CTL<VA_SIZE>, takes BAD_JUMP_IVA at the jump, which does not
 * complete (Ra unwritten): EXC_ADDR and EXC_SUM [pp. 629, 633].
 **/
void CAlphaCPU::ev8_jump(u32 ins) {
  const u64 target = state.r[REG_2];
  const bool pal = (state.pc & 1) != 0;
  u64 npc;
  if (pal) {
    const u64 m = target & 3;
    npc = (target & ~U64(3)) | (m == 1 ? 1 : m == 3 ? 3 : 0);
  } else {
    npc = target & ~U64(3);
  }
  if (!(npc & 1)) {
    u64 dummy;
    if (ev8_superpage(npc, true, 0, &dummy) < 0) {
      state.exc_addr = state.current_pc;
      state.exc_sum = ev8_regs_of(ins);
      cSystem->cpu_clear_lock(state.iProcNum);
      ev8_trap(kBadJumpIva, ins);
      return;
    }
  }
  const u64 link = pal ? state.pc : (state.pc & ~U64(3));
  state.r[REG_1] = link;
  set_pc(npc);
  if (trace_calls_on() && ((ins >> 14) & 3) == 1)
    trace_call((link & ~U64(3)) - 4, npc & ~U64(3));
  // Back in normal mode after an interrupt: what is still pending is looked
  // at again, as the 21264 rows' HW_RET does (m_int_resignal).
  if (pal && !(npc & 1) && m_int_resignal) {
    m_int_resignal = 0;
    state.check_int = true;
  }
}

/**
 * CALL_PAL (its function checked by DO_CALL_PAL): PC<51:15> = PAL_BASE,
 * PC<13> = 1, PC<12> = function<7>, PC<11:6> = function<5:0>, PC<1> the
 * current PC<1>, PC<0> = 1 [IDS p. 499]. It writes "ShadowReg1" [p. 149]:
 * S1 gets the return PC with the caller's mode in <1:0>, and EXC_ADDR is
 * not written (Table 22-3 does not list CALL_PAL) [inference, 1.3].
 **/
void CAlphaCPU::ev8_call_pal(u32 function) {
  state.r[32 + 25] = state.pc;
  set_pc((state.pal_base & kPalBaseMask) | (U64(1) << 13) |
         ((u64)(function & 0x80) << 5) | ((u64)(function & 0x3f) << 6) |
         (state.current_pc & 2) | 1);
}

/**
 * A CMOV in PALmode (ev8-native.md 1.3). The 21464 splits a native CMOV,
 * one followed by the `bis $31,$31,$31` its CMOV2 replaces, into `CMOVx1
 * Ra, Rc -> Rc; CMOV2 Rc, Rb -> Rc` [IDS p. 580], whose first half reads
 * the old Rc as operand B: the destination is Rc's destination register
 * (R24, R25 -> S0, S1), the value kept when the condition fails is Rc's
 * operand-B register (R22, R23 -> S0, S1). That is Restriction 13's failure
 * for R24 and R25, and the same for R22 and R23 [inference]; Alphabox
 * executes exactly that, so a PALcode breaking the restriction gets what
 * the chip would give it. A legacy CMOV (no placeholder after it) has "all
 * replacements of Rc" disabled [p. 574]: it reads and writes the
 * architectural Rc. Ra and Rb are replaced as usual. Returns false for an
 * opcode-11 word that is no CMOV.
 **/
bool CAlphaCPU::ev8_pal_cmov(u32 ins) {
  const u32 fn = (ins >> 5) & 0x7f;
  bool taken;
  const u64 a = state.r[REG_1];
  switch (fn) {
  case 0x14: // CMOVLBS
    taken = (a & 1) != 0;
    break;
  case 0x16: // CMOVLBC
    taken = (a & 1) == 0;
    break;
  case 0x24: // CMOVEQ
    taken = a == 0;
    break;
  case 0x26: // CMOVNE
    taken = a != 0;
    break;
  case 0x44: // CMOVLT
    taken = (s64)a < 0;
    break;
  case 0x46: // CMOVGE
    taken = (s64)a >= 0;
    break;
  case 0x64: // CMOVLE
    taken = (s64)a <= 0;
    break;
  case 0x66: // CMOVGT
    taken = (s64)a > 0;
    break;
  default:
    return false;
  }
  const u64 b = (ins & 0x1000) ? ((ins >> 13) & 0xff) : state.r[REG_2];
  // PALmode fetches are physical: the next word is at the next PC.
  const u64 next = state.pc & ~U64(3);
  const u32 w = (u32)(next + 4 <= dram_size ? dram_read(dram_ptr, next, 32)
                                            : sys_read(next, 32));
  const u32 rc = I_GETRC(ins);
  int dst, keep;
  if (w == kCmovPlaceholder) {
    dst = RREG_A(rc);
    keep = RREG_B(rc);
  } else {
    dst = keep = (int)rc;
  }
  state.r[dst] = taken ? b : state.r[keep];
  return true;
}

// --- HW_LD and HW_ST ---------------------------------------------------------

/**
 * HW_LD and HW_ST [IDS pp. 569-570]: type <15:13>, length <12:11> (byte,
 * word, longword, quadword), an 11-bit signed displacement <10:0>. No
 * alignment trap: the address is forced to the data size [p. 569]. The data
 * are those of LDBU, LDWU, LDL and LDQ. Types: 000 physical; 010
 * virtual/PTE (loads only: kernel access checks, a TB miss is the double
 * miss); 100 virtual; 101 virtual/WrChk (loads only: read and write checks,
 * FOR and FOW); 110 virtual/Alt (M_MODE<ALT>); 111 virtual/WrChk/Alt. The
 * other codes, 001 and 011 (N4), and the load-only ones on a store
 * [decision: an unassigned code], raise OPCDEC.
 **/
void CAlphaCPU::ev8_hw_ld(u32 ins) {
  const u32 type = (ins >> 13) & 7;
  const int bits = 8 << ((ins >> 11) & 3);
  int flags;
  switch (type) {
  case 0:
    flags = -1;
    break;
  case 2:
    flags = ACCESS_READ | VPTE;
    break;
  case 4:
    flags = ACCESS_READ;
    break;
  case 5:
    flags = ACCESS_READ | WRCHK;
    break;
  case 6:
    flags = ACCESS_READ | ALT;
    break;
  case 7:
    flags = ACCESS_READ | ALT | WRCHK;
    break;
  default:
    report_opcdec(0x1b, (int)type);
    GO_PAL(OPCDEC);
    return;
  }
  const u64 va = (state.r[REG_2] + sext_from(ins, 10)) & ~(u64)(bits / 8 - 1);
  u64 pa = va;
  if (flags >= 0 && virt2phys(va, &pa, flags, NULL, ins))
    return;
  u64 v = pa + bits / 8 <= dram_size ? dram_read(dram_ptr, pa, bits)
                                     : sys_read(pa, bits);
  if (bits == 32)
    v = sext_u64_32(v);
  state.r[REG_1] = v;
}

void CAlphaCPU::ev8_hw_st(u32 ins) {
  const u32 type = (ins >> 13) & 7;
  const int bits = 8 << ((ins >> 11) & 3);
  int flags;
  switch (type) {
  case 0:
    flags = -1;
    break;
  case 4:
    flags = ACCESS_WRITE;
    break;
  case 6:
    flags = ACCESS_WRITE | ALT;
    break;
  default:
    report_opcdec(0x1f, (int)type);
    GO_PAL(OPCDEC);
    return;
  }
  const u64 va = (state.r[REG_2] + sext_from(ins, 10)) & ~(u64)(bits / 8 - 1);
  u64 pa = va;
  if (flags >= 0 && virt2phys(va, &pa, flags, NULL, ins))
    return;
  const u64 v = state.r[REG_1];
  if (pa + bits / 8 <= dram_size) {
    dram_write(dram_ptr, pa, bits, v);
    note_dram_write(pa);
  } else {
    sys_write(pa, bits, v);
  }
}

// --- QUIESCE's timeout -------------------------------------------------------

/// QUIESCE_TIMEOUT<TIMEOUT> cycles at this processor's clock, when
/// TIMEOUT_EN is set; ~0 (no timeout) when it is not [IDS pp. 510-511]. The
/// field <19:4> counts cycles in place: its reset value 0x280 is the
/// register's 0x2800, 10240 cycles [ev8-isa.md 3.1, inference].
u64 CAlphaCPU::ev8_quiesce_ns() const {
  const u64 qt = m_ev8.quiesce_timeout;
  if (!(qt & 1))
    return ~U64(0);
  const u64 cycles = qt & U64(0xffff0);
  return (u64)((__uint128_t)cycles * 1000000000u / (cpu_hz ? cpu_hz : 1));
}

// --- The state file ----------------------------------------------------------

namespace {
constexpr u32 kEv8Magic1 = 0x45563831; // "EV81"
constexpr u32 kEv8Magic2 = 0x31385645;
} // namespace

/// After `state`, for this family only: the IPRs it has no field for, and
/// the chip's (each TPU writes them; they are the same).
int CAlphaCPU::ev8_save(FILE *f) {
  const u64 chip[2] = {m_ev8_chip ? m_ev8_chip->cpu_cnfg.load() : 0,
                       m_ev8_chip ? m_ev8_chip->dc_ctl.load() : 0};
  fwrite(&kEv8Magic1, sizeof(u32), 1, f);
  fwrite(&m_ev8, sizeof(m_ev8), 1, f);
  fwrite(chip, sizeof(chip), 1, f);
  fwrite(&kEv8Magic2, sizeof(u32), 1, f);
  return 0;
}

int CAlphaCPU::ev8_restore(FILE *f) {
  u32 m1 = 0, m2 = 0;
  u64 chip[2];
  if (fread(&m1, sizeof(u32), 1, f) != 1 || m1 != kEv8Magic1 ||
      fread(&m_ev8, sizeof(m_ev8), 1, f) != 1 ||
      fread(chip, sizeof(chip), 1, f) != 1 ||
      fread(&m2, sizeof(u32), 1, f) != 1 || m2 != kEv8Magic2) {
    printf("%s: the native EV8's IPRs are missing or damaged!\n", devid_string);
    return -1;
  }
  if (m_ev8_chip) {
    m_ev8_chip->cpu_cnfg.store(chip[0]);
    m_ev8_chip->dc_ctl.store(chip[1]);
  }
  return 0;
}
