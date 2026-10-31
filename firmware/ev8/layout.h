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
 * The EV8 firmware's memory layout and the constants its PALcode uses
 * (docs/platforms/ev8-native.md, sections 3.3, 3.6 and S12a). Read by
 * pal.c through the C preprocessor and by build.py, which parses the
 * #define lines (numbers only: nada's assembler evaluates no expression,
 * so every value an asm text names is written out here, and build.py
 * checks the ones derived from others).
 *
 * Everything is below 2 MB of PID 0's memory (N28), physical:
 *
 *   0x000000  the PALcode, PAL_BASE 0: entries 0x000-0xF80, CALL_PAL
 *             slots 0x2000-0x3FC0, the flows' bodies in between and after
 *   0x008000  PALcode data shared by every TPU: the GIO lock words
 *   0x00A000  the impure areas, one per TPU, indexed by WHAMI (N25)
 *   0x010000  the HWRPB, built by the console; its per-CPU slots after it
 *   0x020000  the console, entered at its first instruction
 *   0x200000  the end of what the firmware may use
 */
#ifndef EV8_LAYOUT_H
#define EV8_LAYOUT_H

#define PAL_BASE 0x0
#define PAL_TEXT_TOP 0x8000
#define PAL_DATA 0x8000
#define PAL_IMPURE 0xa000
#define IMPURE_SIZE 0x200
#define IMPURE_SHIFT 9
#define MAX_WHAMI 32
#define HWRPB_PA 0x10000
#define CONSOLE_PA 0x20000
#define FIRMWARE_TOP 0x200000

/* Linux's KSEG, SPE43's form (IDS pp. 688-689): KSEG | PA */
#define KSEG 0xfffffc0000000000
#define KSEG_CONSOLE 0xfffffc0000020000 /* KSEG | CONSOLE_PA */
#define KSEG_RXRDY 0xfffffc0000010128   /* KSEG | HWRPB_PA + HWRPB_RXRDY */

/* The HWRPB's fields the PALcode reads [ARM pp. 617-629, 726] */
#define HWRPB_ID 0x08
#define HWRPB_ID_VALUE 0x4250525748 /* "HWRPB\0\0\0" */
#define HWRPB_VPTB 0x78
#define HWRPB_NR_PROC 0x90
#define HWRPB_PROC_SIZE 0x98
#define HWRPB_PROC_OFF 0xa0
#define HWRPB_RESTART 0x100
#define HWRPB_RESTART_DATA 0x108
#define HWRPB_RXRDY 0x128

/* A per-CPU slot [ARM pp. 631-637, 693] */
#define SLOT_FLAGS 0x80
#define SLOT_HALT_PCBB 0xe8
#define SLOT_HALT_PC 0xf0
#define SLOT_HALT_PS 0xf8
#define SLOT_HALT_ARG 0x100
#define SLOT_HALT_RA 0x108
#define SLOT_HALT_PV 0x110
#define SLOT_HALT_REASON 0x118
#define SLOT_RXLEN 0x128
#define SLOT_RXBUF 0x130
#define FLAG_BIP 0x1
#define FLAG_RC 0x2
#define FLAG_CV_BIT 5
#define START_CMD 0x000a0d5452415453 /* "START\r\n", RXLEN 7 */

/* The HWPCB [ARM p. 560 (swpctx), 18.2] */
#define PCB_KSP 0x00
#define PCB_USP 0x08
#define PCB_PTBR 0x10
#define PCB_PCC_ASN 0x18
#define PCB_UNIQUE 0x20
#define PCB_FLAGS 0x28

/* The PALcode's own fields of a slot [ARM pp. 632-633], written at a
   START: its memory (text and shared data) and scratch (the impure areas),
   and its revision: <63:48> the processors that may share it, <47:32>
   compatibility 1, <23:16> variation 2 (Tru64 UNIX and Alpha Linux [ARM
   p. 808]), major 1, minor 0 [decision] */
#define SLOT_PAL_MEM_LEN 0x88
#define SLOT_PAL_SCR_LEN 0x90
#define SLOT_PAL_MEM_PA 0x98
#define SLOT_PAL_SCR_PA 0xa0
#define SLOT_PAL_REV 0xa8
#define PAL_MEM_PA 0x0
#define PAL_MEM_LEN 0xa000
#define PAL_SCR_PA 0xa000
#define PAL_SCR_LEN 0x4000
#define PAL_REVISION 0x0020000100020100
#define FLAG_OH 0x10
#define FLAG_PALOK 0x1c0 /* PL, PMV, PV */

/* Halt reasons [ARM p. 635]; 0x10000 | the PALcode offset: an entry
   never raised, or a fault inside the PALcode [decision, implementation
   specific] */
#define HALT_OPERATOR 1
#define HALT_KSNV 2
#define HALT_CALLPAL 5
#define HALT_UNIMPL 0x10000

/* The Rbox registers in a chip's window (ev8-native.md 2.3, N22) */
#define R_INT_STAT 0x50
#define R_INT_REQ 0x70
#define R_INT_QUE 0x80
#define R_INT_MASK_TPU 0x3f0000
#define CSR_WINDOW 0x7ffc /* << 20 */

/* GIO (2.5, N23) and the CMM's far-side registers (marvel.md, M4): the
   byte window's status (GO, the lanes), data and address, the terminal */
#define GIO_CNFG 0x100
#define GIO_DATA 0x110
#define GIO_ADDR 0x120
#define CMM_STATUS 0
#define CMM_DATA 1
#define CMM_ADDRESS 2
#define CMM_TX 4
#define CMM_GO 1
#define CMM_AREA 0x40000      /* + (PID & 1) x 0x6000 */
#define CMM_TOY 0x1aac        /* + index, 0-11: the MC146818's registers */
#define CMM_TOY_NV 0x4b8c     /* + index, 12-0x10b: 0x4b98 + (index - 12) */
#define TOY_LAST 0x10b
/* One GIO lock word per PID (2.5, part of N22): the console takes it too */
#define PAL_GIO_LOCK 0x8100   /* PAL_DATA + 0x100, + PID x 0x40 */
#define KSEG_GIO_LOCK 0xfffffc0000008100

/* An IO7 port's registers: ipe8(PE) | (~port & 7) << 32 | 0xFF800000, and
   its EOI_DAT (Linux's io7_ioport_csrs) */
#define IO7_EOI_HI 0xff8      /* << 20 */
#define IO7_EOI_DAT 0x2400

/* The impure area of a TPU (3.3): its PA is in PAL_TEMP1 */
#define IMP_ENTINT 0x000 /* wrent's a1 x 8 [ARM p. 568] */
#define IMP_ENTARITH 0x008
#define IMP_ENTMM 0x010
#define IMP_ENTIF 0x018
#define IMP_ENTUNA 0x020
#define IMP_ENTSYS 0x028
#define IMP_KGP 0x030
#define IMP_PTBR 0x038
#define IMP_PCBB 0x040
#define IMP_MCES 0x048
#define IMP_SYSVAL 0x050
#define IMP_PS 0x058
#define IMP_KSP 0x060
#define IMP_USP 0x068
#define IMP_WHAMI 0x070
#define IMP_UNIQUE 0x078
#define IMP_VPTB 0x080
#define IMP_CSR 0x088    /* this chip's register window, physical */
#define IMP_TPUBIT 0x090 /* 1 << TPU_ID */
#define IMP_LOCK 0x098   /* the target of rti's STQ_C */
#define IMP_P_SEL 0x0a0  /* post: the entry and its IPL */
#define IMP_P_PC 0x0a8
#define IMP_P_A0 0x0b0
#define IMP_P_A1 0x0b8
#define IMP_P_A2 0x0c0
#define IMP_H_REASON 0x0c8
#define IMP_H_PC 0x0d0
#define IMP_INT_R1 0x0d8
#define IMP_DM_R1 0x0e0
#define IMP_DM_R2 0x0e8
#define IMP_INT_R2 0x0f0
#define IMP_RET 0x0f8    /* a CALL_PAL's return PC, kept across calls */
#define IMP_IERTAB 0x100 /* IER_CM for each IPL, 8 quadwords */
#define IMP_EOI 0x140    /* the IID in service, <32> valid */
#define IMP_E_R1 0x148   /* do_eoi's saves */
#define IMP_E_R2 0x150
#define IMP_E_R16 0x158
#define IMP_T1 0x160     /* scratch */
#define IMP_T2 0x168
#define IMP_T3 0x170
#define IMP_T4 0x178
#define IMP_CPCB 0x180   /* the console's PCB, 16 quadwords */

/* post's selector: <7:0> the IMP_ENT* offset, <15:8> the new IPL + 1
   (0: unchanged) */
#define SEL_ARITH 0x008
#define SEL_MM 0x010
#define SEL_IF 0x018
#define SEL_UNA 0x020
#define SEL_SYS 0x028
#define SEL_IPI 0x600   /* entInt, IPL 5: interprocessor and clock */
#define SEL_DEVICE 0x500 /* entInt, IPL 4: a device [ARM p. 605; 3.1] */

/* IER_CM's EIEN <38:33> for each IPL (3.1, part of N22): EI1 and EI4
   below 4, EI2 and EI3 below 5, EI0 below 6, EI5 below 7 */
#define IER_IPL0 0x7e00000000 /* 0x3f << 33 */
#define IER_IPL4 0x5a00000000 /* 0x2d << 33 */
#define IER_IPL5 0x4200000000 /* 0x21 << 33 */
#define IER_IPL6 0x4000000000 /* 0x20 << 33 */

/* I_CTL and M_CTL at reset: 43-bit VA, 8 KB pages, SPE52 and SPE43 */
#define CTL_RESET 0x6000000
#define QT_PARK 0x2802 /* QUIESCE_TIMEOUT: WATCH_EN, the reset TIMEOUT */
#define QT_WTINT 0xffff3 /* WATCH_EN, TIMEOUT_EN, the largest TIMEOUT */

#endif
