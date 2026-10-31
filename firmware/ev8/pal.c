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
 * S12a: the reset and parking of every TPU, the start of a parked TPU
 * through the HWRPB's RXRDY; the DTB and ITB fills from the virtual page
 * table, the double miss walking the three-level table; every OS entry of
 * the ARM's Alpha Linux chapters (entMM, entIF, entArith, entUna, entSys,
 * entInt with the timer, interprocessor, device and halt sources); every
 * CALL_PAL of section 3.1, the vector enable 0x3B (N8) among them; the
 * halt, its record in the per-CPU slot and its message. An entry never
 * raised, or a fault inside the PALcode, halts its TPU with reason 0x10000
 * | the offset; a CALL_PAL the interface lacks is opDec.
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
 *     A CALL_PAL leaves its return PC in S1 (ev8-native.md 1.3); a flow
 *     that needs S1 keeps the return PC in the impure area (IMP_RET).
 *   - PAL_TEMP1 holds this TPU's impure area (IDS p. 500); PAL_TEMP2 holds
 *     an ITB miss's VA across a double miss.
 *   - A flow leaves PALmode with RET (IDS p. 573, not JMP: Restriction
 *     14), written as a word: nada assembles only the bare `ret`. A PALmode
 *     subroutine is reached with BSR, whose link keeps the mode bits (N2),
 *     and returns with RET through that link.
 *   - Mbox IPRs are written twice in a row (Restriction 3); an IFETCHB
 *     separates writes to one Ibox group (Restriction 2) and ends every
 *     flow that wrote an IPR (Restriction 1).
 *   - The PALcode's own virtual accesses (the kernel stack, the HWRPB's
 *     RXRDY and the GIO lock words through KSEG) must not miss: a DTB miss
 *     in PALmode halts the TPU, kernel stack not valid [decision].
 */
#include "layout.h"

#define S_(x) #x
#define S(x) S_(x)

/* RET $31, (S1), RET $31, (S0) and RET $31, (R6): opcode 1A, function 2
   (alpha_asm's ret(23), ret(22) and ret(6); build.py checks them) */
#define RET_S1 "    .long 0x6bf78000\n"
#define RET_S0 "    .long 0x6bf68000\n"
#define RET_R6 "    .long 0x6be68000\n"

/* Each entry and CALL_PAL slot starts with a no-op naming its own offset,
   which build.py finds at that offset: the layout's proof. */
#define ENTRY(off) "    .align 128\n    lda $31, " S(off) "($31)\n"
#define NEVER(off)                                                             \
  ENTRY(off) "    lda $s1, " S(off) "($31)\n    br $31, unexpected\n"
#define SLOT(off) "    .align 64\n    lda $31, " S(off) "($31)\n"
#define OPDEC(off) SLOT(off) "    br $31, pal_opdec\n"
#define TO(off, label) SLOT(off) "    br $31, " label "\n"

/* Wait for GIO_DATA<DONE> (the port idle, a read's data valid): base the
   window, t a scratch register, l a fresh label. */
#define GIO_WAIT(base, t, l)                                                   \
  l ":\n    hw_ldq/p " t ", " S(GIO_DATA) "(" base ")\n    bge " t ", " l "\n"

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
        NEVER(0x080)            /* available: never entered */

        /* DTBM_DOUBLE and DTBM_DOUBLE_ALT: a HW_LD/PTE of a miss flow
           missed (N12: the same walk for both) */
        ENTRY(0x100)
        "    br $31, dtbm_double\n"
        ENTRY(0x180)
        "    br $31, dtbm_double\n"

        ENTRY(0x200)            /* FEN */
        "    br $31, fen_flow\n"
        ENTRY(0x280)            /* UNALIGN */
        "    br $31, una_flow\n"

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

        ENTRY(0x380)            /* DFAULT */
        "    br $31, dfault_flow\n"
        ENTRY(0x400)            /* OPCDEC */
        "    br $31, opcdec_flow\n"
        ENTRY(0x480)            /* IACV */
        "    br $31, iacv_flow\n"
        NEVER(0x500)            /* MCHK: never raised */

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

        ENTRY(0x600)            /* ARITH */
        "    br $31, arith_flow\n"

        /* INTERRUPT */
        ENTRY(0x680)
        "    br $31, interrupt\n"

        /* MT_FPCR: the IDS's own flow (p. 581) */
        ENTRY(0x700)
        "    hw_mfpr $s1, exc_addr\n"
        "    ifetchb\n"
        RET_S1

        NEVER(0x780)            /* IMCHK: never raised */

        ENTRY(0x800)            /* DTBM_SINGLE_CONS: the same flow */
        "    br $31, dtbm_flow\n"
        ENTRY(0x880)            /* ITB_MISS_CONS: the same flow */
        "    br $31, itb_flow\n"

        ENTRY(0x900)            /* BAD_JUMP_IVA */
        "    br $31, badjump_flow\n"
        NEVER(0x980)            /* FAST_RESET: never raised (N1, N11) */
        NEVER(0xa00)            /* WAKEUP: never raised */
        NEVER(0xa80)            /* TPU_RESTART: never raised */

        /* MILD_RESET: power-up, every TPU of every chip (N11, N17) */
        ENTRY(0xb00)
        "    br $31, reset\n"

        NEVER(0xb80)            /* DST_NXM: never raised */
        NEVER(0xc00)
        NEVER(0xc80)
        NEVER(0xd00)
        NEVER(0xd80)
        NEVER(0xe00)
        NEVER(0xe80)
        NEVER(0xf00)
        NEVER(0xf80)
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
        /* R_INT_MASK[t] (N22): this TPU's interprocessor, timer and halt
           bits, and for TPU 0 the interrupt queue [decision]: Linux takes
           devices on one TPU of a chip. The GIO bits are left off: Linux's
           console terminal is polled (srmcons). */
        "    sll $5, 4, $6\n"
        "    bis $5, $6, $6\n"
        "    sll $5, 8, $7\n"
        "    bis $6, $7, $6\n"
        "    bne $2, reset_mask\n"
        "    lda $7, 0x1000($31)\n"
        "    bis $6, $7, $6\n"
        "reset_mask:\n"
        "    li $7, " S(R_INT_MASK_TPU) "\n"
        "    addq $4, $7, $7\n"
        "    sll $2, 6, $8\n"
        "    addq $7, $8, $7\n"
        "    hw_stq/p $6, 0($7)\n"
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
           start: BIP set, RC clear, the PALcode's fields of the slot, the
           halt fields, the HWPCB's context, the VPTB, R27, and the HWRPB's
           restart routine. Anything else is ignored [decision]. r3 = WHAMI,
           r4 = RXRDY (KSEG), r5 = the slot. */
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
        "    lda $7, " S(FLAG_PALOK) "($31)\n"
        "    bis $6, $7, $6\n"                         /* PL, PMV, PV (3.3) */
        "    hw_stq/p $6, " S(SLOT_FLAGS) "($5)\n"
        "    li $7, " S(PAL_MEM_LEN) "\n"
        "    hw_stq/p $7, " S(SLOT_PAL_MEM_LEN) "($5)\n"
        "    li $7, " S(PAL_SCR_LEN) "\n"
        "    hw_stq/p $7, " S(SLOT_PAL_SCR_LEN) "($5)\n"
        "    li $7, " S(PAL_MEM_PA) "\n"
        "    hw_stq/p $7, " S(SLOT_PAL_MEM_PA) "($5)\n"
        "    li $7, " S(PAL_SCR_PA) "\n"
        "    hw_stq/p $7, " S(SLOT_PAL_SCR_PA) "($5)\n"
        "    li $7, " S(PAL_REVISION) "\n"
        "    hw_stq/p $7, " S(SLOT_PAL_REV) "($5)\n"
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

        /* ---- INTERRUPT (3.2). ISUM's levels against this TPU's bits in its
           chip's R_INT_STAT (N22), the highest IPL first: EI2 the timer
           tick <7:4> (entInt type 1, IPL 5), EI3 the interprocessor bit
           <3:0> (type 0, IPL 5), EI4 the halt request <11:8> (the TPU
           halts, reason 1) or the GIO bits <14:13> (cleared and dismissed:
           no mask enables them), EI1 the queue (type 3, IPL 4, the IID's
           vector as Linux decodes it, the IID kept for its end of
           interrupt). Each bit is cleared in R_INT_STAT before the OS runs.
           A level with no bit of ours (a race) is dismissed; EI0 or EI5,
           never raised, halt the TPU. EXC_ADDR is the interrupted PC;
           r1 = ISUM, r2 = R_INT_STAT. */
        ".global interrupt\n"
        "interrupt:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $1, " S(IMP_INT_R1) "($s0)\n"
        "    hw_stq/p $2, " S(IMP_INT_R2) "($s0)\n"
        "    hw_mfpr $1, isum\n"
        "    hw_ldq/p $s1, " S(IMP_CSR) "($s0)\n"
        "    hw_ldq/p $2, " S(R_INT_STAT) "($s1)\n"
        /* EI2: the timer */
        "    srl $1, 35, $s1\n"
        "    blbc $s1, int_ipi\n"
        "    hw_ldq/p $s1, " S(IMP_TPUBIT) "($s0)\n"
        "    sll $s1, 4, $s1\n"
        "    and $2, $s1, $s1\n"
        "    beq $s1, int_ipi\n"
        "    hw_ldq/p $2, " S(IMP_CSR) "($s0)\n"
        "    hw_stq/p $s1, " S(R_INT_STAT) "($2)\n"    /* write 1 to clear */
        "    lda $s1, 1($31)\n"                        /* type 1: the clock */
        "    br $31, int_post_ipl5\n"
        /* EI3: the interprocessor interrupt */
        "int_ipi:\n"
        "    srl $1, 36, $s1\n"
        "    blbc $s1, int_halt\n"
        "    hw_ldq/p $s1, " S(IMP_TPUBIT) "($s0)\n"
        "    and $2, $s1, $s1\n"
        "    beq $s1, int_halt\n"
        "    hw_ldq/p $2, " S(IMP_CSR) "($s0)\n"
        "    hw_stq/p $s1, " S(R_INT_STAT) "($2)\n"
        "    lda $s1, 0($31)\n"                        /* type 0 */
        "int_post_ipl5:\n"
        "    mb\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A1) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A2) "($s0)\n"
        "    lda $s1, " S(SEL_IPI) "($31)\n"
        "int_post:\n"
        "    hw_ldq/p $1, " S(IMP_INT_R1) "($s0)\n"
        "    hw_ldq/p $2, " S(IMP_INT_R2) "($s0)\n"
        "    hw_stq/p $s1, " S(IMP_P_SEL) "($s0)\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_ldq/p $s1, " S(IMP_P_SEL) "($s0)\n"
        "    br $31, post\n"
        /* EI4: a halt request, or GIO */
        "int_halt:\n"
        "    srl $1, 37, $s1\n"
        "    blbc $s1, int_queue\n"
        "    hw_ldq/p $s1, " S(IMP_TPUBIT) "($s0)\n"
        "    sll $s1, 8, $s1\n"
        "    and $2, $s1, $s1\n"
        "    beq $s1, int_gio\n"
        "    hw_ldq/p $2, " S(IMP_CSR) "($s0)\n"
        "    hw_stq/p $s1, " S(R_INT_STAT) "($2)\n"
        "    mb\n"
        "    hw_ldq/p $1, " S(IMP_INT_R1) "($s0)\n"
        "    hw_ldq/p $2, " S(IMP_INT_R2) "($s0)\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"
        "    lda $s1, " S(HALT_OPERATOR) "($31)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    br $31, halt_path\n"
        "int_gio:\n"
        "    srl $2, 13, $s1\n"
        "    and $s1, 3, $s1\n"
        "    beq $s1, int_queue\n"
        "    sll $s1, 13, $s1\n"
        "    hw_ldq/p $2, " S(IMP_CSR) "($s0)\n"
        "    hw_stq/p $s1, " S(R_INT_STAT) "($2)\n"
        "    mb\n"
        "    br $31, int_dismiss\n"
        /* EI1: the interrupt queue: the IID at its head (<24> valid),
           removed by a write (N21, S11b); the vector PE << 16 | 0x800 +
           (LSI << 4), or 0x1000 + (MSI << 4) for an IID with <13>
           (io7_device_interrupt, sys_marvel.c) */
        "int_queue:\n"
        "    srl $1, 34, $s1\n"
        "    blbc $s1, int_other\n"
        "    hw_ldq/p $s1, " S(IMP_CSR) "($s0)\n"
        "    hw_ldq/p $2, " S(R_INT_QUE) "($s1)\n"
        "    srl $2, 24, $1\n"
        "    blbc $1, int_dismiss\n"
        "    hw_stq/p $31, " S(R_INT_QUE) "($s1)\n"
        "    mb\n"
        "    zapnot $2, 7, $2\n"                       /* the IID <23:0> */
        "    lda $1, 1($31)\n"
        "    sll $1, 32, $1\n"
        "    bis $2, $1, $1\n"
        "    hw_stq/p $1, " S(IMP_EOI) "($s0)\n"       /* in service */
        "    srl $2, 14, $1\n"
        "    sll $1, 16, $1\n"                         /* PE << 16 */
        "    sll $2, 55, $s1\n"
        "    srl $s1, 51, $s1\n"                       /* IID<8:0> << 4 */
        "    bis $1, $s1, $1\n"
        "    srl $2, 13, $s1\n"
        "    blbs $s1, int_msi\n"
        "    lda $1, 0x800($1)\n"
        "    br $31, int_vector\n"
        "int_msi:\n"
        "    lda $1, 0x1000($1)\n"
        "int_vector:\n"
        "    hw_stq/p $1, " S(IMP_P_A1) "($s0)\n"
        "    lda $s1, 3($31)\n"                        /* type 3: a device */
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A2) "($s0)\n"
        "    lda $s1, " S(SEL_DEVICE) "($31)\n"
        "    br $31, int_post\n"
        /* EI0, EI5 or nothing: a level never raised halts the TPU; with no
           level left (a race), back to the interrupted code */
        "int_other:\n"
        "    srl $1, 33, $s1\n"
        "    and $s1, 0x21, $s1\n"
        "    bne $s1, int_never\n"
        "int_dismiss:\n"
        "    hw_ldq/p $1, " S(IMP_INT_R1) "($s0)\n"
        "    hw_ldq/p $2, " S(IMP_INT_R2) "($s0)\n"
        "    hw_mfpr $s1, exc_addr\n"
        RET_S1
        "int_never:\n"
        "    hw_ldq/p $1, " S(IMP_INT_R1) "($s0)\n"
        "    hw_ldq/p $2, " S(IMP_INT_R2) "($s0)\n"
        "    lda $s1, 0x680($31)\n"
        "    br $31, unexpected\n"

        /* ---- post: enter the OS at one of the wrent entries [ARM pp.
           601-607]. S0 = the impure area, S1 = the selector (layout.h),
           P_PC the frame's PC, P_A0 to P_A2 the entry's a0 to a2; every
           other register is the OS's. To the kernel stack if in user mode,
           which must be quadword aligned (else a kernel-stack-not-valid
           halt [ARM p. 607]); a frame of PS, PC, GP, a0, a1, a2, then PS
           kernel at the entry's IPL (or the same one), GP = KGP, a0 to a2,
           the entry. */
        ".global post\n"
        "post:\n"
        "    hw_stq/p $s1, " S(IMP_P_SEL) "($s0)\n"
        "    hw_ldq/p $s1, " S(IMP_PS) "($s0)\n"
        "    and $s1, 8, $s1\n"
        "    beq $s1, post_kernel\n"
        "    hw_stq/p $30, " S(IMP_USP) "($s0)\n"
        "    hw_ldq/p $30, " S(IMP_KSP) "($s0)\n"
        "post_kernel:\n"
        "    and $30, 7, $s1\n"
        "    bne $s1, post_ksnv\n"
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
        "post_ksnv:\n"
        "    hw_ldq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"
        "    lda $s1, " S(HALT_KSNV) "($31)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    br $31, halt_path\n"

        /* ---- An entry never raised, or a fault inside the PALcode: S1 =
           the entry's offset; halt with 0x10000 | the offset at EXC_ADDR. */
        ".global unexpected\n"
        "unexpected:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    ldah $s1, 1($s1)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"
        "    br $31, halt_path\n"

        /* ---- do_eoi: the end of interrupt of the IID in service, if any,
           written to EOI_DAT of the IO7 port its <6:5> names, at ipe8(PE)
           | (~port & 7) << 32 | 0xFF800000 + 0x2400 (N18; Linux's
           io7_ioport_csrs): the EV7 console PALcode's protocol, which the
           IO7 model follows (marvel.md, M5) [inference]. Called with BSR
           (link S1) as the IPL drops below 4; keeps r1, r2 and r16. */
        ".global do_eoi\n"
        "do_eoi:\n"
        "    hw_stq/p $1, " S(IMP_E_R1) "($s0)\n"
        "    hw_stq/p $2, " S(IMP_E_R2) "($s0)\n"
        "    hw_stq/p $16, " S(IMP_E_R16) "($s0)\n"
        "    hw_ldq/p $16, " S(IMP_EOI) "($s0)\n"
        "    srl $16, 32, $1\n"
        "    blbc $1, eoi_none\n"
        "    srl $16, 14, $1\n"                        /* PE */
        "    ornot $31, $1, $1\n"
        "    sll $1, 51, $1\n"
        "    srl $1, 16, $1\n"                         /* ipe8(PE) */
        "    srl $16, 5, $2\n"
        "    and $2, 3, $2\n"                          /* the port */
        "    xor $2, 7, $2\n"                          /* ~port & 7 */
        "    sll $2, 12, $2\n"
        "    lda $2, " S(IO7_EOI_HI) "($2)\n"
        "    sll $2, 20, $2\n"
        "    lda $2, " S(IO7_EOI_DAT) "($2)\n"
        "    bis $1, $2, $1\n"
        "    zapnot $16, 7, $16\n"                     /* the IID */
        "    hw_stq/p $16, 0($1)\n"
        "    mb\n"
        "    hw_stq/p $31, " S(IMP_EOI) "($s0)\n"
        "eoi_none:\n"
        "    hw_ldq/p $1, " S(IMP_E_R1) "($s0)\n"
        "    hw_ldq/p $2, " S(IMP_E_R2) "($s0)\n"
        "    hw_ldq/p $16, " S(IMP_E_R16) "($s0)\n"
        RET_S1

        /* ---- Halt (3.1, 3.3): S0 = the impure area, H_REASON and H_PC
           set. Interrupts off in kernel mode; the IID in service ended
           [decision]; the context into the HWPCB at PCBB as swpctx saves
           it (KSP, USP, the process cycle count, UNIQUE) [decision]; a
           message on the terminal for a stop nobody asked for (reasons 2
           and 0x10000 up); the halt in this TPU's per-CPU slot when the
           console has built an HWRPB with a slot for it [ARM pp. 634-635]:
           PCBB, PC, PS, R25, R26, R27, the reason, and OH for an
           operator's halt (reason 1) [ARM p. 638]; then IPL 7, and
           processor 0 back to the console (a0 = the reason, a1 = 0)
           [decision], any other to its parking (N17). */
        ".global halt_path\n"
        "halt_path:\n"
        "    bis $31, $25, $8\n"                       /* the architectural R25 (as Rb) */
        "    hw_mtpr $31, ier_cm\n"
        "    ifetchb\n"
        "    bsr $s1, do_eoi\n"
        "    hw_ldq/p $1, " S(IMP_PCBB) "($s0)\n"
        "    hw_ldq/p $2, " S(IMP_PS) "($s0)\n"
        "    and $2, 8, $2\n"
        "    bne $2, halt_user\n"
        "    hw_stq/p $30, " S(PCB_KSP) "($1)\n"
        "    hw_ldq/p $2, " S(IMP_USP) "($s0)\n"
        "    hw_stq/p $2, " S(PCB_USP) "($1)\n"
        "    br $31, halt_pcb\n"
        "halt_user:\n"
        "    hw_ldq/p $2, " S(IMP_KSP) "($s0)\n"
        "    hw_stq/p $2, " S(PCB_KSP) "($1)\n"
        "    hw_stq/p $30, " S(PCB_USP) "($1)\n"
        "halt_pcb:\n"
        "    rpcc $2\n"
        "    srl $2, 32, $3\n"
        "    addq $2, $3, $2\n"
        "    hw_stl/p $2, " S(PCB_PCC_ASN) "($1)\n"
        "    hw_ldq/p $2, " S(IMP_UNIQUE) "($s0)\n"
        "    hw_stq/p $2, " S(PCB_UNIQUE) "($1)\n"
        /* the message before the record, so that whoever sees the record
           finds the terminal free [decision] */
        "    hw_ldq/p $2, " S(IMP_H_REASON) "($s0)\n"
        "    cmpeq $2, " S(HALT_KSNV) ", $3\n"
        "    bne $3, halt_message\n"
        "    srl $2, 16, $3\n"
        "    beq $3, halt_record\n"
        "halt_message:\n"
        "    bsr $s1, pal_message\n"
        "halt_record:\n"
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
        "    cmpeq $2, " S(HALT_OPERATOR) ", $2\n"
        "    beq $2, halt_flags_done\n"
        "    hw_ldq/p $2, " S(SLOT_FLAGS) "($1)\n"
        "    bis $2, " S(FLAG_OH) ", $2\n"
        "    hw_stq/p $2, " S(SLOT_FLAGS) "($1)\n"
        "halt_flags_done:\n"
        "    mb\n"
        "halt_noslot:\n"
        "    lda $2, 7($31)\n"
        "    hw_stq/p $2, " S(IMP_PS) "($s0)\n"
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
 * take OPCDEC instead (ev8-native.md 1.5), so no slot checks the mode. A
 * code 3.1 does not list is opDec, entIF type 4 at the next PC [ARM pp.
 * 605, 801: reserved to Compaq]. On entry S1 holds the return PC with the
 * caller's mode, 00.
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
        /* 0x01 cflush [ARM p. 552]: no cache is modelled */
        SLOT(0x2040)
        RET_S1
        /* 0x02 draina [ARM p. 265]: machine checks are never raised */
        SLOT(0x2080)
        "    mb\n"
        RET_S1
        OPDEC(0x20c0)
        OPDEC(0x2100)
        OPDEC(0x2140)
        OPDEC(0x2180)
        OPDEC(0x21c0)
        OPDEC(0x2200)
        TO(0x2240, "cserve_body")       /* 0x09 cserve */
        /* 0x0A swppal [ARM p. 563]: one PALcode; v0 = 1, unknown (N24) */
        SLOT(0x2280)
        "    lda $0, 1($31)\n"
        RET_S1
        OPDEC(0x22c0)
        OPDEC(0x2300)
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
        OPDEC(0x2380)
        OPDEC(0x23c0)
        /* 0x10 rdmces [ARM p. 554] */
        SLOT(0x2400)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_MCES) "($s0)\n"
        RET_S1
        /* 0x11 wrmces [ARM p. 572]: a0<2:0> clear MIP, SCE, PCE; DPC and
           DSC become a0<3> and a0<4> */
        SLOT(0x2440)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
        "    hw_ldq/p $1, " S(IMP_MCES) "($s0)\n"
        "    and $16, 7, $s1\n"
        "    bic $1, $s1, $1\n"
        "    bic $1, 0x18, $1\n"
        "    and $16, 0x18, $s1\n"
        "    bis $1, $s1, $1\n"
        "    hw_stq/p $1, " S(IMP_MCES) "($s0)\n"
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        RET_S1
        OPDEC(0x2480)
        OPDEC(0x24c0)
        OPDEC(0x2500)
        OPDEC(0x2540)
        OPDEC(0x2580)
        OPDEC(0x25c0)
        OPDEC(0x2600)
        OPDEC(0x2640)
        OPDEC(0x2680)
        OPDEC(0x26c0)
        OPDEC(0x2700)
        OPDEC(0x2740)
        OPDEC(0x2780)
        OPDEC(0x27c0)
        OPDEC(0x2800)
        OPDEC(0x2840)
        OPDEC(0x2880)
        OPDEC(0x28c0)
        OPDEC(0x2900)
        OPDEC(0x2940)
        OPDEC(0x2980)
        OPDEC(0x29c0)
        OPDEC(0x2a00)
        OPDEC(0x2a40)
        OPDEC(0x2a80)
        TO(0x2ac0, "wrfen_body")        /* 0x2B wrfen */
        OPDEC(0x2b00)
        /* 0x2D wrvptptr [ARM p. 578]: VPTE_BASE of I_CTL and M_CTL */
        SLOT(0x2b40)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $16, " S(IMP_VPTB) "($s0)\n"
        "    hw_mtpr $16, i_ctl_vpte\n"
        "    ifetchb\n"
        "    hw_mtpr $16, m_ctl_vpte\n"
        "    hw_mtpr $16, m_ctl_vpte\n"
        "    ifetchb\n"
        RET_S1
        OPDEC(0x2b80)
        OPDEC(0x2bc0)
        TO(0x2c00, "swpctx_body")       /* 0x30 swpctx */
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
        TO(0x2cc0, "tbi_body")          /* 0x33 tbi */
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
        TO(0x2d40, "swpipl_body")       /* 0x35 swpipl */
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
        /* 0x38 wrusp [ARM p. 575] */
        SLOT(0x2e00)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $16, " S(IMP_USP) "($s0)\n"
        RET_S1
        /* 0x39 wrperfmon [ARM p. 573]: nothing is counted; v0 = 0 */
        SLOT(0x2e40)
        "    bis $31, $31, $0\n"
        RET_S1
        /* 0x3A rdusp [ARM p. 556] */
        SLOT(0x2e80)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_USP) "($s0)\n"
        RET_S1
        /* 0x3B (N8, the owner 2026-10-31): v0 = VCTL, VCTL<VEN> = a0<0>,
           the IPR 0xAC */
        SLOT(0x2ec0)
        "    hw_mfpr $0, vctl\n"
        "    and $16, 1, $1\n"
        "    hw_mtpr $1, vctl\n"
        "    ifetchb\n"
        RET_S1
        /* 0x3C whami [ARM p. 566]: PID x 4 + TPU (N25) */
        SLOT(0x2f00)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_WHAMI) "($s0)\n"
        RET_S1
        TO(0x2f40, "retsys_body")       /* 0x3D retsys */
        TO(0x2f80, "wtint_body")        /* 0x3E wtint */
        TO(0x2fc0, "rti_body")          /* 0x3F rti */

        /* 0x80 bpt, 0x81 bugchk [ARM pp. 544-545]: entIF types 0 and 1 at
           the next PC */
        SLOT(0x3000)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A0) "($s0)\n"
        "    br $31, post_if\n"
        SLOT(0x3040)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    lda $s1, 1($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    br $31, post_if\n"
        OPDEC(0x3080)
        TO(0x30c0, "callsys_body")      /* 0x83 callsys */
        OPDEC(0x3100)
        OPDEC(0x3140)
        TO(0x3180, "imb_body")          /* 0x86 imb */
        OPDEC(0x31c0)
        OPDEC(0x3200)
        OPDEC(0x3240)
        OPDEC(0x3280)
        OPDEC(0x32c0)
        OPDEC(0x3300)
        OPDEC(0x3340)
        OPDEC(0x3380)
        OPDEC(0x33c0)
        OPDEC(0x3400)
        OPDEC(0x3440)
        OPDEC(0x3480)
        OPDEC(0x34c0)
        OPDEC(0x3500)
        OPDEC(0x3540)
        OPDEC(0x3580)
        OPDEC(0x35c0)
        OPDEC(0x3600)
        OPDEC(0x3640)
        OPDEC(0x3680)
        OPDEC(0x36c0)
        OPDEC(0x3700)
        OPDEC(0x3740)
        /* 0x9E rdunique, 0x9F wrunique [ARM pp. 549-550] */
        SLOT(0x3780)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_UNIQUE) "($s0)\n"
        RET_S1
        SLOT(0x37c0)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $16, " S(IMP_UNIQUE) "($s0)\n"
        RET_S1
        OPDEC(0x3800)
        OPDEC(0x3840)
        OPDEC(0x3880)
        OPDEC(0x38c0)
        OPDEC(0x3900)
        OPDEC(0x3940)
        OPDEC(0x3980)
        OPDEC(0x39c0)
        OPDEC(0x3a00)
        OPDEC(0x3a40)
        /* 0xAA gentrap [ARM p. 548]: entIF type 2 */
        SLOT(0x3a80)
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    lda $s1, 2($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    br $31, post_if\n"
        OPDEC(0x3ac0)
        OPDEC(0x3b00)
        OPDEC(0x3b40)
        TO(0x3b80, "clrfen_body")       /* 0xAE clrfen */
        OPDEC(0x3bc0)
        OPDEC(0x3c00)
        OPDEC(0x3c40)
        OPDEC(0x3c80)
        OPDEC(0x3cc0)
        OPDEC(0x3d00)
        OPDEC(0x3d40)
        OPDEC(0x3d80)
        OPDEC(0x3dc0)
        OPDEC(0x3e00)
        OPDEC(0x3e40)
        OPDEC(0x3e80)
        OPDEC(0x3ec0)
        OPDEC(0x3f00)
        OPDEC(0x3f40)
        OPDEC(0x3f80)
        OPDEC(0x3fc0)
        "    .align 64\n");
}

/* ------------------------------------------------------------------------
 * The bodies, from 0x4000
 * ------------------------------------------------------------------------ */
__attribute__((naked)) void pal_more_bodies(void)
{
    asm(
        /* ---- swpipl [ARM p. 562]: v0 the old IPL, the new one's interrupt
           enables into IER_CM (the IPL table, 3.1); below 4, the end of the
           interrupt in service */
        ".global swpipl_body\n"
        "swpipl_body:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $0, " S(IMP_PS) "($s0)\n"
        "    and $16, 7, $1\n"
        "    hw_stq/p $1, " S(IMP_PS) "($s0)\n"
        "    s8addq $1, $s0, $1\n"
        "    hw_ldq/p $1, " S(IMP_IERTAB) "($1)\n"
        "    hw_mtpr $1, ier_cm\n"
        "    ifetchb\n"
        "    and $0, 7, $0\n"
        "    and $16, 7, $1\n"
        "    cmpult $1, 4, $1\n"
        "    beq $1, swpipl_out\n"
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
        "    bsr $s1, do_eoi\n"
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        "swpipl_out:\n"
        RET_S1

        /* ---- rti [ARM p. 559]: pop PS, PC, GP, a0, a1, a2; to user mode,
           the kernel stack is kept and the user's restored, PS 8; the lock
           flag cleared (a STQ_C, which always clears it); below IPL 4 the
           end of the interrupt in service; the PS's mode and IPL into
           IER_CM last, after the frame's loads in kernel mode. a0 to a2
           serve as scratch until they are loaded. A stack not quadword
           aligned: kernel stack not valid [ARM p. 607]. */
        ".global rti_body\n"
        "rti_body:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    and $30, 7, $16\n"
        "    bne $16, ksnv_call\n"
        "    li $16, " S(KSEG) "\n"
        "    addq $16, $s0, $16\n"
        "    stq_c $17, " S(IMP_LOCK) "($16)\n"
        "    ldq $16, 0($30)\n"                        /* PS */
        "    ldq $s1, 8($30)\n"                        /* PC */
        "    bic $s1, 3, $s1\n"                        /* normal mode, whatever was stored */
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
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
        "    hw_ldq/p $16, " S(IMP_PS) "($s0)\n"
        "    and $16, 7, $16\n"
        "    cmpult $16, 4, $16\n"
        "    beq $16, rti_frame\n"
        "    bsr $s1, do_eoi\n"
        "rti_frame:\n"
        "    ldq $29, 16($30)\n"
        "    ldq $16, 24($30)\n"
        "    ldq $17, 32($30)\n"
        "    ldq $18, 40($30)\n"
        "    hw_ldq/p $30, " S(IMP_P_A0) "($s0)\n"
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        "    hw_ldq/p $s0, " S(IMP_P_SEL) "($s0)\n"
        "    hw_mtpr $s0, ier_cm\n"
        "    ifetchb\n"
        RET_S1
        /* A CALL_PAL found the kernel stack not quadword aligned: S1 the
           PC after it */
        "ksnv_call:\n"
        "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"
        "    lda $s1, " S(HALT_KSNV) "($31)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    br $31, halt_path\n"

        /* ---- retsys [ARM p. 558]: PC from (SP+8), GP from (SP+16), KSP
           = SP + 48, SP = USP, the lock flag cleared, PS 8 (user, IPL 0,
           so the end of the interrupt in service); t0, t8-t11
           UNPREDICTABLE */
        ".global retsys_body\n"
        "retsys_body:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    and $30, 7, $1\n"
        "    bne $1, ksnv_call\n"
        "    li $1, " S(KSEG) "\n"
        "    addq $1, $s0, $1\n"
        "    stq_c $1, " S(IMP_LOCK) "($1)\n"
        "    ldq $s1, 8($30)\n"
        "    bic $s1, 3, $s1\n"
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
        "    ldq $29, 16($30)\n"
        "    lda $1, 48($30)\n"
        "    hw_stq/p $1, " S(IMP_KSP) "($s0)\n"
        "    hw_ldq/p $30, " S(IMP_USP) "($s0)\n"
        "    lda $1, 8($31)\n"
        "    hw_stq/p $1, " S(IMP_PS) "($s0)\n"
        "    bsr $s1, do_eoi\n"
        "    hw_ldq/p $1, " S(IMP_IERTAB) "($s0)\n"
        "    bis $1, 0x18, $1\n"
        "    hw_mtpr $1, ier_cm\n"
        "    ifetchb\n"
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        RET_S1

        /* ---- swpctx [ARM p. 560]: the current context into the PCB at
           PCBB (KSP = SP, USP, the process cycle count in <31:0> of +24,
           UNIQUE), then PCBB = a0 and its context: SP, USP, PTBR, UNIQUE,
           PCC_OFF = its count - PCC_CNT, the ASN (<63:32> of +24) into
           I_PCTX and M_PCTX, FEN; v0 the old PCBB. t0, t8-t11 and a0
           UNPREDICTABLE. S1 holds the cycle counter as read. */
        ".global swpctx_body\n"
        "swpctx_body:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
        "    hw_ldq/p $0, " S(IMP_PCBB) "($s0)\n"
        "    hw_stq/p $30, " S(PCB_KSP) "($0)\n"
        "    hw_ldq/p $1, " S(IMP_USP) "($s0)\n"
        "    hw_stq/p $1, " S(PCB_USP) "($0)\n"
        "    rpcc $s1\n"
        "    srl $s1, 32, $1\n"
        "    addq $s1, $1, $1\n"
        "    hw_stl/p $1, " S(PCB_PCC_ASN) "($0)\n"
        "    hw_ldq/p $1, " S(IMP_UNIQUE) "($s0)\n"
        "    hw_stq/p $1, " S(PCB_UNIQUE) "($0)\n"
        "    hw_stq/p $16, " S(IMP_PCBB) "($s0)\n"
        "    hw_ldq/p $30, " S(PCB_KSP) "($16)\n"
        "    hw_ldq/p $1, " S(PCB_USP) "($16)\n"
        "    hw_stq/p $1, " S(IMP_USP) "($s0)\n"
        "    hw_ldq/p $1, " S(PCB_PTBR) "($16)\n"
        "    hw_stq/p $1, " S(IMP_PTBR) "($s0)\n"
        "    hw_ldq/p $1, " S(PCB_UNIQUE) "($16)\n"
        "    hw_stq/p $1, " S(IMP_UNIQUE) "($s0)\n"
        "    hw_ldl/p $1, " S(PCB_PCC_ASN) "($16)\n"
        "    subq $1, $s1, $1\n"
        "    sll $1, 32, $1\n"
        "    bis $1, 1, $1\n"                          /* the counter enabled */
        "    hw_mtpr $1, cc_offset\n"
        "    ifetchb\n"
        "    hw_ldq/p $1, " S(PCB_PCC_ASN) "($16)\n"
        "    srl $1, 32, $1\n"
        "    sll $1, 8, $1\n"
        "    hw_mtpr $1, i_pctx_asn\n"
        "    ifetchb\n"
        "    hw_mtpr $1, m_pctx_asn\n"
        "    hw_mtpr $1, m_pctx_asn\n"
        "    ifetchb\n"
        "    hw_ldq/p $1, " S(PCB_FLAGS) "($16)\n"
        "    and $1, 1, $1\n"
        "    sll $1, 19, $1\n"
        "    hw_mtpr $1, i_pctx_fpe\n"
        "    ifetchb\n"
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        RET_S1

        /* ---- tbi [ARM p. 565]: -2 tbia, ITB_IM and DTB_IM mode 01 (this
           group); -1 tbiap, mode 05 (this group's non-ASM entries); 1, 2,
           3: ITB_IS, DTB_IS, both, with a1; anything else nothing (3.1) */
        ".global tbi_body\n"
        "tbi_body:\n"
        "    lda $1, 2($16)\n"
        "    beq $1, tbi_all\n"
        "    lda $1, 1($16)\n"
        "    beq $1, tbi_nasm\n"
        "    cmpeq $16, 1, $1\n"
        "    bne $1, tbi_i\n"
        "    cmpeq $16, 2, $1\n"
        "    bne $1, tbi_d\n"
        "    cmpeq $16, 3, $1\n"
        "    bne $1, tbi_both\n"
        RET_S1
        "tbi_all:\n"
        "    lda $1, 1($31)\n"
        "    br $31, tbi_mode\n"
        "tbi_nasm:\n"
        "    lda $1, 5($31)\n"
        "tbi_mode:\n"
        "    hw_mtpr $1, itb_im\n"
        "    ifetchb\n"
        "    hw_mtpr $1, dtb_im\n"
        "    hw_mtpr $1, dtb_im\n"
        "    ifetchb\n"
        RET_S1
        "tbi_both:\n"
        "    hw_mtpr $17, itb_is\n"
        "    ifetchb\n"
        "tbi_d:\n"
        "    hw_mtpr $17, dtb_is\n"
        "    hw_mtpr $17, dtb_is\n"
        "    ifetchb\n"
        RET_S1
        "tbi_i:\n"
        "    hw_mtpr $17, itb_is\n"
        "    ifetchb\n"
        RET_S1

        /* ---- imb [ARM p. 267]: the I-cache flushed; from user mode, the
           PCB's IMB bit (+40 <32>) set. No register changes. */
        ".global imb_body\n"
        "imb_body:\n"
        "    hw_mtpr $31, ic_flush\n"
        "    ifetchb\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $1, " S(IMP_T1) "($s0)\n"
        "    hw_ldq/p $1, " S(IMP_PS) "($s0)\n"
        "    and $1, 8, $1\n"
        "    beq $1, imb_out\n"
        "    hw_stq/p $2, " S(IMP_T2) "($s0)\n"
        "    hw_ldq/p $1, " S(IMP_PCBB) "($s0)\n"
        "    hw_ldq/p $2, " S(PCB_FLAGS) "($1)\n"
        "    srl $2, 32, $s0\n"
        "    bis $s0, 1, $s0\n"
        "    sll $s0, 32, $s0\n"
        "    zapnot $2, 0x0f, $2\n"
        "    bis $2, $s0, $2\n"
        "    hw_stq/p $2, " S(PCB_FLAGS) "($1)\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $2, " S(IMP_T2) "($s0)\n"
        "imb_out:\n"
        "    hw_ldq/p $1, " S(IMP_T1) "($s0)\n"
        RET_S1

        /* ---- wrfen [ARM p. 569] and clrfen [p. 547]: I_PCTX<FP_ENABLE>
           and the PCB's FEN (+40 <0>) */
        ".global wrfen_body\n"
        "wrfen_body:\n"
        "    and $16, 1, $1\n"
        "    br $31, fen_set\n"
        ".global clrfen_body\n"
        "clrfen_body:\n"
        "    bis $31, $31, $1\n"
        "fen_set:\n"
        "    sll $1, 19, $22\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
        "    sll $22, 0, $s1\n"
        "    hw_mtpr $s1, i_pctx_fpe\n"
        "    ifetchb\n"
        "    hw_ldq/p $s0, " S(IMP_PCBB) "($s0)\n"
        "    hw_ldq/p $22, " S(PCB_FLAGS) "($s0)\n"
        "    bic $22, 1, $22\n"
        "    bis $22, $1, $22\n"
        "    hw_stq/p $22, " S(PCB_FLAGS) "($s0)\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        RET_S1

        /* ---- wtint [ARM p. 579] (N26): a QUIESCE on a quadword of the
           TPU's own, woken by any interrupt (IPL 0's enables while it
           sleeps, whatever the IPL [decision]: the ARM returns on any
           interrupt) or by QUIESCE_TIMEOUT's largest timeout, about a
           million cycles [decision: the ARM lets wtint return at any time
           up to its limit]; then the IPL's enables, the parking's
           QUIESCE_TIMEOUT, and v0 = 0 ticks skipped. a0 UNPREDICTABLE. */
        ".global wtint_body\n"
        "wtint_body:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
        "    hw_ldq/p $1, " S(IMP_IERTAB) "($s0)\n"
        "    hw_mtpr $1, ier_cm\n"
        "    ifetchb\n"
        "    li $1, " S(QT_WTINT) "\n"
        "    hw_mtpr $1, quiesce_timeout\n"
        "    hw_mtpr $1, quiesce_timeout\n"
        "    ifetchb\n"
        "    li $1, " S(KSEG) "\n"
        "    addq $1, $s0, $1\n"
        "    ldq_arm $16, ($1)\n"
        "    quiesce\n"
        "    li $1, " S(QT_PARK) "\n"
        "    hw_mtpr $1, quiesce_timeout\n"
        "    hw_mtpr $1, quiesce_timeout\n"
        "    ifetchb\n"
        "    hw_ldq/p $1, " S(IMP_PS) "($s0)\n"
        "    and $1, 7, $1\n"
        "    s8addq $1, $s0, $1\n"
        "    hw_ldq/p $1, " S(IMP_IERTAB) "($1)\n"
        "    hw_mtpr $1, ier_cm\n"
        "    ifetchb\n"
        "    bis $31, $31, $0\n"
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        RET_S1

        /* ---- callsys [ARM p. 607]: from user mode entSys, a0-a2 the
           caller's, the frame's PC the next; from kernel mode a bugcheck
           (3.1), entIF type 1 */
        ".global callsys_body\n"
        "callsys_body:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_ldq/p $s1, " S(IMP_PS) "($s0)\n"
        "    and $s1, 8, $s1\n"
        "    beq $s1, callsys_kernel\n"
        "    hw_stq/p $16, " S(IMP_P_A0) "($s0)\n"
        "    hw_stq/p $17, " S(IMP_P_A1) "($s0)\n"
        "    hw_stq/p $18, " S(IMP_P_A2) "($s0)\n"
        "    lda $s1, " S(SEL_SYS) "($31)\n"
        "    br $31, post\n"
        "callsys_kernel:\n"
        "    lda $s1, 1($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        /* entIF with P_A0 the type and P_PC set; a1 and a2 zero */
        ".global post_if\n"
        "post_if:\n"
        "    hw_stq/p $31, " S(IMP_P_A1) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A2) "($s0)\n"
        "    lda $s1, " S(SEL_IF) "($31)\n"
        "    br $31, post\n"
        /* A CALL_PAL 3.1 does not list: opDec, at the next PC */
        ".global pal_opdec\n"
        "pal_opdec:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    lda $s1, 4($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    br $31, post_if\n"

        /* ---- FEN (3.2): entIF type 3 at the instruction [ARM p. 605] */
        ".global fen_flow\n"
        "fen_flow:\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    blbs $s1, fen_pal\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    lda $s1, 3($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    br $31, post_if\n"
        "fen_pal:\n"
        "    lda $s1, 0x200($31)\n"
        "    br $31, unexpected\n"

        /* ---- OPCDEC (3.2): entIF type 4 at the next PC [ARM p. 605]; an
           OPCDEC with VEN off is the OS's to recognise (ev8-isa.md 5) */
        ".global opcdec_flow\n"
        "opcdec_flow:\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    blbs $s1, opcdec_pal\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    lda $s1, 4($s1)\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    lda $s1, 4($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    br $31, post_if\n"
        "opcdec_pal:\n"
        "    lda $s1, 0x400($31)\n"
        "    br $31, unexpected\n"

        /* ---- UNALIGN (3.2): entUna, a0 the VA, a1 the opcode (M_STAT
           <9:4>), a2 the register (EXC_SUM<Ra>, <28:24>) [ARM p. 607]; the
           frame's PC the next instruction, as Linux emulates the access
           and returns past it [inference: do_entUna; qemu-palcode does the
           same] */
        ".global una_flow\n"
        "una_flow:\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    blbs $s1, una_pal\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    lda $s1, 4($s1)\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_mfpr $s1, va\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    hw_mfpr $s1, m_stat\n"
        "    srl $s1, 4, $s1\n"
        "    and $s1, 0x3f, $s1\n"
        "    hw_stq/p $s1, " S(IMP_P_A1) "($s0)\n"
        "    hw_mfpr $s1, exc_sum\n"
        "    srl $s1, 24, $s1\n"
        "    and $s1, 31, $s1\n"
        "    hw_stq/p $s1, " S(IMP_P_A2) "($s0)\n"
        "    lda $s1, " S(SEL_UNA) "($31)\n"
        "    br $31, post\n"
        "una_pal:\n"
        "    lda $s1, 0x280($31)\n"
        "    br $31, unexpected\n"

        /* ---- DFAULT (3.2): entMM, a0 the VA, a1 the MMCSR from M_STAT
           (ACV 1, FOR 2, FOW 4 [ARM p. 593]; a bad VA is ACV, N12), a2
           M_STAT<WR> [ARM p. 606], at the instruction. From PALmode the
           kernel stack is not valid [ARM p. 607]. */
        ".global dfault_flow\n"
        "dfault_flow:\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    blbs $s1, dfault_pal\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_mfpr $s1, va\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    hw_mfpr $s1, m_stat\n"
        "    and $s1, 1, $s1\n"
        "    hw_stq/p $s1, " S(IMP_P_A2) "($s0)\n"
        "    hw_mfpr $s1, m_stat\n"
        "    srl $s1, 1, $s1\n"
        "    blbs $s1, df_acv\n"
        "    hw_mfpr $s1, m_stat\n"
        "    srl $s1, 2, $s1\n"
        "    blbs $s1, df_for\n"
        "    lda $s1, 4($31)\n"
        "    br $31, df_set\n"
        "df_for:\n"
        "    lda $s1, 2($31)\n"
        "    br $31, df_set\n"
        "df_acv:\n"
        "    lda $s1, 1($31)\n"
        "df_set:\n"
        "    hw_stq/p $s1, " S(IMP_P_A1) "($s0)\n"
        "    lda $s1, " S(SEL_MM) "($31)\n"
        "    br $31, post\n"
        "dfault_pal:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_H_PC) "($s0)\n"
        "    lda $s1, " S(HALT_KSNV) "($31)\n"
        "    hw_stq/p $s1, " S(IMP_H_REASON) "($s0)\n"
        "    br $31, halt_path\n"

        /* ---- IACV (3.2): entMM, a0 the instruction's VA, ACV, cause -1,
           at that instruction */
        ".global iacv_flow\n"
        "iacv_flow:\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    blbs $s1, iacv_pal\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    lda $s1, 1($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A1) "($s0)\n"
        "    lda $s1, -1($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A2) "($s0)\n"
        "    lda $s1, " S(SEL_MM) "($31)\n"
        "    br $31, post\n"
        "iacv_pal:\n"
        "    lda $s1, 0x480($31)\n"
        "    br $31, unexpected\n"

        /* ---- ARITH (3.2): entArith, a0 the exception summary (EXC_SUM
           <6:0>, the ARM's layout [p. 603]), a1 the register write mask,
           bit Rc (EXC_SUM<18:14>) of the integer registers when INT <13>
           is set, else of the floating ones (32 + Rc) [inference]; EXC_ADDR
           is already the next PC [IDS p. 485]. EXC_SUM<SET_*> asks the
           PALcode to set those bits in the FPCR [IDS p. 486]: done, with
           SUM, through f0, which is kept (Alphabox's core sets the FPCR
           itself and reads them zero, cpu-fidelity.md). */
        ".global arith_flow\n"
        "arith_flow:\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    blbs $s1, arith_pal\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_mfpr $s1, exc_sum\n"
        "    and $s1, 0x7f, $s1\n"
        "    hw_stq/p $s1, " S(IMP_P_A0) "($s0)\n"
        "    hw_stq/p $31, " S(IMP_P_A2) "($s0)\n"
        "    hw_stq/p $1, " S(IMP_T1) "($s0)\n"
        "    hw_mfpr $s1, exc_sum\n"
        "    srl $s1, 14, $1\n"
        "    and $1, 31, $1\n"                         /* Rc */
        "    srl $s1, 13, $s1\n"
        "    blbs $s1, arith_int\n"
        "    lda $1, 32($1)\n"
        "arith_int:\n"
        "    lda $s1, 1($31)\n"
        "    sll $s1, $1, $s1\n"
        "    hw_stq/p $s1, " S(IMP_P_A1) "($s0)\n"
        "    hw_mfpr $s1, exc_sum\n"
        "    srl $s1, 7, $s1\n"
        "    and $s1, 0x3f, $s1\n"
        "    beq $s1, arith_post\n"
        "    ftoit $f0, $1\n"
        "    hw_stq/p $1, " S(IMP_T2) "($s0)\n"
        "    mf_fpcr $f0\n"
        "    ftoit $f0, $1\n"
        "    sll $s1, 57, $s1\n"
        "    bis $1, $s1, $1\n"
        "    lda $s1, 1($31)\n"
        "    sll $s1, 63, $s1\n"
        "    bis $1, $s1, $1\n"                        /* SUM */
        "    itoft $1, $f0\n"
        "    mt_fpcr $f0\n"
        "    ifetchb\n"                                /* Restriction 16 */
        "    hw_ldq/p $1, " S(IMP_T2) "($s0)\n"
        "    itoft $1, $f0\n"
        "arith_post:\n"
        "    hw_ldq/p $1, " S(IMP_T1) "($s0)\n"
        "    lda $s1, " S(SEL_ARITH) "($31)\n"
        "    br $31, post\n"
        "arith_pal:\n"
        "    lda $s1, 0x600($31)\n"
        "    br $31, unexpected\n"

        /* ---- BAD_JUMP_IVA (3.2): a jump to an address outside the virtual
           address space; it did not complete [IDS p. 629]. entMM, a0 the
           target (the jump's Rb, EXC_SUM<23:19>, read through a table of
           32 moves: R22 and R23 named as operand A, the architectural
           ones), ACV, cause -1, the frame's PC the jump [decision: the
           restart PC of Table 22-2]. */
        ".global badjump_flow\n"
        "badjump_flow:\n"
        "    hw_mfpr $s1, exc_addr\n"
        "    blbs $s1, badjump_pal\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_P_PC) "($s0)\n"
        "    hw_mfpr $s0, exc_sum\n"
        "    srl $s0, 19, $s0\n"
        "    and $s0, 31, $s0\n"
        "    br $s1, bj_here\n"
        "bj_here:\n"
        "    s8addq $s0, $s1, $s1\n"
        "    lda $s1, 16($s1)\n"
        "    jmp $31, ($s1)\n"
        "    ifetchb\n"                                /* Restriction 14 */
        "    bis $31, $0, $s0\n    br $31, bj_got\n"
        "    bis $31, $1, $s0\n    br $31, bj_got\n"
        "    bis $31, $2, $s0\n    br $31, bj_got\n"
        "    bis $31, $3, $s0\n    br $31, bj_got\n"
        "    bis $31, $4, $s0\n    br $31, bj_got\n"
        "    bis $31, $5, $s0\n    br $31, bj_got\n"
        "    bis $31, $6, $s0\n    br $31, bj_got\n"
        "    bis $31, $7, $s0\n    br $31, bj_got\n"
        "    bis $31, $8, $s0\n    br $31, bj_got\n"
        "    bis $31, $9, $s0\n    br $31, bj_got\n"
        "    bis $31, $10, $s0\n    br $31, bj_got\n"
        "    bis $31, $11, $s0\n    br $31, bj_got\n"
        "    bis $31, $12, $s0\n    br $31, bj_got\n"
        "    bis $31, $13, $s0\n    br $31, bj_got\n"
        "    bis $31, $14, $s0\n    br $31, bj_got\n"
        "    bis $31, $15, $s0\n    br $31, bj_got\n"
        "    bis $31, $16, $s0\n    br $31, bj_got\n"
        "    bis $31, $17, $s0\n    br $31, bj_got\n"
        "    bis $31, $18, $s0\n    br $31, bj_got\n"
        "    bis $31, $19, $s0\n    br $31, bj_got\n"
        "    bis $31, $20, $s0\n    br $31, bj_got\n"
        "    bis $31, $21, $s0\n    br $31, bj_got\n"
        "    bis $22, $31, $s0\n    br $31, bj_got\n"
        "    bis $23, $31, $s0\n    br $31, bj_got\n"
        "    bis $31, $24, $s0\n    br $31, bj_got\n"
        "    bis $31, $25, $s0\n    br $31, bj_got\n"
        "    bis $31, $26, $s0\n    br $31, bj_got\n"
        "    bis $31, $27, $s0\n    br $31, bj_got\n"
        "    bis $31, $28, $s0\n    br $31, bj_got\n"
        "    bis $31, $29, $s0\n    br $31, bj_got\n"
        "    bis $31, $30, $s0\n    br $31, bj_got\n"
        "    bis $31, $31, $s0\n    br $31, bj_got\n"
        "bj_got:\n"
        "    bic $s0, 3, $s0\n"
        "    hw_mfpr $s1, pal_temp1\n"
        "    hw_stq/p $s0, " S(IMP_P_A0) "($s1)\n"
        "    bis $31, $s1, $s0\n"
        "    lda $s1, 1($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A1) "($s0)\n"
        "    lda $s1, -1($31)\n"
        "    hw_stq/p $s1, " S(IMP_P_A2) "($s0)\n"
        "    lda $s1, " S(SEL_MM) "($31)\n"
        "    br $31, post\n"
        "badjump_pal:\n"
        "    lda $s1, 0x900($31)\n"
        "    br $31, unexpected\n"

        /* ---- cserve [ARM p. 553] (3.1): 0x48 PUT_TOY and 0x49 GET_TOY,
           a1 the TOY's index, a2 the data, as Linux's __marvel_access_rtc
           passes them (binary values): the MC146818 image in the CMM's
           memory at 0x40000 + (PID & 1) x 0x6000, 0x1aac + index for 0-11
           and 0x4b98 + (index - 12) for 12-0x10b (marvel.md, M4)
           [inference]; through this chip's GIO and the CMM's byte window
           (register 2 the address, 0 GO with the lanes, 1 the 16-bit
           data), under the chip's GIO lock word, GIO_CNFG's divisor
           written first if it is zero (S11b). v0: the byte, 0 for a PUT,
           -1 for any other function or index [decision]. r1, r16-r18,
           t8-t11 UNPREDICTABLE (Linux's clobbers); r2-r5 kept. */
        ".global cserve_body\n"
        "cserve_body:\n"
        "    hw_mfpr $s0, pal_temp1\n"
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
        "    lda $0, -1($31)\n"
        "    lda $1, -0x48($16)\n"
        "    cmpult $1, 2, $1\n"
        "    beq $1, cs_return\n"
        "    lda $1, -0x10c($17)\n"
        "    bge $1, cs_return\n"
        "    blt $17, cs_return\n"                     /* the index unsigned */
        "    cmpult $17, 12, $1\n"
        "    bne $1, cs_toy\n"
        "    lda $1, " S(CMM_TOY_NV) "($17)\n"
        "    br $31, cs_area\n"
        "cs_toy:\n"
        "    lda $1, " S(CMM_TOY) "($17)\n"
        "cs_area:\n"
        "    hw_stq/p $2, " S(IMP_T1) "($s0)\n"
        "    hw_stq/p $3, " S(IMP_T2) "($s0)\n"
        "    hw_stq/p $4, " S(IMP_T3) "($s0)\n"
        "    hw_stq/p $5, " S(IMP_T4) "($s0)\n"
        "    hw_ldq/p $2, " S(IMP_WHAMI) "($s0)\n"
        "    srl $2, 2, $2\n"                          /* the PID */
        "    and $2, 1, $3\n"                          /* its place */
        "    s4subq $3, $3, $3\n"
        "    sll $3, 13, $3\n"                         /* x 0x6000 */
        "    addq $1, $3, $1\n"
        "    ldah $1, 4($1)\n"                         /* + 0x40000 */
        /* the lock: KSEG | PAL_GIO_LOCK + PID x 0x40 */
        "    sll $2, 6, $2\n"
        "    li $3, " S(KSEG_GIO_LOCK) "\n"
        "    addq $2, $3, $2\n"
        "cs_lock:\n"
        "    ldq_l $3, 0($2)\n"
        "    bne $3, cs_spin\n"
        "    lda $3, 1($31)\n"
        "    stq_c $3, 0($2)\n"
        "    beq $3, cs_lock\n"
        "    mb\n"
        "    br $31, cs_locked\n"
        "cs_spin:\n"
        "    ldq $3, 0($2)\n"
        "    bne $3, cs_spin\n"
        "    br $31, cs_lock\n"
        "cs_locked:\n"
        "    hw_ldq/p $4, " S(IMP_CSR) "($s0)\n"       /* the GIO registers */
        "    hw_ldq/p $3, " S(GIO_CNFG) "($4)\n"
        "    and $3, 0xfe, $3\n"
        "    bne $3, cs_div\n"
        "    lda $3, 2($31)\n"
        "    hw_stq/p $3, " S(GIO_CNFG) "($4)\n"
        "cs_div:\n"
        /* register 2, the window's address */
        GIO_WAIT("$4", "$5", "cs_w1")
        "    lda $3, 4($31)\n"
        "    hw_stq/p $3, " S(GIO_ADDR) "($4)\n"
        "    hw_stq/p $1, " S(GIO_DATA) "($4)\n"
        GIO_WAIT("$4", "$5", "cs_w2")
        "    cmpeq $16, 0x48, $3\n"
        "    bne $3, cs_put\n"
        /* GET: register 0 = GO, then a read of register 1 */
        "    hw_stq/p $31, " S(GIO_ADDR) "($4)\n"
        "    lda $3, " S(CMM_GO) "($31)\n"
        "    hw_stq/p $3, " S(GIO_DATA) "($4)\n"
        GIO_WAIT("$4", "$5", "cs_w3")
        "    lda $3, 3($31)\n"                         /* register 1, START_READ */
        "    hw_stq/p $3, " S(GIO_ADDR) "($4)\n"
        GIO_WAIT("$4", "$0", "cs_w4")
        "    zapnot $0, 3, $0\n"
        "    blbc $1, cs_even\n"
        "    srl $0, 8, $0\n"
        "cs_even:\n"
        "    and $0, 0xff, $0\n"
        "    br $31, cs_unlock\n"
        /* PUT: register 1 = the byte in its lane, register 0 = GO with
           the low lane (even address) or the high one (odd) */
        "cs_put:\n"
        "    and $18, 0xff, $3\n"
        "    blbc $1, cs_peven\n"
        "    sll $3, 8, $3\n"
        "cs_peven:\n"
        "    lda $5, 2($31)\n"
        "    hw_stq/p $5, " S(GIO_ADDR) "($4)\n"
        "    hw_stq/p $3, " S(GIO_DATA) "($4)\n"
        GIO_WAIT("$4", "$5", "cs_w5")
        "    and $1, 1, $3\n"
        "    addq $3, 1, $3\n"
        "    sll $3, 1, $3\n"                          /* LOW 2 or HIGH 4 */
        "    bis $3, " S(CMM_GO) ", $3\n"
        "    hw_stq/p $31, " S(GIO_ADDR) "($4)\n"
        "    hw_stq/p $3, " S(GIO_DATA) "($4)\n"
        GIO_WAIT("$4", "$5", "cs_w6")
        "    bis $31, $31, $0\n"
        "cs_unlock:\n"
        "    mb\n"
        "    stq $31, 0($2)\n"
        "    hw_ldq/p $2, " S(IMP_T1) "($s0)\n"
        "    hw_ldq/p $3, " S(IMP_T2) "($s0)\n"
        "    hw_ldq/p $4, " S(IMP_T3) "($s0)\n"
        "    hw_ldq/p $5, " S(IMP_T4) "($s0)\n"
        "cs_return:\n"
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        RET_S1

        /* ---- pal_message: "%PAL8-F-HALT, CPU w, reason r, PC p" on the
           CMM's terminal through this chip's GIO, under its lock (3.2's
           message for an entry never raised) [decision]. BSR with link S1,
           S0 = the impure area, kernel mode; r1-r7, r16, r17 used. */
        ".global pal_message\n"
        "pal_message:\n"
        "    hw_stq/p $s1, " S(IMP_RET) "($s0)\n"
        "    hw_ldq/p $1, " S(IMP_WHAMI) "($s0)\n"
        "    srl $1, 2, $1\n"
        "    sll $1, 6, $1\n"
        "    li $2, " S(KSEG_GIO_LOCK) "\n"
        "    addq $2, $1, $2\n"
        "msg_lock:\n"
        "    ldq_l $1, 0($2)\n"
        "    bne $1, msg_spin\n"
        "    lda $1, 1($31)\n"
        "    stq_c $1, 0($2)\n"
        "    beq $1, msg_lock\n"
        "    mb\n"
        "    br $31, msg_locked\n"
        "msg_spin:\n"
        "    ldq $1, 0($2)\n"
        "    bne $1, msg_spin\n"
        "    br $31, msg_lock\n"
        "msg_locked:\n"
        "    hw_ldq/p $7, " S(IMP_CSR) "($s0)\n"
        "    hw_ldq/p $1, " S(GIO_CNFG) "($7)\n"
        "    and $1, 0xfe, $1\n"
        "    bne $1, msg_div\n"
        "    lda $1, 2($31)\n"
        "    hw_stq/p $1, " S(GIO_CNFG) "($7)\n"
        "msg_div:\n"
        "    br $5, msg_go\n"
        /* "\r\n%PAL8-F-HALT, CPU ", ", reason ", ", PC ", "\r\n" */
        "    .byte 13,10,37,80,65,76,56,45,70,45,72,65,76,84,44,32,67,80,85,32,0\n"
        "    .byte 44,32,114,101,97,115,111,110,32,0\n"
        "    .byte 44,32,80,67,32,0\n"
        "    .byte 13,10,0\n"
        "    .align 4\n"
        "msg_go:\n"
        "    bic $5, 3, $5\n"
        "    lda $1, 0($5)\n"
        "    bsr $6, msg_str\n"
        "    hw_ldq/p $4, " S(IMP_WHAMI) "($s0)\n"
        "    bsr $6, msg_hex\n"
        "    lda $1, 21($5)\n"
        "    bsr $6, msg_str\n"
        "    hw_ldq/p $4, " S(IMP_H_REASON) "($s0)\n"
        "    bsr $6, msg_hex\n"
        "    lda $1, 31($5)\n"
        "    bsr $6, msg_str\n"
        "    hw_ldq/p $4, " S(IMP_H_PC) "($s0)\n"
        "    bsr $6, msg_hex\n"
        "    lda $1, 37($5)\n"
        "    bsr $6, msg_str\n"
        "    mb\n"
        "    stq $31, 0($2)\n"                         /* unlocked */
        "    hw_ldq/p $s1, " S(IMP_RET) "($s0)\n"
        RET_S1
        /* msg_str: the string at r1; msg_hex: r4 in 16 hex digits; each
           character to CMM register 4 (the terminal) through GIO at r7 */
        "msg_str:\n"
        "    hw_ldb/p $16, 0($1)\n"
        "    beq $16, msg_str_end\n"
        GIO_WAIT("$7", "$3", "ms_w1")
        "    lda $3, 8($31)\n"                         /* register 4 << 1 */
        "    hw_stq/p $3, " S(GIO_ADDR) "($7)\n"
        "    hw_stq/p $16, " S(GIO_DATA) "($7)\n"
        GIO_WAIT("$7", "$3", "ms_w2")
        "    lda $1, 1($1)\n"
        "    br $31, msg_str\n"
        "msg_str_end:\n"
        RET_R6
        "msg_hex:\n"
        "    lda $17, 60($31)\n"
        "mh_digit:\n"
        "    srl $4, $17, $16\n"
        "    and $16, 15, $16\n"
        "    cmpult $16, 10, $3\n"
        "    lda $16, 48($16)\n"
        "    bne $3, mh_dec\n"
        "    lda $16, 39($16)\n"
        "mh_dec:\n"
        GIO_WAIT("$7", "$3", "mh_w1")
        "    lda $3, 8($31)\n"
        "    hw_stq/p $3, " S(GIO_ADDR) "($7)\n"
        "    hw_stq/p $16, " S(GIO_DATA) "($7)\n"
        GIO_WAIT("$7", "$3", "mh_w2")
        "    subq $17, 4, $17\n"
        "    bge $17, mh_digit\n"
        RET_R6
        );
}
