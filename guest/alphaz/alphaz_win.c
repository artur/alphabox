/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AlphaZ for Windows on the Alpha: ALPHAZ.EXE (ALPHAZ64.EXE for 64-bit
 * Windows), a window with a tab for each page of facts (az.h) and a last
 * one, Details, with every row; or with /report FILE the same facts as
 * text.
 * Copyright (C) 2026 Artur Goulão
 *
 *   ALPHAZ.EXE                 the window
 *   ALPHAZ.EXE /report FILE    every page as text into FILE, no window
 *   ALPHAZ.EXE /close N        the window closes itself after N seconds
 *   ALPHAZ.EXE /tour N         the window moves to the next tab every N s
 *   ALPHAZ.EXE /keylog FILE    every key the window sees, into FILE (for
 *                              checking injected keys)
 *
 * Keys: Ctrl+Tab and Ctrl+Shift+Tab the next and the previous tab (the
 * tab control itself takes Left and Right), Up, Down, PgUp and PgDn
 * scroll Details, Esc closes.
 *
 * Windows' own words come from GetSystemInfo, GlobalMemoryStatus,
 * GetVersionExA and the registry's HKLM\HARDWARE\DESCRIPTION\System; the
 * processor's from its instructions (az_core.c); the clock from RPCC
 * against QueryPerformanceCounter and against GetTickCount, with the
 * thread on processor 0 so that one counter is read throughout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <commctrl.h>

#include "az_win32.h"
#include "az.h"
#include "az_build.h"

/* this build: ALPHAZ.EXE, a PE32, or ALPHAZ64.EXE, a PE32+ for 64-bit
 * Windows (-t alpha64-windows, whose pointers nada makes 8 bytes) */
#if __nada_sizeof_pointer__ == 8
#define AZ_EXE "ALPHAZ64.EXE"
#define AZ_PE "PE32+"
#define AZ_THIS "ALPHAZ64.EXE, Win64, PE32+ for the Alpha (-t alpha64-windows -mcpu=ev6)"
#else
#define AZ_EXE "ALPHAZ.EXE"
#define AZ_PE "PE32"
#define AZ_THIS "ALPHAZ.EXE, Win32, PE32 for the Alpha (-t alpha-windows -mcpu=ev6)"
#endif

static HINSTANCE self;

/* ---- the OS timers ---- */

static u64 qpc_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}

static u64 tick_now(void) { return GetTickCount(); }

/* ---- the registry ---- */

/* one value as text: strings as they are, a REG_MULTI_SZ's strings joined
 * by " | ", a DWORD in decimal and hex, anything else its type and size
 * and its first bytes */
static void reg_text(char *out, int size, DWORD type, const BYTE *data, DWORD len)
{
    DWORD k;
    int n;
    out[0] = 0;
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        az_snprintf(out, size, "%.*s", (int)(len ? len : 1), (const char *)data);
        return;
    }
    if (type == REG_MULTI_SZ) {
        n = 0;
        for (k = 0; k < len && n < size - 4; k++) {
            if (data[k] == 0) {
                if (k + 1 >= len || data[k + 1] == 0) break;
                out[n++] = ' ';
                out[n++] = '|';
                out[n++] = ' ';
            } else {
                out[n++] = (char)data[k];
            }
        }
        out[n] = 0;
        return;
    }
    if (type == REG_DWORD && len >= 4) {
        DWORD v = data[0] | (DWORD)data[1] << 8 | (DWORD)data[2] << 16 | (DWORD)data[3] << 24;
        az_snprintf(out, size, "%u (0x%x)", v, v);
        return;
    }
    n = az_snprintf(out, size, "type %u, %u bytes:", type, len);
    for (k = 0; k < len && k < 12 && n < size - 4; k++) n += az_snprintf(out + n, size - n, " %02x", data[k]);
    if (len > 12 && n < size - 4) az_snprintf(out + n, size - n, " ...");
}

/* the last two names of a key's path, which is what a row has room for:
 * "CentralProcessor\0" for HKLM\HARDWARE\DESCRIPTION\System\
 * CentralProcessor\0 (README.md lists the whole paths) */
static const char *short_path(const char *path)
{
    const char *p = path + strlen(path);
    int seps = 0;
    while (p > path) {
        if (p[-1] == '\\' && ++seps == 2) break;
        p--;
    }
    return p;
}

/* every value of HKLM\path, a row each, labelled prefix + the value's
 * name; with only set, just the values named there */
static int reg_rows(int page, const char *path, const char *prefix, const char *const *only)
{
    HKEY key;
    char name[128], label[AZ_LABEL], src[AZ_SOURCE], text[AZ_VALUE];
    static BYTE data[2048];
    DWORD i, nlen, dlen, type;
    int rows = 0, k, want;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return -1;
    for (i = 0;; i++) {
        nlen = sizeof name;
        dlen = sizeof data - 2;
        if (RegEnumValueA(key, i, name, &nlen, 0, &type, data, &dlen) != ERROR_SUCCESS) break;
        data[dlen] = 0;
        data[dlen + 1] = 0;
        if (only) {
            want = 0;
            for (k = 0; only[k]; k++)
                if (!strcmp(only[k], name)) want = 1;
            if (!want) continue;
        }
        az_snprintf(label, sizeof label, "%s%s", prefix, name[0] ? name : "(default)");
        az_snprintf(src, sizeof src, "registry %s", short_path(path));
        reg_text(text, sizeof text, type, data, dlen);
        az_add(page, label, src, "%s", text);
        rows++;
    }
    RegCloseKey(key);
    return rows;
}

/* one value of HKLM\path as text, or "not reported" */
static void reg_value(char *out, int size, const char *path, const char *name)
{
    HKEY key;
    static BYTE data[512];
    DWORD type = 0, len = sizeof data - 2;
    az_snprintf(out, size, "not reported");
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    if (RegQueryValueExA(key, name, 0, &type, data, &len) == ERROR_SUCCESS) {
        data[len] = 0;
        data[len + 1] = 0;
        reg_text(out, size, type, data, len);
    }
    RegCloseKey(key);
}

/* ---- the CPU page ---- */

static SYSTEM_INFO si;

static const char *arch_name(WORD a)
{
    switch (a) {
    case 0: return "INTEL";
    case 1: return "MIPS";
    case 2: return "ALPHA";
    case 3: return "PPC";
    case 6: return "IA64";
    case 7: return "ALPHA64";
    case 9: return "AMD64";
    }
    return "not in AlphaZ's table";
}

static void clock_rows(void)
{
    struct az_clock c;
    LARGE_INTEGER f;
    char hz[32], each[64];
    HANDLE th = GetCurrentThread();
    DWORD_PTR old = SetThreadAffinityMask(th, 1);
    Sleep(0);
    if (!QueryPerformanceFrequency(&f)) f.QuadPart = 0;
    az_add(AZ_CPU, "Performance counter", "kernel32 QueryPerformanceFrequency", "%llu Hz", (u64)f.QuadPart);
#ifdef __nada_alpha__
    az_measure_clock(&c, qpc_now, (u64)f.QuadPart, 400);
    az_fmt_hz(hz, sizeof hz, c.hz);
    az_add(AZ_CPU, "Clock", "measured: RPCC against QueryPerformanceCounter", "%s", hz);
    az_fmt_windows(each, sizeof each, &c);
    az_brief(az_add(AZ_CPU, "  windows", "measured: 3 of 400 ms on processor 0", "%s: the median above", each), "%s",
             each);
    az_add(AZ_CPU, "  the median's", "measured: RPCC against QueryPerformanceCounter",
           "%llu cycles in %llu counter ticks; low 32 bits wrapped %llu times; %llu reads", c.cycles, c.ticks,
           c.wraps, c.samples);
    /* A HAL whose performance counter is the cycle counter itself gives
     * as many ticks as cycles: then the clock above is only the frequency
     * Windows was told (by the console, which timed the processor at
     * boot), and the second opinion is the one independent of it. */
    if (c.ticks && (c.cycles > c.ticks ? c.cycles - c.ticks : c.ticks - c.cycles) * 1000 < c.ticks)
        az_brief(az_add(AZ_CPU, "  which is", "measured: ticks = cycles",
                        "the cycle counter itself: the clock above is QueryPerformanceFrequency's"),
                 "QPC is RPCC");
    /* GetTickCount moves in clock interrupts, 10 ms or so: longer windows */
    az_measure_clock(&c, tick_now, 1000, 1000);
    az_fmt_hz(hz, sizeof hz, c.hz);
    az_fmt_windows(each, sizeof each, &c);
    az_brief(az_add(AZ_CPU, "Clock, second opinion", "measured: RPCC against GetTickCount, 3 of 1 s",
                    "%s (windows: %s)", hz, each),
             "%s", hz);
#else
    az_add(AZ_CPU, "Clock", "", "not measured: no cycle counter in this build");
#endif
    if (old) SetThreadAffinityMask(th, old);
}

/* IMPLVER and AMASK on each processor, the thread moved to each in turn */
static void each_cpu_rows(void)
{
    HANDLE th = GetCurrentThread();
    DWORD_PTR proc = 0, sys = 0, old;
    u64 iv[32], pr[32];
    int n = 0, i, same = 1;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) proc = si.dwActiveProcessorMask;
    old = SetThreadAffinityMask(th, proc);
    for (i = 0; i < 32; i++) {
        if (!(proc >> i & 1)) continue;
        if (!SetThreadAffinityMask(th, (DWORD_PTR)1 << i)) continue;
        Sleep(0);
        iv[n] = az_implver();
        pr[n] = AZ_AMASK_ASK & ~az_amask(AZ_AMASK_ASK);
        if (iv[n] != iv[0] || pr[n] != pr[0]) same = 0;
        n++;
    }
    if (old) SetThreadAffinityMask(th, old);
    if (n == 0) return;
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
    static const char *const cp_some[] = {"Identifier", "~MHz", "VendorIdentifier", 0};
    HKEY key;
    char sub[64], path[128], prefix[AZ_LABEL];
    DWORD i, len;
    char text[AZ_VALUE];
    int first = 1;

    reg_value(text, sizeof text, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "Identifier");
    az_add(AZ_CPU, "Name", "registry CentralProcessor\\0", "%s", text);
#ifdef __nada_alpha__
    az_part(text, sizeof text, az_cpu.implver, 0, -1);
    az_add(AZ_CPU, "Part (AlphaZ's reading)", "table value: IMPLVER", "%s", text);
#endif
    az_cpu_rows();
    clock_rows();
    az_brief(az_add(AZ_CPU, "Processors", "kernel32 GetSystemInfo", "%u (active mask 0x%x)", si.dwNumberOfProcessors,
                    (unsigned)si.dwActiveProcessorMask),
             "%u\t0x%x", si.dwNumberOfProcessors, (unsigned)si.dwActiveProcessorMask);
    each_cpu_rows();
    az_brief(az_add(AZ_CPU, "Architecture", "kernel32 GetSystemInfo", "%u (PROCESSOR_ARCHITECTURE_%s)",
                    si.wProcessorArchitecture, arch_name(si.wProcessorArchitecture)),
             "%s (%u)", arch_name(si.wProcessorArchitecture), si.wProcessorArchitecture);
    az_add(AZ_CPU, "Processor type", "kernel32 GetSystemInfo", "%u", si.dwProcessorType);
    az_add(AZ_CPU, "Processor level", "kernel32 GetSystemInfo", "%u", si.wProcessorLevel);
    /* for the Alpha the SDK reads the revision as xxyy: model 'A' + xx,
     * pass yy */
    az_brief(az_add(AZ_CPU, "Processor revision", "kernel32 GetSystemInfo",
                    "0x%04x (the SDK's Alpha reading: model %c, pass %u)", si.wProcessorRevision,
                    'A' + (si.wProcessorRevision >> 8), si.wProcessorRevision & 0xff),
             "model %c, pass %u", 'A' + (si.wProcessorRevision >> 8), si.wProcessorRevision & 0xff);

    /* the registry: every value of processor 0, the main ones of the rest */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor", 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        az_add(AZ_CPU, "CentralProcessor", "registry", "no such key");
        return;
    }
    for (i = 0;; i++) {
        len = sizeof sub;
        if (RegEnumKeyExA(key, i, sub, &len, 0, 0, 0, 0) != ERROR_SUCCESS) break;
        az_snprintf(path, sizeof path, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%s", sub);
        az_snprintf(prefix, sizeof prefix, "CPU %s: ", sub);
        reg_rows(AZ_CPU, path, prefix, first ? 0 : cp_some);
        first = 0;
    }
    RegCloseKey(key);
}

/* ---- the System page ---- */

static void system_rows(void)
{
    static const char *const nt_some[] = {"ProductName", "CurrentVersion", "CurrentBuildNumber", "CSDVersion",
                                          "CurrentType", 0};
    OSVERSIONINFOA v;
    MEMORYSTATUS m;
    char text[AZ_VALUE];
    reg_value(text, sizeof text, "HARDWARE\\DESCRIPTION\\System", "Identifier");
    az_add(AZ_SYSTEM, "Machine", "registry DESCRIPTION\\System", "%s", text);
    if (reg_rows(AZ_SYSTEM, "HARDWARE\\DESCRIPTION\\System", "", 0) < 0)
        az_add(AZ_SYSTEM, "HARDWARE\\DESCRIPTION\\System", "registry", "no such key");
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (GetVersionExA(&v))
        az_add(AZ_SYSTEM, "Windows version", "kernel32 GetVersionExA", "%u.%u build %u, platform %u%s%s", v.dwMajorVersion,
               v.dwMinorVersion, v.dwBuildNumber, v.dwPlatformId, v.szCSDVersion[0] ? ", " : "", v.szCSDVersion);
    reg_rows(AZ_SYSTEM, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "", nt_some);
    memset(&m, 0, sizeof m);
    m.dwLength = sizeof m;
    GlobalMemoryStatus(&m);
    az_brief(az_add(AZ_SYSTEM, "Physical memory", "kernel32 GlobalMemoryStatus", "%u KB, %u KB free (load %u%%)",
                    (unsigned)(m.dwTotalPhys / 1024), (unsigned)(m.dwAvailPhys / 1024), m.dwMemoryLoad),
             "%u KB\t%u KB\t%u%%", (unsigned)(m.dwTotalPhys / 1024), (unsigned)(m.dwAvailPhys / 1024), m.dwMemoryLoad);
    az_brief(az_add(AZ_SYSTEM, "Page file", "kernel32 GlobalMemoryStatus", "%u KB, %u KB free",
                    (unsigned)(m.dwTotalPageFile / 1024), (unsigned)(m.dwAvailPageFile / 1024)),
             "%u KB\t%u KB", (unsigned)(m.dwTotalPageFile / 1024), (unsigned)(m.dwAvailPageFile / 1024));
    az_brief(az_add(AZ_SYSTEM, "Virtual address space", "kernel32 GlobalMemoryStatus", "%u KB, %u KB free",
                    (unsigned)(m.dwTotalVirtual / 1024), (unsigned)(m.dwAvailVirtual / 1024)),
             "%u KB\t%u KB", (unsigned)(m.dwTotalVirtual / 1024), (unsigned)(m.dwAvailVirtual / 1024));
    az_add(AZ_SYSTEM, "Page size", "kernel32 GetSystemInfo", "%u bytes", si.dwPageSize);
    az_add(AZ_SYSTEM, "Allocation granularity", "kernel32 GetSystemInfo", "%u bytes", si.dwAllocationGranularity);
}

static void cache_rows(void)
{
    az_brief(az_add(AZ_CACHES, "What Windows reports", "", "no cache sizes: Windows 2000 has no call that gives them"),
             "no cache sizes");
    az_cache_rows();
}

static void about_rows(void)
{
    az_add(AZ_ABOUT, "AlphaZ", "", "version %s, for the Alpha guests of Alphabox", AZ_VERSION);
    az_add(AZ_ABOUT, "This program", "", "%s", AZ_THIS);
    az_add(AZ_ABOUT, "Compiled by", "", "nada at commit %s%s", AZ_NADA_COMMIT, AZ_NADA_DIRTY ? " (its tree had changes)" : "");
    az_add(AZ_ABOUT, "Licence", "", "GPL-2.0-or-later");
    az_add(AZ_ABOUT, "Copyright", "", "(C) 2026 Artur Goul\343o");
    az_add(AZ_ABOUT, "Source", "", "guest/alphaz in github.com/artur/alphabox");
    az_add(AZ_ABOUT, "Every value says", "", "where it came from: an instruction, an OS call, the registry,");
    az_add(AZ_ABOUT, "", "", "or a table value, which is what a document says, not this machine");
    az_add(AZ_ABOUT, "Keys", "", "Ctrl+Tab, Ctrl+Shift+Tab: next, previous tab; PgDn, PgUp: scroll; Esc: close");
    az_add(AZ_ABOUT, "Text report", "", "%s /report FILE writes every page into FILE", AZ_EXE);
}

static void gather(void)
{
    az_pages_init();
    az_probe();
    GetSystemInfo(&si);
    az_cpu.ncpus = (int)si.dwNumberOfProcessors;
    az_cpu.who = "kernel32 GetSystemInfo";
    cpu_rows();
    cache_rows();
    system_rows();
    if (az_page_shown(AZ_EV8)) az_ev8_rows();
    {
        LARGE_INTEGER f;
        HANDLE th = GetCurrentThread();
        DWORD_PTR old = SetThreadAffinityMask(th, 1);
        Sleep(0);
        if (!QueryPerformanceFrequency(&f)) f.QuadPart = 0;
#ifdef __nada_alpha__
        az_bench_all(qpc_now, (u64)f.QuadPart, 300);
#endif
        if (old) SetThreadAffinityMask(th, old);
    }
    az_bench_rows();
    about_rows();
}

/* ---- the text report ---- */

static void put_file(const char *line, void *ctx)
{
    fputs(line, (FILE *)ctx);
    fputs("\r\n", (FILE *)ctx);
}

static int report(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    fputs("AlphaZ " AZ_VERSION " report\r\n\r\n", f);
    az_report(put_file, f);
    fclose(f);
    return 0;
}

/* ---- the window ----
 * A tab a page, and a last tab, Details. A page is laid out as CPU-Z lays
 * out its own: group boxes (an etched frame, the caption in its top edge)
 * of fields (a label, right-aligned, and the value centred in a white
 * sunken box), each field a row's value or a part of its brief (az.h), so
 * the window shows what the report says and computes nothing of its own.
 * A table value is grey and italic; pointing at a field puts its source
 * in the status line at the page's foot, whose right end keeps the
 * legend. Details lists every row of every page with its source, as the
 * report does, and scrolls. Under the tabs, the footer: AlphaZ's teal
 * band, with the wordmark, the version and the build, and the Report and
 * Close buttons. alphaz_guest.sh finds the window by that band. */

#define AZ_TEAL RGB(0, 128, 128) /* one of the twenty static colours: solid at every depth */
#define AZ_FOOT_H 30             /* the footer's band */
#define AZ_LINE 22               /* a line of fields */
#define AZ_BOX_H 18              /* a field's box */
#define AZ_ROW_H 14              /* a line of Details */
#define AZ_STATUS_H 20
#define AZ_LEGEND 180            /* the status line's legend, at its right end */
#define AZ_DETAILS AZ_NPAGES     /* the Details tab, in tabs[] */
#define AZ_HITS 64
#define AZ_ID_REPORT 200
#define AZ_ID_CLOSE 201
#define AZ_CENTRE (DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS)
#ifndef BF_TOP
#define BF_TOP 0x0002
#endif

static HWND main_w, tab_w, page_w;
static HFONT font, italic, bold, mark, chip, huge;
static HBRUSH teal, white;
static int tabs[AZ_NPAGES + 1], ntabs, cur, top;

/* what the last paint drew where: each field's box, what it is, its
 * source and its row, for the status line */
static struct {
    RECT r;
    const char *what;
    const char *source;
    const struct az_row *row;
} hits[AZ_HITS];
static int nhits;
static int mouse_x = -1, mouse_y, tracking; /* the pointer on the page; mouse_x -1 when it is not */
static char note[MAX_PATH + 40];            /* the status line's message: where the Report button wrote */

static HDC dc;     /* the page's, while it paints */
static int pw, ph; /* the page's size */

static void put(const char *s, int x0, int y0, int x1, int y1, UINT how)
{
    RECT r;
    r.left = x0;
    r.top = y0;
    r.right = x1;
    r.bottom = y1;
    DrawTextA(dc, s, -1, &r, how | DT_NOPREFIX);
}

static int is_table(const char *source) { return !strncmp(source, "table value", 11); }

static void hit(RECT r, const char *what, const char *source, const struct az_row *row)
{
    if (nhits >= AZ_HITS) return;
    hits[nhits].r = r;
    hits[nhits].what = what;
    hits[nhits].source = source;
    hits[nhits].row = row;
    nhits++;
}

/* the field under (x, y), or -1 */
static int over(int x, int y)
{
    int i;
    if (x < 0) return -1;
    for (i = 0; i < nhits; i++)
        if (x >= hits[i].r.left && x < hits[i].r.right && y >= hits[i].r.top && y < hits[i].r.bottom) return i;
    return -1;
}

/* a group of n lines of fields from y, x to x1: an etched frame, its
 * caption in the top edge; returns the y under it. Line i's boxes are at
 * y + 17 + i * AZ_LINE (at()). */
static int group(const char *caption, int x, int y, int x1, int n)
{
    RECT r;
    char c[AZ_VALUE + 4];
    int y1 = y + 15 + n * AZ_LINE + 2;
    r.left = x;
    r.top = y + 6;
    r.right = x1;
    r.bottom = y1;
    DrawEdge(dc, &r, EDGE_ETCHED, BF_RECT);
    if (caption && caption[0]) {
        az_snprintf(c, sizeof c, " %s ", caption);
        SelectObject(dc, font);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        put(c, x + 6, y, x1 - 6, y + 13, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SetBkMode(dc, TRANSPARENT);
    }
    return y1 + 4;
}

static int at(int g, int i) { return g + 17 + i * AZ_LINE; }

static void label(const char *s, int lx, int x, int y)
{
    SelectObject(dc, font);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    put(s, lx, y, x - 5, y + AZ_BOX_H, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

/* a box of text, white and sunken: a table value's text grey and italic,
 * a MISMATCH red, the rest the window's text colour; recorded for the
 * status line */
static void box(int x, int y, int x1, int y1, const char *text, const char *what, const char *source,
                const struct az_row *row, UINT how)
{
    RECT r;
    r.left = x;
    r.top = y;
    r.right = x1;
    r.bottom = y1;
    FillRect(dc, &r, white);
    DrawEdge(dc, &r, BDR_SUNKENOUTER, BF_RECT);
    SelectObject(dc, is_table(source) ? italic : font);
    if (!strncmp(text, "MISMATCH", 8)) SetTextColor(dc, RGB(255, 0, 0));
    else SetTextColor(dc, GetSysColor(is_table(source) ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
    put(text, x + 3, y + 1, x1 - 3, y1 - 1, how);
    hit(r, what, source, row);
}

/* a field on line y: its label right-aligned from lx to the box, the box
 * from x to x1, its text part k of the row's brief (k -1: the row's whole
 * value); "not reported" when there is no such row */
static void field(const char *name, int lx, int x, int x1, int y, const struct az_row *row, int k)
{
    char text[AZ_VALUE];
    if (name[0]) label(name, lx, x, y);
    if (row && k < 0) az_snprintf(text, sizeof text, "%s", row->value);
    else if (row) az_brief_part(text, sizeof text, row, k);
    else az_snprintf(text, sizeof text, "not reported");
    box(x, y, x1, y + AZ_BOX_H, text, name[0] ? name : row ? row->label : "", row ? row->source : "nothing reports it",
        row, AZ_CENTRE);
}

/* the status line: the source of the field the pointer is on, else the
 * Report button's message, else what the page says to do; and at its
 * right end the legend, in the style it explains */
static void status_line(const char *say)
{
    RECT r;
    char text[AZ_LABEL + AZ_VALUE + AZ_SOURCE + 16];
    const char *what, *src;
    int y = ph - AZ_STATUS_H - 2, i = over(mouse_x, mouse_y);
    r.left = 4;
    r.top = y;
    r.right = pw - 4;
    r.bottom = y + AZ_STATUS_H;
    DrawEdge(dc, &r, BDR_SUNKENOUTER, BF_RECT);
    if (i >= 0) {
        for (what = hits[i].what; *what == ' '; what++) continue;
        src = hits[i].source[0] ? hits[i].source : "AlphaZ's own words";
        if (tabs[cur] == AZ_DETAILS)
            az_snprintf(text, sizeof text, "%s%s%s  [%s]", what, what[0] ? ": " : "", hits[i].row->value, src);
        else az_snprintf(text, sizeof text, "%s: %s", what, src);
    } else {
        az_snprintf(text, sizeof text, "%s", note[0] ? note : say);
    }
    SelectObject(dc, font);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    put(text, 10, y, pw - AZ_LEGEND - 8, y + AZ_STATUS_H, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, italic);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    put("table value: what a document says", pw - AZ_LEGEND, y, pw - 10, y + AZ_STATUS_H,
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

/* a teal tile and white text on it */
static RECT tile(int x, int y, int x1, int y1)
{
    RECT r;
    r.left = x;
    r.top = y;
    r.right = x1;
    r.bottom = y1;
    FillRect(dc, &r, teal);
    DrawEdge(dc, &r, BDR_SUNKENOUTER, BF_RECT);
    SetTextColor(dc, RGB(255, 255, 255));
    return r;
}

/* the CPU tab's emblem, drawn by AlphaZ (no maker's logo): the part
 * number and the core IMPLVER names, a table value as the part is */
static void emblem(int x, int y, int x1, int y1)
{
    const struct az_row *p = az_find(AZ_CPU, "Part (AlphaZ's reading)");
    const char *part = 0, *core = 0;
    RECT r = tile(x, y, x1, y1);
    if (az_find(AZ_CPU, "IMPLVER")) az_implver_chip(az_cpu.implver, &part, &core);
    SelectObject(dc, font);
    put("Alpha", x, y + 3, x1, y + 16, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, chip);
    put(part ? part : "?", x, y + 15, x1, y1 - 15, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, font);
    put(core ? core : "", x, y1 - 17, x1, y1 - 3, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    hit(r, "Emblem", p ? p->source : "table value: IMPLVER", p);
}

static void cpu_tab(void)
{
    const struct az_row *r, *which;
    const char *sep = " ";
    char list[AZ_VALUE + 16];
    int x = 4, x1 = pw - 4, half = pw / 2, R = x1 - 8, y = 2, g, i, n, ex;

    /* Processor: what it is, the emblem at the top right */
    g = y;
    y = group("Processor", x, g, x1, 6);
    ex = R - 86;
    emblem(ex, at(g, 0), R, at(g, 2) + AZ_BOX_H);
    field("Name", 10, 88, ex - 8, at(g, 0), az_find(AZ_CPU, "Name"), 0);
    field("Part", 10, 88, ex - 8, at(g, 1), az_find(AZ_CPU, "Part (AlphaZ's reading)"), 0);
    field("IMPLVER", 10, 88, 128, at(g, 2), az_find(AZ_CPU, "IMPLVER"), 0);
    field("Family", 132, 180, ex - 8, at(g, 2), az_find(AZ_CPU, "Family"), 0);
    field("Type", 10, 88, 160, at(g, 3), az_find(AZ_CPU, "Processor type"), 0);
    field("Level", 164, 206, 278, at(g, 3), az_find(AZ_CPU, "Processor level"), 0);
    field("Revision", 282, 334, 432, at(g, 3), az_find(AZ_CPU, "Processor revision"), 0);
    field("Architecture", 436, 506, R, at(g, 3), az_find(AZ_CPU, "Architecture"), 0);
    /* the AMASK features present, by name */
    r = az_find(AZ_CPU, "AMASK, features present");
    n = az_snprintf(list, sizeof list, "AMASK %s:", r ? r->value : "not run");
    for (i = 0; i < az_nfeatures; i++)
        if (r && (az_cpu.present >> az_features[i].bit & 1) && n < (int)sizeof list - 2) {
            n += az_snprintf(list + n, sizeof list - n, "%s%s", sep, az_features[i].brief);
            sep = ", ";
        }
    r = az_find(AZ_CPU, "AMASK, other bits");
    if (r && n < (int)sizeof list - 2) az_snprintf(list + n, sizeof list - n, "; other bits %s", r->value);
    label("Instructions", 10, 88, at(g, 4));
    r = az_find(AZ_CPU, "AMASK, features present");
    box(88, at(g, 4), R, at(g, 5) + AZ_BOX_H, list, "Instructions", r ? r->source : "", r,
        DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

    /* the clock, measured on processor 0; beside it the documented caches */
    g = y;
    group("Clocks (processor 0)", x, g, half - 2, 4);
    which = az_find(AZ_CPU, "  which is");
    field("Clock", 10, 98, which ? 178 : half - 10, at(g, 0), az_find(AZ_CPU, "Clock"), 0);
    if (which) field("", 0, 182, half - 10, at(g, 0), which, 0);
    field("Each window", 10, 98, half - 10, at(g, 1), az_find(AZ_CPU, "  windows"), 0);
    field("Second opinion", 10, 98, half - 10, at(g, 2), az_find(AZ_CPU, "Clock, second opinion"), 0);
    field("Counter rate", 10, 98, half - 10, at(g, 3), az_find(AZ_CPU, "Performance counter"), 0);
    y = group("Cache (documented)", half + 2, g, x1, 4);
    r = az_find(AZ_CACHES, "L1 data (documented)");
    field("L1 data", half + 8, half + 86, half + 186, at(g, 0), r, 0);
    field("", 0, half + 190, R, at(g, 0), r, 1);
    r = az_find(AZ_CACHES, "L1 instruction (documented)");
    field("L1 instruction", half + 8, half + 86, half + 186, at(g, 1), r, 0);
    field("", 0, half + 190, R, at(g, 1), r, 1);
    r = az_find(AZ_CACHES, "L2 (documented)");
    field("Level 2", half + 8, half + 86, half + 186, at(g, 2), r, 0);
    field("", 0, half + 190, R, at(g, 2), r, 1);
    field("Windows", half + 8, half + 86, R, at(g, 3), az_find(AZ_CACHES, "What Windows reports"), 0);

    /* how many, and whether each answered the same */
    g = y;
    group("", x, g, x1, 1);
    field("Processors", 10, 78, 116, at(g, 0), az_find(AZ_CPU, "Processors"), 0);
    r = az_find(AZ_CPU, "Every processor");
    if (r) field("Every processor", 120, 206, R, at(g, 0), r, 0);
    else {
        label("Every processor", 120, 206, at(g, 0));
        r = az_find(AZ_CPU, "Processor");
        box(206, at(g, 0), R, at(g, 0) + AZ_BOX_H, r ? "not the same on each: Details lists each" : "not reported",
            "Every processor", r ? r->source : "", r, AZ_CENTRE);
    }
    status_line("Point at a value for where it came from; Details lists every row");
}

static void caches_tab(void)
{
    static const char *const rows[2] = {"L1 instruction (documented)", "L1 data (documented)"};
    static const char *const caps[2] = {"Level 1 instruction", "Level 1 data"};
    const struct az_row *r, *more;
    char text[AZ_LABEL + AZ_VALUE + 4];
    const char *m;
    int x = 4, x1 = pw - 4, half = pw / 2, R = x1 - 8, y = 2, g, i, gx, gx1;

    for (i = 0; i < 2; i++) {
        gx = i ? half + 2 : x;
        gx1 = i ? x1 : half - 2;
        r = az_find(AZ_CACHES, rows[i]);
        g = group(caps[i], gx, y, gx1, 4);
        field("Size", gx + 6, gx + 70, gx1 - 8, at(y, 0), r, 0);
        field("Ways", gx + 6, gx + 70, gx1 - 8, at(y, 1), r, 1);
        field("Line", gx + 6, gx + 70, gx1 - 8, at(y, 2), r, 2);
        field("Where", gx + 6, gx + 70, gx1 - 8, at(y, 3), r, 3);
    }
    y = g;
    r = az_find(AZ_CACHES, "L2 (documented)");
    more = az_more(AZ_CACHES, r);
    g = y;
    y = group("Level 2", x, g, x1, more ? 3 : 2);
    field("Size", 10, 76, 200, at(g, 0), r, 0);
    field("Ways", 204, 248, 340, at(g, 0), r, 1);
    field("Line", 344, 384, R, at(g, 0), r, 2);
    field("Where", 10, 76, R, at(g, 1), r, 3);
    if (more) {
        for (m = more->label; *m == ' '; m++) continue;
        az_snprintf(text, sizeof text, "%s: %s", m, more->value);
        label("Also", 10, 76, at(g, 2));
        box(76, at(g, 2), R, at(g, 2) + AZ_BOX_H, text, "Also", more->source, more, AZ_CENTRE);
    }
    g = y;
    y = group("What the system reports", x, g, x1, 2);
    field("Windows", 10, 96, R, at(g, 0), az_find(AZ_CACHES, "What Windows reports"), -1);
    field("Timing probe", 10, 96, R, at(g, 1), az_find(AZ_CACHES, "Timing probe"), -1);
    status_line("Each size is the documented one of the part IMPLVER names");
}

static void system_tab(void)
{
    int x = 4, x1 = pw - 4, R = x1 - 8, y = 2, g;

    g = y;
    y = group("Machine", x, g, x1, 3);
    field("Identifier", 10, 96, R, at(g, 0), az_find(AZ_SYSTEM, "Machine"), 0);
    field("Firmware", 10, 96, 230, at(g, 1), az_find(AZ_SYSTEM, "SystemBiosVersion"), 0);
    field("Date", 234, 300, R, at(g, 1), az_find(AZ_SYSTEM, "SystemBiosDate"), 0);
    field("Physical bits", 10, 96, 176, at(g, 2), az_find(AZ_SYSTEM, "PhysicalAddressBits"), 0);
    field("Virtual bits", 180, 250, 330, at(g, 2), az_find(AZ_SYSTEM, "VirtualAddressBits"), 0);
    field("I/O access", 334, 400, R, at(g, 2), az_find(AZ_SYSTEM, "IOAccessMethod"), 0);

    g = y;
    y = group("Windows", x, g, x1, 2);
    field("Product", 10, 96, 320, at(g, 0), az_find(AZ_SYSTEM, "ProductName"), 0);
    field("Type", 324, 410, R, at(g, 0), az_find(AZ_SYSTEM, "CurrentType"), 0);
    field("Version", 10, 96, 320, at(g, 1), az_find(AZ_SYSTEM, "Windows version"), 0);
    field("Service pack", 324, 410, R, at(g, 1), az_find(AZ_SYSTEM, "CSDVersion"), 0);

    g = y;
    y = group("Memory", x, g, x1, 4);
    field("Physical", 10, 96, 200, at(g, 0), az_find(AZ_SYSTEM, "Physical memory"), 0);
    field("Free", 204, 270, 374, at(g, 0), az_find(AZ_SYSTEM, "Physical memory"), 1);
    field("Load", 378, 420, 480, at(g, 0), az_find(AZ_SYSTEM, "Physical memory"), 2);
    field("Page file", 10, 96, 200, at(g, 1), az_find(AZ_SYSTEM, "Page file"), 0);
    field("Free", 204, 270, 374, at(g, 1), az_find(AZ_SYSTEM, "Page file"), 1);
    field("Virtual", 10, 96, 200, at(g, 2), az_find(AZ_SYSTEM, "Virtual address space"), 0);
    field("Free", 204, 270, 374, at(g, 2), az_find(AZ_SYSTEM, "Virtual address space"), 1);
    field("Page size", 10, 96, 200, at(g, 3), az_find(AZ_SYSTEM, "Page size"), 0);
    field("Granularity", 204, 270, 374, at(g, 3), az_find(AZ_SYSTEM, "Allocation granularity"), 0);
    status_line("Point at a value for where it came from; Details lists every row");
}

static void ev8_tab(void)
{
    struct az_page *p = &az_pages[AZ_EV8];
    const struct az_row *check = az_find(AZ_EV8, "Self-check");
    char cap[AZ_VALUE + 16];
    int x = 4, x1 = pw - 4, R = x1 - 8, y = 2, g, i, k, n, cw, cx;

    g = y;
    y = group("The 21464's own", x, g, x1, 4);
    field("IMPLVER", 10, 146, R, at(g, 0), az_find(AZ_EV8, "IMPLVER"), 0);
    field("SMT (AMASK bit 10)", 10, 146, R, at(g, 1), az_find(AZ_EV8, "SMT"), 0);
    field("SIMD (AMASK bit 11)", 10, 146, R, at(g, 2), az_find(AZ_EV8, "SIMD and paired single"), 0);
    field("Tarantula (AMASK bit 3)", 10, 146, R, at(g, 3), az_find(AZ_EV8, "Tarantula"), 0);

    g = y;
    y = group("Threads", x, g, x1, 2);
    field("Threads the system runs", 10, 146, R, at(g, 0), az_find(AZ_EV8, "Threads the system runs"), 0);
    field("TPUs a chip", 10, 146, R, at(g, 1), az_find(AZ_EV8, "TPUs a chip"), 0);

    /* the self-check: its result the caption, a field a case, three a line */
    k = check ? (int)(check - p->rows) + 1 : p->n;
    n = p->n - k;
    g = y;
    if (n <= 0) {
        y = group("Self-check", x, g, x1, 1);
        field("Self-check", 10, 146, R, at(g, 0), check, 0);
    } else {
        az_snprintf(cap, sizeof cap, "Self-check: %s", check->value);
        y = group(cap, x, g, x1, (n + 2) / 3);
        cw = (x1 - x - 12) / 3;
        for (i = 0; i < n; i++) {
            cx = x + 6 + i % 3 * cw;
            field(p->rows[k + i].label, cx, cx + 64, cx + cw - 6, at(g, i / 3), &p->rows[k + i], 0);
        }
    }
    status_line("Point at a case for its instruction; Details has its result in full");
}

static void bench_tab(void)
{
    struct az_page *p = &az_pages[AZ_BENCH];
    const struct az_row *r;
    int x = 4, x1 = pw - 4, half = pw / 2, R = x1 - 8, y = 2, g = 2, i, n = 0, gx, gx1;

    /* a group a loop, two a line */
    for (i = 0; i < p->n; i++) {
        r = &p->rows[i];
        if (strcmp(r->source, "measured: OS timer and RPCC")) continue;
        gx = n % 2 ? half + 2 : x;
        gx1 = n % 2 ? x1 : half - 2;
        g = 2 + n / 2 * (15 + 3 * AZ_LINE + 6);
        y = group(r->label, gx, g, gx1, 3);
        field("Rounds a second", gx + 6, gx + 98, gx1 - 8, at(g, 0), r, 0);
        field("Cycles a round", gx + 6, gx + 98, gx + 150, at(g, 1), r, 1);
        field("Ops a round", gx + 152, gx + 218, gx1 - 8, at(g, 1), r, 2);
        field("Check value", gx + 6, gx + 98, gx1 - 8, at(g, 2), r, 3);
        n++;
    }
    if (n == 0) {
        g = y;
        y = group("Loops", x, g, x1, 1);
        field("Loops", 10, 120, R, at(g, 0), az_find(AZ_BENCH, "Loops"), 0);
    }
    g = y;
    y = group("What these are", x, g, x1, 3);
    field("How", 10, 120, R, at(g, 0), az_find(AZ_BENCH, "How"), 0);
    field("What they are", 10, 120, R, at(g, 1), az_find(AZ_BENCH, "What the figures are"), 0);
    field("What they are not", 10, 120, R, at(g, 2), az_find(AZ_BENCH, "What they are not"), 0);
    status_line("This guest's measurement of AlphaZ's loops, not a claim about Alphabox");
}

static void about_tab(void)
{
    struct az_page *p = &az_pages[AZ_ABOUT];
    const struct az_row *r;
    int x = 4, x1 = pw - 4, R = x1 - 8, y = 2, g, gx = 140;
    RECT t = tile(20, 8, 120, 108);

    /* AlphaZ's tile and wordmark */
    SelectObject(dc, huge);
    put("Z", t.left, t.top, t.right, t.bottom, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    hit(t, "AlphaZ", "", 0);
    SetTextColor(dc, AZ_TEAL);
    SelectObject(dc, mark);
    put("AlphaZ", 4, 110, 136, 132, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, font);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    put("Ver. " AZ_VERSION ", " AZ_PE, 4, 132, 136, 146, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    g = y;
    y = group("AlphaZ", gx, g, x1, 6);
    field("Version", gx + 6, gx + 84, R, at(g, 0), az_find(AZ_ABOUT, "AlphaZ"), 0);
    field("This program", gx + 6, gx + 84, R, at(g, 1), az_find(AZ_ABOUT, "This program"), 0);
    field("Compiled by", gx + 6, gx + 84, R, at(g, 2), az_find(AZ_ABOUT, "Compiled by"), 0);
    field("Licence", gx + 6, gx + 84, R, at(g, 3), az_find(AZ_ABOUT, "Licence"), 0);
    field("Copyright", gx + 6, gx + 84, R, at(g, 4), az_find(AZ_ABOUT, "Copyright"), 0);
    field("Source", gx + 6, gx + 84, R, at(g, 5), az_find(AZ_ABOUT, "Source"), 0);

    g = y;
    y = group("Reading it", x, g, x1, 5);
    r = az_find(AZ_ABOUT, "Every value says");
    field("Every value says", 10, 110, R, at(g, 0), r, 0);
    if (r && r + 1 < p->rows + p->n) field("", 10, 110, R, at(g, 1), r + 1, 0);
    field("Keys", 10, 110, R, at(g, 2), az_find(AZ_ABOUT, "Keys"), 0);
    field("Text report", 10, 110, R, at(g, 3), az_find(AZ_ABOUT, "Text report"), 0);
    label("This window", 10, 110, at(g, 4));
    box(110, at(g, 4), R, at(g, 4) + AZ_BOX_H,
        "point at a value for its source; Details lists every row; Report writes the report", "This window", "", 0,
        AZ_CENTRE);
    status_line("AlphaZ " AZ_VERSION ", " AZ_EXE);
}

/* ---- Details: every row of every page, as the report has them ---- */

static int detail_count(void)
{
    int i, n = 0;
    for (i = 0; i < AZ_NPAGES; i++)
        if (az_page_shown(i)) n += 1 + az_pages[i].n;
    return n;
}

static int detail_visible(void)
{
    RECT r;
    GetClientRect(page_w, &r);
    return (r.bottom - AZ_STATUS_H - 12) / AZ_ROW_H;
}

static void details_tab(void)
{
    RECT r;
    const struct az_row *row;
    char say[96];
    int i, k, line = 0, y = 4, vis = detail_visible(), last = top, c1 = 150, c2 = pw - 176, table;
    for (i = 0; i < AZ_NPAGES; i++) {
        struct az_page *p = &az_pages[i];
        if (!az_page_shown(i)) continue;
        for (k = -1; k < p->n; k++, line++) {
            if (line < top || line >= top + vis) continue;
            r.left = 4;
            r.top = y;
            r.right = pw - 4;
            r.bottom = y + AZ_ROW_H;
            if (k < 0) {
                /* the page's title and what it is, over an etched line */
                SelectObject(dc, bold);
                SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
                put(p->title, 8, y, 80, y + AZ_ROW_H, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                SelectObject(dc, font);
                put(p->banner, 80, y, pw - 6, y + AZ_ROW_H, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                r.top = y + AZ_ROW_H - 1;
                r.bottom = r.top + 2;
                DrawEdge(dc, &r, EDGE_ETCHED, BF_TOP);
            } else {
                row = &p->rows[k];
                table = is_table(row->source);
                SelectObject(dc, font);
                SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
                put(row->label, 8, y, c1, y + AZ_ROW_H, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(dc, table ? italic : font);
                SetTextColor(dc, GetSysColor(table ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
                put(row->value, c1 + 4, y, row->source[0] ? c2 - 4 : pw - 6, y + AZ_ROW_H,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                put(row->source, c2, y, pw - 6, y + AZ_ROW_H, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                hit(r, row->label, row->source, row);
            }
            y += AZ_ROW_H;
            last = line + 1;
        }
    }
    az_snprintf(say, sizeof say, "Lines %d-%d of %d: PgDn, PgUp; point at a row for all of it", top + 1, last,
                detail_count());
    status_line(say);
}

/* ---- the page, the tabs, the footer ---- */

static void paint_page(HWND h)
{
    PAINTSTRUCT ps;
    RECT cl;
    dc = BeginPaint(h, &ps);
    GetClientRect(h, &cl);
    pw = cl.right;
    ph = cl.bottom;
    SetBkMode(dc, TRANSPARENT);
    nhits = 0;
    switch (tabs[cur]) {
    case AZ_CPU: cpu_tab(); break;
    case AZ_CACHES: caches_tab(); break;
    case AZ_SYSTEM: system_tab(); break;
    case AZ_EV8: ev8_tab(); break;
    case AZ_BENCH: bench_tab(); break;
    case AZ_ABOUT: about_tab(); break;
    default: details_tab(); break;
    }
    EndPaint(h, &ps);
    dc = 0;
}

static void redraw_status(void)
{
    RECT r;
    GetClientRect(page_w, &r);
    r.top = r.bottom - AZ_STATUS_H - 4;
    InvalidateRect(page_w, &r, TRUE);
}

/* Details scrolls; the other tabs fit, and their range of 0 hides the
 * scroll bar */
static void scroll_to(int t)
{
    int max = tabs[cur] == AZ_DETAILS ? detail_count() - detail_visible() : 0;
    if (max < 0) max = 0;
    if (t > max) t = max;
    if (t < 0) t = 0;
    top = t;
    SetScrollRange(page_w, SB_VERT, 0, max, FALSE);
    SetScrollPos(page_w, SB_VERT, top, TRUE);
    InvalidateRect(page_w, 0, TRUE);
}

static void select_tab(int i)
{
    cur = (i % ntabs + ntabs) % ntabs;
    SendMessageA(tab_w, TCM_SETCURSEL, cur, 0);
    note[0] = 0;
    scroll_to(0);
}

static LRESULT WINAPI page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        paint_page(h);
        return 0;
    }
    if (msg == WM_VSCROLL) {
        int code = LOWORD(wp), vis = detail_visible();
        if (code == SB_LINEUP) scroll_to(top - 1);
        if (code == SB_LINEDOWN) scroll_to(top + 1);
        if (code == SB_PAGEUP) scroll_to(top - vis);
        if (code == SB_PAGEDOWN) scroll_to(top + vis);
        if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) scroll_to(HIWORD(wp));
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        int was = over(mouse_x, mouse_y);
        mouse_x = (short)LOWORD(lp);
        mouse_y = (short)HIWORD(lp);
        if (!tracking) {
            /* WM_MOUSELEAVE when the pointer leaves the page */
            az_trackmouse t;
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            t.dwHoverTime = 0;
            tracking = TrackMouseEvent(&t);
        }
        if (over(mouse_x, mouse_y) != was) redraw_status();
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        int was = over(mouse_x, mouse_y);
        tracking = 0;
        mouse_x = -1;
        if (was >= 0) redraw_status();
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* the footer: the teal band, AlphaZ's wordmark, the version and the build */
static void paint_footer(HWND h)
{
    PAINTSTRUCT ps;
    RECT cl, r;
    HDC d = BeginPaint(h, &ps);
    GetClientRect(h, &cl);
    r = cl;
    r.top = cl.bottom - AZ_FOOT_H;
    FillRect(d, &r, teal);
    SetBkMode(d, TRANSPARENT);
    SetTextColor(d, RGB(255, 255, 255));
    SelectObject(d, mark);
    r.left = 12;
    r.right = 96;
    DrawTextA(d, "AlphaZ", -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(d, font);
    r.left = 96;
    r.right = cl.right / 2 - 16;
    DrawTextA(d, "Ver. " AZ_VERSION " \267 " AZ_PE, -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    EndPaint(h, &ps);
}

/* the Report button: the report beside the program, ALPHAZ.TXT, or in the
 * temporary directory when that cannot be written; the status line says
 * where */
static void report_button(void)
{
    char path[MAX_PATH + 16], *p;
    DWORD n = GetModuleFileNameA(0, path, MAX_PATH);
    p = n && n < MAX_PATH ? strrchr(path, '\\') : 0;
    if (p) strcpy(p + 1, "ALPHAZ.TXT");
    if (!p || report(path)) {
        n = GetTempPathA(MAX_PATH, path);
        if (n && n < MAX_PATH) strcpy(path + n, "ALPHAZ.TXT");
        if (!n || n >= MAX_PATH || report(path)) {
            az_snprintf(note, sizeof note, "The report could not be written, beside %s or in the temporary directory",
                        AZ_EXE);
            InvalidateRect(page_w, 0, TRUE);
            return;
        }
    }
    az_snprintf(note, sizeof note, "The report is in %s", path);
    InvalidateRect(page_w, 0, TRUE);
}

static void layout(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    MoveWindow(tab_w, 6, 6, r.right - 12, r.bottom - AZ_FOOT_H - 12, TRUE);
    MoveWindow(GetDlgItem(h, AZ_ID_CLOSE), r.right - 84, r.bottom - AZ_FOOT_H + 4, 76, AZ_FOOT_H - 8, TRUE);
    MoveWindow(GetDlgItem(h, AZ_ID_REPORT), r.right - 166, r.bottom - AZ_FOOT_H + 4, 76, AZ_FOOT_H - 8, TRUE);
    r.left = 6;
    r.top = 6;
    r.right -= 6;
    r.bottom -= AZ_FOOT_H + 6;
    SendMessageA(tab_w, TCM_ADJUSTRECT, FALSE, (LPARAM)&r);
    MoveWindow(page_w, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}

static HWND button(HWND parent, const char *text, int id)
{
    HWND b = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                             parent, (HMENU)(UINT_PTR)id, self, 0);
    SendMessageA(b, WM_SETFONT, (WPARAM)font, 0);
    return b;
}

static LRESULT WINAPI main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE) {
        TCITEMA it;
        int i;
        tab_w = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 10,
                                10, h, (HMENU)100, self, 0);
        SendMessageA(tab_w, WM_SETFONT, (WPARAM)font, 0);
        for (i = 0; i < ntabs; i++) {
            memset(&it, 0, sizeof it);
            it.mask = TCIF_TEXT;
            it.pszText = (LPSTR)(tabs[i] == AZ_DETAILS ? "Details" : az_pages[tabs[i]].title);
            SendMessageA(tab_w, TCM_INSERTITEMA, i, (LPARAM)&it);
        }
        page_w = CreateWindowExA(0, "AlphaZPage", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 10, 10, h, (HMENU)101,
                                 self, 0);
        button(h, "Report", AZ_ID_REPORT);
        button(h, "Close", AZ_ID_CLOSE);
        layout(h);
        return 0;
    }
    if (msg == WM_PAINT) {
        paint_footer(h);
        return 0;
    }
    if (msg == WM_COMMAND) {
        if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == AZ_ID_CLOSE) DestroyWindow(h);
        if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == AZ_ID_REPORT) report_button();
        return 0;
    }
    if (msg == WM_NOTIFY) {
        NMHDR *n = (NMHDR *)lp;
        if (n->hwndFrom == tab_w && n->code == TCN_SELCHANGE) {
            cur = (int)SendMessageA(tab_w, TCM_GETCURSEL, 0, 0);
            note[0] = 0;
            scroll_to(0);
        }
        return 0;
    }
    if (msg == WM_TIMER) {
        if (wp == 1) DestroyWindow(h);
        if (wp == 2) select_tab(cur + 1);
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(h);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static FILE *keylog;

/* The keys the window answers, wherever the focus is: true when taken.
 * PgDn, PgUp, Up and Down scroll Details, the one tab that scrolls. */
static int key(MSG *m)
{
    int vis;
    if (m->message != WM_KEYDOWN) return 0;
    if (keylog) {
        fprintf(keylog, "key %u ctrl %d shift %d\n", (unsigned)m->wParam, GetKeyState(VK_CONTROL), GetKeyState(VK_SHIFT));
        fflush(keylog);
    }
    vis = detail_visible();
    switch (m->wParam) {
    case VK_TAB:
        /* the key is down when the SHORT's bit 15 is set, which is how
         * the call documents it, so it is tested as that bit. (On the
         * Alpha, Windows sign-extends $0 itself; nada's x86 builds now
         * extend a short result where the ABI leaves it to the caller,
         * so the test no longer depends on either.) */
        if (!(GetKeyState(VK_CONTROL) & 0x8000)) return 0;
        select_tab(cur + (GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1));
        return 1;
    case VK_ESCAPE:
        DestroyWindow(main_w);
        return 1;
    }
    if (tabs[cur] != AZ_DETAILS) return 0;
    switch (m->wParam) {
    case VK_NEXT:
        scroll_to(top + vis);
        return 1;
    case VK_PRIOR:
        scroll_to(top - vis);
        return 1;
    case VK_DOWN:
        scroll_to(top + 1);
        return 1;
    case VK_UP:
        scroll_to(top - 1);
        return 1;
    }
    return 0;
}

static int window(int close_s, int tour_s)
{
    static WNDCLASSA c, pc;
    az_logfont lf;
    MSG m;
    int i, w = 624, h = 444, x, y;

    for (i = 0; i < AZ_NPAGES; i++)
        if (az_page_shown(i)) tabs[ntabs++] = i;
    tabs[ntabs++] = AZ_DETAILS;
    InitCommonControls();
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    /* the table values' italic: the system's font, made italic */
    memset(&lf, 0, sizeof lf);
    if (GetObjectA(font, sizeof lf, &lf)) {
        lf.lfItalic = 1;
        italic = CreateFontIndirectA(&lf);
    }
    if (!italic) italic = font;
    bold = CreateFontA(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Tahoma");
    mark = CreateFontA(-20, 0, 0, 0, FW_BOLD, 1, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Tahoma");
    chip = CreateFontA(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Tahoma");
    huge = CreateFontA(-88, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Tahoma");
    if (!bold) bold = font;
    if (!mark) mark = font;
    if (!chip) chip = font;
    if (!huge) huge = font;
    teal = CreateSolidBrush(AZ_TEAL);
    white = GetSysColorBrush(COLOR_WINDOW);

    c.lpfnWndProc = main_proc;
    c.hInstance = self;
    c.hIcon = LoadIconA(self, MAKEINTRESOURCE(1));
    if (!c.hIcon) c.hIcon = LoadIconA(0, IDI_APPLICATION);
    c.hCursor = LoadCursorA(0, IDC_ARROW);
    c.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    c.lpszClassName = "AlphaZ";
    pc.style = CS_HREDRAW | CS_VREDRAW;
    pc.lpfnWndProc = page_proc;
    pc.hInstance = self;
    pc.hCursor = LoadCursorA(0, IDC_ARROW);
    pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    pc.lpszClassName = "AlphaZPage";
    if (!RegisterClassA(&c) || !RegisterClassA(&pc)) return 1;

    x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - 28 - h) / 2; /* above a taskbar */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    main_w = CreateWindowExA(WS_EX_CONTROLPARENT, "AlphaZ", "AlphaZ " AZ_VERSION,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, x, y, w, h, 0, 0,
                             self, 0);
    if (!main_w) return 1;
    ShowWindow(main_w, SW_SHOW);
    UpdateWindow(main_w);
    SetForegroundWindow(main_w);
    BringWindowToTop(main_w);
    SetFocus(tab_w);
    if (close_s > 0) SetTimer(main_w, 1, (UINT)close_s * 1000, 0);
    if (tour_s > 0) SetTimer(main_w, 2, (UINT)tour_s * 1000, 0);
    scroll_to(0);

    while (GetMessageA(&m, 0, 0, 0) > 0) {
        if (key(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i, close_s = 0, tour_s = 0;
    const char *out = 0;
    self = GetModuleHandleA(0);
    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/report") && i + 1 < argc) out = argv[++i];
        else if (!_stricmp(argv[i], "/close") && i + 1 < argc) close_s = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "/tour") && i + 1 < argc) tour_s = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "/keylog") && i + 1 < argc) keylog = fopen(argv[++i], "w");
    }
    gather();
    if (out) return report(out);
    return window(close_s, tour_s);
}
