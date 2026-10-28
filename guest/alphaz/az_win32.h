/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AlphaZ: the Win32 declarations ALPHAZ.EXE needs that nada's
 * include/windows/ (windows.h, commctrl.h; nada b8db5784) still lacks:
 * DrawEdge and its BDR_, EDGE_ and BF_ values, TrackMouseEvent with its
 * structure and TME_LEAVE, WM_MOUSEMOVE, WM_MOUSELEAVE, BN_CLICKED, and
 * LOGFONT with CreateFontIndirectA. Everything else AlphaZ calls nada
 * declares, and this file does not repeat it.
 * Copyright (C) 2026 Artur Goulão
 *
 * Written from the Win32 documentation. The constants are guarded, so one
 * that nada's headers come to define is taken from there; the structures
 * have AlphaZ's own names, so a later TRACKMOUSEEVENT or LOGFONTA in
 * nada's headers cannot collide with them. Each function named here is
 * imported through this directory's .def files. */
#ifndef AZ_WIN32_H
#define AZ_WIN32_H

#include <windows.h>

/* ---- user32 ---- */
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

/* ---- gdi32 ---- */
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

#endif
