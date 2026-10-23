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
 * Contains code macros for the processor PALmode instructions, and the
 * HW_MFPR/HW_MTPR implementation both engines call.
 * Based on HRM.
 **/
#if !defined(INCLUDED_CPU_PAL_H)
#define INCLUDED_CPU_PAL_H

/* Unimplemented IPR index on HW_MFPR/HW_MTPR: EV6 reads zero and ignores the
 * write. Warn once per index and continue instead of trapping OPCDEC (PALcode
 * builds such as the Windows 2000 betas touch IPRs Alphabox doesn't model). */
#define UNKNOWN_IPR(dir)                                                       \
  do {                                                                         \
    static bool warned_ipr[256];                                               \
    if (!warned_ipr[function & 0xff]) {                                        \
      warned_ipr[function & 0xff] = true;                                      \
      printf("%%CPU-W-IPR: unimplemented HW_M" dir "PR index %02x at "         \
             "pc=%016" PRIx64 " (ignored)\n",                                  \
             (unsigned)function, state.current_pc);                            \
    }                                                                          \
  } while (0)

/* HW_MFPR's value and HW_MTPR's effect: one implementation, which the
 * interpreter's DO_HW_MFPR/DO_HW_MTPR and compiled code's helpers
 * (jit_hw_mfpr, jit_hw_mtpr) both call. `jit` names the engine in the IRQ
 * trace. Compiled code calls only for the IPRs classify() compiles. */
inline u64 CAlphaCPU::hw_mfpr(u32 function, bool jit) {
  if ((function & 0xc0) == 0x40) /* PCTX */
    return ((u64)state.asn << 39) | ((u64)state.astrr << 9) |
           ((u64)state.aster << 5) | (state.fpen ? U64(0x1) << 2 : 0) |
           (state.ppcen ? U64(0x1) << 1 : 0);
  switch (function) {
  case 0x05: /* PMPC     */
    return state.pmpc;

  case 0x06: /* EXC_ADDR */
    return state.exc_addr;

  case 0x07: /* IVA_FORM */
    return va_form(state.exc_addr, true);

  case 0x08: /* IER_CM   */
  case 0x09: /* CM       */
  case 0x0a: /* IER      */
  case 0x0b: /* IER_CM   */
    return (((u64)state.eien) << 33) | (((u64)state.slen) << 32) |
           (((u64)state.cren) << 31) | (((u64)state.pcen) << 29) |
           (((u64)state.sien) << 13) | (((u64)state.asten) << 13) |
           (((u64)state.cm) << 3);

  case 0x0c: /* SIRR */
    return ((u64)state.sir) << 13;

  case 0x0d: /* ISUM */
  {
    const u64 isum = (((u64)(state.eir & state.eien)) << 33) |
                     (((u64)(state.slr & state.slen)) << 32) |
                     (((u64)(state.crr & state.cren)) << 31) |
                     (((u64)(state.pcr & state.pcen)) << 29) |
                     (((u64)(state.sir & state.sien)) << 13) |
                     (((u64)(((U64(0x1) << (state.cm + 1)) - 1) & state.aster &
                             state.astrr & (state.asten * 0x3)))
                      << 3) |
                     (((u64)(((U64(0x1) << (state.cm + 1)) - 1) & state.aster &
                             state.astrr & (state.asten * 0xc)))
                      << 7);
    irq_trace_ipr(jit ? "jISUM" : "iISUM", function, isum);
    return isum;
  }

  case 0x0f: /* EXC_SUM */
    return state.exc_sum;

  case 0x10: /* PAL_BASE */
    return state.pal_base;

  case 0x11: /* i_ctl */
    return state.i_ctl_other | (((u64)m_model->chip_id) << 24) |
           (u64)state.i_ctl_vptb | (((u64)state.i_ctl_va_mode) << 15) |
           (state.hwe ? U64(0x1) << 12 : 0) | (state.sde ? U64(0x1) << 7 : 0) |
           (((u64)state.i_ctl_spe) << 3);

  case 0x14: /* PCTR_CTL */
    return state.pctr_ctl;

  case 0x16: /* I_STAT */
    return state.i_stat;

  case 0x27: /* MM_STAT */
    return state.mm_stat;

  case 0x2a: /* DC_STAT */
    return state.dc_stat;

  case 0x2b: /* C_DATA */
    return 0;

  case 0xc0: /* CC */
    return (((u64)state.cc_offset) << 32) | (state.cc & U64(0xffffffff));

  case 0xc2: /* VA */
    return state.fault_va;

  case 0xc3: /* VA_FORM */
    return va_form(state.va_form_va, false);

  default:
    UNKNOWN_IPR("F");
    return 0;
  }
}

#define DO_HW_MFPR state.r[REG_1] = hw_mfpr(function);

inline void CAlphaCPU::hw_mtpr(u32 function, u64 value, bool jit) {
  const char *const trace = jit ? "jMTPR" : "iMTPR";
  if ((function & 0xc0) == 0x40) {
    if (function & 1) {
      state.asn = (int)(value >> 39) & 0xff;
      flush_data_page_cache();
      jit_note_asn_change();
    }
    if (function & 2) {
      state.aster = (int)(value >> 5) & 0xf;
      if (int_deliverable())
        state.check_int = true;
    }
    if (function & 4) {
      state.astrr = (int)(value >> 9) & 0xf;
      if (int_deliverable())
        state.check_int = true;
    }
    if (function & 8)
      state.ppcen = (int)(value >> 1) & 1;
    if (function & 16)
      state.fpen = (int)(value >> 2) & 1;
    return;
  }
  switch (function) {
  case 0x00: /* ITB_TAG */
    state.last_tb_virt = value;
    break;

  case 0x01: /* ITB_PTE */
    add_tb_i(state.last_tb_virt, value);
    break;

  case 0x02: /* ITB_IAP */
    tbiap(ACCESS_EXEC);
    break;

  case 0x03: /* ITB_IA */
    tbia(ACCESS_EXEC);
    break;

  case 0x04: /* ITB_IS */
    tbis(value, ACCESS_EXEC);
    break;

  case 0x09: /* CM */
    state.cm = (int)(value >> 3) & 3;
    dpc_context_changed();
    irq_trace_ipr(trace, function, value);
    if (int_deliverable())
      state.check_int = true;
    break;

  case 0x0b: /* IER_CM */
    state.cm = (int)(value >> 3) & 3;
    dpc_context_changed();
    if (int_deliverable())
      state.check_int = true;
    [[fallthrough]];
  case 0x0a: /* IER */
    state.asten = (int)(value >> 13) & 1;
    state.sien = (int)(value >> 13) & 0xfffe;
    state.pcen = (int)(value >> 29) & 3;
    state.cren = (int)(value >> 31) & 1;
    state.slen = (int)(value >> 32) & 1;
    state.eien = (int)(value >> 33) & 0x3f;
    irq_trace_ipr(trace, function, value);
    if (int_deliverable())
      state.check_int = true;
    break;

  case 0x0c: /* SIRR */
    state.sir = (int)(value >> 13) & 0xfffe;
    irq_trace_ipr(trace, function, value);
    if (int_deliverable())
      state.check_int = true;
    break;

  case 0x0e: /* HW_INT_CLR */
    state.pcr &= ~((value >> 29) & U64(0x3));
    state.crr &= ~((value >> 31) & U64(0x1));
    state.slr &= ~((value >> 32) & U64(0x1));
    break;

  case 0x10: /* PAL_BASE */
    set_PAL_BASE(value & U64(0x00000fffffff8000));
    break;

  case 0x11: /* i_ctl */
    /* Bit 20 is ST_WAIT_64K on the EV68CB, read-write (HRM Table 5-11) */
    state.i_ctl_other = value & U64(0x00000000007e2f67);
    state.i_ctl_vptb = sext_u64_48(value & U64(0x0000ffffc0000000));
    state.i_ctl_spe = (int)((value >> 3) & 7);
    state.sde = (value >> 7) & 1;
    state.hwe = (value >> 12) & 1;
    state.i_ctl_va_mode = (int)(value >> 15) & 3;
    break;

  case 0x12: /* ic_flush_asm */
    flush_icache_asm();
    break;

  case 0x13: /* IC_FLUSH */
    JIT_FLUSH_CAUSE(EPOCH_ICFLUSH);
    flush_icache();
    break;

  case 0x14: /* PCTR_CTL */
    state.pctr_ctl = value & U64(0xffffffffffffffdf);
    break;

  case 0x08: /* IER_CM with neither field selected */
  case 0x0f: /* EXC_SUM: read-only; beta NT PALs write 0 */
  case 0x15: /* CLR_MAP */
  case 0x17: /* SLEEP   */
  case 0x27: /* MM_STAT */
  case 0x2b: /* C_DATA  */
  case 0x2c: /* C_SHIFT */
  case 0x2d: /* M_FIX */
    break;

  case 0x16:                /* I_STAT */
    state.i_stat &= ~value; /* W1C */
    break;

  case 0x20: /* DTB_TAG0 */
    last_dtb_virt[0] = value;
    break;

  case 0x21: /* DTB_PTE0 */
    add_tb_d(last_dtb_virt[0], value, 0);
    break;

  case 0x24: /* DTB_IS0 */
    tbis_d(value, state.asn0);
    break;

  case 0x25: /* DTB_ASN0 */
    state.asn0 = (int)(value >> 56);
    dpc_context_changed();
    flush_data_page_cache();
    break;

  case 0x26: /* DTB_ALTMODE */
    state.alt_cm = (int)(value & 3);
    break;

  case 0x28: /* M_CTL */
    state.smc = (int)(value >> 4) & 3;
    state.m_ctl_spe = (int)(value >> 1) & 7;
    flush_data_page_cache();
    break;

  case 0x29: /* DC_CTL */
    state.dc_ctl = value;
    break;

  case 0x2a: /* DC_STAT */
    state.dc_stat &= ~value;
    break;

  case 0xa0: /* DTB_TAG1 */
    last_dtb_virt[1] = value;
    break;

  case 0xa1: /* DTB_PTE1 */
    add_tb_d(last_dtb_virt[1], value, 1);
    break;

  case 0xa2: /* DTB_IAP */
    tbiap(ACCESS_READ);
    break;

  case 0xa3: /* DTB_IA */
    tbia(ACCESS_READ);
    break;

  case 0xa4: /* DTB_IS1 */
    tbis_d(value, state.asn1);
    break;

  case 0xa5: /* DTB_ASN1 */
    state.asn1 = (int)(value >> 56);
    flush_data_page_cache();
    break;

  case 0xc0: /* CC */
    state.cc_offset = (u32)(value >> 32);
    break;

  case 0xc1: /* CC_CTL */
    state.cc_ena = (value >> 32) & 1;
    state.cc = (u32)(value & U64(0xfffffff0));
    cc_last_read = state.cc; /* counter rewritten: rebase the RPCC floor */
    cc_borrow = 0;
    cc_wall_remainder = 0;
    break;

  case 0xc4: /* VA_CTL */
    state.va_ctl_vptb = sext_u64_48(value & U64(0x0000ffffc0000000));
    state.va_ctl_va_mode = (int)(value >> 1) & 3;
    flush_data_page_cache();
    tbia(ACCESS_READ);
    break;

  default:
    UNKNOWN_IPR("T");
    break;
  }
}

#define DO_HW_MTPR hw_mtpr(function, state.r[REG_2]);

/*
 * HW_RET (HRM 6.4.3) is a simple jump-to-target on EV6/EV68.
 */
/* A return to native mode after an interrupt was delivered raises check_int,
   so an interrupt still pending is taken (CAlphaCPU::m_int_resignal). The
   compiled HW_RETs do the same. */
#define DO_HW_RET                                                              \
  do {                                                                         \
    u64 target = state.r[REG_2] & ~U64(0x2);                                   \
    set_pc(target);                                                            \
    if (!(target & 1) && m_int_resignal) {                                     \
      m_int_resignal = 0;                                                      \
      state.check_int = true;                                                  \
    }                                                                          \
  } while (0)

#define DO_HW_LDL                                                              \
  switch (function) {                                                          \
  case 0: /* longword physical */                                              \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    state.r[REG_1] = READ_PHYS_NT(32);                                         \
    state.r[REG_1] = sext_u64_32(                                              \
        state.r[REG_1]); /* HW_LDL canonical longword (vmspal hw_ldl) */       \
    break;                                                                     \
                                                                               \
  case 2: /* longword physical locked */                                       \
  {                                                                            \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    CSystem::CLLSCDRAMGuard _llsc_guard(cSystem, phys_address < dram_size);    \
    state.r[REG_1] = READ_PHYS_NT(32);                                         \
    state.r[REG_1] = sext_u64_32(                                              \
        state.r[REG_1]); /* HW_LDL canonical longword (vmspal hw_ldl*/         \
    cSystem->cpu_lock(state.iProcNum, phys_address, state.r[REG_1]);           \
    m_llsc_clean = 1;                                                          \
    break;                                                                     \
  }                                                                            \
                                                                               \
  case 4: /* longword virtual VPTE (HRM 6.4.1 TYPE 0102: LD_VPTE) --           \
           * page-table-entry fetch; access checked against KERNEL             \
           * mode regardless of executing CM. virt2phys forces cm=0            \
           * when the VPTE flag is set. */                                     \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | VPTE);                \
    state.r[REG_1] = READ_PHYS_NT(32);                                         \
    state.r[REG_1] = sext_u64_32(                                              \
        state.r[REG_1]); /* HW_LDL canonical longword (vmspal hw_ldl,) */      \
    break;                                                                     \
                                                                               \
  case 8: /* longword virtual (HRM 6.4.1 TYPE 1002) -- access checked          \
           * against current mode */                                           \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ);                       \
    state.r[REG_1] = READ_PHYS_NT(32);                                         \
    state.r[REG_1] = sext_u64_32(                                              \
        state.r[REG_1]); /* HW_LDL canonical longword (vmspal hw_ldl) */       \
    break;                                                                     \
                                                                               \
  case 10: /* longword virtual check (HRM 6.4.1 TYPE 1012: WrChk) */           \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | WRCHK);               \
    state.r[REG_1] = READ_PHYS_NT(32);                                         \
    state.r[REG_1] = sext_u64_32(                                              \
        state.r[REG_1]); /* HW_LDL canonical longword (vmspal hw_ldl) */       \
    break;                                                                     \
                                                                               \
  case 12: /* longword virtual alt (HRM 6.4.1 TYPE 1102) -- access             \
            * checked using DTB_ALT_MODE, matching QEMU brokenpipe             \
            * AlphaMMUIdx_AltMode (was incorrectly bypassed via                \
            * NO_CHECK). */                                                    \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | ALT);                 \
    state.r[REG_1] = READ_PHYS_NT(32);                                         \
    state.r[REG_1] = sext_u64_32(                                              \
        state.r[REG_1]); /* HW_LDL canonical longword (vmspal hw_ldl) */       \
    break;                                                                     \
                                                                               \
  case 14: /* longword virtual alt check (HRM TYPE 1112: WrChk/Alt) */         \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | ALT | WRCHK);         \
    state.r[REG_1] = READ_PHYS_NT(32);                                         \
    state.r[REG_1] = sext_u64_32(                                              \
        state.r[REG_1]); /* HW_LDL canonical longword (vmspal hw_ldl) */       \
    break;                                                                     \
                                                                               \
  default:                                                                     \
    UNKNOWN2;                                                                  \
  }

#define DO_HW_LDQ                                                              \
  switch (function) {                                                          \
  case 1: /* quadword physical */                                              \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    state.r[REG_1] = READ_PHYS_NT(64);                                         \
    break;                                                                     \
                                                                               \
  case 3: /* quadword physical locked */                                       \
  {                                                                            \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    CSystem::CLLSCDRAMGuard _llsc_guard(cSystem, phys_address < dram_size);    \
    state.r[REG_1] = READ_PHYS_NT(64);                                         \
    cSystem->cpu_lock(state.iProcNum, phys_address, state.r[REG_1]);           \
    m_llsc_clean = 1;                                                          \
    break;                                                                     \
  }                                                                            \
                                                                               \
  case 5: /* quadword virtual VPTE (HRM 6.4.1 TYPE 0102: LD_VPTE) --           \
           * see HW_LDL case 4 for full notes. */                              \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | VPTE);                \
    state.r[REG_1] = READ_PHYS_NT(64);                                         \
    break;                                                                     \
                                                                               \
  case 9: /* quadword virtual (HRM 6.4.1 TYPE 1002) -- see case 8 */           \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ);                       \
    state.r[REG_1] = READ_PHYS_NT(64);                                         \
    break;                                                                     \
                                                                               \
  case 11: /* quadword virtual check (HRM 6.4.1 TYPE 1012: WrChk) */           \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | WRCHK);               \
    state.r[REG_1] = READ_PHYS_NT(64);                                         \
    break;                                                                     \
                                                                               \
  case 13: /* quadword virtual alt (HRM 6.4.1 TYPE 1102) -- see case 12 */     \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | ALT);                 \
    state.r[REG_1] = READ_PHYS_NT(64);                                         \
    break;                                                                     \
                                                                               \
  case 15: /* quadword virtual alt check (HRM TYPE 1112: WrChk/Alt) */         \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | ALT | WRCHK);         \
    state.r[REG_1] = READ_PHYS_NT(64);                                         \
    break;                                                                     \
                                                                               \
  default:                                                                     \
    UNKNOWN2;                                                                  \
  }

#define DO_HW_STL                                                              \
  switch (function) {                                                          \
  case 0: /* longword physical */                                              \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    WRITE_PHYS_NT(state.r[REG_1], 32);                                         \
    break;                                                                     \
                                                                               \
  case 2: /* longword physical conditional */                                  \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    {                                                                          \
      CSystem::CLLSCDRAMGuard _llsc_guard(cSystem, phys_address < dram_size);  \
      state.r[REG_1] =                                                         \
          cSystem->cpu_stx_c(state.iProcNum, phys_address, 32, state.r[REG_1], \
                             dram_ptr, dram_size, this, llsc_intervened());    \
    }                                                                          \
    break;                                                                     \
                                                                               \
  case 4: /* longword virtual (HRM 6.4.1 Table 6-4 TYPE 0102) -- write         \
           * checked against current mode, matching QEMU brokenpipe            \
           * AlphaMMUIdx_Privileged. */                                        \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_WRITE);                      \
    WRITE_PHYS_NT(state.r[REG_1], 32);                                         \
    break;                                                                     \
                                                                               \
  case 12: /* longword virtual alt (HRM TYPE 1102) -- write checked            \
            * using DTB_ALT_MODE, matching brokenpipe AlphaMMUIdx_AltMode.*/   \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_WRITE | ALT);                \
    WRITE_PHYS_NT(state.r[REG_1], 32);                                         \
    break;                                                                     \
                                                                               \
  default:                                                                     \
    UNKNOWN2;                                                                  \
  }

#define DO_HW_STQ                                                              \
  switch (function) {                                                          \
  case 1: /* quadword physical */                                              \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    WRITE_PHYS_NT(state.r[REG_1], 64);                                         \
    break;                                                                     \
                                                                               \
  case 3: /* quadword physical conditional */                                  \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    {                                                                          \
      CSystem::CLLSCDRAMGuard _llsc_guard(cSystem, phys_address < dram_size);  \
      state.r[REG_1] =                                                         \
          cSystem->cpu_stx_c(state.iProcNum, phys_address, 64, state.r[REG_1], \
                             dram_ptr, dram_size, this, llsc_intervened());    \
    }                                                                          \
    break;                                                                     \
                                                                               \
  case 5: /* quadword virtual (HRM 6.4.1 Table 6-4 TYPE 0102) --               \
           * see HW_STL case 4. */                                             \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_WRITE);                      \
    WRITE_PHYS_NT(state.r[REG_1], 64);                                         \
    break;                                                                     \
                                                                               \
  case 13: /* quadword virtual alt (HRM TYPE 1102) -- see HW_STL 12. */        \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_WRITE | ALT);                \
    WRITE_PHYS_NT(state.r[REG_1], 64);                                         \
    break;                                                                     \
                                                                               \
  default:                                                                     \
    UNKNOWN2;                                                                  \
  }

#endif // INCLUDED_CPU_PAL_H
