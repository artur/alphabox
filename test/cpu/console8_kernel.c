/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. See LICENSE at
 * the top of the repository.
 */

/*
 * A test "kernel" for the EV8 console (firmware/ev8/console.c, S12b;
 * docs/platforms/ev8-native.md, section 4). test/cpu/console8_test.py
 * builds it with nada as an ELF linked where Linux's vmlinux is
 * (0xfffffc0001010000, KERNEL_START_PHYS + 0x10000, setup.h), loads it
 * through the system block's kernel, initrd and cmdline keys, and checks
 * what it prints. It reads what the console built the way Linux reads it
 * (setup.c's setup_arch and setup_memory, smp.c's setup_smp and
 * secondary_cpu_start, gct.c, core_marvel.c, srmcons.c, mm/init.c's
 * callback_init, switch_to_system_map and srm_paging_stop), with the field
 * layouts of Linux's hwrpb.h, gct.h and setup.h, and prints every value as
 * "@ name hex" through the console's own PUTS. Written before the console.
 *
 * Nothing here decides what is right: console8_test.py compares the values
 * with the ARM's and Linux's requirements.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

#define KSEG 0xfffffc0000000000
#define INIT_HWRPB 0x10000000          /* Linux hwrpb.h */
#define VMALLOC_BASE 0xfffffffc00000000 /* -2 x PGDIR_SIZE: level 1 entry 1022 */
#define VPTB_LINUX 0xfffffffe00000000
#define VPTB_CONSOLE 0x200000000       /* srm_paging_stop's */
#define ZERO_PGE (KSEG + 0x100a000)    /* setup.h: PARAM */

/* This kernel's memory (32 MB up, an OS cluster's): page tables, PCBs, the
   per-processor records and stacks */
#define TK 0x2000000
#define TK_L1 (TK + 0x0000)
#define TK_L2V (TK + 0x2000)
#define TK_L3V (TK + 0x4000)
#define TK_PCB0 (TK + 0x6000)
#define TK_ORIG (TK + 0x6100)
#define TK_BAD (TK + 0x6200)
#define TK_IPCB (TK + 0x7000) /* + w x 0x80 */
#define TK_REC (TK + 0x8000)  /* + w x 0x100 */
#define TK_STACK (TK + 0x10000) /* + w x 0x4000 */
#define MAXW 32

/* hwrpb_struct (hwrpb.h) */
#define H_PHYS 0
#define H_ID 8
#define H_REVISION 16
#define H_SIZE 24
#define H_CPUID 32
#define H_PAGESIZE 40
#define H_PA_BITS 48
#define H_MAX_ASN 56
#define H_SSN 64
#define H_SYS_TYPE 80
#define H_SYS_VARIATION 88
#define H_SYS_REVISION 96
#define H_INTR_FREQ 104
#define H_CYCLE_FREQ 112
#define H_VPTB 120
#define H_RES1 128
#define H_TBHB 136
#define H_NR_PROC 144
#define H_PROC_SIZE 152
#define H_PROC_OFF 160
#define H_CTB_NR 168
#define H_CTB_SIZE 176
#define H_CTBT_OFF 184
#define H_CRB_OFF 192
#define H_MDDT_OFF 200
#define H_CDB_OFF 208
#define H_FRUT_OFF 216
#define H_RESTART 256
#define H_RESTART_DATA 264
#define H_CHKSUM 288
#define H_RXRDY 296
#define H_TXRDY 304
#define H_DSR_OFF 312

/* percpu_struct (hwrpb.h) */
#define S_FLAGS 128
#define S_PAL_MEM_SIZE 136
#define S_PAL_SCR_SIZE 144
#define S_PAL_MEM_PA 152
#define S_PAL_SCR_PA 160
#define S_PAL_REV 168
#define S_TYPE 176
#define S_VARIATION 184
#define S_REVISION 192
#define S_SERIAL 200
#define S_LOGOUT_PA 216
#define S_LOGOUT_LEN 224
#define S_HALT_PCBB 232
#define S_HALT_PC 240
#define S_HALT_PS 248
#define S_HALT_ARG 256
#define S_HALT_RA 264
#define S_HALT_PV 272
#define S_HALT_REASON 280
#define S_IPC 296
#define S_PAL_AVAIL 464
#define S_COMPAT 592
#define S_BCACHE 616

/* The per-processor records this kernel keeps */
#define R_DONE 0
#define R_R27 8
#define R_SP 16
#define R_PS 24
#define R_WHAMI 32
#define R_TICKS 40
#define R_TYPE 48
#define R_T1 56
#define R_T2 64
#define R_OTHER 72
#define R_ENTRY_SP 80
#define R_FLAGS 88

static u64 hw;          /* the HWRPB, KSEG */
static u64 regs_bad;    /* registers a callback changed, all calls */
static u64 calls;
static u64 own_map;     /* the switch to this kernel's page tables is done */
static u64 cyc_hz;
u64 regs_after[32];
u64 regs_sp_before;

/* The asm entry points (the text below) */
u64 call_pd(u64 pd, u64 entry, u64 a0, u64 a1, u64 a2, u64 a3);
u64 get_sp(void);
u64 pal_whami(void);
u64 pal_rdps(void);
u64 pal_rdmces(void);
u64 pal_swpipl(u64 ipl);
void pal_wrent(u64 entry, u64 which);
void pal_wrvptptr(u64 vptb);
void pal_tbia(void);
void pal_halt(void);
u64 pal_swpctx_keep(u64 pcb_pa);
u64 rpcc32(void);
void do_mb(void);
void set_bit_atomic(u64 bit, u64 va);
void ent_int(void);
void ent_arith(void);
void ent_mm(void);
void ent_if(void);
void ent_una(void);
void ent_sys(void);
void sec_start(void);
void kmain(u64 entry_sp);
void sec_main(u64 r27);
void int_c(u64 type, u64 vector, u64 param);

static void text(void)
{
    asm(
        /* Entered by the console at the ELF's entry, kernel mode, IPL 7,
           the SP the boot PCB's KSP */
        ".global _start\n"
        "_start:\n"
        "    bis $30, $30, $16\n"
        "    li $30, 0xfffffc0002013fc0\n"   /* KSEG | TK_STACK + 0x3fc0 */
        "    bsr $26, kmain\n"
        "    call_pal 0\n"

        /* call_pd(pd, entry, a0..a3): a console routine through its
           procedure descriptor as Linux's callback_srm.S calls it: $27 the
           descriptor, $25 the argument information, R16 up the arguments.
           $2-$15 and $29 hold patterns across the call; what they hold after
           it goes to regs_after (the ARM: a callback alters no register but
           R0 [p. 645]; the VMS standard keeps R2-R15, R29, SP) */
        ".global call_pd\n"
        "call_pd:\n"
        "    lda $30, -96($30)\n"
        "    stq $26, 0($30)\n"
        "    stq $9, 8($30)\n"
        "    stq $10, 16($30)\n"
        "    stq $11, 24($30)\n"
        "    stq $12, 32($30)\n"
        "    stq $13, 40($30)\n"
        "    stq $14, 48($30)\n"
        "    stq $15, 56($30)\n"
        "    stq $29, 64($30)\n"
        "    la $1, regs_sp_before\n"
        "    stq $30, 0($1)\n"
        "    bis $16, $16, $27\n"
        "    bis $17, $17, $28\n"
        "    bis $18, $18, $16\n"
        "    bis $19, $19, $17\n"
        "    bis $20, $20, $18\n"
        "    bis $21, $21, $19\n"
        "    lda $25, 4($31)\n"
        "    lda $2, 0x102($31)\n"
        "    lda $3, 0x103($31)\n"
        "    lda $4, 0x104($31)\n"
        "    lda $5, 0x105($31)\n"
        "    lda $6, 0x106($31)\n"
        "    lda $7, 0x107($31)\n"
        "    lda $8, 0x108($31)\n"
        "    lda $9, 0x109($31)\n"
        "    lda $10, 0x10a($31)\n"
        "    lda $11, 0x10b($31)\n"
        "    lda $12, 0x10c($31)\n"
        "    lda $13, 0x10d($31)\n"
        "    lda $14, 0x10e($31)\n"
        "    lda $15, 0x10f($31)\n"
        "    lda $29, 0x11d($31)\n"
        "    jsr $26, ($28)\n"
        "    la $1, regs_after\n"
        "    stq $2, 16($1)\n"
        "    stq $3, 24($1)\n"
        "    stq $4, 32($1)\n"
        "    stq $5, 40($1)\n"
        "    stq $6, 48($1)\n"
        "    stq $7, 56($1)\n"
        "    stq $8, 64($1)\n"
        "    stq $9, 72($1)\n"
        "    stq $10, 80($1)\n"
        "    stq $11, 88($1)\n"
        "    stq $12, 96($1)\n"
        "    stq $13, 104($1)\n"
        "    stq $14, 112($1)\n"
        "    stq $15, 120($1)\n"
        "    stq $29, 232($1)\n"
        "    stq $30, 240($1)\n"
        "    ldq $26, 0($30)\n"
        "    ldq $9, 8($30)\n"
        "    ldq $10, 16($30)\n"
        "    ldq $11, 24($30)\n"
        "    ldq $12, 32($30)\n"
        "    ldq $13, 40($30)\n"
        "    ldq $14, 48($30)\n"
        "    ldq $15, 56($30)\n"
        "    ldq $29, 64($30)\n"
        "    lda $30, 96($30)\n"
        "    ret\n"

        ".global get_sp\n"
        "get_sp:\n"
        "    bis $30, $30, $0\n"
        "    ret\n"
        ".global pal_whami\n"
        "pal_whami:\n"
        "    call_pal 0x3c\n"
        "    ret\n"
        ".global pal_rdmces\n"
        "pal_rdmces:\n"
        "    call_pal 0x10\n"
        "    ret\n"
        ".global pal_rdps\n"
        "pal_rdps:\n"
        "    call_pal 0x36\n"
        "    ret\n"
        ".global pal_swpipl\n"
        "pal_swpipl:\n"
        "    call_pal 0x35\n"
        "    ret\n"
        ".global pal_wrent\n"
        "pal_wrent:\n"
        "    call_pal 0x34\n"
        "    ret\n"
        ".global pal_wrvptptr\n"
        "pal_wrvptptr:\n"
        "    call_pal 0x2d\n"
        "    ret\n"
        ".global pal_tbia\n"
        "pal_tbia:\n"
        "    lda $16, -2($31)\n"
        "    call_pal 0x33\n"
        "    ret\n"
        ".global pal_halt\n"
        "pal_halt:\n"
        "    call_pal 0\n"
        "    ret\n"
        /* Linux's load_PCB: the current SP into the new PCB's KSP, then
           swpctx (its PA), so the stack goes on */
        ".global pal_swpctx_keep\n"
        "pal_swpctx_keep:\n"
        "    li $1, 0xfffffc0000000000\n"
        "    bis $16, $1, $1\n"
        "    stq $30, 0($1)\n"
        "    call_pal 0x30\n"
        "    ret\n"
        ".global rpcc32\n"
        "rpcc32:\n"
        "    rpcc $0\n"
        "    zapnot $0, 15, $0\n"
        "    ret\n"
        ".global do_mb\n"
        "do_mb:\n"
        "    mb\n"
        "    ret\n"
        /* smp.c's set_bit(cpuid, &hwrpb->rxrdy) */
        ".global set_bit_atomic\n"
        "set_bit_atomic:\n"
        "    lda $1, 1($31)\n"
        "    sll $1, $16, $1\n"
        "sb_again:\n"
        "    ldq_l $2, 0($17)\n"
        "    bis $2, $1, $2\n"
        "    stq_c $2, 0($17)\n"
        "    beq $2, sb_again\n"
        "    mb\n"
        "    ret\n"

        /* entInt: the PALcode's frame is on the stack; keep what the frame
           does not, call int_c(a0, a1, a2), rti */
        ".global ent_int\n"
        "ent_int:\n"
        "    lda $30, -160($30)\n"
        "    stq $0, 0($30)\n"
        "    stq $1, 8($30)\n"
        "    stq $2, 16($30)\n"
        "    stq $3, 24($30)\n"
        "    stq $4, 32($30)\n"
        "    stq $5, 40($30)\n"
        "    stq $6, 48($30)\n"
        "    stq $7, 56($30)\n"
        "    stq $8, 64($30)\n"
        "    stq $19, 72($30)\n"
        "    stq $20, 80($30)\n"
        "    stq $21, 88($30)\n"
        "    stq $22, 96($30)\n"
        "    stq $23, 104($30)\n"
        "    stq $24, 112($30)\n"
        "    stq $25, 120($30)\n"
        "    stq $26, 128($30)\n"
        "    stq $27, 136($30)\n"
        "    stq $28, 144($30)\n"
        "    bsr $26, int_c\n"
        "    ldq $0, 0($30)\n"
        "    ldq $1, 8($30)\n"
        "    ldq $2, 16($30)\n"
        "    ldq $3, 24($30)\n"
        "    ldq $4, 32($30)\n"
        "    ldq $5, 40($30)\n"
        "    ldq $6, 48($30)\n"
        "    ldq $7, 56($30)\n"
        "    ldq $8, 64($30)\n"
        "    ldq $19, 72($30)\n"
        "    ldq $20, 80($30)\n"
        "    ldq $21, 88($30)\n"
        "    ldq $22, 96($30)\n"
        "    ldq $23, 104($30)\n"
        "    ldq $24, 112($30)\n"
        "    ldq $25, 120($30)\n"
        "    ldq $26, 128($30)\n"
        "    ldq $27, 136($30)\n"
        "    ldq $28, 144($30)\n"
        "    lda $30, 160($30)\n"
        "    call_pal 0x3f\n"

        /* Any other entry: an unexpected fault, inside the console or here.
           R27 = 0xba0 | the entry (1 arith, 2 mm, 3 if, 4 una, 5 sys), R26
           = a0, R25 = the frame's PC: the halt records them in the slot,
           the console prints them */
        ".global ent_arith\n"
        "ent_arith:\n"
        "    lda $27, 0xba1($31)\n"
        "    br $31, ent_bad\n"
        ".global ent_mm\n"
        "ent_mm:\n"
        "    lda $27, 0xba2($31)\n"
        "    br $31, ent_bad\n"
        ".global ent_if\n"
        "ent_if:\n"
        "    lda $27, 0xba3($31)\n"
        "    br $31, ent_bad\n"
        ".global ent_una\n"
        "ent_una:\n"
        "    lda $27, 0xba4($31)\n"
        "    br $31, ent_bad\n"
        ".global ent_sys\n"
        "ent_sys:\n"
        "    lda $27, 0xba5($31)\n"
        "    br $31, ent_bad\n"
        "ent_bad:\n"
        "    zapnot $27, 3, $27\n"
        "    bis $16, $16, $26\n"
        "    ldq $25, 8($30)\n"
        "    call_pal 0\n"
        "    br $31, ent_bad\n"

        /* A secondary's CPU_restart, as Linux's __smp_callin: the HWPCB of
           its slot loaded, UNIQUE the PA of the PCB to swap to */
        ".global sec_start\n"
        "sec_start:\n"
        "    bis $27, $27, $9\n"
        "    call_pal 0x9e\n"
        "    bis $0, $0, $16\n"
        "    call_pal 0x30\n"
        "    bis $9, $9, $16\n"
        "    bsr $26, sec_main\n"
        "    call_pal 0\n");
}

static u64 rq(u64 va) { return *(volatile u64 *)va; }
static void wq(u64 va, u64 v) { *(volatile u64 *)va = v; }
static u32 rl(u64 va) { return *(volatile u32 *)va; }
static u8 rb(u64 va) { return *(volatile u8 *)va; }
static u64 kseg(u64 pa) { return KSEG | pa; }

/* ------------------------------------------------------------------------
 * The console's callbacks, through the CRB (Linux's callback_srm.S)
 * ------------------------------------------------------------------------ */
static u64 crb(void) { return hw + rq(hw + H_CRB_OFF); }

static void note_regs(void)
{
    int n;
    calls++;
    for (n = 2; n <= 15; n++)
        if (regs_after[n] != 0x100 + (u64)n)
            regs_bad |= 1UL << n;
    if (regs_after[29] != 0x11d)
        regs_bad |= 1UL << 29;
    if (regs_after[30] != regs_sp_before)
        regs_bad |= 1UL << 30;
}

/* Until the region 0 checks are printed, the descriptor through its PA
   (KSEG), so that a wrong region 0 is reported rather than jumped through;
   then through its VA, as Linux calls it */
static u64 by_pa = 1;

static u64 callback(u64 code, u64 a1, u64 a2, u64 a3)
{
    u64 pd = by_pa ? kseg(rq(crb() + 8)) : rq(crb() + 0);
    u64 r = call_pd(pd, rq(pd + 8), code, a1, a2, a3);
    note_regs();
    return r;
}

static u64 cb_fixup(u64 new_base, u64 new_hwrpb)
{
    u64 pd = rq(crb() + 16);
    u64 r = call_pd(pd, rq(pd + 8), new_base, new_hwrpb, 0, 0);
    note_regs();
    return r;
}

static void puts_all(const char *s, u64 n)
{
    while (n) {
        u64 r = callback(2, 0, (u64)s, n);
        u64 done = r & 0xffffffff;
        if (done > n)
            done = n;
        s += done;
        n -= done;
    }
}

static u64 slen(const char *s)
{
    u64 n = 0;
    while (s[n])
        n++;
    return n;
}

static void put(const char *s) { puts_all(s, slen(s)); }

static char line[400];

/* "@ name value\r\n", the value in hex */
static void rep(const char *name, u64 v)
{
    u64 n = 0, i;
    int d;
    line[n++] = '@';
    line[n++] = ' ';
    for (i = 0; name[i] && n < 300; i++)
        line[n++] = name[i];
    line[n++] = ' ';
    for (d = 60; d > 0 && !((v >> d) & 15); d -= 4)
        ;
    for (; d >= 0; d -= 4) {
        int x = (int)((v >> d) & 15);
        line[n++] = (char)(x < 10 ? '0' + x : 'a' + x - 10);
    }
    line[n++] = '\r';
    line[n++] = '\n';
    puts_all(line, n);
}

/* "@ name.i value": a name with an index */
static char nbuf[80];
static const char *nm(const char *a, u64 i, const char *b)
{
    u64 n = 0, k;
    int d, started = 0;
    for (k = 0; a[k]; k++)
        nbuf[n++] = a[k];
    for (d = 60; d >= 0; d -= 4) {
        int x = (int)((i >> d) & 15);
        if (x || started || d == 0) {
            nbuf[n++] = (char)(x < 10 ? '0' + x : 'a' + x - 10);
            started = 1;
        }
    }
    for (k = 0; b[k]; k++)
        nbuf[n++] = b[k];
    nbuf[n] = 0;
    return nbuf;
}

/* "@ name text\r\n": printable bytes as they are, others as '.' */
static void rep_text(const char *name, u64 va, u64 len)
{
    u64 n = 0, i;
    line[n++] = '@';
    line[n++] = ' ';
    for (i = 0; name[i]; i++)
        line[n++] = name[i];
    line[n++] = ' ';
    line[n++] = '[';
    for (i = 0; i < len && n < 390; i++) {
        u8 c = rb(va + i);
        line[n++] = (char)(c >= 32 && c < 127 ? c : '.');
    }
    line[n++] = ']';
    line[n++] = '\r';
    line[n++] = '\n';
    puts_all(line, n);
}

/* Whether fewer than `cycles` have passed since `start` (the counter's low
   32 bits, so at most 0xf0000000 of them) */
static int wait_cycles(u64 start, u64 cycles)
{
    if (cycles > 0xf0000000)
        cycles = 0xf0000000;
    return ((rpcc32() - start) & 0xffffffff) < cycles;
}

static char cbuf[80];
static const char *cat(const char *a, const char *b)
{
    u64 n = 0, k;
    for (k = 0; a[k]; k++)
        cbuf[n++] = a[k];
    for (k = 0; b[k]; k++)
        cbuf[n++] = b[k];
    cbuf[n] = 0;
    return cbuf;
}

/* ------------------------------------------------------------------------
 * What setup_arch reads
 * ------------------------------------------------------------------------ */
static void hwrpb_fields(void)
{
    u64 i, sum = 0, size, bad = 0;
    rep("hw.phys", rq(hw + H_PHYS));
    rep("hw.id", rq(hw + H_ID));
    rep("hw.revision", rq(hw + H_REVISION));
    rep("hw.size", rq(hw + H_SIZE));
    rep("hw.cpuid", rq(hw + H_CPUID));
    rep("hw.pagesize", rq(hw + H_PAGESIZE));
    rep("hw.pa_bits", rl(hw + H_PA_BITS));
    rep("hw.ext_va", rl(hw + H_PA_BITS + 4));
    rep("hw.max_asn", rq(hw + H_MAX_ASN));
    rep("hw.ssn0", rq(hw + H_SSN));
    rep("hw.ssn1", rq(hw + H_SSN + 8));
    rep_text("hw.ssn", hw + H_SSN, 16);
    rep("hw.sys_type", rq(hw + H_SYS_TYPE));
    rep("hw.sys_variation", rq(hw + H_SYS_VARIATION));
    rep("hw.sys_revision", rq(hw + H_SYS_REVISION));
    rep("hw.intr_freq", rq(hw + H_INTR_FREQ));
    rep("hw.cycle_freq", rq(hw + H_CYCLE_FREQ));
    rep("hw.vptb", rq(hw + H_VPTB));
    rep("hw.res1", rq(hw + H_RES1));
    rep("hw.tbhb_offset", rq(hw + H_TBHB));
    rep("hw.nr_processors", rq(hw + H_NR_PROC));
    rep("hw.processor_size", rq(hw + H_PROC_SIZE));
    rep("hw.processor_offset", rq(hw + H_PROC_OFF));
    rep("hw.ctb_nr", rq(hw + H_CTB_NR));
    rep("hw.ctb_size", rq(hw + H_CTB_SIZE));
    rep("hw.ctbt_offset", rq(hw + H_CTBT_OFF));
    rep("hw.crb_offset", rq(hw + H_CRB_OFF));
    rep("hw.mddt_offset", rq(hw + H_MDDT_OFF));
    rep("hw.cdb_offset", rq(hw + H_CDB_OFF));
    rep("hw.frut_offset", rq(hw + H_FRUT_OFF));
    for (i = 224; i <= 280; i += 8)
        rep(nm("hw.q", i, ""), rq(hw + i));
    rep("hw.chksum", rq(hw + H_CHKSUM));
    for (i = 0; i <= 280; i += 8)
        sum += rq(hw + i);
    rep("hw.chksum_calc", sum);
    rep("hw.rxrdy", rq(hw + H_RXRDY));
    rep("hw.txrdy", rq(hw + H_TXRDY));
    rep("hw.dsr_offset", rq(hw + H_DSR_OFF));
    /* The HWRPB and its required blocks are virtually contiguous from
       0x10000000 in the bootstrap address space [ARM p. 618]: the same
       bytes through it as through KSEG */
    size = rq(hw + H_SIZE);
    for (i = 0; i + 8 <= size && i < 0x10000; i += 8)
        if (rq(INIT_HWRPB + i) != rq(hw + i))
            bad++;
    rep("region0.mismatch", bad);
    /* The TB hint block, eight quadwords [ARM p. 630] */
    for (i = 0; i < 64; i += 8)
        rep(nm("tbb.q", i, ""), rq(hw + rq(hw + H_TBHB) + i));
    /* setup.c's platform_string: the DSR's system name */
    {
        u64 dsr = hw + rq(hw + H_DSR_OFF);
        u64 name = dsr + rq(dsr + 16);
        rep("dsr.smm", rq(dsr));
        rep("dsr.lurt_off", rq(dsr + 8));
        rep("dsr.sysname_off", rq(dsr + 16));
        rep("dsr.sysname_len", rq(name));
        rep_text("dsr.sysname", name + 8, rq(name));
    }
}

/* setup_smp and the per-CPU slots [ARM pp. 630-639] */
static void slots(void)
{
    u64 n = rq(hw + H_NR_PROC), w, probed = 0;
    for (w = 0; w < n && w < 64; w++) {
        u64 s = hw + rq(hw + H_PROC_OFF) + w * rq(hw + H_PROC_SIZE);
        u64 i, nz = 0;
        if ((rq(s + S_FLAGS) & 0x1cc) == 0x1cc)
            probed++;
        for (i = 0; i < rq(hw + H_PROC_SIZE); i += 8)
            if (rq(s + i))
                nz++;
        rep(nm("slot.", w, ".nonzero"), nz);
        if (!nz)
            continue;
        rep(nm("slot.", w, ".pa"), s - KSEG);
        rep(nm("slot.", w, ".flags"), rq(s + S_FLAGS));
        rep(nm("slot.", w, ".pal_mem_size"), rq(s + S_PAL_MEM_SIZE));
        rep(nm("slot.", w, ".pal_scr_size"), rq(s + S_PAL_SCR_SIZE));
        rep(nm("slot.", w, ".pal_mem_pa"), rq(s + S_PAL_MEM_PA));
        rep(nm("slot.", w, ".pal_scr_pa"), rq(s + S_PAL_SCR_PA));
        rep(nm("slot.", w, ".pal_rev"), rq(s + S_PAL_REV));
        rep(nm("slot.", w, ".type"), rq(s + S_TYPE));
        rep(nm("slot.", w, ".variation"), rq(s + S_VARIATION));
        rep(nm("slot.", w, ".revision"), rq(s + S_REVISION));
        rep(nm("slot.", w, ".serial0"), rq(s + S_SERIAL));
        rep(nm("slot.", w, ".serial1"), rq(s + S_SERIAL + 8));
        rep(nm("slot.", w, ".logout_pa"), rq(s + S_LOGOUT_PA));
        rep(nm("slot.", w, ".logout_len"), rq(s + S_LOGOUT_LEN));
        rep(nm("slot.", w, ".halt_pcbb"), rq(s + S_HALT_PCBB));
        rep(nm("slot.", w, ".halt_pc"), rq(s + S_HALT_PC));
        rep(nm("slot.", w, ".halt_ps"), rq(s + S_HALT_PS));
        rep(nm("slot.", w, ".halt_arg"), rq(s + S_HALT_ARG));
        rep(nm("slot.", w, ".halt_ra"), rq(s + S_HALT_RA));
        rep(nm("slot.", w, ".halt_pv"), rq(s + S_HALT_PV));
        rep(nm("slot.", w, ".halt_reason"), rq(s + S_HALT_REASON));
        rep(nm("slot.", w, ".ipc0"), rq(s + S_IPC));
        rep(nm("slot.", w, ".pal_avail0"), rq(s + S_PAL_AVAIL));
        rep(nm("slot.", w, ".pal_avail2"), rq(s + S_PAL_AVAIL + 16));
        rep(nm("slot.", w, ".compat"), rq(s + S_COMPAT));
        rep(nm("slot.", w, ".bcache"), rq(s + S_BCACHE));
        for (i = 0; i < 6; i++)
            rep(nm("slot.", w, i == 0 ? ".pcb.ksp" : i == 1 ? ".pcb.usp" :
                   i == 2 ? ".pcb.ptbr" : i == 3 ? ".pcb.pcc_asn" :
                   i == 4 ? ".pcb.unique" : ".pcb.flags"), rq(s + 8 * i));
    }
    rep("smp.probed", probed);
}

/* setup_memory: the memory descriptors [ARM pp. 706-707] */
static void memory(void)
{
    u64 md = hw + rq(hw + H_MDDT_OFF), n = rq(md + 16), i, sum = 0;
    rep("mem.chksum", rq(md));
    rep("mem.optional_pa", rq(md + 8));
    rep("mem.numclusters", n);
    for (i = 8; i < (3 + 7 * n) * 8; i += 8)
        sum += rq(md + i);
    rep("mem.chksum_calc", sum);
    for (i = 0; i < n && i < 40; i++) {
        u64 c = md + 24 + 56 * i;
        rep(nm("mem.", i, ".start_pfn"), rq(c));
        rep(nm("mem.", i, ".numpages"), rq(c + 8));
        rep(nm("mem.", i, ".numtested"), rq(c + 16));
        rep(nm("mem.", i, ".bitmap_va"), rq(c + 24));
        rep(nm("mem.", i, ".bitmap_pa"), rq(c + 32));
        rep(nm("mem.", i, ".bitmap_chksum"), rq(c + 40));
        rep(nm("mem.", i, ".usage"), rq(c + 48));
    }
}

/* gct.c's gct6_find_nodes from GCT_NODE_PTR(0): the magic, siblings first,
   then children; core_marvel.c's marvel_io7_present for the hoses */
static u64 gct_visits;
static void gct_walk(u64 off, u64 depth)
{
    u64 node = hw + rq(hw + H_FRUT_OFF) + off;
    u64 k = gct_visits++;
    rep(nm("gct.", k, ".off"), off);
    rep(nm("gct.", k, ".type"), rb(node));
    rep(nm("gct.", k, ".subtype"), rb(node + 1));
    rep(nm("gct.", k, ".size"), *(volatile unsigned short *)(node + 2));
    rep(nm("gct.", k, ".id"), rq(node + 0x18));
    rep(nm("gct.", k, ".parent"), rq(node + 0x40));
    rep(nm("gct.", k, ".magic"), rl(node + 0x7c));
    if (rl(node + 0x7c) != 0x59584c47 || gct_visits > 40 || depth > 8)
        return;
    if (rb(node) == 0x0e && rb(node + 1) == 0x2c)
        rep("gct.io7_pid", (rq(node + 0x18) >> 8) & 0xff);
    if (rq(node + 0x48))
        gct_walk(rq(node + 0x48), depth + 1);
    if (rq(node + 0x58))
        gct_walk(rq(node + 0x58), depth + 1);
}

/* The CTB (marvel_find_console_vga_hose) and the CRB [ARM pp. 685-690] */
static void ctb_crb(void)
{
    u64 c = hw + rq(hw + H_CTBT_OFF), r = crb(), i;
    rep("ctb.q0", rq(c));
    rep("ctb.q1", rq(c + 8));
    rep("ctb.q2", rq(c + 16));
    rep("ctb.q3", rq(c + 24));
    rep("ctb.term_type", rq(c + 56));
    rep("ctb.term_out_loc", rq(c + 240));
    rep("crb.dispatch_va", rq(r));
    rep("crb.dispatch_pa", rq(r + 8));
    rep("crb.fixup_va", rq(r + 16));
    rep("crb.fixup_pa", rq(r + 24));
    rep("crb.map_entries", rq(r + 32));
    rep("crb.map_pages", rq(r + 40));
    for (i = 0; i < rq(r + 32) && i < 8; i++) {
        rep(nm("crb.map.", i, ".va"), rq(r + 48 + 24 * i));
        rep(nm("crb.map.", i, ".pa"), rq(r + 56 + 24 * i));
        rep(nm("crb.map.", i, ".count"), rq(r + 64 + 24 * i));
    }
    /* the procedure descriptors, through their VAs (the console's map) and
       their PAs */
    rep("crb.pd_dispatch.q0", rq(rq(r)));
    rep("crb.pd_dispatch.entry", rq(rq(r) + 8));
    rep("crb.pd_dispatch.entry_pa", rq(kseg(rq(r + 8)) + 8));
    rep("crb.pd_fixup.q0", rq(rq(r + 16)));
    rep("crb.pd_fixup.entry", rq(rq(r + 16) + 8));
    rep("crb.pd_fixup.entry_pa", rq(kseg(rq(r + 24)) + 8));
}

/* The zero page (setup.h): COMMAND_LINE, INITRD_START, INITRD_SIZE; the
   initrd's bytes as setup_memory would take them */
static void zero_page(void)
{
    u64 start = rq(ZERO_PGE + 0x100), size = rq(ZERO_PGE + 0x108), i;
    u64 h = 0xcbf29ce484222325;
    rep_text("zp.cmdline", ZERO_PGE, slen((const char *)ZERO_PGE));
    rep("zp.cmdline_len", slen((const char *)ZERO_PGE));
    rep("zp.initrd_start", start);
    rep("zp.initrd_size", size);
    if (start && size) {
        for (i = 0; i < size; i++) {
            h ^= rb(start + i);
            h *= 0x100000001b3;
        }
        rep("zp.initrd_fnv", h);
    }
}

static char envbuf[300];

static void env(const char *pre, u64 id, u64 len)
{
    u64 r, i;
    for (i = 0; i < sizeof(envbuf); i++)
        envbuf[i] = 0;
    r = callback(0x22, id, (u64)envbuf, len);
    rep(nm(pre, id, len < 256 ? ".short.status" : ".status"), r >> 61);
    rep(nm(pre, id, len < 256 ? ".short.count" : ".count"), r & 0xffffffff);
    rep_text(nm(pre, id, len < 256 ? ".short.text" : ".text"),
             (u64)envbuf, r & 0xffffffff);
}

static void envs(const char *tag, const char *pre)
{
    static const u64 ids[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                              13, 14, 15, 0, 0x10, 0x40, 0x80, 0xff};
    u64 i;
    put(tag);
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        env(pre, ids[i], 256);
    env(pre, 8, 5); /* BOOTED_OSFLAGS truncated */
}

/* GETC as srmcons does it; the test types `what` after the marker */
static void getc_test(const char *pre, const char *marker)
{
    u64 r, t, polls = 0;
    r = callback(1, 0, 0, 0);
    rep(cat(pre, ".empty"), r);
    put(marker);
    t = rpcc32();
    do {
        r = callback(1, 0, 0, 0);
        polls++;
    } while ((r >> 62) == 2 && wait_cycles(t, cyc_hz * 4));
    rep(cat(pre, ".status"), r >> 61);
    rep(cat(pre, ".char"), r & 0xffffffff);
    rep(cat(pre, ".unit"), (r >> 32) & 0xff);
    rep(cat(pre, ".polled"), polls > 1);
}

/* ------------------------------------------------------------------------
 * mm/init.c: callback_init (FIXUP, the console's pages at VMALLOC) and
 * switch_to_system_map (this kernel's own page tables and PCB)
 * ------------------------------------------------------------------------ */
static u64 pte(u64 pa, u64 bits) { return (pa >> 13) << 32 | bits; }

static void own_tables(void)
{
    u64 r = crb(), i, j, va = VMALLOC_BASE;
    u64 l3 = kseg(TK_L3V), map0;
    for (i = 0; i < 0x6000; i += 8)
        wq(kseg(TK_L1) + i, 0);
    wq(kseg(TK_L1) + 8 * 1023, pte(TK_L1, 0x1101));
    wq(kseg(TK_L1) + 8 * 1022, pte(TK_L2V, 0x1101));
    wq(kseg(TK_L2V), pte(TK_L3V, 0x1101));
    map0 = rq(r + 48);
    /* Linux: dispatch_va = VMALLOC_START + dispatch_va - map[0].va */
    wq(r, VMALLOC_BASE + rq(r) - map0);
    wq(r + 16, VMALLOC_BASE + rq(r + 16) - map0);
    for (i = 0; i < rq(r + 32); i++) {
        u64 pfn_pa = rq(r + 56 + 24 * i);
        wq(r + 48 + 24 * i, va);
        for (j = 0; j < rq(r + 64 + 24 * i) && va < VMALLOC_BASE + 0x800000;
             j++) {
            wq(l3 + ((va >> 13) & 1023) * 8, pte(pfn_pa, 0x1111));
            pfn_pa += 0x2000;
            va += 0x2000;
        }
    }
}

static void switch_map(void)
{
    u64 pcb = kseg(TK_PCB0), i, old;
    for (i = 0; i < 0x80; i += 8)
        wq(pcb + i, 0);
    wq(pcb + 16, TK_L1 >> 13);
    wq(pcb + 40, 1);
    old = pal_swpctx_keep(TK_PCB0);
    pal_tbia();
    for (i = 0; i < 0x80; i += 8)
        wq(kseg(TK_ORIG) + i, rq(kseg(old) + i));
    own_map = 1;
    rep("switch.old_pcbb", old);
    rep("switch.orig_ksp", rq(kseg(TK_ORIG)));
    rep("switch.orig_ptbr", rq(kseg(TK_ORIG) + 16));
}

/* ------------------------------------------------------------------------
 * Interrupts, the timer, the secondaries (smp.c)
 * ------------------------------------------------------------------------ */
static u64 rec(u64 w) { return kseg(TK_REC) + w * 0x100; }

void int_c(u64 type, u64 vector, u64 param)
{
    u64 w = pal_whami(), r = rec(w);
    if (w >= MAXW)
        return;
    if (type == 1) {
        u64 n = rq(r + R_TICKS) + 1;
        wq(r + R_TICKS, n);
        if (n == 1)
            wq(r + R_T1, rpcc32());
        if (n == 3)
            wq(r + R_T2, rpcc32());
        wq(r + R_TYPE, type);
    } else
        wq(r + R_OTHER, rq(r + R_OTHER) + 1);
    (void)vector;
    (void)param;
}

static void entries(void)
{
    pal_wrent((u64)ent_int, 0);
    pal_wrent((u64)ent_arith, 1);
    pal_wrent((u64)ent_mm, 2);
    pal_wrent((u64)ent_if, 3);
    pal_wrent((u64)ent_una, 4);
    pal_wrent((u64)ent_sys, 5);
}

static void take_ticks(u64 w, u64 want)
{
    u64 t = rpcc32();
    pal_swpipl(0);
    while (rq(rec(w) + R_TICKS) < want && wait_cycles(t, cyc_hz * 2))
        ;
    pal_swpipl(7);
}

void sec_main(u64 r27)
{
    u64 w = pal_whami(), r, s;
    if (w >= MAXW)
        for (;;)
            ;
    r = rec(w);
    s = hw + rq(hw + H_PROC_OFF) + w * rq(hw + H_PROC_SIZE);
    wq(r + R_FLAGS, rq(s + S_FLAGS));
    wq(r + R_R27, r27);
    wq(r + R_SP, get_sp());
    wq(r + R_PS, pal_rdps());
    wq(r + R_WHAMI, w);
    entries();
    take_ticks(w, 2);
    do_mb();
    wq(r + R_DONE, 1);
    /* Linux's common_shutdown_1 for a secondary: "remain halted" */
    wq(s + S_FLAGS, (rq(s + S_FLAGS) & ~0x00ff0001UL) | 0x00040000UL);
    pal_halt();
    for (;;)
        ;
}

static void hwrpb_update_checksum(void)
{
    u64 i, sum = 0;
    for (i = 0; i < H_CHKSUM; i += 8)
        sum += rq(hw + i);
    wq(hw + H_CHKSUM, sum);
}

/* smp.c's send_secondary_console_msg and secondary_cpu_start */
static void start_secondary(u64 w)
{
    u64 s = hw + rq(hw + H_PROC_OFF) + w * rq(hw + H_PROC_SIZE);
    u64 ipcb = kseg(TK_IPCB + w * 0x80), stack = TK_STACK + w * 0x4000;
    u64 i, t, mask = 1UL << w;
    const char *msg = "START\r\n";
    for (i = 0; i < 0x80; i += 8)
        wq(ipcb + i, 0);
    wq(ipcb, kseg(stack + 0x3000));
    wq(ipcb + 16, TK_L1 >> 13);
    wq(ipcb + 40, 1);
    wq(s, kseg(stack + 0x4000 - 16));
    wq(s + 8, 0);
    wq(s + 16, TK_L1 >> 13);
    wq(s + 24, 0);
    wq(s + 32, TK_IPCB + w * 0x80);
    wq(s + 40, 1);
    wq(s + 48, 0);
    wq(s + 56, 0);
    wq(hw + H_RESTART, (u64)sec_start);
    wq(hw + H_RESTART_DATA, (u64)sec_start);
    hwrpb_update_checksum();
    wq(s + S_FLAGS, (rq(s + S_FLAGS) | 0x22) & ~1UL);
    do_mb();
    t = rpcc32();
    while ((rq(hw + H_TXRDY) & mask) && wait_cycles(t, cyc_hz))
        ;
    *(volatile u32 *)(s + S_IPC) = 7;
    for (i = 0; i < 7; i++)
        *(volatile u8 *)(s + S_IPC + 8 + i) = (u8)msg[i];
    do_mb();
    set_bit_atomic(w, hw + H_RXRDY);
    /* smp.c waits for the console's ack, BIP; the secondary notes the
       flags it starts with, before its halt clears BIP again */
    t = rpcc32();
    while (!(rq(s + S_FLAGS) & 1) && !rq(rec(w) + R_DONE) &&
           wait_cycles(t, cyc_hz * 2))
        ;
    t = rpcc32();
    while (!rq(rec(w) + R_DONE) && wait_cycles(t, cyc_hz * 3))
        ;
    rep(nm("sec.", w, ".done"), rq(rec(w) + R_DONE));
    rep(nm("sec.", w, ".bip"), rq(rec(w) + R_FLAGS) & 1);
    rep(nm("sec.", w, ".flags"), rq(rec(w) + R_FLAGS));
    rep(nm("sec.", w, ".r27"), rq(rec(w) + R_R27));
    rep(nm("sec.", w, ".sp"), rq(rec(w) + R_SP));
    rep(nm("sec.", w, ".ps"), rq(rec(w) + R_PS));
    rep(nm("sec.", w, ".whami"), rq(rec(w) + R_WHAMI));
    rep(nm("sec.", w, ".ticks"), rq(rec(w) + R_TICKS) >= 2);
    rep(nm("sec.", w, ".ticktype"), rq(rec(w) + R_TYPE));
    t = rpcc32();
    while (rq(s + S_HALT_REASON) != 5 && wait_cycles(t, cyc_hz))
        ;
    rep(nm("sec.", w, ".halt_reason"), rq(s + S_HALT_REASON));
    rep(nm("sec.", w, ".halt_flags"), rq(s + S_FLAGS));
}

void kmain(u64 entry_sp)
{
    u64 init, w, n, r0;
    hw = kseg(rq(INIT_HWRPB));
    init = rq(INIT_HWRPB);
    cyc_hz = rq(hw + H_CYCLE_FREQ);
    if (!cyc_hz)
        cyc_hz = 800000000;
    for (w = 0; w < MAXW * 0x100; w += 8)
        wq(kseg(TK_REC) + w, 0);
    entries(); /* an unexpected fault halts, with what it was */
    put("\r\n@@BEGIN\r\n");
    rep("entry.sp", entry_sp);
    rep("entry.ps", pal_rdps());
    rep("entry.whami", pal_whami());
    rep("entry.mces", pal_rdmces());
    rep("kernel.sec_start", (u64)sec_start);
    rep("init_hwrpb.phys", init);
    hwrpb_fields();
    slots();
    memory();
    put("@@GCT\r\n");
    gct_walk(0, 0);
    rep("gct.visits", gct_visits);
    ctb_crb();
    zero_page();
    by_pa = 0;
    rep("puts.result", callback(2, 0, (u64)"puts\r\n", 6));
    envs("@@ENV1\r\n", "env.");
    getc_test("getc", "@@TYPE1\r\n");
    /* callback_init: FIXUP on the console's map, then the CRB's pages at
       VMALLOC and this kernel's own page tables */
    r0 = cb_fixup(VMALLOC_BASE, hw);
    rep("fixup.result", r0);
    own_tables();
    switch_map();
    rep("own.dispatch_va", rq(crb()));
    rep("own.map0_va", rq(crb() + 48));
    rep("own.puts", callback(2, 0, (u64)"own\r\n", 5));
    envs("@@ENV2\r\n", "env2.");
    getc_test("getc2", "@@TYPE2\r\n");
    /* the boot processor's timer */
    entries();
    take_ticks(0, 3);
    rep("p0.ticks", rq(rec(0) + R_TICKS));
    rep("p0.ticktype", rq(rec(0) + R_TYPE));
    rep("p0.period", (rq(rec(0) + R_T2) - rq(rec(0) + R_T1)) & 0xffffffff);
    rep("p0.other", rq(rec(0) + R_OTHER));
    /* each chip's R_INTER_TIM (N22: ITV <7:0>, 0x0A0 of its window at
       ipe8(PID), through KSEG) */
    for (w = 0; w < rq(hw + H_NR_PROC) / 4; w++)
        rep(nm("itv.", w, ""),
            rq(KSEG | (((~w & 0x1fff) << 35 | 0x7ffcUL << 20 | 0xa0) &
                       0x1ffffffffffUL)) & 0xff);
    /* the secondaries, one at a time, as smp_boot_one_cpu */
    n = rq(hw + H_NR_PROC);
    for (w = 1; w < n && w < MAXW; w++) {
        u64 s = hw + rq(hw + H_PROC_OFF) + w * rq(hw + H_PROC_SIZE);
        if ((rq(s + S_FLAGS) & 0x1cc) == 0x1cc)
            start_secondary(w);
    }
    rep("cb.calls", calls);
    rep("cb.regs_bad", regs_bad);
    put("@@DONE\r\n");
    /* srm_paging_stop and the halt: Linux's poweroff */
    pal_wrvptptr(VPTB_CONSOLE);
    wq(hw + H_VPTB, VPTB_CONSOLE);
    hwrpb_update_checksum();
    pal_swpctx_keep(TK_ORIG);
    pal_tbia();
    {
        u64 s = hw + rq(hw + H_PROC_OFF);
        wq(s + S_FLAGS, (rq(s + S_FLAGS) & ~0x00ff0001UL) | 0x00040000UL);
    }
    pal_halt();
    for (;;)
        ;
}
