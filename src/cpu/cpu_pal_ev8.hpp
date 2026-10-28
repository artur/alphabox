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
 * The native EV8's internal processor registers: HW_MFPR's value and
 * HW_MTPR's effect on the `ev8n` row (docs/platforms/ev8-native.md, 1.4),
 * beside the 21264's in cpu_pal.hpp.
 *
 * HW_MFPR is 0x19 with the index in <12:5> and the destination Rc in <4:0>
 * (N3); HW_MTPR is 0x1D with the index in <12:5> and the source Rb in
 * <20:16>; the reader and writer classes (<24:21>, <4:0>) order the
 * pipeline only, and an emulator that retires in order ignores them
 * [IDS pp. 571-573, inference]. An index's MSB is 1 for the Ibox, 0 for the
 * Mbox. Where Table 16-1 [IDS pp. 477-481] gives a register several
 * indexes, the low bits select the fields a write changes, and any of them
 * reads the whole register [inference].
 *
 * Indexes N6 to N8 are Alphabox decisions: CPU_BASE 0xAB, VCTL 0xAC, and
 * IER_CM, SIRR, ISUM, HW_INT_CLR at 0xB0-0xB3 with the 21264's layouts
 * without ASTEN. The performance registers (chapter 19) read zero and
 * ignore writes [decision]: nothing is counted. An index the table does not
 * have reads zero and ignores the write, as on the other rows (UNKNOWN_IPR).
 **/
#if !defined(INCLUDED_CPU_PAL_EV8_H)
#define INCLUDED_CPU_PAL_EV8_H

#include "cpu_pal.hpp"

/// I_CTL and M_CTL's fields [IDS pp. 489-490, 506-507].
#define EV8_CTL_VPTE U64(0x000ffffe00000000)    /* VPTE_BASE<51:33> */
#define EV8_CTL_REDUCED U64(0x0000000008000000) /* REDUCED_PT <27> */
#define EV8_CTL_SPE U64(0x0000000007000000)     /* SPE<2:0> <26:24> */
#define EV8_CTL_BIGEND U64(0x0000000000800000)  /* M_CTL BIG_ENDIAN <23> */
#define EV8_CTL_DBLALT U64(0x0000000000400000)  /* M_CTL DBL_ALT <22> */
#define EV8_CTL_SIZES U64(0x0000000000300000)   /* VA_SIZE<21>, PAGE_SIZE<20> */

inline u64 CAlphaCPU::ev8_mfpr(u32 index) {
  switch (index) {
  case 0x80: // I_CTL
  case 0x81:
  case 0x82:
  case 0x83:
    return m_ev8.i_ctl;
  case 0x88: // I_MODE: CONSOLE <5>, CURRENT <4:3>
  case 0x89:
  case 0x8a:
  case 0x8b:
    return (m_ev8.i_console ? U64(0x20) : 0) | ((u64)state.cm << 3);
  case 0x90: // I_PCTX: FP_ENABLE <19>, TPU_GRP <17:16>, ASN <15:8>
  case 0x91:
  case 0x92:
  case 0x93:
  case 0x94:
  case 0x95:
  case 0x96:
  case 0x97:
    return (state.fpen ? U64(1) << 19 : 0) | ((u64)m_ev8.i_grp << 16) |
           ((u64)(state.asn & 0xff) << 8);
  case 0xa0: // EXC_SUM
    return state.exc_sum;
  case 0xa1: // EXC_ADDR
    return state.exc_addr;
  case 0xa3: // IVA_FORM, from EXC_ADDR and I_CTL
    return ev8_va_form(state.exc_addr & ~U64(3), true);
  case 0xa6: // DTBMS_RET_ADDR
  case 0xa7: // ... and blocks loads and stores: nothing to block in order
    return m_ev8.dtbms_ret_addr;
  case 0xa8: // PAL_BASE
    return state.pal_base;
  case 0xa9: // PAL_TEMP1
    return m_ev8.pal_temp[0];
  case 0xaa: // PAL_TEMP2
    return m_ev8.pal_temp[1];
  case 0xab: // CPU_BASE (N6): the chip's PID <9:0>
    return m_ev8_chip ? (u64)(m_ev8_chip->pid & 0x3ff) : 0;
  case 0xac: // VCTL (N8): VEN <0>
    return m_ev8.vctl;
  case 0xb0: // IER_CM (N7): the 21264's layout without ASTEN <13>
    return (((u64)state.eien) << 33) | (((u64)state.slen) << 32) |
           (((u64)state.cren) << 31) | (((u64)state.pcen) << 29) |
           (((u64)state.sien) << 13) | (((u64)state.cm) << 3);
  case 0xb1: // SIRR
    return ((u64)state.sir) << 13;
  case 0xb2: // ISUM: what is pending and enabled
  {
    const u64 isum = (((u64)(state.eir & state.eien)) << 33) |
                     (((u64)(state.slr & state.slen)) << 32) |
                     (((u64)(state.crr & state.cren)) << 31) |
                     (((u64)(state.pcr & state.pcen)) << 29) |
                     (((u64)(state.sir & state.sien)) << 13);
    irq_trace_ipr("iISUM8", index, isum);
    return isum;
  }
  case 0xb8: // CC: OFFSET <63:32>, COUNT <31:4>, ENABLE <0>; MFPR "TBD"
  case 0xb9: // [IDS p. 482]: it reads the register (N7)
  case 0xba:
  case 0xbb:
    return (rpcc_read() & ~U64(0xf)) | (state.cc_ena ? 1 : 0);
  case 0xc8: // CPU_CNFG: CHIP_ID <63:56>, read-only
    return ((u64)m_model->chip_id << 56) |
           (m_ev8_chip ? m_ev8_chip->cpu_cnfg.load() : 0);
  case 0xc9: // IC_STAT: no parity error is ever raised
    return 0;
  case 0xd8: // TPU_CNFG: TPU_ID <57:56>, MCHK_EN <7>, PREF_RANGE <6:4>
    return ((u64)(m_tpu & 3) << 56) | m_ev8.tpu_cnfg;
  case 0x00: // M_CTL
  case 0x01:
  case 0x02:
  case 0x03:
    return m_ev8.m_ctl;
  case 0x08: // M_MODE: ALT <7:6>, CONSOLE <5>, CURRENT <4:3>
  case 0x09:
  case 0x0a:
  case 0x0b:
  case 0x0c:
  case 0x0d:
  case 0x0e:
  case 0x0f:
    return ((u64)(state.alt_cm & 3) << 6) | (m_ev8.m_console ? U64(0x20) : 0) |
           ((u64)state.cm << 3);
  case 0x10: // M_PCTX: TPU_GRP <17:16>, ASN <15:8>
  case 0x11:
  case 0x12:
  case 0x13:
    return ((u64)m_ev8.m_grp << 16) | ((u64)(state.asn0 & 0xff) << 8);
  case 0x20: // M_STAT
    return state.mm_stat;
  case 0x21: // VA
    return state.fault_va;
  case 0x23: // VA_FORM, from VA and M_CTL
    return ev8_va_form(state.va_form_va, false);
  case 0x38: // QUIESCE_TIMEOUT
    return m_ev8.quiesce_timeout;
  case 0x48: // DC_CTL
    return m_ev8_chip ? m_ev8_chip->dc_ctl.load() : 0;
  case 0x49: // DC_STAT: no error is ever raised
    return state.dc_stat;
  default:
    // The performance registers (Ibox 0xE0-0xE3, 0xE8-0xED, 0xF0-0xF7, Mbox
    // 0x60-0x63, 0x68, 0x6A-0x6C, 0x70-0x71): zero, nothing is counted.
    if ((index >= 0xe0 && index <= 0xe3) || (index >= 0xe8 && index <= 0xed) ||
        (index >= 0xf0 && index <= 0xf7) || (index >= 0x60 && index <= 0x63) ||
        index == 0x68 || (index >= 0x6a && index <= 0x6c) || index == 0x70 ||
        index == 0x71)
      return 0;
    // The write-only ones read zero too (ITB_IM .. IC_FLUSH_ASM, DTB_IM ..
    // DTB_TAG1, HW_INT_CLR); anything else is an index the table lacks.
    if (!((index >= 0xc0 && index <= 0xc6) || index == 0xcc || index == 0xcd ||
          (index >= 0x40 && index <= 0x47) || index == 0xb3)) {
      const u32 function = index;
      UNKNOWN_IPR("F");
    }
    return 0;
  }
}

inline void CAlphaCPU::ev8_mtpr(u32 index, u64 value) {
  // A field of several IPRs, written by any of them: the current mode is
  // I_MODE's, M_MODE's and IER_CM's CM (N7).
  auto set_cm = [&](int cm) {
    state.cm = cm & 3;
    dpc_context_changed();
    if (int_deliverable())
      state.check_int = true;
  };
  switch (index) {
  case 0x80: // I_CTL: 01 VPTE_BASE, 10 SUPERPAGE, VA_SIZE, PAGE_SIZE, 11 all
  case 0x81:
  case 0x82:
  case 0x83:
  case 0x00: // M_CTL: 01 VPTE_BASE, 10 SUPERPAGE, BIG_ENDIAN, VA_SIZE,
  case 0x01: // PAGE_SIZE, 11 all [IDS pp. 489, 506]; REDUCED_PT and
  case 0x02: // DBL_ALT with 11 only, as the select lists them
  case 0x03: {
    const bool ibox = (index & 0x80) != 0;
    u64 &ctl = ibox ? m_ev8.i_ctl : m_ev8.m_ctl;
    u64 mask = 0;
    if (index & 1)
      mask |= EV8_CTL_VPTE;
    if (index & 2)
      mask |= EV8_CTL_SPE | EV8_CTL_SIZES | (ibox ? 0 : EV8_CTL_BIGEND);
    if ((index & 3) == 3)
      mask |= EV8_CTL_REDUCED | (ibox ? 0 : EV8_CTL_DBLALT);
    ctl = (ctl & ~mask) | (value & mask);
    (ibox ? state.i_ctl_spe : state.m_ctl_spe) = (int)((ctl >> 24) & 7);
    if (ibox) {
      flush_icache(); // a fetch may now translate otherwise
    } else {
      flush_data_page_cache();
      vec_xlate_drop();
    }
    break;
  }
  case 0x88: // I_MODE: 01 CURRENT, 10 CONSOLE, 11 all
  case 0x89:
  case 0x8a:
  case 0x8b:
    if (index & 1)
      set_cm((int)(value >> 3));
    if (index & 2)
      m_ev8.i_console = (value >> 5) & 1;
    break;
  case 0x08: // M_MODE: 001 CURRENT, 010 CONSOLE, 100 ALT, 111 all
  case 0x09:
  case 0x0a:
  case 0x0b:
  case 0x0c:
  case 0x0d:
  case 0x0e:
  case 0x0f:
    if (index & 1)
      set_cm((int)(value >> 3));
    if (index & 2)
      m_ev8.m_console = (value >> 5) & 1;
    if (index & 4)
      state.alt_cm = (int)(value >> 6) & 3;
    break;
  case 0x90: // I_PCTX: 001 ASN, 010 TPU_GRP, 100 FP_ENABLE, 111 all
  case 0x91:
  case 0x92:
  case 0x93:
  case 0x94:
  case 0x95:
  case 0x96:
  case 0x97:
    if (index & 1) {
      state.asn = (int)(value >> 8) & 0xff;
      jit_note_asn_change();
    }
    if (index & 2)
      ev8_set_group(TB_INDEX_ITB, (int)(value >> 16) & 3);
    if (index & 4)
      state.fpen = (value >> 19) & 1;
    break;
  case 0x10: // M_PCTX: 01 ASN, 10 TPU_GRP, 11 all
  case 0x11:
  case 0x12:
  case 0x13:
    if (index & 1) {
      state.asn0 = state.asn1 = (int)(value >> 8) & 0xff;
      dpc_context_changed();
      flush_data_page_cache();
    }
    if (index & 2)
      ev8_set_group(TB_INDEX_DATA, (int)(value >> 16) & 3);
    break;
  case 0xa8: // PAL_BASE<47:15> (N6)
    set_PAL_BASE(value & U64(0x0000ffffffff8000));
    break;
  case 0xa9: // PAL_TEMP1
    m_ev8.pal_temp[0] = value;
    break;
  case 0xaa: // PAL_TEMP2
    m_ev8.pal_temp[1] = value;
    break;
  case 0xac: // VCTL (N8): VEN <0>; stored, the vectors are S7's
    m_ev8.vctl = value & 1;
    break;
  case 0xb0: // IER_CM (N7)
    set_cm((int)(value >> 3));
    state.sien = (int)(value >> 13) & 0xfffe;
    state.pcen = (int)(value >> 29) & 3;
    state.cren = (int)(value >> 31) & 1;
    state.slen = (int)(value >> 32) & 1;
    state.eien = (int)(value >> 33) & 0x3f;
    irq_trace_ipr("iMTPR8", index, value);
    if (int_deliverable())
      state.check_int = true;
    break;
  case 0xb1: // SIRR
    state.sir = (int)(value >> 13) & 0xfffe;
    irq_trace_ipr("iMTPR8", index, value);
    if (int_deliverable())
      state.check_int = true;
    break;
  case 0xb3: // HW_INT_CLR: CR <31>, PC <30:29>, SL <32> (N7)
    state.pcr &= ~((value >> 29) & U64(0x3));
    state.crr &= ~((value >> 31) & U64(0x1));
    state.slr &= ~((value >> 32) & U64(0x1));
    break;
  case 0xb8: // CC: 01 OFFSET and ENABLE, 10 COUNT, 11 all [IDS p. 483]
  case 0xb9:
  case 0xba:
  case 0xbb:
    if (index & 1) {
      sync_cc_wallclock(); // the old enable bills the time up to now
      state.cc_offset = (u32)(value >> 32);
      state.cc_ena = value & 1;
    }
    if (index & 2) {
      state.cc = (u32)(value & U64(0xfffffff0));
      cc_last_read = state.cc; // rewritten: rebase the RPCC floor
      cc_borrow = 0;
      cc_wall_remainder = 0;
    }
    break;
  case 0xc0: // ITB_IM
    ev8_tb_im(TB_INDEX_ITB, value & 0x1f);
    break;
  case 0xc2: // ITB_IS, which shares its storage with ITB_TAG [IDS p. 495]
    state.last_tb_virt = value & U64(0x000fffffffffe000);
    ev8_tb_is(TB_INDEX_ITB, value);
    break;
  case 0xc4: // ITB_PTE
    ev8_add_tb(TB_INDEX_ITB, state.last_tb_virt, value);
    break;
  case 0xc6: // ITB_TAG
    state.last_tb_virt = value & U64(0x000fffffffffe000);
    break;
  case 0xc8: // CPU_CNFG<24:0>; CHIP_ID read-only, <55:25> MBZ
    if (m_ev8_chip)
      m_ev8_chip->cpu_cnfg.store(value & U64(0x1ffffff));
    break;
  case 0xcc: // IC_FLUSH: this group's blocks; the I-cache has no group tag,
  case 0xcd: // so all of it [inference]. IC_FLUSH_ASM "performs an IC_FLUSH"
    JIT_FLUSH_CAUSE(EPOCH_ICFLUSH);
    flush_icache();
    break;
  case 0xd8: // TPU_CNFG: MCHK_EN <7>, PREF_RANGE <6:4>; TPU_ID read-only
    m_ev8.tpu_cnfg = value & U64(0xf0);
    break;
  case 0x38: // QUIESCE_TIMEOUT: TIMEOUT <19:4>, WATCH_EN <1>, TIMEOUT_EN <0>
    m_ev8.quiesce_timeout = value & U64(0xffff3);
    break;
  case 0x40: // DTB_IM
    ev8_tb_im(TB_INDEX_DATA, value & 0x1f);
    break;
  case 0x42: // DTB_IS
    ev8_tb_is(TB_INDEX_DATA, value);
    break;
  case 0x44: // DTB_PTE0, DTB_PTE1: one entry (N9)
  case 0x45:
    ev8_add_tb(TB_INDEX_DATA, last_dtb_virt[0], value);
    break;
  case 0x46: // DTB_TAG0, DTB_TAG1
  case 0x47:
    last_dtb_virt[0] = value & U64(0x000fffffffffe000);
    break;
  case 0x48: // DC_CTL<55:48>, a chip's
    if (m_ev8_chip)
      m_ev8_chip->dc_ctl.store(value & U64(0x00ff000000000000));
    break;
  case 0x49: // DC_STAT: write 1 to clear; nothing is ever set
    state.dc_stat &= ~value;
    break;
  case 0xc9: // IC_STAT: the same
  case 0xa0: // EXC_SUM, EXC_ADDR, IVA_FORM, DTBMS_RET_ADDR, CPU_BASE, ISUM,
  case 0xa1: // M_STAT, VA, VA_FORM: read-only
  case 0xa3:
  case 0xa6:
  case 0xa7:
  case 0xab:
  case 0xb2:
  case 0x20:
  case 0x21:
  case 0x23:
    break;
  default:
    if ((index >= 0xe0 && index <= 0xe3) || (index >= 0xe8 && index <= 0xed) ||
        (index >= 0xf0 && index <= 0xf7) || (index >= 0x60 && index <= 0x63) ||
        index == 0x68 || (index >= 0x6a && index <= 0x6c) || index == 0x70 ||
        index == 0x71)
      break; // a performance register: nothing is counted
    {
      const u32 function = index;
      UNKNOWN_IPR("T");
    }
    break;
  }
}

#endif // INCLUDED_CPU_PAL_EV8_H
