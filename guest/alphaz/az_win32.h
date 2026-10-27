/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AlphaZ: the Win32 declarations ALPHAZ.EXE needs beyond nada's
 * include/windows/ (windows.h, commctrl.h).
 * Copyright (C) 2026 Artur Goulão
 *
 * Written from the Win32 documentation. The constants are guarded, so one
 * that nada's headers come to define is taken from there; the structures
 * have AlphaZ's own names, so a later NMHDR, TCITEMA, LOGFONTA or
 * TRACKMOUSEEVENT in nada's headers cannot collide with them. Each
 * function named here is imported through this directory's .def files. */
#ifndef AZ_WIN32_H
#define AZ_WIN32_H

#include <windows.h>
#include <commctrl.h>

/* ---- user32 ---- */
BOOL WINAPI InvalidateRect(HWND w, const RECT *r, BOOL erase);
SHORT WINAPI GetKeyState(int key);
HWND WINAPI SetFocus(HWND w);
int WINAPI GetSystemMetrics(int index);
BOOL WINAPI SetForegroundWindow(HWND w);
BOOL WINAPI BringWindowToTop(HWND w);
BOOL WINAPI SetScrollRange(HWND w, int bar, int lo, int hi, BOOL redraw);
int WINAPI SetScrollPos(HWND w, int bar, int pos, BOOL redraw);
BOOL WINAPI DrawEdge(HDC dc, RECT *r, UINT edge, UINT flags);

/* the pointer leaving a window: WM_MOUSELEAVE, once per request */
typedef struct {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
} az_trackmouse;
BOOL WINAPI TrackMouseEvent(az_trackmouse *t);
#ifndef TME_LEAVE
#define TME_LEAVE 0x0002
#endif
#ifndef WM_MOUSEMOVE
#define WM_MOUSEMOVE 0x0200
#endif
#ifndef WM_MOUSELEAVE
#define WM_MOUSELEAVE 0x02A3
#endif
#ifndef BN_CLICKED
#define BN_CLICKED 0
#endif

/* DrawEdge's edges and sides */
#ifndef BDR_SUNKENOUTER
#define BDR_RAISEDOUTER 0x0001
#define BDR_SUNKENOUTER 0x0002
#define BDR_RAISEDINNER 0x0004
#define BDR_SUNKENINNER 0x0008
#endif
#ifndef EDGE_ETCHED
#define EDGE_ETCHED (BDR_SUNKENOUTER | BDR_RAISEDINNER)
#endif
#ifndef BF_RECT
#define BF_RECT 0x000F
#endif

#ifndef SB_VERT
#define SB_VERT 1
#endif
#ifndef SB_LINEUP
#define SB_LINEUP 0
#define SB_LINEDOWN 1
#define SB_PAGEUP 2
#define SB_PAGEDOWN 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#endif
#ifndef WM_VSCROLL
#define WM_VSCROLL 0x0115
#endif

#ifndef SM_CXSCREEN
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#endif
#ifndef WM_NOTIFY
#define WM_NOTIFY 0x004E
#endif
#ifndef COLOR_GRAYTEXT
#define COLOR_GRAYTEXT 17
#endif
#ifndef DT_LEFT
#define DT_LEFT 0x0000
#endif
#ifndef DT_RIGHT
#define DT_RIGHT 0x0002
#endif
#ifndef DT_WORDBREAK
#define DT_WORDBREAK 0x0010
#endif
#ifndef DT_NOPREFIX
#define DT_NOPREFIX 0x0800
#endif
#ifndef DT_END_ELLIPSIS
#define DT_END_ELLIPSIS 0x8000
#endif
#ifndef WS_EX_CONTROLPARENT
#define WS_EX_CONTROLPARENT 0x00010000
#endif

/* ---- gdi32 ---- */
HFONT WINAPI CreateFontA(int height, int width, int escapement, int orientation, int weight, DWORD italic,
                         DWORD underline, DWORD strikeout, DWORD charset, DWORD outprec, DWORD clipprec,
                         DWORD quality, DWORD pitch, LPCSTR face);
#ifndef DEFAULT_GUI_FONT
#define DEFAULT_GUI_FONT 17
#endif

/* a font's description: GetObjectA fills one in, CreateFontIndirectA
 * makes the font (AlphaZ's italic is the system's font made italic) */
typedef struct {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    CHAR lfFaceName[32];
} az_logfont;
HFONT WINAPI CreateFontIndirectA(const az_logfont *f);
#ifndef FW_NORMAL
#define FW_NORMAL 400
#endif
#ifndef FW_BOLD
#define FW_BOLD 700
#endif

/* ---- comctl32: the tab control ---- */
typedef struct {
    HWND hwndFrom;
    UINT_PTR idFrom;
    UINT code;
} az_nmhdr;

typedef struct {
    UINT mask;
    DWORD dwState;
    DWORD dwStateMask;
    LPSTR pszText;
    int cchTextMax;
    int iImage;
    LPARAM lParam;
} az_tcitem;

#define AZ_WC_TABCONTROL "SysTabControl32"
#ifndef TCIF_TEXT
#define TCIF_TEXT 0x0001
#endif
#ifndef TCM_FIRST
#define TCM_FIRST 0x1300
#endif
#ifndef TCM_GETCURSEL
#define TCM_GETCURSEL (TCM_FIRST + 11)
#define TCM_SETCURSEL (TCM_FIRST + 12)
#define TCM_ADJUSTRECT (TCM_FIRST + 40)
#endif
#define AZ_TCM_INSERTITEMA (TCM_FIRST + 7)
#ifndef TCN_SELCHANGE
#define TCN_SELCHANGE ((UINT)(0U - 551U))
#endif

/* ---- kernel32 ---- */
void WINAPI Sleep(DWORD ms);
DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
BOOL WINAPI GetProcessAffinityMask(HANDLE process, PDWORD_PTR mask, PDWORD_PTR system);

/* ---- advapi32 ---- */
LONG WINAPI RegEnumValueA(HKEY key, DWORD i, LPSTR name, LPDWORD namelen, LPDWORD reserved, LPDWORD type,
                          LPBYTE data, LPDWORD datalen);
#ifndef REG_RESOURCE_LIST
#define REG_RESOURCE_LIST 8
#define REG_FULL_RESOURCE_DESCRIPTOR 9
#endif
#ifndef ERROR_NO_MORE_ITEMS
#define ERROR_NO_MORE_ITEMS 259
#endif

#endif
