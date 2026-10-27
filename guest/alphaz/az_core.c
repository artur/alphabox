/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AlphaZ: the core both front ends share (az.h).
 * Copyright (C) 2026 Artur Goulão */
#include <stdio.h>
#include <string.h>

#include "az.h"

struct az_page az_pages[AZ_NPAGES];
struct az_cpu az_cpu;

void az_pages_init(void)
{
    static const char *const titles[AZ_NPAGES] = {"CPU", "Caches", "System", "EV8", "Bench", "About"};
    static const char *const banners[AZ_NPAGES] = {
        "The processor, as its own instructions and the operating system report it",
        "What reports a cache, and the documented sizes of the part IMPLVER names",
        "The machine and the operating system, in their own words",
        "The 21464's own extensions, and a live check of its SIMD and paired single",
        "This guest's own measurement of AlphaZ's loops: not a claim about Alphabox",
        "AlphaZ, the build, and the licence",
    };
    int i;
    memset(az_pages, 0, sizeof az_pages);
    for (i = 0; i < AZ_NPAGES; i++) {
        az_pages[i].title = titles[i];
        az_pages[i].banner = banners[i];
    }
}

static void copy(char *dst, int size, const char *src)
{
    int k = 0;
    if (!src) src = "";
    while (src[k] && k < size - 1) {
        dst[k] = src[k];
        k++;
    }
    dst[k] = 0;
}

struct az_row *az_add(int page, const char *label, const char *source, const char *fmt, ...)
{
    struct az_page *p = &az_pages[page];
    struct az_row *r;
    va_list ap;
    if (p->n >= AZ_ROWS) return 0;
    r = &p->rows[p->n++];
    copy(r->label, AZ_LABEL, label);
    copy(r->source, AZ_SOURCE, source);
    va_start(ap, fmt);
    vsnprintf(r->value, AZ_VALUE, fmt, ap);
    va_end(ap);
    r->brief[0] = 0;
    return r;
}

void az_brief(struct az_row *r, const char *fmt, ...)
{
    va_list ap;
    if (!r) return;
    va_start(ap, fmt);
    vsnprintf(r->brief, AZ_BRIEF, fmt, ap);
    va_end(ap);
}

const struct az_row *az_find(int page, const char *label)
{
    struct az_page *p = &az_pages[page];
    int k;
    for (k = 0; k < p->n; k++)
        if (!strcmp(p->rows[k].label, label)) return &p->rows[k];
    return 0;
}

const struct az_row *az_more(int page, const struct az_row *r)
{
    struct az_page *p = &az_pages[page];
    int k = r ? (int)(r - p->rows) + 1 : p->n;
    if (k <= 0 || k >= p->n) return 0;
    return p->rows[k].label[0] == ' ' && p->rows[k].label[1] == ' ' ? &p->rows[k] : 0;
}

void az_brief_part(char *buf, int size, const struct az_row *r, int k)
{
    const char *s;
    int n = 0;
    buf[0] = 0;
    if (!r) return;
    if (!r->brief[0]) {
        if (k == 0) copy(buf, size, r->value);
        return;
    }
    for (s = r->brief; k > 0 && *s; s++)
        if (*s == '\t') k--;
    if (k > 0) return;
    while (s[n] && s[n] != '\t' && n < size - 1) {
        buf[n] = s[n];
        n++;
    }
    buf[n] = 0;
}

int az_snprintf(char *buf, int size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void az_hex(char *buf, int size, u64 v)
{
    az_snprintf(buf, size, "0x%llx", v);
}

int az_page_shown(int page)
{
    if (page == AZ_EV8) return az_cpu.implver == 4;
    return 1;
}

void az_report(void (*put)(const char *line, void *ctx), void *ctx)
{
    char line[AZ_LABEL + AZ_VALUE + AZ_SOURCE + 16];
    int i, k;
    for (i = 0; i < AZ_NPAGES; i++) {
        struct az_page *p = &az_pages[i];
        if (!az_page_shown(i)) continue;
        az_snprintf(line, sizeof line, "== %s: %s", p->title, p->banner);
        put(line, ctx);
        for (k = 0; k < p->n; k++) {
            struct az_row *r = &p->rows[k];
            if (r->source[0])
                az_snprintf(line, sizeof line, "%-28s %s  [%s]", r->label, r->value, r->source);
            else
                az_snprintf(line, sizeof line, "%-28s %s", r->label, r->value);
            put(line, ctx);
        }
        put("", ctx);
    }
}

/* ---- the processor's own answers ----
 * Each instruction in a function of its own with no prologue: the operand
 * arrives in $a0 and the result leaves in $v0. IMPLVER, AMASK and RPCC are
 * base architecture, which every Alpha decodes and nada assembles at every
 * -mcpu. Built for another processor (a check of the GUI under Wine), the
 * same calls answer "no Alpha": IMPLVER 0, every feature absent, no
 * counter. */
#ifdef __nada_alpha__
static u64 __attribute__((naked)) i_implver(void)
{
    __asm__("    implver $v0\n    ret\n");
}

static u64 __attribute__((naked)) i_amask(u64 bits)
{
    __asm__("    amask $a0, $v0\n    ret\n");
}

static u64 __attribute__((naked)) i_rpcc(void)
{
    __asm__("    rpcc $v0\n    ret\n");
}

u64 az_implver(void) { return i_implver(); }
u64 az_amask(u64 bits) { return i_amask(bits); }
u64 az_rpcc(void) { return i_rpcc(); }
#else
u64 az_implver(void) { return 0; }
u64 az_amask(u64 bits) { return bits; }
u64 az_rpcc(void) { return 0; }
#endif

/* The AMASK bits [Alpha Architecture Reference Manual, 4th ed., p. 808],
 * and the 21464's [IDS Rev 1.1k, p. 76, p. 668] with the meanings
 * Alphabox's EV8 sheet gives bits 3 and 11 (docs/platforms/ev8-isa.md,
 * section 2). */
const struct az_feature az_features[] = {
    {0, "BWX", "BWX", "byte and word loads and stores"},
    {1, "FIX", "FIX", "square root, integer-FP moves"},
    {2, "CIX", "CIX", "count instructions"},
    {3, "Tarantula", "Tarantula", "the vector extension (bit 3)"},
    {8, "MVI", "MVI", "multimedia instructions"},
    {9, "Precise traps", "precise traps", "trap PC is the instruction's"},
    {10, "SMT", "SMT", "LDx_ARM and QUIESCE (bit 10)"},
    {11, "EV8 SIMD, paired single", "SIMD and paired single", "opcodes 1C and 07 (bit 11)"},
    {12, "Prefetch, modify intent", "prefetch with modify intent", "prefetch with modify intent"},
};
const int az_nfeatures = sizeof az_features / sizeof az_features[0];

void az_probe(void)
{
    az_cpu.implver = az_implver();
    az_cpu.present = AZ_AMASK_ASK & ~az_amask(AZ_AMASK_ASK);
}

const char *az_implver_name(u64 implver)
{
    switch (implver) {
    case 0: return "EV4 family (21064)";
    case 1: return "EV5 family (21164)";
    case 2: return "EV6 family (21264)";
    case 3: return "EV7 family (21364)";
    case 4: return "EV8 (21464)";
    }
    return "not in the table";
}

void az_implver_chip(u64 implver, const char **part, const char **core)
{
    static const char *const parts[] = {"21064", "21164", "21264", "21364", "21464"};
    static const char *const cores[] = {"EV4", "EV5", "EV6", "EV7", "EV8"};
    *part = implver <= 4 ? parts[implver] : 0;
    *core = implver <= 4 ? cores[implver] : 0;
}

static const char *const type_names[] = {
    0,        "EV3",    "EV4",    "Simulator", "LCA4",   "EV5",  "EV45", "EV56", "EV6", "PCA56",
    "PCA57",  "EV67",   "EV68CB", "EV68AL",    "EV68CX", "EV7",  "EV79", "EV69", "EV8 (Alphabox's type 18)",
};
#define AZ_NTYPES (int)(sizeof type_names / sizeof type_names[0])

const char *az_type_name(int type)
{
    return type > 0 && type < AZ_NTYPES ? type_names[type] : 0;
}

int az_type_of(const char *name)
{
    int t;
    for (t = 1; t < AZ_NTYPES; t++)
        if (!strcmp(type_names[t], name)) return t;
    return 0;
}

void az_part(char *buf, int size, u64 implver, int type, int minor)
{
    const char *tn = az_type_name(type);
    switch (implver) {
    case 4:
        /* the `ev8` row shows the console the EV7's type, 15; the `ev8n`
         * row, on the 21464's own interface, type 18 */
        if (type == 15) az_snprintf(buf, size, "21464 (EV8), in EV7-compatible mode");
        else if (type == 18) az_snprintf(buf, size, "21464 (EV8), on its own interface");
        else az_snprintf(buf, size, "21464 (EV8)");
        return;
    case 3:
        /* the EV7 console names minor 1 to 5 "EV7 rev 1.0" to "3.0"; a
         * 1.3 GHz EV7z reports rev 3.0 */
        if (minor == 5) az_snprintf(buf, size, "21364 (%s), rev 3.0: the EV7z", tn ? tn : "EV7");
        else az_snprintf(buf, size, "21364 (%s)", tn ? tn : "EV7");
        return;
    case 2:
        if (tn) az_snprintf(buf, size, "%s, of the 21264 family", tn);
        else az_snprintf(buf, size, "the 21264 family (EV6, EV67, EV68)");
        return;
    }
    az_snprintf(buf, size, "%s", tn ? tn : az_implver_name(implver));
}

void az_cpu_rows(void)
{
    char known[16];
    u64 other;
    int i;
#ifndef __nada_alpha__
    az_add(AZ_CPU, "Processor", "this build", "not an Alpha: AlphaZ built for another processor");
    return;
#endif
    az_add(AZ_CPU, "IMPLVER", "instruction IMPLVER", "%llu", az_cpu.implver);
    az_add(AZ_CPU, "Family", "table value: ARM Table D-4, 21464 IDS", "%s", az_implver_name(az_cpu.implver));
    az_add(AZ_CPU, "AMASK, features present", "instruction AMASK 0xffff", "0x%04llx", az_cpu.present);
    for (i = 0; i < az_nfeatures; i++) {
        const struct az_feature *f = &az_features[i];
        az_snprintf(known, sizeof known, "AMASK bit %d", f->bit);
        az_add(AZ_CPU, f->name, known, "%s: %s", (az_cpu.present >> f->bit) & 1 ? "present" : "absent", f->what);
    }
    other = az_cpu.present;
    for (i = 0; i < az_nfeatures; i++) other &= ~(1ULL << az_features[i].bit);
    if (other) az_add(AZ_CPU, "AMASK, other bits", "instruction AMASK 0xffff", "0x%04llx: no name in AlphaZ's table", other);
}

/* ---- the clock ---- */

static void one_window(struct az_clock *c, u64 (*now)(void), u64 freq, u64 window_ms)
{
    u64 t0, t, want, prev, cur;
    memset(c, 0, sizeof *c);
    c->freq = freq;
    want = freq / 1000 * window_ms;
    t0 = now();
    prev = az_rpcc() & 0xffffffffULL;
    do {
        cur = az_rpcc() & 0xffffffffULL;
        if (cur < prev) c->wraps++;
        c->cycles += (cur - prev) & 0xffffffffULL;
        prev = cur;
        c->samples++;
        t = now();
    } while (t - t0 < want);
    c->ticks = t - t0;
    /* cycles * freq / ticks, in two parts so the product cannot overflow */
    c->hz = c->cycles / c->ticks * freq + c->cycles % c->ticks * freq / c->ticks;
}

/* Three windows, and the median's: an operating system's timer can jump
 * for a moment (in a guest whose host is busy, the clock interrupts that
 * were late are made up in a burst), and one window in three that does
 * should not be the answer. The others' rates are kept, so the spread is
 * shown with it. */
void az_measure_clock(struct az_clock *c, u64 (*now)(void), u64 freq, u64 window_ms)
{
    struct az_clock w[AZ_WINDOWS];
    int i, k, order[AZ_WINDOWS];
    memset(c, 0, sizeof *c);
    c->freq = freq;
    if (freq == 0) return;
    for (i = 0; i < AZ_WINDOWS; i++) {
        one_window(&w[i], now, freq, window_ms);
        order[i] = i;
    }
    for (i = 1; i < AZ_WINDOWS; i++)
        for (k = i; k > 0 && w[order[k]].hz < w[order[k - 1]].hz; k--) {
            int s = order[k];
            order[k] = order[k - 1];
            order[k - 1] = s;
        }
    *c = w[order[AZ_WINDOWS / 2]];
    for (i = 0; i < AZ_WINDOWS; i++) c->each[i] = w[i].hz;
    c->windows = AZ_WINDOWS;
}

void az_fmt_hz(char *buf, int size, u64 hz)
{
    u64 tenth = (hz + 50000) / 100000;
    az_snprintf(buf, size, "%llu.%llu MHz", tenth / 10, tenth % 10);
}

void az_fmt_windows(char *buf, int size, const struct az_clock *c)
{
    int i, n = 0;
    buf[0] = 0;
    for (i = 0; i < c->windows && n < size - 16; i++) {
        u64 tenth = (c->each[i] + 50000) / 100000;
        n += az_snprintf(buf + n, size - n, "%s%llu.%llu", i ? ", " : "", tenth / 10, tenth % 10);
    }
    if (n < size - 5) az_snprintf(buf + n, size - n, " MHz");
}

/* ---- the caches ----
 * The documented on-chip caches of each part, by IMPLVER. A table value:
 * no instruction reports a cache size, and Alphabox models no cache
 * latency, so no timing probe would find one either. */
void az_cache_rows(void)
{
    const char *src21264 = "table value: 21264 HRM";
    const char *l1 = "64 KB\t2-way\t64 bytes\ton the chip";
    switch (az_cpu.implver) {
    case 2:
        az_brief(az_add(AZ_CACHES, "L1 instruction (documented)", src21264, "64 KB, 2-way, 64-byte lines"), "%s", l1);
        az_brief(az_add(AZ_CACHES, "L1 data (documented)", src21264, "64 KB, 2-way, 64-byte lines"), "%s", l1);
        az_brief(az_add(AZ_CACHES, "L2 (documented)", src21264, "off the chip (the Bcache): its size is the board's"),
                 "the board's\t\t\toff the chip (the Bcache)");
        break;
    case 3:
        az_brief(az_add(AZ_CACHES, "L1 instruction (documented)", "table value: the EV68 core",
                        "64 KB, 2-way, 64-byte lines"),
                 "%s", l1);
        az_brief(az_add(AZ_CACHES, "L1 data (documented)", "table value: the EV68 core", "64 KB, 2-way, 64-byte lines"),
                 "%s", l1);
        az_brief(az_add(AZ_CACHES, "L2 (documented)", "table value: 21364 (Linux setup.c)",
                        "on the chip, 1.75 MB, 7-way"),
                 "1.75 MB\t7-way\t\ton the chip");
        az_add(AZ_CACHES, "  as consoles list it", "table value: ES47 consoles",
               "1.50 MB on rev 2 parts (six ways on), 1.75 MB on rev 3.0");
        break;
    case 4:
        az_brief(az_add(AZ_CACHES, "L1 instruction (documented)", "table value: 21464 IDS pp. 42-43", "64 KB"),
                 "64 KB\t\t\ton the chip");
        az_brief(az_add(AZ_CACHES, "L1 data (documented)", "table value: 21464 IDS pp. 42-43", "64 KB"),
                 "64 KB\t\t\ton the chip");
        az_brief(az_add(AZ_CACHES, "L2 (documented)", "table value: 21464 IDS pp. 42-43", "on the chip, 3 MB"),
                 "3 MB\t\t\ton the chip");
        az_add(AZ_CACHES, "  as an EV7 console sees it", "table value: Alphabox's ev8 row",
               "1.75 MB: seven 256 KB ways in BBOX_CTL");
        break;
    default:
        az_add(AZ_CACHES, "Documented sizes", "", "none in AlphaZ's table for IMPLVER %llu", az_cpu.implver);
        break;
    }
    az_add(AZ_CACHES, "Timing probe", "", "none run: Alphabox models no cache latency, so one would find nothing");
}

/* ---- the benchmarks ---- */

static struct az_bench benches[4];
static int nbenches;

/* run `step` until ms of the OS timer have passed, a thousand rounds
 * between looks at the timer */
static void bench(const char *name, u64 work, u64 (*step)(u64 x), u64 (*now)(void), u64 freq, u64 ms)
{
    struct az_bench *b = &benches[nbenches++];
    u64 t0, t, c0, c1, want = freq / 1000 * ms, x = 0x9e3779b97f4a7c15ULL;
    int k;
    b->name = name;
    b->work = work;
    t0 = now();
    c0 = az_rpcc() & 0xffffffffULL;
    do {
        for (k = 0; k < 1000; k++) x = step(x);
        b->rounds += 1000;
        t = now();
    } while (t - t0 < want);
    c1 = az_rpcc() & 0xffffffffULL;
    b->ticks = t - t0;
    /* the low 32 bits only, so a window over 4.3 s of cycles would wrap:
     * ms stays well under it */
    b->cycles = (c1 - c0) & 0xffffffffULL;
    b->check = x;
}

/* xorshift64: shifts, xors and an add, all in registers */
static u64 int_round(u64 x)
{
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x + 1;
}

/* a multiply and an add on doubles, each depending on the last */
static double fp_acc = 1.0;
static u64 fp_round(u64 x)
{
    fp_acc = fp_acc * 0.999999 + 0.000001;
    return x + 1;
}

static u64 bench_now_ticks; /* the OS timer's frequency, for the rows */

void az_bench_all(u64 (*now)(void), u64 freq, u64 ms)
{
    nbenches = 0;
    bench_now_ticks = freq;
    if (freq == 0) return;
    bench("Integer (xorshift64)", 7, int_round, now, freq, ms);
    bench("Floating point (multiply-add)", 2, fp_round, now, freq, ms);
    if (az_cpu.implver == 4 && (az_cpu.present & AZ_AMASK_SIMD) && nbenches + 2 <= 4) {
        az_ev8_bench(now, freq, ms, &benches[nbenches], &benches[nbenches + 1]);
        nbenches += 2;
    }
}

static void bench_row(struct az_bench *b)
{
    u64 per_s = b->ticks ? b->rounds * bench_now_ticks / b->ticks : 0;
    u64 cpr10 = b->rounds ? b->cycles * 10 / b->rounds : 0;
    struct az_row *r;
    r = az_add(AZ_BENCH, b->name, "measured: OS timer and RPCC",
               "%llu rounds/s, %llu.%llu cycles a round (%llu ops a round)", per_s, cpr10 / 10, cpr10 % 10, b->work);
    az_brief(r, "%llu\t%llu.%llu\t%llu\t%016llx", per_s, cpr10 / 10, cpr10 % 10, b->work, b->check);
}

void az_bench_rows(void)
{
    int i;
    if (nbenches == 0) {
        az_add(AZ_BENCH, "Loops", "", "not run: no OS timer");
        return;
    }
    for (i = 0; i < nbenches; i++) bench_row(&benches[i]);
    az_add(AZ_BENCH, "How", "", "each loop for about 300 ms of the OS timer");
    az_add(AZ_BENCH, "What the figures are", "", "this guest's own measurement of these loops, compiled by nada");
    az_add(AZ_BENCH, "What they are not", "", "an Alphabox performance claim: those come from perf_ab.py");
}
