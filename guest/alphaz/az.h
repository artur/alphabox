/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AlphaZ: what an Alpha guest of Alphabox says about its processors and
 * its machine.
 * Copyright (C) 2026 Artur Goulão
 *
 * The core both front ends share: the facts as pages of rows, each row a
 * label, a value and where the value came from; the processor's own
 * answers (IMPLVER, AMASK, the cycle counter); the clock measured from the
 * cycle counter against an OS timer; the documented caches of the part
 * IMPLVER names; the short benchmark loops; the EV8 self-check.
 *
 * alphaz_win.c (the Win32 GUI, ALPHAZ.EXE) and alphaz_linux.c (the text
 * report, alphaz-linux) add the operating system's own words and draw or
 * print the pages. Nothing here calls the operating system: what needs a
 * clock takes it as a function. */
#ifndef AZ_H
#define AZ_H

#include <stdarg.h>

#define AZ_VERSION "1.0.0"

typedef unsigned long long u64;
typedef long long s64;
typedef unsigned int u32;

/* ---- pages of facts ---- */

#define AZ_ROWS 48
#define AZ_LABEL 40
#define AZ_VALUE 120
#define AZ_SOURCE 72
#define AZ_BRIEF 64

/* One fact: what it is, its value, and where the value came from (an
 * instruction, an OS call, the registry, /proc, or "table value" for what
 * a document says rather than what this machine answered). The brief is
 * the value in short for a window's fields, where the value is too long
 * for one or is several fields: its parts separated by tabs. The report
 * never prints it. */
struct az_row {
    char label[AZ_LABEL];
    char value[AZ_VALUE];
    char source[AZ_SOURCE];
    char brief[AZ_BRIEF];
};

struct az_page {
    const char *title;  /* the tab's name */
    const char *banner; /* one line under the title: what the page is */
    int n;
    struct az_row rows[AZ_ROWS];
};

enum { AZ_CPU, AZ_CACHES, AZ_SYSTEM, AZ_EV8, AZ_BENCH, AZ_ABOUT, AZ_NPAGES };

extern struct az_page az_pages[AZ_NPAGES];

void az_pages_init(void);
/* a row: label, source, then the value as printf would format it; the
 * row, or 0 when the page is full */
struct az_row *az_add(int page, const char *label, const char *source, const char *fmt, ...);
/* the row's brief, as printf would format it (nothing when r is 0) */
void az_brief(struct az_row *r, const char *fmt, ...);
/* the first row of a page with this label, or 0 */
const struct az_row *az_find(int page, const char *label);
/* the row after r on its page when that one's label starts with two
 * spaces (it says more about r), or 0 */
const struct az_row *az_more(int page, const struct az_row *r);
/* part k of a row's brief, or its value as part 0 when it has no brief;
 * "" for no such part or no row */
void az_brief_part(char *buf, int size, const struct az_row *r, int k);
/* whether a page is shown: EV8 only when IMPLVER is 4 */
int az_page_shown(int page);
/* the report: every shown page as "label: value [source]" lines */
void az_report(void (*put)(const char *line, void *ctx), void *ctx);

/* ---- the processor's own answers ---- */

u64 az_implver(void);
/* AMASK of the feature bits in `bits`: the bits of the features that are
 * absent stay set (Rc = Rbv AND NOT mask, Alpha Architecture Handbook) */
u64 az_amask(u64 bits);
/* the cycle counter, all 64 bits: the low 32 count cycles, the high 32
 * are an offset the operating system keeps */
u64 az_rpcc(void);

/* the bits AMASK is asked about, each with its name */
struct az_feature {
    int bit;
    const char *name;
    const char *brief; /* the name in a list of them */
    const char *what;
};
extern const struct az_feature az_features[];
extern const int az_nfeatures;
#define AZ_AMASK_ASK 0xffffULL     /* every bit of the low 16 */
#define AZ_AMASK_SIMD (1ULL << 11) /* the 21464's 1C SIMD and 07 paired single */
#define AZ_AMASK_SMT (1ULL << 10)
#define AZ_AMASK_TARANTULA (1ULL << 3)

/* the probe's results, read once at start */
struct az_cpu {
    u64 implver;
    u64 present;     /* the features AMASK reported present, of AZ_AMASK_ASK */
    int ncpus;       /* the processors the OS reports, 0 when it does not */
    const char *who; /* the OS call that gave ncpus */
};
extern struct az_cpu az_cpu;
void az_probe(void);
/* what IMPLVER means, from the Architecture Reference Manual's Table D-4
 * and the 21464 IDS: a table value */
const char *az_implver_name(u64 implver);
/* the part number and the core IMPLVER names ("21264", "EV6"), for an
 * emblem: 0 for both when the table has none */
void az_implver_chip(u64 implver, const char **part, const char **core);

/* The CPU page's rows that come from instructions alone. */
void az_cpu_rows(void);

/* The processor types the console writes into the HWRPB [ARM pp. 805-807;
 * Linux's cpu_names], 1 to 17, and 18, Alphabox's for the 21464 on its own
 * interface: the name of type t, or 0 for none. */
const char *az_type_name(int type);
/* the type a name gives (/proc/cpuinfo's "cpu model"), or 0 */
int az_type_of(const char *name);
/* AlphaZ's reading of the part: IMPLVER together with the processor type
 * the operating system reports (0 when it reports none) and its minor
 * number (-1 when it reports none). A table value, said so in its row. */
void az_part(char *buf, int size, u64 implver, int type, int minor);

/* ---- the clock: RPCC against an OS timer ----
 * now() returns the timer in ticks of freq per second. The low 32 bits of
 * RPCC wrap (every 4.3 s at 1 GHz), so the loop samples the counter
 * continuously and adds up the 32-bit differences: any number of wraps is
 * counted as long as two samples are less than one wrap apart. Three
 * windows are timed and the median's are the results. */
#define AZ_WINDOWS 3
struct az_clock {
    u64 cycles;  /* counted by RPCC over the median window */
    u64 ticks;   /* of the OS timer over the same window */
    u64 freq;    /* the OS timer's ticks per second */
    u64 wraps;   /* times the low 32 bits wrapped */
    u64 samples; /* RPCC reads */
    u64 hz;      /* cycles * freq / ticks */
    int windows;
    u64 each[AZ_WINDOWS]; /* every window's hz, in the order timed */
};
/* "799.9, 758.1, 800.0 MHz": every window's rate */
void az_fmt_windows(char *buf, int size, const struct az_clock *c);
void az_measure_clock(struct az_clock *c, u64 (*now)(void), u64 freq, u64 window_ms);
/* "833.3 MHz" */
void az_fmt_hz(char *buf, int size, u64 hz);

/* ---- the caches ---- */

/* The Caches page's rows that a table gives: the documented sizes of the
 * part IMPLVER names. Each level's brief is its size, ways, line and
 * where it is, a part the document does not give left empty. */
void az_cache_rows(void);

/* ---- the benchmarks ----
 * Each loop runs for about ms milliseconds of the OS timer and counts its
 * rounds; the cycle counter counts the cycles over the same time. */
struct az_bench {
    const char *name;
    u64 rounds; /* loop rounds completed */
    u64 work;   /* operations per round */
    u64 ticks;  /* OS timer ticks taken */
    u64 cycles; /* RPCC cycles taken */
    u64 check;  /* the loop's result, so it cannot be left out */
};
void az_bench_all(u64 (*now)(void), u64 freq, u64 ms);
/* a row a loop; its brief: rounds a second, cycles a round, operations a
 * round, the check value */
void az_bench_rows(void);

/* ---- the EV8 ----
 * Only after AMASK has said bit 11 is present (az_cpu.present &
 * AZ_AMASK_SIMD) does any of this run an EV8 instruction. */
void az_ev8_rows(void);
/* the EV8 instructions' check; returns the number that disagree */
int az_ev8_check(void);
/* vaddub8 over n quadwords, against the same in C: rounds of each */
void az_ev8_bench(u64 (*now)(void), u64 freq, u64 ms, struct az_bench *simd, struct az_bench *scalar);

/* ---- formatting ---- */
int az_snprintf(char *buf, int size, const char *fmt, ...);
void az_hex(char *buf, int size, u64 v);

#endif
