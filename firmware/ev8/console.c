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
 * The Alphabox console for the native 21464 (S12b): the minimal console of
 * docs/platforms/ev8-native.md section 4, way B (N29). The emulator loads
 * the kernel, the initrd and the command line and says where in a boot
 * block (layout.h BOOT_BLOCK, src/chipsets/marvel/DirectBoot.cpp); this
 * console builds what Linux reads (the HWRPB with its per-CPU slots, CTB,
 * CRB, memory descriptors, DSRDB and GCT, the bootstrap page tables, the
 * zero page), starts the interval timer, and enters the kernel as the
 * PALcode's S12a list says; GETC, PUTS, GET_ENV and FIXUP are its
 * callbacks; a halt comes back to its prompt on the CMM's terminal.
 * Written by the project; nothing is copied from another console.
 *
 * C compiled by nada (main 9152b521) for alpha-none at KSEG | CONSOLE_PA
 * with the large code model (build.py), entered by the PALcode at its
 * first word in kernel mode, IPL 7, with no stack: a0 0 at power-up or the
 * reason of a halt, a1 the WHAMI. It runs on processor 0 alone, at KSEG
 * addresses only, its callbacks on the caller's processor and stack.
 *
 * Sources: ARM is the Alpha Architecture Reference Manual 4th edition, IDS
 * the 21464 Internal Design Specification Rev 1.1k, by PDF page; Linux's
 * files as ev8-native.md names them (read, not copied).
 */
#include "layout.h"

#define S_(x) #x
#define S(x) S_(x)

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

/* ------------------------------------------------------------------------
 * The assembler entry points: the console's start, the CALL_PALs it
 * makes, the GIO lock, the kernel's entry and the callbacks' doors
 * ------------------------------------------------------------------------ */
void cmain(u64 reason, u64 whami);
u64 dispatch_c(u64 code, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
u64 fixup_c(u64 new_base, u64 new_hwrpb);
u64 pal_whami(void);
u64 pal_wtint(void);
void pal_tbia(void);
void pal_wrmces(u64 v);
void gio_take(u64 lock);
void gio_give(u64 lock);
void enter_kernel(u64 pcb_pa, u64 entry, u64 vptb);
void crb_dispatch(void);
void crb_fixup(void);

static void console_text(void)
{
    asm(
        /* nada's br $31, _start at KSEG | CONSOLE_PA leads here */
        ".global _start\n"
        "_start:\n"
        "    li $30, " S(KSEG_CON_STACK) "\n"
        "    bsr $26, cmain\n"
        "    call_pal 0\n"

        ".global pal_whami\n"
        "pal_whami:\n"
        "    call_pal 0x3c\n"
        "    ret\n"
        /* wtint (N26): a QUIESCE until an interrupt or the timeout */
        ".global pal_wtint\n"
        "pal_wtint:\n"
        "    bis $31, $31, $16\n"
        "    call_pal 0x3e\n"
        "    ret\n"
        ".global pal_tbia\n"
        "pal_tbia:\n"
        "    lda $16, -2($31)\n"
        "    call_pal 0x33\n"
        "    ret\n"
        ".global pal_wrmces\n"
        "pal_wrmces:\n"
        "    call_pal 0x11\n"
        "    ret\n"

        /* The chip's GIO lock word (KSEG | PAL_GIO_LOCK + PID x 0x40),
           taken with LDQ_L/STQ_C as the PALcode takes it (S12a) */
        ".global gio_take\n"
        "gio_take:\n"
        "gt_try:\n"
        "    ldq_l $1, 0($16)\n"
        "    bne $1, gt_spin\n"
        "    lda $1, 1($31)\n"
        "    stq_c $1, 0($16)\n"
        "    beq $1, gt_try\n"
        "    mb\n"
        "    ret\n"
        "gt_spin:\n"
        "    ldq $1, 0($16)\n"
        "    bne $1, gt_spin\n"
        "    br $31, gt_try\n"
        ".global gio_give\n"
        "gio_give:\n"
        "    mb\n"
        "    stq $31, 0($16)\n"
        "    ret\n"

        /* The kernel's entry (S12a's list): wrvptptr, swpctx to the boot
           PCB (KSP, PTBR, FEN; SP becomes its KSP), the jump in kernel
           mode, R27 the entry as a procedure value */
        ".global enter_kernel\n"
        "enter_kernel:\n"
        "    bis $16, $16, $9\n"
        "    bis $17, $17, $10\n"
        "    bis $18, $18, $16\n"
        "    call_pal 0x2d\n"
        "    bis $9, $9, $16\n"
        "    call_pal 0x30\n"
        "    bis $10, $10, $27\n"
        "    bis $31, $31, $26\n"
        "    jmp $31, ($27)\n"

        /* The DISPATCH and FIXUP procedures, as the OpenVMS calling
           standard has them [ARM p. 646]: R16 up the arguments, R26 the
           return, R27 the procedure descriptor. R2-R15, R29 and SP are kept
           (nada's code keeps R9-R15 and never names R29), R0 the result */
        ".global crb_dispatch\n"
        "crb_dispatch:\n"
        "    lda $30, -80($30)\n"
        "    stq $26, 0($30)\n"
        "    stq $2, 8($30)\n"
        "    stq $3, 16($30)\n"
        "    stq $4, 24($30)\n"
        "    stq $5, 32($30)\n"
        "    stq $6, 40($30)\n"
        "    stq $7, 48($30)\n"
        "    stq $8, 56($30)\n"
        "    stq $29, 64($30)\n"
        "    bsr $26, dispatch_c\n"
        "    br $31, crb_out\n"
        ".global crb_fixup\n"
        "crb_fixup:\n"
        "    lda $30, -80($30)\n"
        "    stq $26, 0($30)\n"
        "    stq $2, 8($30)\n"
        "    stq $3, 16($30)\n"
        "    stq $4, 24($30)\n"
        "    stq $5, 32($30)\n"
        "    stq $6, 40($30)\n"
        "    stq $7, 48($30)\n"
        "    stq $8, 56($30)\n"
        "    stq $29, 64($30)\n"
        "    bsr $26, fixup_c\n"
        "crb_out:\n"
        "    ldq $26, 0($30)\n"
        "    ldq $2, 8($30)\n"
        "    ldq $3, 16($30)\n"
        "    ldq $4, 24($30)\n"
        "    ldq $5, 32($30)\n"
        "    ldq $6, 40($30)\n"
        "    ldq $7, 48($30)\n"
        "    ldq $8, 56($30)\n"
        "    ldq $29, 64($30)\n"
        "    lda $30, 80($30)\n"
        "    ret\n");
}

/* ------------------------------------------------------------------------
 * Memory: everything through KSEG (SPE43, which the PALcode enables at
 * reset and Linux keeps), I/O too: PA<47:41> all ones is KSEG with VA<40>
 * set, which reaches the register windows of PIDs 0-31 (N18)
 * ------------------------------------------------------------------------ */
#define PAGE 0x2000
#define MASK41 0x1ffffffffff

static u64 kva(u64 pa) { return KSEG | (pa & MASK41); }
static u64 rq(u64 pa) { return *(volatile u64 *)kva(pa); }
static void wq(u64 pa, u64 v) { *(volatile u64 *)kva(pa) = v; }
static void zero(u64 pa, u64 n)
{
    u64 i;
    for (i = 0; i < n; i += 8)
        wq(pa + i, 0);
}
static u64 slen(const char *s)
{
    u64 n = 0;
    while (s[n])
        n++;
    return n;
}

/* A chip's register window, ipe8(PID) | 0x7FFC << 20 (N18, N20) */
static u64 csr(u64 pid) { return (~pid & 0x1fff) << 35 | (u64)0x7ffc << 20; }
#define R_INTER_TIM 0xa0

/* The machine, from the boot block (the emulator's; the real console's
   from the CMM [decision]) */
static u64 hz, chips, tpus, mem_pid, io7_mask, entry, k_low, k_high;
static u64 rd_pa, rd_size, auto_boot, built, started, itv;
static u64 fixups, fixup_hwrpb;

static const char *sysname(void) { return (const char *)kva(BOOT_BLOCK + BB_SYSNAME); }
static const char *cmdline(void) { return (const char *)kva(BOOT_BLOCK + BB_CMDLINE); }

/* ------------------------------------------------------------------------
 * The CMM's terminal through GIO (2.5, N23; marvel.md, M4): register 4
 * takes a character, register 0's <8> says one waits in register 5. Each
 * transaction under the chip's lock word, GIO_CNFG's divisor written first
 * if it is zero (S11b), as cserve and the PALcode's message do
 * ------------------------------------------------------------------------ */
#define CMM_RX 5
#define ST_RXREADY 0x100

static void gio_wait(u64 w)
{
    while ((long)rq(w + GIO_DATA) >= 0)
        ;
}

static void gio_write(u64 w, u64 reg, u64 data)
{
    gio_wait(w);
    wq(w + GIO_ADDR, reg << 1);
    wq(w + GIO_DATA, data);
    gio_wait(w);
}

static u64 gio_read(u64 w, u64 reg)
{
    gio_wait(w);
    wq(w + GIO_ADDR, reg << 1 | 1);
    gio_wait(w);
    return rq(w + GIO_DATA) & 0xffffffff;
}

/* This processor's chip: its window, and its lock taken */
static u64 gio_open(u64 *lock)
{
    u64 pid = pal_whami() >> 2, w = csr(pid);
    *lock = KSEG_GIO_LOCK + pid * 0x40;
    gio_take(*lock);
    if (!(rq(w + GIO_CNFG) & 0xfe))
        wq(w + GIO_CNFG, 2);
    return w;
}

static void tx(const u8 *s, u64 n)
{
    u64 lock, w = gio_open(&lock), i;
    for (i = 0; i < n; i++)
        gio_write(w, CMM_TX, s[i]);
    gio_give(lock);
}

/* A character from the terminal, or -1; *more whether another waits */
static long rx(int *more)
{
    u64 lock, w = gio_open(&lock);
    long c = -1;
    if (gio_read(w, CMM_STATUS) & ST_RXREADY) {
        c = (long)(gio_read(w, CMM_RX) & 0xff);
        *more = (gio_read(w, CMM_STATUS) & ST_RXREADY) != 0;
    }
    gio_give(lock);
    return c;
}

static void puts_(const char *s) { tx((const u8 *)s, slen(s)); }

static void put_hex(u64 v)
{
    char b[17];
    int d, n = 0;
    for (d = 60; d > 0 && !((v >> d) & 15); d -= 4)
        ;
    for (; d >= 0; d -= 4) {
        int x = (int)((v >> d) & 15);
        b[n++] = (char)(x < 10 ? '0' + x : 'a' + x - 10);
    }
    b[n] = 0;
    puts_(b);
}

static void put_dec(u64 v)
{
    char b[21];
    int n = 20;
    b[n] = 0;
    do {
        b[--n] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    puts_(b + n);
}

/* ------------------------------------------------------------------------
 * The callbacks [ARM pp. 645-660]: DISPATCH by function code, FIXUP. They
 * run on the caller's processor, stack, mode and IPL, and touch nothing
 * but the terminal and the caller's buffers
 * ------------------------------------------------------------------------ */
#define ST_OK 0
#define ST_MORE (1UL << 61)          /* 001 */
#define ST_NOTREADY (4UL << 61)      /* 100: GETC, nothing yet */
#define ST_UNKNOWN (6UL << 61)       /* 110: GET_ENV, not recognized */

/* GETC [ARM p. 650]: unit 0, the only terminal; R0<39:32> the unit */
static u64 cb_getc(void)
{
    int more = 0;
    long c = rx(&more);
    if (c < 0)
        return ST_NOTREADY;
    return (more ? ST_MORE : ST_OK) | (u64)c;
}

/* PUTS [ARM p. 652]: every byte, status 000 */
static u64 cb_puts(u64 s, u64 n)
{
    n &= 0xffffffff;
    tx((const u8 *)s, n);
    return n;
}

/* The environment variables the ARM requires of every console [ARM
   pp. 642-644], as this console has them [decision]: AUTO_ACTION from
   auto_action; the boot device and file names empty (the emulator loaded
   the kernel); the OS flags the command line; the rest the ARM's defaults */
static const char *env_value(u64 id)
{
    switch (id) {
    case 0x01: return auto_boot ? "BOOT" : "HALT"; /* AUTO_ACTION */
    case 0x02: return "";                          /* BOOT_DEV */
    case 0x03: return "";                          /* BOOTDEF_DEV */
    case 0x04: return "";                          /* BOOTED_DEV */
    case 0x05: return "";                          /* BOOT_FILE */
    case 0x06: return "";                          /* BOOTED_FILE */
    case 0x07: return cmdline();                   /* BOOT_OSFLAGS */
    case 0x08: return cmdline();                   /* BOOTED_OSFLAGS */
    case 0x09: return "OFF";                       /* BOOT_RESET */
    case 0x0a: return "";                          /* DUMP_DEV */
    case 0x0b: return "ON";                        /* ENABLE_AUDIT */
    case 0x0c: return "";                          /* LICENSE */
    case 0x0d: return "0";                         /* CHAR_SET: ISO Latin-1 */
    case 0x0e: return "36";                        /* LANGUAGE: English */
    case 0x0f: return "0";                         /* TTY_DEV: CTB 0 */
    default: return 0;
    }
}

/* GET_ENV [ARM p. 676]: the value's bytes, no terminator; 001 when cut */
static u64 cb_getenv(u64 id, u64 buf, u64 len)
{
    const char *v = env_value(id);
    u64 n, i;
    if (!v)
        return ST_UNKNOWN;
    n = slen(v);
    len &= 0xffffffff;
    for (i = 0; i < n && i < len; i++)
        ((volatile char *)buf)[i] = v[i];
    return (n > len ? ST_MORE : ST_OK) | i;
}

u64 dispatch_c(u64 code, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
    (void)a1;
    (void)a4;
    (void)a5;
    switch (code) {
    case 0x01:
        return cb_getc();
    case 0x02:
        return cb_puts(a2, a3);
    case 0x22:
        return cb_getenv(a1, a2, a3);
    default:
        /* the other routines are not in this console [decision] */
        return ST_UNKNOWN;
    }
}

/* FIXUP [ARM pp. 680, 687-689]: the console's code runs at KSEG addresses
   and its descriptors carry KSEG entry addresses, which no remapping
   changes, and it reaches the HWRPB through KSEG too: nothing to relocate
   [decision]. It notes the new HWRPB address and succeeds */
u64 fixup_c(u64 new_base, u64 new_hwrpb)
{
    (void)new_base;
    fixups++;
    fixup_hwrpb = new_hwrpb;
    return 0;
}

/* The procedure descriptors: the first quadword the kind, the second the
   entry [ARM p. 687, note] */
static u64 pd_dispatch[2];
static u64 pd_fixup[2];

/* ------------------------------------------------------------------------
 * What the console builds [ARM pp. 617-639, 685-692, 705-721]
 * ------------------------------------------------------------------------ */
static u64 off_ctb, off_crb, off_mem, off_gct, hw_size, nclusters, nio7;

static u64 slot_pa(u64 w) { return HWRPB_PA + HW_SLOTS + w * SLOT_SIZE; }
static u64 region0(u64 pa) { return REGION0_VA + (pa - HWRPB_PA); }
static u64 pte(u64 pa, u64 bits) { return (pa >> 13) << 32 | bits; }
#define PTE_KRW 0x1101 /* KWE, KRE, V: kernel read/write [ARM p. 716] */

static u64 checksum(u64 pa, u64 n)
{
    u64 s = 0, i;
    for (i = 0; i < n; i += 8)
        s += rq(pa + i);
    return s;
}

static u64 ascii8(const char *s)
{
    u64 v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = v << 8 | (u8)s[i];
    return v;
}

/* The per-CPU slot of processor w (PID w >> 2, TPU w & 3) [ARM
   pp. 630-639]: zero when no such TPU; else the flags and the PALcode's
   fields, which the PALcode also writes at a START [S12a], the type 18
   (D3), IEEE and VAX FP, the halt PCBB its own HWPCB */
static void build_slot(u64 w)
{
    u64 s = slot_pa(w);
    if ((w >> 2) >= chips || (w & 3) >= tpus)
        return;
    wq(s + SLOT_FLAGS, 0x1cc); /* PL, PMV, PV, PP, PA */
    wq(s + SLOT_PAL_MEM_LEN, PAL_MEM_LEN);
    wq(s + SLOT_PAL_SCR_LEN, PAL_SCR_LEN);
    wq(s + SLOT_PAL_MEM_PA, PAL_MEM_PA);
    wq(s + SLOT_PAL_SCR_PA, PAL_SCR_PA);
    wq(s + SLOT_PAL_REV, PAL_REVISION);
    wq(s + SLOT_TYPE, CPU_TYPE_EV8);
    /* PE for processor 0, the console's; IEEE and VAX FP for all */
    wq(s + SLOT_VARIATION, w ? 3 : 7);
    /* COMPAQ STD 12 fields, made up as Alphabox's serials are: the
       revision "B1", the serial "EV8EM000" and the WHAMI in two digits */
    wq(s + SLOT_REVISION, ascii8("B1\0\0\0\0\0\0"));
    wq(s + SLOT_SERIAL, ascii8("EV8EM000"));
    wq(s + SLOT_SERIAL + 8, ((u64)'0' + w / 10) | ((u64)'0' + w % 10) << 8);
    wq(s + SLOT_HALT_PCBB, s);
    wq(s + SLOT_PAL_AVAIL, CONSOLE_REVISION);
    wq(s + SLOT_PAL_AVAIL + 16, PAL_REVISION);
}

/* Processor 0's: bootstrap in progress, context valid [ARM pp. 718-721],
   the boot PCB as S12a's list has it: KSP in KSEG (the PALcode takes no DTB
   miss in PALmode, so not Region 1's virtual stack), PTBR the level 1
   table, FEN set [decision], ASN, PCC and UNIQUE zero (Table 27-10) */
static void boot_slot(void)
{
    u64 s = slot_pa(0);
    wq(s + PCB_KSP, KSEG_BOOT_STACK);
    wq(s + PCB_USP, 0);
    wq(s + PCB_PTBR, CON_L1 >> 13);
    wq(s + PCB_PCC_ASN, 0);
    wq(s + PCB_UNIQUE, 0);
    wq(s + PCB_FLAGS, 1);
    wq(s + SLOT_FLAGS, 0x1cc | FLAG_BIP | 1UL << FLAG_CV_BIT);
}

/* The memory descriptors [ARM pp. 705-707]: the console's first 2 MB
   (usage 1), the rest of PID 0's memory, and every other PID's at N18's
   place, each untested (no bitmap) [decision] */
static u64 cluster(u64 c, u64 pa, u64 bytes, u64 usage)
{
    u64 d = HWRPB_PA + off_mem + 24 + c * 56;
    wq(d, pa >> 13);
    wq(d + 8, bytes >> 13);
    wq(d + 48, usage);
    return c + 1;
}

static u64 pid_base(u64 n) { return ((n & 3) | (n & 0x3fc) << 1) << 34; }

static void build_memory(void)
{
    u64 c = 0, p, m = HWRPB_PA + off_mem;
    c = cluster(c, 0, FIRMWARE_TOP, 1);
    c = cluster(c, FIRMWARE_TOP, mem_pid - FIRMWARE_TOP, 0);
    for (p = 1; p < chips; p++)
        c = cluster(c, pid_base(p), mem_pid, 0);
    wq(m + 16, c);
    wq(m, checksum(m + 8, (3 + 7 * c) * 8 - 8));
}

/* The GCT at frut_offset, as gct.c walks it from GCT_NODE_PTR(0): a root
   node, its child the first of one node per IO7, type 0x0E (hose),
   subtype 0x2C (I/O port module), id the PID << 8 (core_marvel.c), linked
   by next; 'GLXY' in each; handles are offsets from the table [decision] */
static void gct_node(u64 off, u64 type, u64 sub, u64 id, u64 parent, u64 next,
                     u64 child)
{
    u64 n = HWRPB_PA + off_gct + off;
    wq(n, type | sub << 8 | (u64)0x80 << 16);
    wq(n + 0x18, id);
    wq(n + 0x40, parent);
    wq(n + 0x48, next);
    wq(n + 0x58, child);
    wq(n + 0x78, (u64)0x59584c47 << 32);
}

static void build_gct(void)
{
    u64 p, k = 0;
    gct_node(0, 1, 1, 0, 0, 0, nio7 ? 0x80 : 0);
    for (p = 0; p < chips && p < 64; p++)
        if (io7_mask >> p & 1) {
            k++;
            gct_node(k * 0x80, 0x0e, 0x2c, p << 8, 0, k < nio7 ? (k + 1) * 0x80 : 0,
                     0);
        }
}

/* The CTB [ARM pp. 689-690]: one, a serial line (type 2); its device-
   specific part zero but for the quadword Linux reads as TERM_TYPE
   (core_marvel.c, CTB + 56), 2 as well, not 3, graphics [inference] */
static void build_ctb(void)
{
    u64 c = HWRPB_PA + off_ctb;
    wq(c, 2);
    wq(c + 24, CTB_SIZE - 32);
    wq(c + 56, 2);
}

/* The CRB [ARM pp. 685-689]: DISPATCH and FIXUP by their descriptors in
   the console's data, one map entry for the console's pages, which region
   0 maps after the HWRPB's */
static void build_crb(void)
{
    u64 c = HWRPB_PA + off_crb;
    u64 dpa = (u64)pd_dispatch - KSEG, fpa = (u64)pd_fixup - KSEG;
    pd_dispatch[0] = 0;
    pd_dispatch[1] = (u64)crb_dispatch;
    pd_fixup[0] = 0;
    pd_fixup[1] = (u64)crb_fixup;
    wq(c, region0(dpa));
    wq(c + 8, dpa);
    wq(c + 16, region0(fpa));
    wq(c + 24, fpa);
    wq(c + 32, 1);
    wq(c + 40, (CONSOLE_TOP - CONSOLE_PA) / PAGE);
    wq(c + 48, region0(CONSOLE_PA));
    wq(c + 56, CONSOLE_PA);
    wq(c + 64, (CONSOLE_TOP - CONSOLE_PA) / PAGE);
}

/* The DSRDB as Linux reads it (setup.c, dsr_struct): SMM 0, an empty LURT,
   the system's name after its length [inference, decision] */
static void build_dsr(void)
{
    u64 d = HWRPB_PA + HW_DSR, n = slen(sysname()), i;
    wq(d, 0);
    wq(d + 8, 0x18);
    wq(d + 16, 0x20);
    wq(d + 0x20, n);
    for (i = 0; i < n && i < 0x38; i++)
        *(volatile u8 *)kva(d + 0x28 + i) = (u8)sysname()[i];
}

/* The bootstrap page tables [ARM pp. 713-717], 43-bit VA, 8 KB pages:
   level 1 maps itself at entry 1023 (Linux's VPTB) and at entry 1 (the
   ARM's, which srm_paging_stop puts back), and region 0 at 0x10000000:
   the HWRPB's pages and after them the console's, to CONSOLE_TOP */
static void build_tables(void)
{
    u64 i, n = (CONSOLE_TOP - HWRPB_PA) / PAGE;
    zero(CON_L1, 3 * PAGE);
    wq(CON_L1 + 8 * 1023, pte(CON_L1, PTE_KRW));
    wq(CON_L1 + 8 * 1, pte(CON_L1, PTE_KRW));
    wq(CON_L1 + 8 * ((REGION0_VA >> 33) & 1023), pte(CON_L2, PTE_KRW));
    wq(CON_L2 + 8 * ((REGION0_VA >> 23) & 1023), pte(CON_L3, PTE_KRW));
    for (i = 0; i < n; i++)
        wq(CON_L3 + 8 * (((REGION0_VA >> 13) + i) & 1023),
           pte(HWRPB_PA + i * PAGE, PTE_KRW));
}

/* The interval timer [decision]: Linux's Marvel code starts none and takes
   the rate from the HWRPB, as the EV7 console leaves it running (marvel.md,
   M4): ITV n with n + 1 = ceil(hz / 2^27), at most 1024 Hz */
static void timers(u64 on)
{
    u64 p;
    for (p = 0; p < chips; p++) {
        wq(csr(p) + R_INTER_TIM, on ? itv : 0);
        /* stopped: the ticks left pending cleared (N22, write 1 to clear),
           so that the prompt's wtint sleeps */
        if (!on)
            wq(csr(p) + R_INT_STAT, 0xf0);
    }
}

static int build_all(void)
{
    u64 h = HWRPB_PA, p, nslots = 4 * chips, n1;
    nio7 = 0;
    for (p = 0; p < 64; p++)
        nio7 += io7_mask >> p & 1;
    nclusters = 2 + (chips - 1);
    off_ctb = HW_SLOTS + nslots * SLOT_SIZE;
    off_crb = off_ctb + CTB_SIZE;
    off_mem = off_crb + 128;
    off_gct = (off_mem + (3 + 7 * nclusters) * 8 + 127) & ~127UL;
    hw_size = off_gct + 0x80 * (1 + nio7);
    if (h + hw_size > CONSOLE_PA)
        return 0;
    zero(h, CONSOLE_PA - HWRPB_PA);
    n1 = (hz + (1UL << 27) - 1) >> 27;
    if (n1 < 1)
        n1 = 1;
    if (n1 > 256)
        n1 = 256;
    itv = n1 - 1;
    wq(h + 0x00, HWRPB_PA);
    wq(h + HWRPB_ID, HWRPB_ID_VALUE);
    wq(h + 0x10, HWRPB_REVISION);
    wq(h + 0x18, hw_size);
    wq(h + 0x20, 0);                       /* the primary: WHAMI 0 */
    wq(h + 0x28, PAGE);
    wq(h + 0x30, 48);                      /* PA bits; no extended VA */
    wq(h + 0x38, 255);                     /* 8-bit ASNs [IDS p. 43] */
    wq(h + 0x40, ascii8("EV8EM000"));
    wq(h + 0x48, ascii8("00\0\0\0\0\0\0"));
    wq(h + 0x50, 39);                      /* ST_DEC_MARVEL */
    wq(h + 0x58, 5);                       /* MPCAP, embedded console */
    wq(h + 0x60, 0);
    wq(h + 0x68, 4096 * hz / (n1 << 17));  /* ticks per second x 4096 */
    wq(h + 0x70, hz);
    wq(h + HWRPB_VPTB, VPTB_LINUX);
    wq(h + 0x88, HW_TBB);
    wq(h + HWRPB_NR_PROC, nslots);
    wq(h + HWRPB_PROC_SIZE, SLOT_SIZE);
    wq(h + HWRPB_PROC_OFF, HW_SLOTS);
    wq(h + 0xa8, 1);
    wq(h + 0xb0, CTB_SIZE);
    wq(h + 0xb8, off_ctb);
    wq(h + 0xc0, off_crb);
    wq(h + 0xc8, off_mem);
    wq(h + 0xd0, 0);                       /* no CONFIG block */
    wq(h + 0xd8, off_gct);
    wq(h + 0x138, HW_DSR);
    for (p = 0; p < nslots; p++)
        build_slot(p);
    boot_slot();
    build_ctb();
    build_crb();
    build_memory();
    build_gct();
    build_dsr();
    wq(h + 0x120, checksum(h, 0x120));
    build_tables();
    if (entry) {
        /* the zero page (setup.h): COMMAND_LINE, INITRD_START (KSEG, as
           setup_memory takes it: virt_to_phys), INITRD_SIZE */
        u64 i;
        const char *c = cmdline();
        zero(ZERO_PAGE, PAGE);
        for (i = 0; c[i] && i < 255; i++)
            *(volatile u8 *)kva(ZERO_PAGE + i) = (u8)c[i];
        wq(ZERO_PAGE + 0x100, rd_pa ? KSEG | rd_pa : 0);
        wq(ZERO_PAGE + 0x108, rd_pa ? rd_size : 0);
    }
    return 1;
}

/* ------------------------------------------------------------------------
 * The prompt
 * ------------------------------------------------------------------------ */
static void summary(void)
{
    u64 p;
    puts_("%CON8-I-MACHINE, ");
    puts_(sysname());
    puts_(": ");
    put_dec(chips);
    puts_(chips == 1 ? " chip of " : " chips of ");
    put_dec(tpus);
    puts_(" TPUs, ");
    put_dec(hz / 1000000);
    puts_(" MHz\r\n");
    for (p = 0; p < chips; p++) {
        puts_("%CON8-I-MEMORY, PID ");
        put_dec(p);
        puts_(": ");
        put_dec(mem_pid >> 20);
        puts_(" MB at ");
        put_hex(pid_base(p));
        puts_("\r\n");
    }
    for (p = 0; p < chips && p < 64; p++)
        if (io7_mask >> p & 1) {
            puts_("%CON8-I-IO7, PID ");
            put_dec(p);
            puts_("\r\n");
        }
    puts_("%CON8-I-HWRPB, at ");
    put_hex(HWRPB_PA);
    puts_(", ");
    put_dec(4 * chips);
    puts_(" slots, ");
    put_hex(hw_size);
    puts_(" bytes, the interval timer at ");
    put_dec(hz / ((itv + 1) << 17));
    puts_(" Hz\r\n");
    if (entry) {
        puts_("%CON8-I-KERNEL, entry ");
        put_hex(entry);
        puts_(", ");
        put_hex(k_low);
        puts_(" to ");
        put_hex(k_high);
        puts_("\r\n");
        if (rd_pa) {
            puts_("%CON8-I-INITRD, ");
            put_hex(rd_pa);
            puts_(", ");
            put_dec(rd_size);
            puts_(" bytes\r\n");
        }
        puts_("%CON8-I-CMDLINE, \"");
        puts_(cmdline());
        puts_("\"\r\n");
    }
}

static void boot(void)
{
    if (!entry) {
        puts_("%CON8-W-NOKERNEL, no kernel: the system block's kernel names "
              "one\r\n");
        return;
    }
    if (started) {
        puts_("%CON8-W-BOOTED, the kernel has run: reset the machine to load "
              "it again\r\n");
        return;
    }
    started = 1;
    puts_("%CON8-I-BOOT, entering the kernel at ");
    put_hex(entry);
    puts_("\r\n");
    timers(1);
    pal_wrmces(0xf);      /* MCES 8 [ARM p. 720] */
    pal_tbia();           /* the TB invalidated [ARM p. 720] */
    enter_kernel(slot_pa(0), entry, VPTB_LINUX);
}

static const char *reason_name(u64 r)
{
    switch (r) {
    case 1: return "operator";
    case 2: return "kernel stack not valid";
    case 5: return "CALL_PAL HALT";
    default: return r >= HALT_UNIMPL ? "the PALcode" : "other";
    }
}

/* A halt brought processor 0 back [ARM pp. 634-638]: what its slot says */
static void halted(u64 reason)
{
    u64 s = slot_pa(0);
    puts_("\r\n%CON8-I-HALT, CPU 0, reason ");
    put_hex(reason);
    puts_(" (");
    puts_(reason_name(reason));
    puts_("), PC ");
    put_hex(rq(s + SLOT_HALT_PC));
    puts_(", PS ");
    put_hex(rq(s + SLOT_HALT_PS));
    puts_(", R25 ");
    put_hex(rq(s + SLOT_HALT_ARG));
    puts_(", R26 ");
    put_hex(rq(s + SLOT_HALT_RA));
    puts_(", R27 ");
    put_hex(rq(s + SLOT_HALT_PV));
    puts_(", requested ");
    put_dec(rq(s + SLOT_FLAGS) >> 16 & 0xff);
    puts_("\r\n");
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static char linebuf[80];

/* A line from the terminal, echoed; a wtint between polls, the timer off,
   so that an idle prompt sleeps (N26) */
static void get_line(void)
{
    u64 n = 0;
    for (;;) {
        int more;
        long c = rx(&more);
        u8 e[3];
        if (c < 0) {
            pal_wtint();
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (!n && c == '\n')
                continue;
            puts_("\r\n");
            linebuf[n] = 0;
            return;
        }
        if ((c == 8 || c == 127) && n) {
            n--;
            e[0] = 8, e[1] = ' ', e[2] = 8;
            tx(e, 3);
            continue;
        }
        if (c >= ' ' && c < 127 && n < sizeof(linebuf) - 1) {
            linebuf[n++] = (char)c;
            e[0] = (u8)c;
            tx(e, 1);
        }
    }
}

static void prompt(void)
{
    timers(0);
    for (;;) {
        char *w;
        puts_("P00>>>");
        get_line();
        w = linebuf;
        while (*w == ' ')
            w++;
        if (!*w)
            continue;
        if (same(w, "boot"))
            boot();
        else if (same(w, "show"))
            summary();
        else if (same(w, "help"))
            puts_("%CON8-I-HELP, boot: enter the kernel; show: what the "
                  "console built; help\r\n");
        else
            puts_("%CON8-W-WHAT, boot, show or help\r\n");
    }
}

void cmain(u64 reason, u64 whami)
{
    u64 b = BOOT_BLOCK;
    (void)whami;
    if (built) {
        halted(reason);
        prompt();
    }
    puts_("\r\nAlphabox EV8 console 1.0 (S12b), PALcode ");
    put_hex(PAL_REVISION);
    puts_("\r\n");
    if (rq(b + BB_MAGIC) != BB_MAGIC_VALUE || !rq(b + BB_FILLED)) {
        puts_("%CON8-F-NOBLOCK, the emulator filled no boot block\r\n");
        built = 1;
        prompt();
    }
    hz = rq(b + BB_CYCLE_HZ);
    chips = rq(b + BB_CHIPS);
    tpus = rq(b + BB_TPUS);
    mem_pid = rq(b + BB_MEM_PER_PID);
    io7_mask = rq(b + BB_IO7_MASK);
    entry = rq(b + BB_ENTRY);
    k_low = rq(b + BB_KERNEL_LOW);
    k_high = rq(b + BB_KERNEL_HIGH);
    rd_pa = rq(b + BB_INITRD_PA);
    rd_size = rq(b + BB_INITRD_SIZE);
    auto_boot = rq(b + BB_FLAGS) & 1;
    built = 1;
    /* The PALcode's impure areas and the HWRPB's slots hold 64 processor
       numbers (MAX_WHAMI), WHAMI = PID x 4 + TPU (N25): 16 chips [decision,
       the owner 2026-10-31: 16 chips], as many as the GS1280 has; a machine
       the emulator could make bigger is refused */
    if (4 * chips > MAX_WHAMI || !chips || !tpus || tpus > 4) {
        puts_("%CON8-F-TOOMANY, ");
        put_dec(chips);
        puts_(" chips: the PALcode keeps 64 processor numbers, PID x 4 + "
              "TPU, 16 chips at most\r\n");
        entry = 0;
        prompt();
    }
    if (!build_all()) {
        puts_("%CON8-F-HWRPB, the HWRPB does not fit below the console\r\n");
        entry = 0;
        prompt();
    }
    summary();
    if (entry && auto_boot)
        boot();
    prompt();
}
