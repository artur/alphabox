/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AlphaZ for Linux on the Alpha: alphaz-linux, the facts of az.h as a
 * text report on standard output, since no Linux guest here has a desktop.
 * Copyright (C) 2026 Artur Goulão
 *
 * Linux's own words come from /proc/cpuinfo (the console's HWRPB as the
 * kernel prints it), /proc/meminfo and /proc/version; the processor's from
 * its instructions (az_core.c); the clock from RPCC against
 * clock_gettime(CLOCK_MONOTONIC), with the process on one processor so
 * that one counter is read throughout. nada's libc has no clock_gettime or
 * sched_setaffinity, so both are system calls made here. */
#include <stdio.h>
#include <string.h>

#include "az.h"
#include "az_build.h"

/* nada's runtime stub (lib/linux.c declares it the same way) */
long long __syscall6(long long n, long long a, long long b, long long c, long long d, long long e, long long f);

/* Linux's system call numbers on the Alpha (arch/alpha/kernel/syscalls/
 * syscall.tbl) */
#define AZ_SYS_SCHED_SETAFFINITY 395
#define AZ_SYS_SCHED_GETAFFINITY 396
#define AZ_SYS_CLOCK_GETTIME 420
#define AZ_CLOCK_MONOTONIC 1

static u64 mono_now(void)
{
    long long ts[2];
    ts[0] = 0;
    ts[1] = 0;
    __syscall6(AZ_SYS_CLOCK_GETTIME, AZ_CLOCK_MONOTONIC, (long long)ts, 0, 0, 0, 0);
    return (u64)ts[0] * 1000000000ULL + (u64)ts[1];
}

static int set_cpus(u64 mask)
{
    return (int)__syscall6(AZ_SYS_SCHED_SETAFFINITY, 0, sizeof mask, (long long)&mask, 0, 0, 0);
}

static u64 get_cpus(void)
{
    u64 mask = 0;
    if (__syscall6(AZ_SYS_SCHED_GETAFFINITY, 0, sizeof mask, (long long)&mask, 0, 0, 0) < 0) return 0;
    return mask;
}

/* ---- /proc: "key : value" lines ---- */

#define AZ_KEYS 64
struct az_kv {
    char key[AZ_LABEL];
    char value[AZ_VALUE];
    int used;
};
static struct az_kv cpuinfo[AZ_KEYS], meminfo[AZ_KEYS];
static int ncpuinfo, nmeminfo;

static void trim(char *s)
{
    int n = (int)strlen(s), k = 0;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    while (s[k] == ' ' || s[k] == '\t') k++;
    if (k) memmove(s, s + k, n - k + 1);
}

static int read_kv(const char *path, struct az_kv *kv, int max)
{
    char line[256], *colon;
    int n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (n < max && fgets(line, sizeof line, f)) {
        colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        az_snprintf(kv[n].key, AZ_LABEL, "%s", line);
        az_snprintf(kv[n].value, AZ_VALUE, "%s", colon + 1);
        trim(kv[n].key);
        trim(kv[n].value);
        kv[n].used = 0;
        n++;
    }
    fclose(f);
    return n;
}

static struct az_kv *find(struct az_kv *kv, int n, const char *key)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(kv[i].key, key)) return &kv[i];
    return 0;
}

static const char *cpuinfo_value(const char *key)
{
    struct az_kv *k = find(cpuinfo, ncpuinfo, key);
    return k ? k->value : 0;
}

/* a /proc/cpuinfo row on a page, or nothing when the kernel prints no
 * such line */
static void cpuinfo_row(int page, const char *key)
{
    struct az_kv *k = find(cpuinfo, ncpuinfo, key);
    if (!k) return;
    k->used = 1;
    az_add(page, key, "/proc/cpuinfo", "%s", k->value);
}

static void meminfo_row(const char *key)
{
    struct az_kv *k = find(meminfo, nmeminfo, key);
    if (!k) return;
    k->used = 1;
    az_add(AZ_SYSTEM, key, "/proc/meminfo", "%s", k->value);
}

/* ---- the pages ---- */

static void clock_rows(void)
{
    struct az_clock c;
    char hz[32];
    const char *cf;
    u64 old = get_cpus();
    set_cpus(1);
    az_measure_clock(&c, mono_now, 1000000000ULL, 500);
    if (old) set_cpus(old);
    az_fmt_hz(hz, sizeof hz, c.hz);
    az_add(AZ_CPU, "Clock", "measured: RPCC against CLOCK_MONOTONIC", "%s", hz);
    az_add(AZ_CPU, "  how", "measured: 500 ms on processor 0",
           "%llu cycles in %llu ns; low 32 bits wrapped %llu times; %llu reads", c.cycles, c.ticks, c.wraps, c.samples);
    cf = cpuinfo_value("cycle frequency [Hz]");
    if (cf) {
        u64 v = 0;
        int k;
        for (k = 0; cf[k] >= '0' && cf[k] <= '9'; k++) v = v * 10 + (u64)(cf[k] - '0');
        az_fmt_hz(hz, sizeof hz, v);
        az_add(AZ_CPU, "Clock, the console's", "/proc/cpuinfo cycle frequency [Hz]", "%s (%s Hz)", hz, cf);
        find(cpuinfo, ncpuinfo, "cycle frequency [Hz]")->used = 1;
    } else {
        az_add(AZ_CPU, "Clock, the console's", "/proc/cpuinfo", "not reported");
    }
}

/* IMPLVER and AMASK on each processor, the process moved to each in turn */
static void each_cpu_rows(void)
{
    u64 mask = get_cpus(), iv[64], pr[64];
    int n = 0, i, same = 1;
    if (!mask) {
        az_add(AZ_CPU, "Every processor", "sched_getaffinity", "not reported");
        return;
    }
    for (i = 0; i < 64; i++) {
        if (!(mask >> i & 1)) continue;
        if (set_cpus(1ULL << i) < 0) continue;
        iv[n] = az_implver();
        pr[n] = AZ_AMASK_ASK & ~az_amask(AZ_AMASK_ASK);
        if (iv[n] != iv[0] || pr[n] != pr[0]) same = 0;
        n++;
    }
    set_cpus(mask);
    if (same) {
        az_add(AZ_CPU, "Every processor", "instructions, on each processor in turn",
               "%d of %d: IMPLVER %llu, AMASK features 0x%04llx on each", n, n, iv[0], pr[0]);
        return;
    }
    for (i = 0; i < n; i++)
        az_add(AZ_CPU, "Processor", "instructions, on this processor", "#%d: IMPLVER %llu, AMASK features 0x%04llx", i,
               iv[i], pr[i]);
}

static void cpu_rows(void)
{
    static const char *const keys[] = {"cpu", "cpu model", "cpu variation", "cpu revision", "cpu serial number",
                                       "BogoMIPS", "timer frequency [Hz]", "page size [bytes]", "phys. address bits",
                                       "max. addr. space #", "cpus detected", "cpus active", "cpu active mask", 0};
    char part[AZ_VALUE];
    const char *model = cpuinfo_value("cpu model");
    int i;
    az_cpu_rows();
    az_part(part, sizeof part, az_cpu.implver, model ? az_type_of(model) : 0, -1);
    az_add(AZ_CPU, "Part (AlphaZ's reading)", "table value: IMPLVER and cpu model", "%s", part);
    for (i = 0; keys[i]; i++) cpuinfo_row(AZ_CPU, keys[i]);
    clock_rows();
    each_cpu_rows();
}

static void cache_rows(void)
{
    static const char *const keys[] = {"L1 Icache", "L1 Dcache", "L2 cache", "L3 cache", 0};
    int i, any = 0;
    for (i = 0; keys[i]; i++)
        if (find(cpuinfo, ncpuinfo, keys[i])) {
            cpuinfo_row(AZ_CACHES, keys[i]);
            any = 1;
        }
    if (!any) az_add(AZ_CACHES, "What Linux reports", "/proc/cpuinfo", "no cache lines");
    az_cache_rows();
}

static void system_rows(void)
{
    static const char *const keys[] = {"platform string", "system type", "system variation", "system revision",
                                       "system serial number", 0};
    static const char *const mem[] = {"MemTotal", "MemFree", "MemAvailable", "SwapTotal", "SwapFree", 0};
    char line[256];
    FILE *f;
    int i;
    if (!find(cpuinfo, ncpuinfo, "platform string")) az_add(AZ_SYSTEM, "platform string", "/proc/cpuinfo", "not reported");
    for (i = 0; keys[i]; i++) cpuinfo_row(AZ_SYSTEM, keys[i]);
    f = fopen("/proc/version", "r");
    if (f) {
        if (fgets(line, sizeof line, f)) {
            trim(line);
            az_add(AZ_SYSTEM, "Kernel", "/proc/version", "%s", line);
        }
        fclose(f);
    }
    if (nmeminfo <= 0) az_add(AZ_SYSTEM, "Memory", "/proc/meminfo", "not reported");
    for (i = 0; mem[i]; i++) meminfo_row(mem[i]);
    /* whatever else the kernel printed about the processor, each key once
     * (a kernel that prints a block a processor repeats them) */
    for (i = 0; i < ncpuinfo; i++)
        if (!cpuinfo[i].used && find(cpuinfo, ncpuinfo, cpuinfo[i].key) == &cpuinfo[i])
            az_add(AZ_SYSTEM, cpuinfo[i].key, "/proc/cpuinfo", "%s", cpuinfo[i].value);
}

static void about_rows(void)
{
    az_add(AZ_ABOUT, "AlphaZ", "", "version %s, for the Alpha guests of Alphabox", AZ_VERSION);
    az_add(AZ_ABOUT, "This program", "", "alphaz-linux, a static ELF for the Alpha (-t alpha -mcpu=ev6)");
    az_add(AZ_ABOUT, "Compiled by", "", "nada at commit %s%s", AZ_NADA_COMMIT, AZ_NADA_DIRTY ? " (its tree had changes)" : "");
    az_add(AZ_ABOUT, "Licence", "", "GPL-2.0-or-later");
    az_add(AZ_ABOUT, "Copyright", "", "(C) 2026 Artur Goul\303\243o");
    az_add(AZ_ABOUT, "Source", "", "guest/alphaz in github.com/artur/alphabox");
    az_add(AZ_ABOUT, "Every value says", "", "where it came from: an instruction, an OS call, /proc,");
    az_add(AZ_ABOUT, "", "", "or a table value, which is what a document says, not this machine");
}

static void put_line(const char *line, void *ctx)
{
    (void)ctx;
    printf("%s\n", line);
}

int main(void)
{
    u64 old;
    az_pages_init();
    az_probe();
    ncpuinfo = read_kv("/proc/cpuinfo", cpuinfo, AZ_KEYS);
    nmeminfo = read_kv("/proc/meminfo", meminfo, AZ_KEYS);
    if (ncpuinfo < 0) {
        ncpuinfo = 0;
        az_add(AZ_CPU, "/proc/cpuinfo", "", "not readable");
    }
    cpu_rows();
    cache_rows();
    if (az_page_shown(AZ_EV8)) az_ev8_rows();
    old = get_cpus();
    set_cpus(1);
    az_bench_all(mono_now, 1000000000ULL, 300);
    if (old) set_cpus(old);
    az_bench_rows();
    system_rows();
    about_rows();
    printf("ALPHAZ-REPORT-BEGIN\nAlphaZ %s report\n\n", AZ_VERSION);
    az_report(put_line, 0);
    printf("ALPHAZ-REPORT-END\n");
    return 0;
}
