/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AlphaZ for Windows on the Alpha: ALPHAZ.EXE, a window with a tab for
 * each page of facts (az.h), or with /report FILE the same facts as text.
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
 * scroll a page longer than the window, Esc closes.
 *
 * Windows' own words come from GetSystemInfo, GlobalMemoryStatus,
 * GetVersionExA and the registry's HKLM\HARDWARE\DESCRIPTION\System; the
 * processor's from its instructions (az_core.c); the clock from RPCC
 * against QueryPerformanceCounter and against GetTickCount, with the
 * thread on processor 0 so that one counter is read throughout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "az_win32.h"
#include "az.h"
#include "az_build.h"

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
    char hz[32];
    HANDLE th = GetCurrentThread();
    DWORD_PTR old = SetThreadAffinityMask(th, 1);
    Sleep(0);
    if (!QueryPerformanceFrequency(&f)) f.QuadPart = 0;
    az_add(AZ_CPU, "Performance counter", "kernel32 QueryPerformanceFrequency", "%llu Hz", (u64)f.QuadPart);
#ifdef __nada_alpha__
    az_measure_clock(&c, qpc_now, (u64)f.QuadPart, 500);
    az_fmt_hz(hz, sizeof hz, c.hz);
    az_add(AZ_CPU, "Clock", "measured: RPCC against QueryPerformanceCounter", "%s", hz);
    az_add(AZ_CPU, "  how", "measured: 500 ms on processor 0",
           "%llu cycles in %llu counter ticks; low 32 bits wrapped %llu times; %llu reads", c.cycles, c.ticks,
           c.wraps, c.samples);
    az_measure_clock(&c, tick_now, 1000, 500);
    az_fmt_hz(hz, sizeof hz, c.hz);
    az_add(AZ_CPU, "Clock, second opinion", "measured: RPCC against GetTickCount", "%s (%llu cycles in %llu ms)", hz,
           c.cycles, c.ticks);
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
    az_add(AZ_CPU, "Processors", "kernel32 GetSystemInfo", "%u (active mask 0x%x)", si.dwNumberOfProcessors,
           (unsigned)si.dwActiveProcessorMask);
    each_cpu_rows();
    az_add(AZ_CPU, "Architecture", "kernel32 GetSystemInfo", "%u (PROCESSOR_ARCHITECTURE_%s)", si.wProcessorArchitecture,
           arch_name(si.wProcessorArchitecture));
    az_add(AZ_CPU, "Processor type", "kernel32 GetSystemInfo", "%u", si.dwProcessorType);
    az_add(AZ_CPU, "Processor level", "kernel32 GetSystemInfo", "%u", si.wProcessorLevel);
    /* for the Alpha the SDK reads the revision as xxyy: model 'A' + xx,
     * pass yy */
    az_add(AZ_CPU, "Processor revision", "kernel32 GetSystemInfo", "0x%04x (the SDK's Alpha reading: model %c, pass %u)",
           si.wProcessorRevision, 'A' + (si.wProcessorRevision >> 8), si.wProcessorRevision & 0xff);

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
    az_add(AZ_SYSTEM, "Physical memory", "kernel32 GlobalMemoryStatus", "%u KB, %u KB free (load %u%%)",
           (unsigned)(m.dwTotalPhys / 1024), (unsigned)(m.dwAvailPhys / 1024), m.dwMemoryLoad);
    az_add(AZ_SYSTEM, "Page file", "kernel32 GlobalMemoryStatus", "%u KB, %u KB free", (unsigned)(m.dwTotalPageFile / 1024),
           (unsigned)(m.dwAvailPageFile / 1024));
    az_add(AZ_SYSTEM, "Virtual address space", "kernel32 GlobalMemoryStatus", "%u KB, %u KB free",
           (unsigned)(m.dwTotalVirtual / 1024), (unsigned)(m.dwAvailVirtual / 1024));
    az_add(AZ_SYSTEM, "Page size", "kernel32 GetSystemInfo", "%u bytes", si.dwPageSize);
    az_add(AZ_SYSTEM, "Allocation granularity", "kernel32 GetSystemInfo", "%u bytes", si.dwAllocationGranularity);
}

static void cache_rows(void)
{
    az_add(AZ_CACHES, "What Windows reports", "", "no cache sizes: Windows 2000 has no call that gives them");
    az_cache_rows();
}

static void about_rows(void)
{
    az_add(AZ_ABOUT, "AlphaZ", "", "version %s, for the Alpha guests of Alphabox", AZ_VERSION);
    az_add(AZ_ABOUT, "This program", "", "ALPHAZ.EXE, Win32, PE32 for the Alpha (-t alpha-windows -mcpu=ev6)");
    az_add(AZ_ABOUT, "Compiled by", "", "nada at commit %s%s", AZ_NADA_COMMIT, AZ_NADA_DIRTY ? " (its tree had changes)" : "");
    az_add(AZ_ABOUT, "Licence", "", "GPL-2.0-or-later");
    az_add(AZ_ABOUT, "Copyright", "", "(C) 2026 Artur Goul\343o");
    az_add(AZ_ABOUT, "Source", "", "guest/alphaz in github.com/artur/alphabox");
    az_add(AZ_ABOUT, "Every value says", "", "where it came from: an instruction, an OS call, the registry,");
    az_add(AZ_ABOUT, "", "", "or a table value, which is what a document says, not this machine");
    az_add(AZ_ABOUT, "Keys", "", "Ctrl+Tab, Ctrl+Shift+Tab: next, previous tab; PgDn, PgUp: scroll; Esc: close");
    az_add(AZ_ABOUT, "Text report", "", "ALPHAZ.EXE /report FILE writes every page into FILE");
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

/* ---- the window ---- */

#define AZ_ROW_H 14
#define AZ_BAND_H 24
#define AZ_TEAL RGB(0, 128, 128) /* one of the twenty static colours: solid at every depth */

static HWND main_w, tab_w, page_w;
static HFONT font, bold;
static HBRUSH band;
static int shown[AZ_NPAGES], nshown, cur, top;

/* the rows that fit under the band, less one for the "rows a-b of n"
 * line when the page has more than fit */
static int visible_rows(void)
{
    RECT r;
    int fit;
    GetClientRect(page_w, &r);
    fit = (r.bottom - AZ_BAND_H - 6) / AZ_ROW_H;
    return az_pages[shown[cur]].n > fit ? fit - 1 : fit;
}

static void scroll_to(int t)
{
    int max = az_pages[shown[cur]].n - visible_rows();
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
    cur = (i % nshown + nshown) % nshown;
    SendMessageA(tab_w, TCM_SETCURSEL, cur, 0);
    scroll_to(0);
}

static void cell(HDC dc, const char *s, int x0, int x1, int y)
{
    RECT r;
    r.left = x0;
    r.right = x1;
    r.top = y;
    r.bottom = y + AZ_ROW_H;
    DrawTextA(dc, s, -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void paint_page(HWND h)
{
    PAINTSTRUCT ps;
    RECT cl, r;
    HDC dc = BeginPaint(h, &ps);
    struct az_page *p = &az_pages[shown[cur]];
    char more[64];
    int i, y, vis = visible_rows(), w;
    GetClientRect(h, &cl);
    w = cl.right;
    SetBkMode(dc, TRANSPARENT);

    /* the band: the page's name and what it is */
    r = cl;
    r.bottom = AZ_BAND_H;
    FillRect(dc, &r, band);
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, bold);
    cell(dc, p->title, 8, 80, 4);
    SelectObject(dc, font);
    cell(dc, p->banner, 80, w - 6, 4);

    y = AZ_BAND_H + 4;
    for (i = top; i < p->n && i < top + vis; i++) {
        struct az_row *row = &p->rows[i];
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        cell(dc, row->label, 8, 150, y);
        cell(dc, row->value, 154, row->source[0] ? w - 174 : w - 4, y);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        cell(dc, row->source, w - 170, w - 4, y);
        y += AZ_ROW_H;
    }
    if (p->n > vis) {
        az_snprintf(more, sizeof more, "rows %d-%d of %d: PgDn, PgUp", top + 1, i, p->n);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        r.left = 8;
        r.right = w - 8;
        r.top = cl.bottom - AZ_ROW_H - 2;
        r.bottom = cl.bottom - 2;
        DrawTextA(dc, more, -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    }
    EndPaint(h, &ps);
}

static LRESULT WINAPI page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        paint_page(h);
        return 0;
    }
    if (msg == WM_VSCROLL) {
        int code = LOWORD(wp), vis = visible_rows();
        if (code == SB_LINEUP) scroll_to(top - 1);
        if (code == SB_LINEDOWN) scroll_to(top + 1);
        if (code == SB_PAGEUP) scroll_to(top - vis);
        if (code == SB_PAGEDOWN) scroll_to(top + vis);
        if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) scroll_to(HIWORD(wp));
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void layout(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    MoveWindow(tab_w, 6, 6, r.right - 12, r.bottom - 12, TRUE);
    r.left = 6;
    r.top = 6;
    r.right -= 6;
    r.bottom -= 6;
    SendMessageA(tab_w, TCM_ADJUSTRECT, FALSE, (LPARAM)&r);
    MoveWindow(page_w, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}

static LRESULT WINAPI main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE) {
        az_tcitem it;
        int i;
        tab_w = CreateWindowExA(0, AZ_WC_TABCONTROL, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 10,
                                10, h, (HMENU)100, self, 0);
        SendMessageA(tab_w, WM_SETFONT, (WPARAM)font, 0);
        for (i = 0; i < nshown; i++) {
            memset(&it, 0, sizeof it);
            it.mask = TCIF_TEXT;
            it.pszText = (LPSTR)az_pages[shown[i]].title;
            SendMessageA(tab_w, AZ_TCM_INSERTITEMA, i, (LPARAM)&it);
        }
        page_w = CreateWindowExA(0, "AlphaZPage", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 10, 10, h, (HMENU)101,
                                 self, 0);
        layout(h);
        return 0;
    }
    if (msg == WM_NOTIFY) {
        az_nmhdr *n = (az_nmhdr *)lp;
        if (n->hwndFrom == tab_w && n->code == TCN_SELCHANGE) {
            cur = (int)SendMessageA(tab_w, TCM_GETCURSEL, 0, 0);
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

/* The keys the window answers, wherever the focus is: true when taken. */
static int key(MSG *m)
{
    int vis;
    if (m->message != WM_KEYDOWN) return 0;
    if (keylog) {
        fprintf(keylog, "key %u ctrl %d shift %d\n", (unsigned)m->wParam, GetKeyState(VK_CONTROL), GetKeyState(VK_SHIFT));
        fflush(keylog);
    }
    vis = visible_rows();
    switch (m->wParam) {
    case VK_TAB:
        /* the key is down when the SHORT's bit 15 is set: tested as a bit,
         * since the caller cannot count on the SHORT being sign-extended
         * (under Wine, nada's i386 build saw 0xff81 for a held Ctrl) */
        if (!(GetKeyState(VK_CONTROL) & 0x8000)) return 0;
        select_tab(cur + (GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1));
        return 1;
    case VK_ESCAPE:
        DestroyWindow(main_w);
        return 1;
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
    MSG m;
    int i, w = 624, h = 444, x, y;

    for (i = 0; i < AZ_NPAGES; i++)
        if (az_page_shown(i)) shown[nshown++] = i;
    InitCommonControls();
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    bold = CreateFontA(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Tahoma");
    if (!bold) bold = font;
    band = CreateSolidBrush(AZ_TEAL);

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
