/* d3d7.h -- the part of DirectDraw 7 and Direct3D 7 d3dcheck.c uses, for a
 * compiler with no DirectX SDK (nada). COM from C: an interface is a
 * pointer to a struct whose first member points to its method table, and a
 * method takes the interface as its first argument. Every table below
 * lists all of its methods in the SDK's order (ddraw.h, d3d.h, DirectX 7);
 * the ones d3dcheck does not call are plain pointers, kept only so the
 * ones it does call sit in the right slot. Windows NT for Alpha has one
 * calling convention, so there is no __stdcall to spell. */
#ifndef D3D7_H
#define D3D7_H

#include <windows.h>

typedef long HRESULT_T;
#define D3D_OK 0
#define FAILED_HR(h) ((HRESULT_T)(h) < 0)

typedef struct {
    unsigned long Data1;
    unsigned short Data2, Data3;
    unsigned char Data4[8];
} GUID_T;

/* ---- structures ---- */

typedef struct {
    DWORD dwCaps, dwCaps2, dwCaps3, dwCaps4;
} DDSCAPS2_T;

typedef struct {
    DWORD dwColorSpaceLowValue, dwColorSpaceHighValue;
} DDCOLORKEY_T;

typedef struct {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;     /* also dwZBufferBitDepth */
    DWORD dwRBitMask;        /* also dwStencilBitDepth */
    DWORD dwGBitMask;        /* also dwZBitMask */
    DWORD dwBBitMask;
    DWORD dwRGBAlphaBitMask;
} DDPIXELFORMAT_T;

typedef struct {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwHeight;
    DWORD dwWidth;
    long lPitch;
    DWORD dwBackBufferCount;
    DWORD dwMipMapCount;
    DWORD dwAlphaBitDepth;
    DWORD dwReserved;
    void *lpSurface;
    DDCOLORKEY_T ddckCKDestOverlay;
    DDCOLORKEY_T ddckCKDestBlt;
    DDCOLORKEY_T ddckCKSrcOverlay;
    DDCOLORKEY_T ddckCKSrcBlt;
    DDPIXELFORMAT_T ddpfPixelFormat;
    DDSCAPS2_T ddsCaps;
    DWORD dwTextureStage;
} DDSURFACEDESC2_T;

/* A pre-transformed, pre-lit vertex: D3DFVF_XYZRHW | DIFFUSE | SPECULAR | TEX1. */
typedef struct {
    float sx, sy, sz, rhw;
    DWORD color, specular;
    float tu, tv;
} TLVERTEX;

#define FVF_TLVERTEX (0x004 | 0x040 | 0x080 | 0x100)

/* The same with two texture coordinate sets: D3DFVF_TEX2. */
typedef struct {
    float sx, sy, sz, rhw;
    DWORD color, specular;
    float tu, tv, tu2, tv2;
} TLVERTEX2;

#define FVF_TLVERTEX2 (0x004 | 0x040 | 0x080 | 0x200)

/* ---- interfaces ---- */

typedef struct IDirectDraw7 IDirectDraw7;
typedef struct IDirectDrawSurface7 IDirectDrawSurface7;
typedef struct IDirect3D7 IDirect3D7;
typedef struct IDirect3DDevice7 IDirect3DDevice7;

typedef struct {
    HRESULT_T (*QueryInterface)(IDirectDraw7 *, const GUID_T *, void **);
    DWORD (*AddRef)(IDirectDraw7 *);
    DWORD (*Release)(IDirectDraw7 *);
    void *Compact, *CreateClipper, *CreatePalette;
    HRESULT_T (*CreateSurface)(IDirectDraw7 *, DDSURFACEDESC2_T *,
                               IDirectDrawSurface7 **, void *);
    void *DuplicateSurface, *EnumDisplayModes, *EnumSurfaces,
        *FlipToGDISurface;
    HRESULT_T (*GetCaps)(IDirectDraw7 *, void *, void *); /* DDCAPS, HAL/HEL */
    HRESULT_T (*GetDisplayMode)(IDirectDraw7 *, DDSURFACEDESC2_T *);
    HRESULT_T (*GetFourCCCodes)(IDirectDraw7 *, DWORD *, DWORD *);
    void *GetGDISurface, *GetMonitorFrequency, *GetScanLine,
        *GetVerticalBlankStatus, *Initialize, *RestoreDisplayMode;
    HRESULT_T (*SetCooperativeLevel)(IDirectDraw7 *, HWND, DWORD);
    void *SetDisplayMode, *WaitForVerticalBlank;
    HRESULT_T (*GetAvailableVidMem)(IDirectDraw7 *, DDSCAPS2_T *, DWORD *,
                                    DWORD *);
    void *GetSurfaceFromDC, *RestoreAllSurfaces, *TestCooperativeLevel,
        *GetDeviceIdentifier, *StartModeTest, *EvaluateMode;
} IDirectDraw7Vtbl;
struct IDirectDraw7 {
    const IDirectDraw7Vtbl *lpVtbl;
};

typedef struct {
    HRESULT_T (*QueryInterface)(IDirectDrawSurface7 *, const GUID_T *, void **);
    DWORD (*AddRef)(IDirectDrawSurface7 *);
    DWORD (*Release)(IDirectDrawSurface7 *);
    HRESULT_T (*AddAttachedSurface)(IDirectDrawSurface7 *,
                                    IDirectDrawSurface7 *);
    void *AddOverlayDirtyRect;
    HRESULT_T (*Blt)(IDirectDrawSurface7 *, RECT *, IDirectDrawSurface7 *,
                     RECT *, DWORD, void *);
    void *BltBatch, *BltFast,
        *DeleteAttachedSurface, *EnumAttachedSurfaces, *EnumOverlayZOrders,
        *Flip;
    HRESULT_T (*GetAttachedSurface)(IDirectDrawSurface7 *, DDSCAPS2_T *,
                                    IDirectDrawSurface7 **);
    void *GetBltStatus, *GetCaps, *GetClipper, *GetColorKey, *GetDC,
        *GetFlipStatus, *GetOverlayPosition, *GetPalette, *GetPixelFormat;
    HRESULT_T (*GetSurfaceDesc)(IDirectDrawSurface7 *, DDSURFACEDESC2_T *);
    void *Initialize, *IsLost;
    HRESULT_T (*Lock)(IDirectDrawSurface7 *, RECT *, DDSURFACEDESC2_T *, DWORD,
                      HANDLE);
    void *ReleaseDC, *Restore, *SetClipper;
    HRESULT_T (*SetColorKey)(IDirectDrawSurface7 *, DWORD, DDCOLORKEY_T *);
    void *SetOverlayPosition, *SetPalette;
    HRESULT_T (*Unlock)(IDirectDrawSurface7 *, RECT *);
    HRESULT_T (*UpdateOverlay)(IDirectDrawSurface7 *, RECT *,
                               IDirectDrawSurface7 *, RECT *, DWORD, void *);
    void *UpdateOverlayDisplay, *UpdateOverlayZOrder,
        *GetDDInterface, *PageLock, *PageUnlock, *SetSurfaceDesc,
        *SetPrivateData, *GetPrivateData, *FreePrivateData,
        *GetUniquenessValue, *ChangeUniquenessValue, *SetPriority,
        *GetPriority, *SetLOD, *GetLOD;
} IDirectDrawSurface7Vtbl;
struct IDirectDrawSurface7 {
    const IDirectDrawSurface7Vtbl *lpVtbl;
};

typedef HRESULT_T (*ENUMPIXFMT_CB)(DDPIXELFORMAT_T *, void *);

typedef struct {
    HRESULT_T (*QueryInterface)(IDirect3D7 *, const GUID_T *, void **);
    DWORD (*AddRef)(IDirect3D7 *);
    DWORD (*Release)(IDirect3D7 *);
    void *EnumDevices;
    HRESULT_T (*CreateDevice)(IDirect3D7 *, const GUID_T *,
                              IDirectDrawSurface7 *, IDirect3DDevice7 **);
    void *CreateVertexBuffer, *EnumZBufferFormats, *EvictManagedTextures;
} IDirect3D7Vtbl;
struct IDirect3D7 {
    const IDirect3D7Vtbl *lpVtbl;
};

typedef struct {
    HRESULT_T (*QueryInterface)(IDirect3DDevice7 *, const GUID_T *, void **);
    DWORD (*AddRef)(IDirect3DDevice7 *);
    DWORD (*Release)(IDirect3DDevice7 *);
    void *GetCaps;
    HRESULT_T (*EnumTextureFormats)(IDirect3DDevice7 *, ENUMPIXFMT_CB, void *);
    HRESULT_T (*BeginScene)(IDirect3DDevice7 *);
    HRESULT_T (*EndScene)(IDirect3DDevice7 *);
    void *GetDirect3D, *SetRenderTarget, *GetRenderTarget;
    HRESULT_T (*Clear)(IDirect3DDevice7 *, DWORD, void *, DWORD, DWORD, float,
                       DWORD);
    void *SetTransform, *GetTransform, *SetViewport, *MultiplyTransform,
        *GetViewport, *SetMaterial, *GetMaterial, *SetLight, *GetLight;
    HRESULT_T (*SetRenderState)(IDirect3DDevice7 *, DWORD, DWORD);
    void *GetRenderState, *BeginStateBlock, *EndStateBlock, *PreLoad;
    HRESULT_T (*DrawPrimitive)(IDirect3DDevice7 *, DWORD, DWORD, void *, DWORD,
                               DWORD);
    void *DrawIndexedPrimitive, *SetClipStatus, *GetClipStatus,
        *DrawPrimitiveStrided, *DrawIndexedPrimitiveStrided, *DrawPrimitiveVB,
        *DrawIndexedPrimitiveVB, *ComputeSphereVisibility, *GetTexture;
    HRESULT_T (*SetTexture)(IDirect3DDevice7 *, DWORD, IDirectDrawSurface7 *);
    void *GetTextureStageState;
    HRESULT_T (*SetTextureStageState)(IDirect3DDevice7 *, DWORD, DWORD, DWORD);
    HRESULT_T (*ValidateDevice)(IDirect3DDevice7 *, DWORD *);
    void *ApplyStateBlock, *CaptureStateBlock, *DeleteStateBlock,
        *CreateStateBlock, *Load, *LightEnable, *GetLightEnable,
        *SetClipPlane, *GetClipPlane, *GetInfo;
} IDirect3DDevice7Vtbl;
struct IDirect3DDevice7 {
    const IDirect3DDevice7Vtbl *lpVtbl;
};

typedef HRESULT_T (*DIRECTDRAWCREATEEX_FN)(GUID_T *, void **, const GUID_T *,
                                           void *);

/* ---- constants ---- */

#define DDSCL_NORMAL 0x00000008

#define DDSD_CAPS 0x00000001
#define DDSD_HEIGHT 0x00000002
#define DDSD_WIDTH 0x00000004
#define DDSD_PIXELFORMAT 0x00001000
#define DDSD_MIPMAPCOUNT 0x00020000

#define DDCKEY_SRCBLT 0x00000008

#define DDPF_ALPHAPIXELS 0x00000001
#define DDPF_FOURCC 0x00000004
#define DDPF_RGB 0x00000040
#define DDPF_ZBUFFER 0x00000400

#define DDSCAPS_COMPLEX 0x00000008
#define DDSCAPS_OVERLAY 0x00000080
#define DDSCAPS_PRIMARYSURFACE 0x00000200
#define DDSCAPS_OFFSCREENPLAIN 0x00000040
#define DDSCAPS_SYSTEMMEMORY 0x00000800
#define DDSCAPS_TEXTURE 0x00001000
#define DDSCAPS_3DDEVICE 0x00002000
#define DDSCAPS_VIDEOMEMORY 0x00004000
#define DDSCAPS_ZBUFFER 0x00020000
#define DDSCAPS_MIPMAP 0x00400000

#define DDLOCK_WAIT 0x00000001
#define DDBLT_WAIT 0x01000000
#define DDOVER_HIDE 0x00000200
#define DDOVER_SHOW 0x00004000
#define FOURCC_YUY2 0x32595559u
#define FOURCC_YV12 0x32315659u

void __stdcall Sleep(DWORD ms);
#define DDLOCK_READONLY 0x00000010

#define D3DCLEAR_TARGET 1
#define D3DCLEAR_ZBUFFER 2

#define D3DPT_TRIANGLELIST 4
#define D3DPT_TRIANGLESTRIP 5

/* render states */
#define RS_TEXTUREPERSPECTIVE 4
#define RS_ZENABLE 7
#define RS_SHADEMODE 9
#define RS_ZWRITEENABLE 14
#define RS_ALPHATESTENABLE 15
#define RS_SRCBLEND 19
#define RS_DESTBLEND 20
#define RS_CULLMODE 22
#define RS_ZFUNC 23
#define RS_ALPHAREF 24
#define RS_ALPHAFUNC 25
#define RS_DITHERENABLE 26
#define RS_ALPHABLENDENABLE 27
#define RS_FOGENABLE 28
#define RS_SPECULARENABLE 29
#define RS_FOGCOLOR 34
#define RS_FOGTABLEMODE 35
#define RS_COLORKEYENABLE 41
#define RS_LIGHTING 137

#define D3DSHADE_FLAT 1
#define D3DSHADE_GOURAUD 2
#define D3DCULL_NONE 1
#define D3DBLEND_ZERO 1
#define D3DBLEND_ONE 2
#define D3DBLEND_SRCALPHA 5
#define D3DBLEND_INVSRCALPHA 6
#define D3DCMP_LESSEQUAL 4
#define D3DCMP_GREATEREQUAL 7
#define D3DCMP_ALWAYS 8
#define D3DFOG_NONE 0

/* texture stage states */
#define TSS_COLOROP 1
#define TSS_COLORARG1 2
#define TSS_COLORARG2 3
#define TSS_ALPHAOP 4
#define TSS_ALPHAARG1 5
#define TSS_ALPHAARG2 6
#define TSS_ADDRESS 12
#define TSS_ADDRESSU 13
#define TSS_ADDRESSV 14
#define TSS_MAGFILTER 16
#define TSS_MINFILTER 17
#define TSS_MIPFILTER 18

#define D3DTOP_DISABLE 1
#define D3DTOP_SELECTARG1 2
#define D3DTOP_MODULATE 4
#define D3DTA_DIFFUSE 0
#define D3DTA_TEXTURE 2
#define D3DTADDRESS_WRAP 1
#define D3DTADDRESS_CLAMP 3
#define TSS_TEXCOORDINDEX 11
#define D3DTA_CURRENT 1
#define D3DTFG_POINT 1
#define D3DTFG_LINEAR 2
#define D3DTFN_POINT 1
#define D3DTFN_LINEAR 2
#define D3DTFP_NONE 1
#define D3DTFP_POINT 2
#define D3DTFP_LINEAR 3

#endif
