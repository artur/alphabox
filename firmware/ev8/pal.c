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

/*
 * The Alphabox PALcode for the native 21464 (the `ev8n` row): the Alpha
 * Linux interface (the Tru64 UNIX one with fewer calls) on the 21464's own
 * privileged registers, as docs/platforms/ev8-native.md section 3 has it.
 * Written by the project; nothing is copied from another PALcode.
 *
 * S12a phase 1: the reset and parking of every TPU, the start of a parked
 * TPU through the HWRPB's RXRDY, the DTB and ITB fills from the virtual
 * page table with the double miss walking the three-level table, entMM and
 * entInt (interprocessor interrupts), and the CALL_PALs halt, wripir,
 * wrval, rdval, wrent, swpipl, rdps, wrkgp, whami and rti. Every other
 * entry and CALL_PAL halts its TPU with reason 0x10000 | its offset
 * (layout.h): phase 2 fills them in.
 *
 * Assembled by nada (main 9152b521) for alpha-none with -mcpu=ev8 -mpal,
 * as a flat image linked 16 bytes below PAL_BASE so that every label is
 * its physical address and every .align counts from PAL_BASE; build.py
 * drops the 16 bytes (nada's branch to _start) and checks the layout.
 *
 * Sources: IDS is the 21464 Internal Design Specification Rev 1.1k, ARM
 * the Alpha Architecture Reference Manual 4th edition, by PDF page.
 *
 * Conventions:
 *   - S0 and S1 are the PALcode's (IDS p. 580): `$s0`, `$s1` in nada.
 *     A CALL_PAL leaves its return PC in S1 (ev8-native.md 1.3).
 *   - PAL_TEMP1 holds this TPU's impure area (IDS p. 500); PAL_TEMP2 holds
 *     an ITB miss's VA across a double miss.
 *   - A flow leaves PALmode with RET (IDS p. 573, not JMP: Restriction
 *     14), written as a word: nada assembles only the bare `ret`.
 *   - Mbox IPRs are written twice in a row (Restriction 3); an IFETCHB
 *     separates writes to one Ibox group (Restriction 2) and ends every
 *     flow that wrote an IPR (Restriction 1).
 *   - The PALcode's own virtual accesses (the kernel stack, the HWRPB's
 *     RXRDY through KSEG) must not miss: a DTB miss in PALmode halts the
 *     TPU, kernel stack not valid [decision, S12a phase 1].
 */
#include "layout.h"

#define S_(x) #x
#define S(x) S_(x)

/* RET $31, (S1) and RET $31, (S0): opcode 1A, function 2 (alpha_asm's
   ret(23) and ret(22); build.py checks them) */
#define RET_S1 "    .long 0x6bf78000\n"
#define RET_S0 "    .long 0x6bf68000\n"

/* Each entry and CALL_PAL slot starts with a no-op naming its own offset,
   which build.py finds at that offset: the layout's proof. */
#define ENTRY(off) "    .align 128\n    lda $31, " S(off) "($31)\n"
#define UNIMPL_ENTRY(off)                                                      \
  ENTRY(off) "    lda $s1, " S(off) "($31)\n    br $31, unimpl_entry\n"
#define SLOT(off) "    .align 64\n    lda $31, " S(off) "($31)\n"
#define UNIMPL(off)                                                            \
  SLOT(off) "    hw_mfpr $s0, pal_temp1\n"                                     \
            "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"                         \
            "    lda $s1, " S(off) "($31)\n"                                   \
            "    br $31, unimpl_pal\n"

/* ------------------------------------------------------------------------
 * The entries, PAL_BASE + 0x000 to 0xF80 (IDS pp. 498-499, 633-634; N11)
 * ------------------------------------------------------------------------ */
__attribute__((naked)) void pal_entries(void)
{
    asm(
        /* 0x000: the entry from SWPPAL or the SROM into the reset code;
           Alphabox's reset enters at 0xB00 */
        ".global pal_base\n"
        "pal_base:\n"
        "    lda $31, 0x0($31)\n"
        "    br $31, reset\n"
        UNIMPL_ENTRY(0x080)

        /* DTBM_DOUBLE and DTBM_DOUBLE_ALT: a HW_LD/PTE of a miss flow
           missed (N12: the same walk for both) */
        ENTRY(0x100)
        "    br $31, dtbm_double\n"
        ENTRY(0x180)
        "    br $31, dtbm_double\n"

        UNIMPL_ENTRY(0x200)     /* FEN */
        UNIMPL_ENTRY(0x280)     /* UNALIGN */

        /* DTBM_SINGLE: DTBMS_RET_ADDR, VA, VA_FORM, M_STAT written (IDS
           p. 633). The PTE through the virtual page table, then the IDS's
           DTB writer block (p. 576), aligned to a fetch block with its
           ALIGN_NOP in slot 7 (Restrictions 4 to 8), and RET through
           DTBMS_RET_ADDR. A PTE with VALID clear is never written
           (Restriction 9): entMM, translation not valid. */
        ENTRY(0x300)
        "dtbm_flow:\n"
        "    hw_mfpr $s1, dtbms_ret_addr\n"
        "    blbs $s1, dtbm_pal_near\n"
        "    hw_mfpr $s0, va_form\n"
        "    hw_ldq/pte $s1, 0($s0)\n"
        "    blbc $s1, dtbm_tnv_near\n"
        "    hw_mfpr $s0, va\n"
        "    br $31, dtbm_wb\n"
        "    .align 32\n"
        "dtbm_wb:\n"
        "    hw_mtpr $s0, dtb_tag0, 9, 8\n"
        "    hw_mtpr $s0, dtb_tag1, 9, 9\n"
        "    hw_mtpr $s1, dtb_pte0, 8, 0\n"
        "    hw_mtpr $s1, dtb_pte1, 9, 25\n"
        "    hw_mfpr $s1, dtbms_ret_addr_blk\n"
        "    nop\n"
        "    nop\n"
        "    map_align\n"
        RET_S1
        "dtbm_tnv_near:\n"      /* a conditional branch reaches its own text */
        "    br $31, dtbm_tnv\n"
        "dtbm_pal_near:\n"
        "    br $31, dtbm_pal\n"

        UNIMPL_ENTRY(0x380)     /* DFAULT */
        UNIMPL_ENTRY(0x400)     /* OPCDEC */
        UNIMPL_ENTRY(0x480)     /* IACV */
        UNIMPL_ENTRY(0x500)     /* MCHK: never raised */

        /* ITB_MISS: EXC_ADDR and IVA_FORM written (IDS p. 633). The PTE
           through the virtual page table; ITB_TAG, ITB_PTE (Restriction
           10), and back to the instruction. The ITB has no VALID or FOE
           bit (pp. 495-496): both are checked here. The VA is kept in
           PAL_TEMP2, since a double miss rewrites EXC_ADDR. This flow's
           HW_LD/PTE stays in this slot: dtbm_double tells an I-stream
           miss by it. */
        ENTRY(0x580)
        "itb_flow:\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    hw_mtpr $s1, pal_temp2\n"
        "    hw_mfpr $s0, iva_form\n"
        "    hw_ldq/pte $s1, 0($s0)\n"
        "    blbc $s1, itb_tnv\n"
        "    and $s1, 8, $s0\n"                        /* FOE <3> */
        "    bne $s0, itb_foe\n"
        "    hw_mfpr $s0, pal_temp2\n"
        "    hw_mtpr $s0, itb_tag\n"
        "    hw_mtpr $s1, itb_pte\n"
        "    ifetchb\n"
        RET_S0
        "itb_tnv:\n"
        "    lda $s1, 0($31)\n"                        /* TNV */
        "    br $31, itb_mm\n"
        "itb_foe:\n"
        "    lda $s1, 3($31)\n"                        /* fault on execute */
        "itb_mm:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_A1) "($s0)\n"
        "    br $31, itb_post\n"

        UNIMPL_ENTRY(0x600)     /* ARITH */

        /* INTERRUPT */
        ENTRY(0x680)
        "    br $31, interrupt\n"

        /* MT_FPCR: the IDS's own flow (p. 581) */
        ENTRY(0x700)
        "    hw_mfpr $s1, exc_addr\n"
        "    ifetchb\n"
        RET_S1

        UNIMPL_ENTRY(0x780)     /* IMCHK: never raised */

        ENTRY(0x800)            /* DTBM_SINGLE_CONS: the same flow */
        "    br $31, dtbm_flow\n"
        ENTRY(0x880)            /* ITB_MISS_CONS: the same flow */
        "    br $31, itb_flow\n"

        UNIMPL_ENTRY(0x900)     /* BAD_JUMP_IVA */
        UNIMPL_ENTRY(0x980)     /* FAST_RESET: never raised (N1, N11) */
        UNIMPL_ENTRY(0xa00)     /* WAKEUP: never raised */
        UNIMPL_ENTRY(0xa80)     /* TPU_RESTART: never raised */

        /* MILD_RESET: power-up, every TPU of every chip (N11, N17) */
        ENTRY(0xb00)
        "    br $31, reset\n"

        UNIMPL_ENTRY(0xb80)     /* DST_NXM: never raised */
        UNIMPL_ENTRY(0xc00)
        UNIMPL_ENTRY(0xc80)
        UNIMPL_ENTRY(0xd00)
        UNIMPL_ENTRY(0xd80)
        UNIMPL_ENTRY(0xe00)
        UNIMPL_ENTRY(0xe80)
        UNIMPL_ENTRY(0xf00)
        UNIMPL_ENTRY(0xf80)
        "    .align 128\n");
}

/* ------------------------------------------------------------------------
 * The entries' bodies, from 0x1000
 * ------------------------------------------------------------------------ */
__attribute__((naked)) void pal_bodies(void)
{
    asm(
        /* ---- MILD_RESET (3.2): every TPU sets up its impure area and its
           IPRs; processor 0 goes on to the console, the others park (N17).
           r3 = WHAMI, r2 = TPU_ID, r1 = the chip's PID throughout. */
        ".global reset\n"
        ".global _start\n"
        "reset:\n"
        "_start:\n"
        "    hw_mfpr $1, cpu_base\n"                   /* the PID <9:0> (N6) */
        "    sll $1, 54, $1\n"
        "    srl $1, 54, $1\n"
        "    hw_mfpr $2, tpu_cnfg\n"
        "    srl $2, 56, $2\n"
        "    and $2, 3, $2\n"                          /* TPU_ID <57:56> */
        "    s4addq $1, $2, $3\n"                      /* WHAMI = PID x 4 + TPU (N25) */
        "    sll $3, " S(IMPURE_SHIFT) ", $4\n"
        "    li $5, " S(PAL_IMPURE) "\n"
        "    addq $4, $5, $s0\n"
        "    hw_mtpr $s0, pal_temp1\n"                 /* the impure area (IDS p. 500) */
        "    ifetchb\n"
        "    lda $4, 0($31)\n"
        "    lda $6, " S(IMPURE_SIZE) "($31)\n"
        "reset_clear:\n"
        "    addq $4, $s0, $5\n"
        "    hw_stq/p $31, 0($5)\n"
        "    lda $4, 8($4)\n"
        "    cmpult $4, $6, $7\n"
        "    bne $7, reset_clear\n"
        "    hw_stq/p $3, " S(IMP_WHAMI) "($s0)\n"
        "    lda $4, 7($31)\n"
        "    hw_stq/p $4, " S(IMP_PS) "($s0)\n"         /* kernel, IPL 7 */
        "    lda $4, " S(IMP_CPCB) "($s0)\n"
        "    hw_stq/p $4, " S(IMP_PCBB) "($s0)\n"       /* the console's PCB */
        /* IER_CM for each IPL: the IPL table (3.1) */
        "    li $4, " S(IER_IPL0) "\n"
        "    lda $5, " S(IMP_IERTAB) "($s0)\n"
        "    hw_stq/p $4, 0($5)\n"
        "    hw_stq/p $4, 8($5)\n"
        "    hw_stq/p $4, 16($5)\n"
        "    hw_stq/p $4, 24($5)\n"
        "    li $4, " S(IER_IPL4) "\n"
        "    hw_stq/p $4, 32($5)\n"
        "    li $4, " S(IER_IPL5) "\n"
        "    hw_stq/p $4, 40($5)\n"
        "    li $4, " S(IER_IPL6) "\n"
        "    hw_stq/p $4, 48($5)\n"
        "    hw_stq/p $31, 56($5)\n"                    /* IPL 7: nothing */
        /* The chip's register window, ipe8(PID) | 0x7FFC << 20 (N18, N20):
           (~PID << 15 | 0x7FFC) << 20, PA<47:0> */
        "    ornot $31, $1, $4\n"
        "    sll $4, 15, $4\n"
        "    lda $4, " S(CSR_WINDOW) "($4)\n"
        "    sll $4, 20, $4\n"
        "    zapnot $4, 0x3f, $4\n"
        "    hw_stq/p $4, " S(IMP_CSR) "($s0)\n"
        "    lda $5, 1($31)\n"
        "    sll $5, $2, $5\n"
        "    hw_stq/p $5, " S(IMP_TPUBIT) "($s0)\n"
        /* R_INT_MASK[t] (N22): this TPU's interprocessor bit, the one
           source phase 1 takes */
        "    li $6, " S(R_INT_MASK_TPU) "\n"
        "    addq $4, $6, $6\n"
        "    sll $2, 6, $7\n"
        "    addq $6, $7, $6\n"
        "    hw_stq/p $5, 0($6)\n"
        /* I_CTL, M_CTL: 43-bit VA, 8 KB pages, SPE52 and SPE43 (3.2) */
        "    li $4, " S(CTL_RESET) "\n"
        "    hw_mtpr $4, i_ctl\n"
        "    ifetchb\n"
        "    hw_mtpr $4, m_ctl\n"
        "    hw_mtpr $4, m_ctl\n"
        "    ifetchb\n"
        /* kernel mode, the CONSOLE bits clear, no interrupt (IPL 7) */
        "    hw_mtpr $31, i_mode\n"
        "    ifetchb\n"
        "    hw_mtpr $31, ier_cm\n"
        "    ifetchb\n"
        "    hw_mtpr $31, m_mode\n"
        "    hw_mtpr $31, m_mode\n"
        "    ifetchb\n"
        /* TPU_GRP = TPU_ID: every TPU in a group of its own (N25), ASN 0,
           FP enabled for the console */
        "    sll $2, 16, $4\n"
        "    lda $5, 1($31)\n"
        "    sll $5, 19, $5\n"
        "    bis $4, $5, $5\n"
        "    hw_mtpr $5, i_pctx\n"
        "    ifetchb\n"
        "    hw_mtpr $4, m_pctx\n"
        "    hw_mtpr $4, m_pctx\n"
        "    ifetchb\n"
        /* a reused group is flushed (IDS p. 311): this group's ITB and
           DTB entries, the I-cache */
        "    lda $4, 1($31)\n"
        "    hw_mtpr $4, itb_im\n"
        "    ifetchb\n"
        "    hw_mtpr $4, dtb_im\n"
        "    hw_mtpr $4, dtb_im\n"
        "    ifetchb\n"
        "    hw_mtpr $31, ic_flush\n"
        "    ifetchb\n"
        /* the cycle counter: count 0, offset 0, enabled (IDS p. 483) */
        "    lda $4, 1($31)\n"
        "    hw_mtpr $4, cc\n"
        "    ifetchb\n"
        "    bne $3, park\n"
        /* processor 0: the console, in kernel mode at IPL 7, a0 = 0 (power
           up), a1 = its WHAMI [decision] */
        "    bis $31, $31, $16\n"
        "    bis $31, $31, $17\n"
        "    li $s1, " S(KSEG_CONSOLE) "\n"
        RET_S1

        /* ---- Park (N17, 3.4): wait for a command in the HWRPB's RXRDY,
           with LDQ_ARM on it and QUIESCE, no timeout. The bitmask's line
           is watched, so any store there wakes the TPU, which looks again.
           S0 = the impure area; kernel mode (the KSEG load). */
        ".global park\n"
        "park:\n"
        "    li $4, " S(QT_PARK) "\n"
        "    hw_mtpr $4, quiesce_timeout\n"
        "    hw_mtpr $4, quiesce_timeout\n"
        "    ifetchb\n"
        "    hw_ldq/p $3, " S(IMP_WHAMI) "($s0)\n"
        "    li $4, " S(KSEG_RXRDY) "\n"
        "park_loop:\n"
        "    ldq_arm $1, ($4)\n"
        "    srl $1, $3, $1\n"
        "    blbs $1, start\n"
        "    quiesce\n"
        "    br $31, park_loop\n"

        /* ---- A command for a parked TPU [ARM pp. 693-694, 725-726]: read
           it, clear the RXRDY bit, and if it is START with a valid context,
           start: BIP set, RC clear, the halt fields, the HWPCB's context,
           the VPTB, R27, and the HWRPB's restart routine. Anything else is
           ignored [decision]. r3 = WHAMI, r4 = RXRDY (KSEG), r5 = the
           slot. */
        "start:\n"
        "    mb\n"
        "    li $5, " S(HWRPB_PA) "\n"
        "    hw_ldq/p $6, " S(HWRPB_PROC_SIZE) "($5)\n"
        "    mulq $3, $6, $6\n"
        "    hw_ldq/p $7, " S(HWRPB_PROC_OFF) "($5)\n"
        "    addq $5, $7, $5\n"
        "    addq $5, $6, $5\n"
        "    hw_ldq/p $6, " S(SLOT_RXLEN) "($5)\n"
        "    hw_ldq/p $7, " S(SLOT_RXBUF) "($5)\n"
        "    lda $8, 1($31)\n"
        "    sll $8, $3, $8\n"
        "start_clear:\n"                               /* LDQ_L/STQ_C [ARM p. 694] */
        "    ldq_l $9, 0($4)\n"
        "    bic $9, $8, $9\n"
        "    stq_c $9, 0($4)\n"
        "    beq $9, start_clear\n"
        "    mb\n"
        "    zapnot $6, 0x0f, $6\n"
        "    cmpeq $6, 7, $6\n"
        "    beq $6, park\n"
        "    li $8, " S(START_CMD) "\n"
        "    zapnot $7, 0x7f, $7\n"
        "    cmpeq $7, $8, $7\n"
        "    beq $7, park\n"
        "    hw_ldq/p $6, " S(SLOT_FLAGS) "($5)\n"
        "    srl $6, " S(FLAG_CV_BIT) ", $7\n"
        "    blbc $7, park\n"                          /* no valid context: stay */
        "    bis $6, " S(FLAG_BIP) ", $6\n"
        "    bic $6, " S(FLAG_RC) ", $6\n"
        "    hw_stq/p $6, " S(SLOT_FLAGS) "($5)\n"
        "    hw_stq/p $5, " S(SLOT_HALT_PCBB) "($5)\n"
        "    hw_stq/p $31, " S(SLOT_HALT_PC) "($5)\n"
        "    hw_stq/p $31, " S(SLOT_HALT_PS) "($5)\n"
        "    hw_stq/p $31, " S(SLOT_HALT_ARG) "($5)\n"
        "    hw_stq/p $31, " S(SLOT_HALT_RA) "($5)\n"
        "    hw_stq/p $31, " S(SLOT_HALT_PV) "($5)\n"
        /* the HWPCB: KSP, USP, PTBR, the process cycle count and the ASN,
           UNIQUE, FEN [ARM p. 560; Table 27-9, p. 720] */
        "    hw_stq/p $5, " S(IMP_PCBB) "($s0)\n"
        "    hw_ldq/p $30, " S(PCB_KSP) "($5)\n"
        "    hw_ldq/p $6, " S(PCB_USP) "($5)\n"
        "    hw_stq/p $6, " S(IMP_USP) "($s0)\n"
        "    hw_ldq/p $6, " S(PCB_PTBR) "($5)\n"
        "    hw_stq/p $6, " S(IMP_PTBR) "($s0)\n"
        "    hw_ldq/p $7, " S(PCB_UNIQUE) "($5)\n"
        "    hw_stq/p $7, " S(IMP_UNIQUE) "($s0)\n"
        "    hw_ldq/p $6, " S(PCB_PCC_ASN) "($5)\n"
        "    srl $6, 32, $7\n"
        "    sll $7, 8, $7\n"
        "    hw_mtpr $7, i_pctx_asn\n"
        "    ifetchb\n"
        "    hw_mtpr $7, m_pctx_asn\n"
        "    hw_mtpr $7, m_pctx_asn\n"
        "    ifetchb\n"
        "    hw_ldq/p $8, " S(PCB_FLAGS) "($5)\n"
        "    and $8, 1, $8\n"
        "    sll $8, 19, $8\n"
        "    hw_mtpr $8, i_pctx_fpe\n"
        "    ifetchb\n"
        "    rpcc $7\n"                                /* PCC_OFF = PCB's count - PCC_CNT */
        "    subq $6, $7, $7\n"
        "    sll $7, 32, $7\n"
        "    bis $7, 1, $7\n"                          /* the counter enabled */
        "    hw_mtpr $7, cc_offset\n"
        "    ifetchb\n"
        /* the TB invalidated, the I-cache too [Table 27-9] */
        "    lda $7, 1($31)\n"
        "    hw_mtpr $7, itb_im\n"
        "    ifetchb\n"
        "    hw_mtpr $7, dtb_im\n"
        "    hw_mtpr $7, dtb_im\n"
        "    ifetchb\n"
        "    hw_mtpr $31, ic_flush\n"
        "    ifetchb\n"
        /* the VPTB from HWRPB[120] (step 11) */
        "    li $7, " S(HWRPB_PA) "\n"
        "    hw_ldq/p $8, " S(HWRPB_VPTB) "($7)\n"
        "    hw_stq/p $8, " S(IMP_VPTB) "($s0)\n"
        "    hw_mtpr $8, i_ctl_vpte\n"
        "    ifetchb\n"
        "    hw_mtpr $8, m_ctl_vpte\n"
        "    hw_mtpr $8, m_ctl_vpte\n"
        "    ifetchb\n"
        /* MCES 8, kernel mode at IPL 7 [Table 27-9] */
        "    lda $8, 8($31)\n"
        "    hw_stq/p $8, " S(IMP_MCES) "($s0)\n"
        "    lda $8, 7($31)\n"
        "    hw_stq/p $8, " S(IMP_PS) "($s0)\n"
        "    hw_mtpr $31, ier_cm\n"
        "    ifetchb\n"
        /* R27 the restart data, R26 and R25 zero, then the restart routine
           (steps 9, 10, 12). R25 is written by a legacy CMOV, the one way
           to the architectural register in PALmode (IDS p. 580). */
        "    hw_ldq/p $27, " S(HWRPB_RESTART_DATA) "($7)\n"
        "    hw_ldq/p $s1, " S(HWRPB_RESTART) "($7)\n"
        "    bic $s1, 3, $s1\n"
        "    bis $31, $31, $26\n"
        "    cmoveq $31, $31, $25\n"
        RET_S1

        /* ---- DTBM_SINGLE's other paths: a PTE with VALID clear, entMM with
           the VA, translation not valid [ARM p. 593] and the access's
           cause, M_STAT<WR> (1 a store, 0 a load), at the instruction; a
           miss in PALmode, kernel stack not valid. */
        ".global dtbm_tnv\n"
        "dtbm_tnv:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_mfpr $s1, va\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A1) "($s0)\n"
        "    hw_mfpr $s1, m_stat\n"
        "    and $s1, 1, $s1\n"
        "    hw_stq/p $s1, " S(IMP_P_A2) "($s0)\n"
        "    hw_mfpr $s1, dtbms_ret_addr\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    ifetchb\n"                                /* Restriction 6 */
        "    lda $s1, " S(SEL_MM) "($31)\n"
        "    br $31, post\n"
        ".global dtbm_pal\n"
        "dtbm_pal:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"
        "    lda $s1, " S(HALT_KSNV) "($31)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    ifetchb\n"
        "    br $31, halt_path\n"

        /* ---- DTBM_DOUBLE: the HW_LD/PTE of a miss flow missed. S0 = the
           PTE's VA (the load's base, kept for its retry), EXC_ADDR = the
           load. The level 1 and level 2 PTEs from PTBR, physically: the
           level 2 PTE maps the page of level 3 PTEs, and goes into the DTB
           as it is (ARM p. 582: valid level 1 and 2 PTEs allow kernel
           read). 43-bit VA, 8 KB pages: the PTE's VA <32:23> is the level 1
           index, <22:13> the level 2 one. S1 and r1, r2 are scratch. */
        ".global dtbm_double\n"
        "dtbm_double:\n"
        "    hw_mfpr $s1, pal_temp1\n"
        "    hw_stq/p $1, " S(IMP_DM_R1) "($s1)\n"
        "    hw_stq/p $2, " S(IMP_DM_R2) "($s1)\n"
        "    hw_ldq/p $1, " S(IMP_PTBR) "($s1)\n"
        "    sll $1, 13, $1\n"
        "    sll $s0, 31, $2\n"
        "    srl $2, 54, $2\n"
        "    s8addq $2, $1, $2\n"
        "    hw_ldq/p $2, 0($2)\n"                     /* level 1 */
        "    blbc $2, dm_bad\n"
        "    srl $2, 32, $1\n"
        "    sll $1, 13, $1\n"
        "    sll $s0, 41, $2\n"
        "    srl $2, 54, $2\n"
        "    s8addq $2, $1, $2\n"
        "    hw_ldq/p $2, 0($2)\n"                     /* level 2 */
        "    blbc $2, dm_bad\n"
        "    srl $2, 8, $1\n"                          /* KRE: kernel reads it */
        "    blbc $1, dm_bad\n"
        "    hw_mtpr $s0, dtb_tag0\n"
        "    hw_mtpr $s0, dtb_tag1\n"
        "    hw_mtpr $2, dtb_pte0\n"
        "    hw_mtpr $2, dtb_pte1\n"
        "    hw_ldq/p $1, " S(IMP_DM_R1) "($s1)\n"
        "    hw_ldq/p $2, " S(IMP_DM_R2) "($s1)\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    ifetchb\n"
        RET_S1
        /* An invalid level 1 or 2 PTE, or a valid one without KRE: entMM
           for the access that missed, ACV when KRE is clear, else TNV [ARM
           p. 594]. The I-stream when the load is ITB_MISS's (in its slot,
           0x580, or 0x880 if it had one): VA in PAL_TEMP2, cause -1, the
           frame's PC that VA; else VA, M_STAT<WR>, DTBMS_RET_ADDR. */
        "dm_bad:\n"
        "    srl $2, 8, $2\n"
        "    and $2, 1, $2\n"
        "    xor $2, 1, $2\n"
        "    hw_stq/p $2, " S(IMP_P_A1) "($s1)\n"
        "    hw_mfpr $1, exc_addr\n"
        "    srl $1, 7, $1\n"
        "    and $1, 0xff, $1\n"
        "    cmpeq $1, 0x0b, $2\n"
        "    cmpeq $1, 0x11, $1\n"
        "    bis $1, $2, $1\n"
        "    bis $31, $s1, $s0\n"
        "    hw_ldq/p $2, " S(IMP_DM_R2) "($s0)\n"
        "    bne $1, dm_istream\n"
        "    hw_ldq/p $1, " S(IMP_DM_R1) "($s0)\n"
        "    hw_mfpr $s1, va\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    hw_mfpr $s1, m_stat\n"
        "    and $s1, 1, $s1\n"
        "    hw_stq/p $s1, " S(IMP_P_A2) "($s0)\n"
        "    hw_mfpr $s1, dtbms_ret_addr\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    ifetchb\n"                                /* Restriction 6 */
        "    lda $s1, " S(SEL_MM) "($31)\n"
        "    br $31, post\n"
        "dm_istream:\n"
        "    hw_ldq/p $1, " S(IMP_DM_R1) "($s0)\n"
        /* ITB_MISS's paths join here: S0 = impure, P_A1 = MMCSR */
        ".global itb_post\n"
        "itb_post:\n"
        "    hw_mfpr $s1, pal_temp2\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    lda $s1, -1($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A2) "($s0)\n"
        "    lda $s1, " S(SEL_MM) "($31)\n"
        "    br $31, post\n"

        /* ---- INTERRUPT (3.2): phase 1 takes the interprocessor interrupt,
           EI3 with this TPU's bit in R_INT_STAT, cleared there (N22)
           before entInt runs, type 0 at IPL 5 [ARM p. 605]. Any other is
           not yet taken: the TPU halts. EXC_ADDR is the interrupted PC. */
        ".global interrupt\n"
        "interrupt:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $1, " S(IMP_INT_R1) "($s0)\n"
        "    hw_mfpr $s1, isum\n"
        "    srl $s1, 36, $s1\n"                       /* EI3 <36> */
        "    blbc $s1, int_other\n"
        "    hw_ldq/p $s1, " S(IMP_CSR) "($s0)\n"
        "    hw_ldq/p $1, " S(R_INT_STAT) "($s1)\n"
        "    hw_ldq/p $s1, " S(IMP_TPUBIT) "($s0)\n"
        "    and $1, $s1, $1\n"
        "    beq $1, int_other\n"
        "    hw_ldq/p $s1, " S(IMP_CSR) "($s0)\n"
        "    hw_stq/p $1, " S(R_INT_STAT) "($s1)\n"    /* write 1 to clear */
        "    mb\n"
        "    hw_ldq/p $1, " S(IMP_INT_R1) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A0) "($s0)\n"     /* type 0: interprocessor */
        "    hw_stq/p $31, " S(IMP_P_A1) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A2) "($s0)\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    lda $s1, " S(SEL_IPI) "($31)\n"
        "    br $31, post\n"
        "int_other:\n"
        "    hw_ldq/p $1, " S(IMP_INT_R1) "($s0)\n"
        "    lda $s1, 0x680($31)\n"
        "    br $31, unimpl_entry\n"

        /* ---- post: enter the OS at one of the wrent entries [ARM pp.
           601-607]. S0 = the impure area, S1 = the selector (layout.h),
           P_PC the frame's PC, P_A0 to P_A2 the entry's a0 to a2; every
           other register is the OS's. To the kernel stack if in user mode,
           a frame of PS, PC, GP, a0, a1, a2, then PS kernel at the entry's
           IPL (or the same one), GP = KGP, a0 to a2, the entry. */
        ".global post\n"
        "post:\n"
        "    hw_stq/p $s1, " S(IMP_P_SEL) "($s0)\n"
        "    hw_ldq/p $s1, " S(IMP_PS) "($s0)\n"
        "    and $s1, 8, $s1\n"
        "    beq $s1, post_kernel\n"
        "    hw_stq/p $30, " S(IMP_USP) "($s0)\n"
        "    hw_ldq/p $30, " S(IMP_KSP) "($s0)\n"
        "post_kernel:\n"
        "    hw_mtpr $31, i_mode_cur\n"               /* CM kernel: the frame's stores */
        "    ifetchb\n"
        "    lda $30, -48($30)\n"
        "    stq $29, 16($30)\n"
        "    stq $16, 24($30)\n"
        "    stq $17, 32($30)\n"
        "    stq $18, 40($30)\n"
        "    hw_ldq/p $16, " S(IMP_PS) "($s0)\n"
        "    stq $16, 0($30)\n"
        "    hw_ldq/p $17, " S(IMP_P_PC) "($s0)\n"
        "    stq $17, 8($30)\n"
        "    hw_ldq/p $17, " S(IMP_P_SEL) "($s0)\n"
        "    and $16, 7, $16\n"                        /* the old IPL (0 in user mode) */
        "    srl $17, 8, $18\n"
        "    beq $18, post_ipl\n"
        "    subq $18, 1, $16\n"                       /* the entry's IPL */
        "post_ipl:\n"
        "    hw_stq/p $16, " S(IMP_PS) "($s0)\n"
        "    s8addq $16, $s0, $18\n"
        "    hw_ldq/p $18, " S(IMP_IERTAB) "($18)\n"
        "    hw_mtpr $18, ier_cm\n"
        "    and $17, 0xff, $17\n"
        "    addq $17, $s0, $17\n"
        "    hw_ldq/p $s1, 0($17)\n"
        "    bic $s1, 3, $s1\n"                        /* normal mode, whatever was written */
        "    hw_ldq/p $29, " S(IMP_KGP) "($s0)\n"
        "    hw_ldq/p $16, " S(IMP_P_A0) "($s0)\n"
        "    hw_ldq/p $17, " S(IMP_P_A1) "($s0)\n"
        "    hw_ldq/p $18, " S(IMP_P_A2) "($s0)\n"
        "    ifetchb\n"
        RET_S1

        /* ---- An entry this phase does not take: S1 = its offset; halt
           with 0x10000 | the offset, at EXC_ADDR. */
        ".global unimpl_entry\n"
        "unimpl_entry:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    ldah $s1, 1($s1)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"
        "    br $31, halt_path\n"
        /* A CALL_PAL this phase does not take: S1 = its offset, H_PC set */
        ".global unimpl_pal\n"
        "unimpl_pal:\n"
        "    ldah $s1, 1($s1)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    br $31, halt_path\n"

        /* ---- Halt (3.1): S0 = the impure area, H_REASON and H_PC set.
           The halt in this TPU's per-CPU slot, if the console has built an
           HWRPB with a slot for it [ARM pp. 634-635]: PCBB, PC, PS, R25,
           R26, R27, the reason. Then interrupts off at IPL 7 in kernel
           mode; processor 0 goes back to the console (a0 = the reason, a1
           = 0) [decision], any other parks (N17). */
        ".global halt_path\n"
        "halt_path:\n"
        "    bis $31, $25, $8\n"                       /* the architectural R25 (as Rb) */
        "    li $1, " S(HWRPB_PA) "\n"
        "    hw_ldq/p $2, " S(HWRPB_ID) "($1)\n"
        "    li $3, " S(HWRPB_ID_VALUE) "\n"
        "    cmpeq $2, $3, $2\n"
        "    beq $2, halt_noslot\n"
        "    hw_ldq/p $2, " S(HWRPB_NR_PROC) "($1)\n"
        "    hw_ldq/p $3, " S(IMP_WHAMI) "($s0)\n"
        "    cmpult $3, $2, $2\n"
        "    beq $2, halt_noslot\n"
        "    hw_ldq/p $2, " S(HWRPB_PROC_SIZE) "($1)\n"
        "    mulq $3, $2, $3\n"
        "    hw_ldq/p $2, " S(HWRPB_PROC_OFF) "($1)\n"
        "    addq $1, $2, $1\n"
        "    addq $1, $3, $1\n"
        "    hw_ldq/p $2, " S(IMP_PCBB) "($s0)\n"
        "    hw_stq/p $2, " S(SLOT_HALT_PCBB) "($1)\n"
        "    hw_ldq/p $2, " S(IMP_H_PC) "($s0)\n"
        "    hw_stq/p $2, " S(SLOT_HALT_PC) "($1)\n"
        "    hw_ldq/p $2, " S(IMP_PS) "($s0)\n"
        "    hw_stq/p $2, " S(SLOT_HALT_PS) "($1)\n"
        "    hw_stq/p $8, " S(SLOT_HALT_ARG) "($1)\n"
        "    hw_stq/p $26, " S(SLOT_HALT_RA) "($1)\n"
        "    hw_stq/p $27, " S(SLOT_HALT_PV) "($1)\n"
        "    hw_ldq/p $2, " S(IMP_H_REASON) "($s0)\n"
        "    hw_stq/p $2, " S(SLOT_HALT_REASON) "($1)\n"
        "    mb\n"
        "halt_noslot:\n"
        "    lda $2, 7($31)\n"
        "    hw_stq/p $2, " S(IMP_PS) "($s0)\n"
        "    hw_mtpr $31, ier_cm\n"                   /* kernel mode, nothing enabled */
        "    ifetchb\n"
        "    hw_ldq/p $2, " S(IMP_WHAMI) "($s0)\n"
        "    bne $2, park\n"
        "    hw_ldq/p $16, " S(IMP_H_REASON) "($s0)\n"
        "    bis $31, $31, $17\n"
        "    li $s1, " S(KSEG_CONSOLE) "\n"
        RET_S1
        );
}

/* ------------------------------------------------------------------------
 * The CALL_PAL slots: privileged 0x00-0x3F at 0x2000 + 0x40 x function,
 * unprivileged 0x80-0xBF at 0x3000 + 0x40 x (function - 0x80) (IDS p. 499).
 * Privileged ones from user mode, and functions 0x40-0x7F and above 0xBF,
 * take OPCDEC instead (ev8-native.md 1.5), so no slot checks the mode.
 * On entry S1 holds the return PC with the caller's mode, 00.
 * ------------------------------------------------------------------------ */
__attribute__((naked)) void pal_callpal(void)
{
    asm(
        "    .align 8192\n"
        /* 0x00 halt [ARM p. 266]: reason 5, the PC after the HALT */
        SLOT(0x2000)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"
        "    lda $s1, " S(HALT_CALLPAL) "($31)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    br $31, halt_path\n"
        UNIMPL(0x2040)          /* 0x01 cflush */
        UNIMPL(0x2080)          /* 0x02 draina */
        UNIMPL(0x20c0)
        UNIMPL(0x2100)
        UNIMPL(0x2140)
        UNIMPL(0x2180)
        UNIMPL(0x21c0)
        UNIMPL(0x2200)
        UNIMPL(0x2240)          /* 0x09 cserve */
        UNIMPL(0x2280)          /* 0x0A swppal */
        UNIMPL(0x22c0)
        UNIMPL(0x2300)
        /* 0x0D wripir [ARM p. 570]: processor a0 is PID a0 >> 2, TPU a0 & 3
           (N25); 1 << TPU into that chip's R_INT_REQ, ipe8(PID) | 0x7FFC
           << 20 | 0x70, physical (3.1, N22). t0, t8 and a0 UNPREDICTABLE. */
        SLOT(0x2340)
        "    srl $16, 2, $1\n"
        "    ornot $31, $1, $1\n"
        "    sll $1, 15, $1\n"
        "    lda $1, " S(CSR_WINDOW) "($1)\n"
        "    sll $1, 20, $1\n"
        "    zapnot $1, 0x3f, $1\n"
        "    and $16, 3, $s0\n"
        "    lda $22, 1($31)\n"
        "    sll $22, $s0, $22\n"
        "    mb\n"
        "    hw_stq/p $22, " S(R_INT_REQ) "($1)\n"
        RET_S1
        UNIMPL(0x2380)
        UNIMPL(0x23c0)
        UNIMPL(0x2400)          /* 0x10 rdmces */
        UNIMPL(0x2440)          /* 0x11 wrmces */
        UNIMPL(0x2480)
        UNIMPL(0x24c0)
        UNIMPL(0x2500)
        UNIMPL(0x2540)
        UNIMPL(0x2580)
        UNIMPL(0x25c0)
        UNIMPL(0x2600)
        UNIMPL(0x2640)
        UNIMPL(0x2680)
        UNIMPL(0x26c0)
        UNIMPL(0x2700)
        UNIMPL(0x2740)
        UNIMPL(0x2780)
        UNIMPL(0x27c0)
        UNIMPL(0x2800)
        UNIMPL(0x2840)
        UNIMPL(0x2880)
        UNIMPL(0x28c0)
        UNIMPL(0x2900)
        UNIMPL(0x2940)
        UNIMPL(0x2980)
        UNIMPL(0x29c0)
        UNIMPL(0x2a00)
        UNIMPL(0x2a40)
        UNIMPL(0x2a80)
        UNIMPL(0x2ac0)          /* 0x2B wrfen */
        UNIMPL(0x2b00)
        UNIMPL(0x2b40)          /* 0x2D wrvptptr */
        UNIMPL(0x2b80)
        UNIMPL(0x2bc0)
        UNIMPL(0x2c00)          /* 0x30 swpctx */
        /* 0x31 wrval [ARM p. 576] */
        SLOT(0x2c40)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $16, " S(IMP_SYSVAL) "($s0)\n"
        RET_S1
        /* 0x32 rdval [ARM p. 557] */
        SLOT(0x2c80)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_SYSVAL) "($s0)\n"
        RET_S1
        UNIMPL(0x2cc0)          /* 0x33 tbi */
        /* 0x34 wrent [ARM p. 568]: a1 0 to 5 selects entInt, entArith,
           entMM, entIF, entUna, entSys; any other is ignored */
        SLOT(0x2d00)
        "    cmpult $17, 6, $1\n"
        "    beq $1, wrent_out\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    s8addq $17, $s0, $1\n"
        "    hw_stq/p $16, " S(IMP_ENTINT) "($1)\n"
        "wrent_out:\n"
        RET_S1
        /* 0x35 swpipl [ARM p. 562]: v0 the old IPL, the new one's
           interrupt enables into IER_CM (the IPL table, 3.1) */
        SLOT(0x2d40)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_PS) "($s0)\n"
        "    and $16, 7, $1\n"
        "    hw_stq/p $1, " S(IMP_PS) "($s0)\n"
        "    s8addq $1, $s0, $1\n"
        "    hw_ldq/p $1, " S(IMP_IERTAB) "($1)\n"
        "    hw_mtpr $1, ier_cm\n"
        "    and $0, 7, $0\n"
        "    ifetchb\n"
        RET_S1
        /* 0x36 rdps [ARM p. 555] */
        SLOT(0x2d80)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_PS) "($s0)\n"
        RET_S1
        /* 0x37 wrkgp [ARM p. 571] */
        SLOT(0x2dc0)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $16, " S(IMP_KGP) "($s0)\n"
        RET_S1
        UNIMPL(0x2e00)          /* 0x38 wrusp */
        UNIMPL(0x2e40)          /* 0x39 wrperfmon */
        UNIMPL(0x2e80)          /* 0x3A rdusp */
        UNIMPL(0x2ec0)          /* 0x3B: free (N8's proposal) */
        /* 0x3C whami [ARM p. 566]: PID x 4 + TPU (N25) */
        SLOT(0x2f00)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_WHAMI) "($s0)\n"
        RET_S1
        UNIMPL(0x2f40)          /* 0x3D retsys */
        UNIMPL(0x2f80)          /* 0x3E wtint */
        /* 0x3F rti */
        SLOT(0x2fc0)
        "    br $31, rti_body\n"

        UNIMPL(0x3000)          /* 0x80 bpt */
        UNIMPL(0x3040)          /* 0x81 bugchk */
        UNIMPL(0x3080)
        UNIMPL(0x30c0)          /* 0x83 callsys */
        UNIMPL(0x3100)
        UNIMPL(0x3140)
        UNIMPL(0x3180)          /* 0x86 imb */
        UNIMPL(0x31c0)
        UNIMPL(0x3200)
        UNIMPL(0x3240)
        UNIMPL(0x3280)
        UNIMPL(0x32c0)
        UNIMPL(0x3300)
        UNIMPL(0x3340)
        UNIMPL(0x3380)
        UNIMPL(0x33c0)
        UNIMPL(0x3400)
        UNIMPL(0x3440)
        UNIMPL(0x3480)
        UNIMPL(0x34c0)
        UNIMPL(0x3500)
        UNIMPL(0x3540)
        UNIMPL(0x3580)
        UNIMPL(0x35c0)
        UNIMPL(0x3600)
        UNIMPL(0x3640)
        UNIMPL(0x3680)
        UNIMPL(0x36c0)
        UNIMPL(0x3700)
        UNIMPL(0x3740)
        UNIMPL(0x3780)          /* 0x9E rdunique */
        UNIMPL(0x37c0)          /* 0x9F wrunique */
        UNIMPL(0x3800)
        UNIMPL(0x3840)
        UNIMPL(0x3880)
        UNIMPL(0x38c0)
        UNIMPL(0x3900)
        UNIMPL(0x3940)
        UNIMPL(0x3980)
        UNIMPL(0x39c0)
        UNIMPL(0x3a00)
        UNIMPL(0x3a40)
        UNIMPL(0x3a80)          /* 0xAA gentrap */
        UNIMPL(0x3ac0)
        UNIMPL(0x3b00)
        UNIMPL(0x3b40)
        UNIMPL(0x3b80)          /* 0xAE clrfen */
        UNIMPL(0x3bc0)
        UNIMPL(0x3c00)
        UNIMPL(0x3c40)
        UNIMPL(0x3c80)
        UNIMPL(0x3cc0)
        UNIMPL(0x3d00)
        UNIMPL(0x3d40)
        UNIMPL(0x3d80)
        UNIMPL(0x3dc0)
        UNIMPL(0x3e00)
        UNIMPL(0x3e40)
        UNIMPL(0x3e80)
        UNIMPL(0x3ec0)
        UNIMPL(0x3f00)
        UNIMPL(0x3f40)
        UNIMPL(0x3f80)
        UNIMPL(0x3fc0)
        "    .align 64\n");
}

/* ------------------------------------------------------------------------
 * The CALL_PALs' bodies, from 0x4000
 * ------------------------------------------------------------------------ */
__attribute__((naked)) void pal_callpal_bodies(void)
{
    asm(
        /* ---- rti [ARM p. 559]: pop PS, PC, GP, a0, a1, a2; to user mode,
           the kernel stack is kept and the user's restored, PS 8; the lock
           flag cleared (a STQ_C, which always clears it); the PS's mode
           and IPL into IER_CM last, after the frame's loads in kernel
           mode. a0 to a2 serve as scratch until they are loaded. */
        ".global rti_body\n"
        "rti_body:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    li $16, " S(KSEG) "\n"
        "    addq $16, $s0, $16\n"
        "    stq_c $17, " S(IMP_LOCK) "($16)\n"
        "    ldq $16, 0($30)\n"                        /* PS */
        "    ldq $s1, 8($30)\n"                        /* PC */
        "    bic $s1, 3, $s1\n"                        /* normal mode, whatever was stored */
        "    lda $17, 48($30)\n"
        "    and $16, 8, $18\n"
        "    beq $18, rti_kernel\n"
        "    hw_stq/p $17, " S(IMP_KSP) "($s0)\n"
        "    hw_ldq/p $17, " S(IMP_USP) "($s0)\n"
        "    lda $16, 8($31)\n"
        "rti_kernel:\n"
        "    and $16, 15, $16\n"
        "    hw_stq/p $16, " S(IMP_PS) "($s0)\n"
        "    and $16, 7, $18\n"
        "    s8addq $18, $s0, $18\n"
        "    hw_ldq/p $18, " S(IMP_IERTAB) "($18)\n"
        "    and $16, 8, $16\n"
        "    beq $16, rti_mode\n"
        "    bis $18, 0x18, $18\n"                     /* CM user <4:3> */
        "rti_mode:\n"
        "    hw_stq/p $18, " S(IMP_P_SEL) "($s0)\n"
        "    hw_stq/p $17, " S(IMP_P_A0) "($s0)\n"
        "    ldq $29, 16($30)\n"
        "    ldq $16, 24($30)\n"
        "    ldq $17, 32($30)\n"
        "    ldq $18, 40($30)\n"
        "    hw_ldq/p $30, " S(IMP_P_A0) "($s0)\n"
        "    hw_ldq/p $s0, " S(IMP_P_SEL) "($s0)\n"
        "    hw_mtpr $s0, ier_cm\n"
        "    ifetchb\n"
        RET_S1
        );
}
