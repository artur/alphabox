/* d3dcheck.c -- does a Direct3D 7 HAL draw what Microsoft's software
 * rasteriser draws?
 *
 * Each scene is rendered twice into a 256x256 16-bit surface: once by the
 * HAL device (the display card), once by the RGB device (d3dim700.dll's
 * software rasteriser, on the CPU). The two are compared pixel by pixel,
 * with a tolerance for the rounding two rasterisers do differently, and
 * both images are written as PPMs next to the program. A scene is "same"
 * with no pixel different, "close" when only a few are (edge pixels, which
 * each rasteriser rounds its own way), and "DIFF" otherwise. Output goes to
 * stdout, one line per step, so a crash shows where it happened.
 *
 * Built with nada, which has no DirectX SDK: d3d7.h declares the part of
 * DirectDraw 7 and Direct3D 7 used here.
 *
 * usage: d3dcheck [outdir]    (default: the current directory) */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "d3d7.h"

#define W 256
#define H 256
#define TOL 24 /* per channel, 0..255, before a pixel counts as different */
/* Two rasterisers disagree on pixels exactly on an edge -- a triangle's, or
 * a texel's under perspective -- each rounding its own way. A scene whose
 * differing pixels stay under this many is "close"; more is a real
 * difference. */
#define EDGE_PIXELS (W * H / 400)

static const GUID_T IID_IDirectDraw7 = {
    0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
static const GUID_T IID_IDirect3D7 = {
    0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
static const GUID_T IID_IDirect3DHALDevice = {
    0x84e63de0, 0x46aa, 0x11cf, {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
static const GUID_T IID_IDirect3DRGBDevice = {
    0xa4665c60, 0x2673, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};

static IDirectDraw7 *dd;
static IDirect3D7 *d3d;
static const char *outdir = ".";

#define SAY(...)                                                                \
    do {                                                                       \
        printf(__VA_ARGS__);                                                   \
        fflush(stdout);                                                        \
    } while (0)

/* One rendering path: a device with its target, Z buffer and textures. */
typedef struct {
    const char *name;
    int hal;
    IDirectDrawSurface7 *rt, *z;
    IDirect3DDevice7 *dev;
    IDirectDrawSurface7 *tex_checker, *tex_small, *tex_alpha;
} path_t;

static DWORD mem_caps(int hal)
{
    return hal ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY;
}

static IDirectDrawSurface7 *make_surface(DWORD caps, int w, int h,
                                         DDPIXELFORMAT_T *pf)
{
    DDSURFACEDESC2_T sd;
    IDirectDrawSurface7 *s = 0;
    HRESULT_T hr;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.dwWidth = w;
    sd.dwHeight = h;
    sd.ddsCaps.dwCaps = caps;
    if (pf) {
        sd.dwFlags |= DDSD_PIXELFORMAT;
        sd.ddpfPixelFormat = *pf;
    }
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &s, 0);
    if (FAILED_HR(hr)) {
        SAY("  CreateSurface caps %08lx %dx%d failed: %08lx\n",
            (unsigned long)caps, w, h, (unsigned long)hr);
        return 0;
    }
    return s;
}

static void pf_rgb(DDPIXELFORMAT_T *pf, int bits, DWORD r, DWORD g, DWORD b,
                   DWORD a)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
    pf->dwRGBBitCount = bits;
    pf->dwRBitMask = r;
    pf->dwGBitMask = g;
    pf->dwBBitMask = b;
    pf->dwRGBAlphaBitMask = a;
}

/* Fill a 16-bit texture through Lock; `texel` gives each one. */
static int fill_texture(IDirectDrawSurface7 *s, int w, int h,
                        unsigned (*texel)(int, int))
{
    DDSURFACEDESC2_T sd;
    int x, y;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    if (FAILED_HR(s->lpVtbl->Lock(s, 0, &sd, DDLOCK_WAIT, 0))) {
        SAY("  texture Lock failed\n");
        return 0;
    }
    for (y = 0; y < h; y++) {
        unsigned short *row =
            (unsigned short *)((char *)sd.lpSurface + y * sd.lPitch);
        for (x = 0; x < w; x++)
            row[x] = (unsigned short)texel(x, y);
    }
    s->lpVtbl->Unlock(s, 0);
    return 1;
}

/* 64x64 565: 8x8-texel red/white squares, a blue stripe along the top row
 * of squares so orientation shows. */
static unsigned texel_checker(int x, int y)
{
    if (y < 8)
        return ((x >> 3) & 1) ? 0x001f : 0xffff;
    return (((x >> 3) ^ (y >> 3)) & 1) ? 0xf800 : 0xffff;
}

/* 8x8 565: four coloured quadrants, for bilinear magnification. */
static unsigned texel_small(int x, int y)
{
    static const unsigned q[4] = {0xf800, 0x07e0, 0x001f, 0xffe0};
    return q[(x >= 4) + 2 * (y >= 4)];
}

/* 64x64 1555: a checker of opaque green and transparent black. */
static unsigned texel_alpha(int x, int y)
{
    return (((x >> 3) ^ (y >> 3)) & 1) ? 0x83e0 : 0x0000;
}

static IDirectDrawSurface7 *make_texture(int hal, int w, int h, int alpha,
                                         unsigned (*texel)(int, int))
{
    DDPIXELFORMAT_T pf;
    IDirectDrawSurface7 *s;
    if (alpha)
        pf_rgb(&pf, 16, 0x7c00, 0x03e0, 0x001f, 0x8000);
    else
        pf_rgb(&pf, 16, 0xf800, 0x07e0, 0x001f, 0);
    s = make_surface(DDSCAPS_TEXTURE | mem_caps(hal), w, h, &pf);
    if (s && !fill_texture(s, w, h, texel))
        return 0;
    return s;
}

static int open_path(path_t *p)
{
    DDPIXELFORMAT_T zpf;
    HRESULT_T hr;
    SAY("%s: render target\n", p->name);
    p->rt = make_surface(DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE |
                             mem_caps(p->hal),
                         W, H, 0);
    if (!p->rt)
        return 0;
    memset(&zpf, 0, sizeof zpf);
    zpf.dwSize = sizeof zpf;
    zpf.dwFlags = DDPF_ZBUFFER;
    zpf.dwRGBBitCount = 16; /* dwZBufferBitDepth */
    zpf.dwGBitMask = 0xffff; /* dwZBitMask */
    SAY("%s: Z buffer\n", p->name);
    p->z = make_surface(DDSCAPS_ZBUFFER | mem_caps(p->hal), W, H, &zpf);
    if (!p->z)
        return 0;
    hr = p->rt->lpVtbl->AddAttachedSurface(p->rt, p->z);
    if (FAILED_HR(hr)) {
        SAY("  AddAttachedSurface failed: %08lx\n", (unsigned long)hr);
        return 0;
    }
    SAY("%s: CreateDevice\n", p->name);
    hr = d3d->lpVtbl->CreateDevice(
        d3d, p->hal ? &IID_IDirect3DHALDevice : &IID_IDirect3DRGBDevice, p->rt,
        &p->dev);
    if (FAILED_HR(hr)) {
        SAY("  CreateDevice failed: %08lx\n", (unsigned long)hr);
        return 0;
    }
    SAY("%s: textures\n", p->name);
    p->tex_checker = make_texture(p->hal, 64, 64, 0, texel_checker);
    p->tex_small = make_texture(p->hal, 8, 8, 0, texel_small);
    p->tex_alpha = make_texture(p->hal, 64, 64, 1, texel_alpha);
    return 1;
}

static void V(TLVERTEX *v, float x, float y, float z, float rhw, DWORD c,
              DWORD s, float u, float t)
{
    v->sx = x;
    v->sy = y;
    v->sz = z;
    v->rhw = rhw;
    v->color = c;
    v->specular = s;
    v->tu = u;
    v->tv = t;
}

/* Every state a scene might touch, back to one baseline. */
static void baseline(IDirect3DDevice7 *d)
{
    const IDirect3DDevice7Vtbl *f = d->lpVtbl;
    f->SetRenderState(d, RS_LIGHTING, 0);
    f->SetRenderState(d, RS_CULLMODE, D3DCULL_NONE);
    f->SetRenderState(d, RS_SHADEMODE, D3DSHADE_GOURAUD);
    f->SetRenderState(d, RS_DITHERENABLE, 0);
    f->SetRenderState(d, RS_ZENABLE, 0);
    f->SetRenderState(d, RS_ZWRITEENABLE, 0);
    f->SetRenderState(d, RS_ALPHABLENDENABLE, 0);
    f->SetRenderState(d, RS_ALPHATESTENABLE, 0);
    f->SetRenderState(d, RS_FOGENABLE, 0);
    f->SetRenderState(d, RS_SPECULARENABLE, 0);
    f->SetRenderState(d, RS_COLORKEYENABLE, 0);
    f->SetRenderState(d, RS_TEXTUREPERSPECTIVE, 1);
    f->SetTexture(d, 0, 0);
    f->SetTextureStageState(d, 0, TSS_COLOROP, D3DTOP_SELECTARG1);
    f->SetTextureStageState(d, 0, TSS_COLORARG1, D3DTA_DIFFUSE);
    f->SetTextureStageState(d, 0, TSS_ALPHAOP, D3DTOP_SELECTARG1);
    f->SetTextureStageState(d, 0, TSS_ALPHAARG1, D3DTA_DIFFUSE);
    f->SetTextureStageState(d, 0, TSS_ADDRESS, D3DTADDRESS_WRAP);
    f->SetTextureStageState(d, 0, TSS_MAGFILTER, D3DTFG_POINT);
    f->SetTextureStageState(d, 0, TSS_MINFILTER, D3DTFN_POINT);
    f->SetTextureStageState(d, 0, TSS_MIPFILTER, D3DTFP_NONE);
    f->SetTextureStageState(d, 1, TSS_COLOROP, D3DTOP_DISABLE);
    f->SetTextureStageState(d, 1, TSS_ALPHAOP, D3DTOP_DISABLE);
}

static void use_texture(IDirect3DDevice7 *d, IDirectDrawSurface7 *t,
                        int modulate)
{
    const IDirect3DDevice7Vtbl *f = d->lpVtbl;
    f->SetTexture(d, 0, t);
    f->SetTextureStageState(d, 0, TSS_COLOROP,
                            modulate ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
    f->SetTextureStageState(d, 0, TSS_COLORARG1, D3DTA_TEXTURE);
    f->SetTextureStageState(d, 0, TSS_COLORARG2, D3DTA_DIFFUSE);
    f->SetTextureStageState(d, 0, TSS_ALPHAOP, D3DTOP_SELECTARG1);
    f->SetTextureStageState(d, 0, TSS_ALPHAARG1, D3DTA_TEXTURE);
}

static void quad(IDirect3DDevice7 *d, float x0, float y0, float x1, float y1,
                 float z, DWORD c, float u1, float v1)
{
    TLVERTEX v[4];
    V(&v[0], x0, y0, z, 1.0f, c, 0xff000000, 0.0f, 0.0f);
    V(&v[1], x1, y0, z, 1.0f, c, 0xff000000, u1, 0.0f);
    V(&v[2], x0, y1, z, 1.0f, c, 0xff000000, 0.0f, v1);
    V(&v[3], x1, y1, z, 1.0f, c, 0xff000000, u1, v1);
    d->lpVtbl->DrawPrimitive(d, D3DPT_TRIANGLESTRIP, FVF_TLVERTEX, v, 4, 0);
}

/* ---- the scenes ---- */

static void scene_gouraud(path_t *p)
{
    TLVERTEX v[3];
    V(&v[0], 20.0f, 20.0f, 0.5f, 1.0f, 0xffff0000, 0xff000000, 0, 0);
    V(&v[1], 236.0f, 40.0f, 0.5f, 1.0f, 0xff00ff00, 0xff000000, 0, 0);
    V(&v[2], 60.0f, 230.0f, 0.5f, 1.0f, 0xff0000ff, 0xff000000, 0, 0);
    p->dev->lpVtbl->DrawPrimitive(p->dev, D3DPT_TRIANGLELIST, FVF_TLVERTEX, v,
                                  3, 0);
}

static void scene_flat(path_t *p)
{
    p->dev->lpVtbl->SetRenderState(p->dev, RS_SHADEMODE, D3DSHADE_FLAT);
    scene_gouraud(p);
}

/* A floor going away from the viewer: perspective-correct texturing,
 * and the texture repeated (wrap) four times across. */
static void scene_perspective(path_t *p)
{
    TLVERTEX v[4];
    use_texture(p->dev, p->tex_checker, 1);
    /* near edge at the bottom (w = 1), far edge at the top (w = 4) */
    V(&v[0], 96.0f, 40.0f, 0.9f, 0.25f, 0xffffffff, 0xff000000, 0.0f, 0.0f);
    V(&v[1], 160.0f, 40.0f, 0.9f, 0.25f, 0xffffffff, 0xff000000, 4.0f, 0.0f);
    V(&v[2], 0.0f, 250.0f, 0.1f, 1.0f, 0xffffffff, 0xff000000, 0.0f, 4.0f);
    V(&v[3], 256.0f, 250.0f, 0.1f, 1.0f, 0xffffffff, 0xff000000, 4.0f, 4.0f);
    p->dev->lpVtbl->DrawPrimitive(p->dev, D3DPT_TRIANGLESTRIP, FVF_TLVERTEX, v,
                                  4, 0);
}

/* The texture once across a screen-aligned quad, lit by a diffuse
 * gradient (modulate). */
static void scene_texture_modulate(path_t *p)
{
    TLVERTEX v[4];
    use_texture(p->dev, p->tex_checker, 1);
    V(&v[0], 32.0f, 32.0f, 0.5f, 1.0f, 0xffffffff, 0xff000000, 0.0f, 0.0f);
    V(&v[1], 224.0f, 32.0f, 0.5f, 1.0f, 0xffff8000, 0xff000000, 1.0f, 0.0f);
    V(&v[2], 32.0f, 224.0f, 0.5f, 1.0f, 0xff00ffff, 0xff000000, 0.0f, 1.0f);
    V(&v[3], 224.0f, 224.0f, 0.5f, 1.0f, 0xff404040, 0xff000000, 1.0f, 1.0f);
    p->dev->lpVtbl->DrawPrimitive(p->dev, D3DPT_TRIANGLESTRIP, FVF_TLVERTEX, v,
                                  4, 0);
}

/* Drawn back to front's opposite: the near green square first, then the
 * far red one over it, then a blue triangle cutting through both. */
static void scene_zbuffer(path_t *p)
{
    const IDirect3DDevice7Vtbl *f = p->dev->lpVtbl;
    TLVERTEX v[3];
    f->SetRenderState(p->dev, RS_ZENABLE, 1);
    f->SetRenderState(p->dev, RS_ZWRITEENABLE, 1);
    f->SetRenderState(p->dev, RS_ZFUNC, D3DCMP_LESSEQUAL);
    quad(p->dev, 40.0f, 40.0f, 160.0f, 160.0f, 0.3f, 0xff00ff00, 0, 0);
    quad(p->dev, 96.0f, 96.0f, 216.0f, 216.0f, 0.6f, 0xffff0000, 0, 0);
    V(&v[0], 10.0f, 128.0f, 0.0f, 1.0f, 0xff0000ff, 0xff000000, 0, 0);
    V(&v[1], 246.0f, 100.0f, 1.0f, 1.0f, 0xff0000ff, 0xff000000, 0, 0);
    V(&v[2], 246.0f, 156.0f, 1.0f, 1.0f, 0xff0000ff, 0xff000000, 0, 0);
    f->DrawPrimitive(p->dev, D3DPT_TRIANGLELIST, FVF_TLVERTEX, v, 3, 0);
}

/* A red square, and half-transparent green over part of it. */
static void scene_blend(path_t *p)
{
    const IDirect3DDevice7Vtbl *f = p->dev->lpVtbl;
    quad(p->dev, 20.0f, 20.0f, 150.0f, 236.0f, 0.5f, 0xffff0000, 0, 0);
    f->SetRenderState(p->dev, RS_ALPHABLENDENABLE, 1);
    f->SetRenderState(p->dev, RS_SRCBLEND, D3DBLEND_SRCALPHA);
    f->SetRenderState(p->dev, RS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    quad(p->dev, 90.0f, 60.0f, 236.0f, 196.0f, 0.5f, 0x8000ff00, 0, 0);
}

/* Per-vertex fog: the specular alpha is the fog factor, 255 none. */
static void scene_fog(path_t *p)
{
    const IDirect3DDevice7Vtbl *f = p->dev->lpVtbl;
    TLVERTEX v[4];
    f->SetRenderState(p->dev, RS_FOGENABLE, 1);
    f->SetRenderState(p->dev, RS_FOGCOLOR, 0x000000ff);
    f->SetRenderState(p->dev, RS_FOGTABLEMODE, D3DFOG_NONE);
    V(&v[0], 16.0f, 16.0f, 0.5f, 1.0f, 0xffffffff, 0xff000000, 0, 0);
    V(&v[1], 240.0f, 16.0f, 0.5f, 1.0f, 0xffffffff, 0x00000000, 0, 0);
    V(&v[2], 16.0f, 240.0f, 0.5f, 1.0f, 0xffffff00, 0xff000000, 0, 0);
    V(&v[3], 240.0f, 240.0f, 0.5f, 1.0f, 0xffffff00, 0x00000000, 0, 0);
    f->DrawPrimitive(p->dev, D3DPT_TRIANGLESTRIP, FVF_TLVERTEX, v, 4, 0);
}

/* Specular highlights added to a grey Gouraud triangle. */
static void scene_specular(path_t *p)
{
    const IDirect3DDevice7Vtbl *f = p->dev->lpVtbl;
    TLVERTEX v[3];
    f->SetRenderState(p->dev, RS_SPECULARENABLE, 1);
    V(&v[0], 20.0f, 20.0f, 0.5f, 1.0f, 0xff404040, 0xffc00000, 0, 0);
    V(&v[1], 236.0f, 128.0f, 0.5f, 1.0f, 0xff404040, 0xff000000, 0, 0);
    V(&v[2], 20.0f, 236.0f, 0.5f, 1.0f, 0xff404040, 0xff00c0c0, 0, 0);
    f->DrawPrimitive(p->dev, D3DPT_TRIANGLELIST, FVF_TLVERTEX, v, 3, 0);
}

/* An 8x8 texture magnified to 192x192, bilinearly filtered. */
static void scene_bilinear(path_t *p)
{
    const IDirect3DDevice7Vtbl *f = p->dev->lpVtbl;
    use_texture(p->dev, p->tex_small, 0);
    f->SetTextureStageState(p->dev, 0, TSS_MAGFILTER, D3DTFG_LINEAR);
    f->SetTextureStageState(p->dev, 0, TSS_MINFILTER, D3DTFN_LINEAR);
    quad(p->dev, 32.0f, 32.0f, 224.0f, 224.0f, 0.5f, 0xffffffff, 1.0f, 1.0f);
}

/* The same texture, point-sampled: which texel lands where. */
static void scene_point(path_t *p)
{
    use_texture(p->dev, p->tex_small, 0);
    quad(p->dev, 32.0f, 32.0f, 224.0f, 224.0f, 0.5f, 0xffffffff, 1.0f, 1.0f);
}

/* A 1555 texture whose transparent texels the alpha test drops, over a
 * magenta square. */
static void scene_alphatest(path_t *p)
{
    const IDirect3DDevice7Vtbl *f = p->dev->lpVtbl;
    quad(p->dev, 0.0f, 0.0f, 256.0f, 256.0f, 0.5f, 0xffff00ff, 0, 0);
    use_texture(p->dev, p->tex_alpha, 0);
    f->SetRenderState(p->dev, RS_ALPHATESTENABLE, 1);
    f->SetRenderState(p->dev, RS_ALPHAREF, 0x80);
    f->SetRenderState(p->dev, RS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    quad(p->dev, 32.0f, 32.0f, 224.0f, 224.0f, 0.5f, 0xffffffff, 1.0f, 1.0f);
}

/* The same texture alpha-blended instead of tested. */
static void scene_texalpha_blend(path_t *p)
{
    const IDirect3DDevice7Vtbl *f = p->dev->lpVtbl;
    quad(p->dev, 0.0f, 0.0f, 256.0f, 256.0f, 0.5f, 0xffff00ff, 0, 0);
    use_texture(p->dev, p->tex_alpha, 0);
    f->SetRenderState(p->dev, RS_ALPHABLENDENABLE, 1);
    f->SetRenderState(p->dev, RS_SRCBLEND, D3DBLEND_SRCALPHA);
    f->SetRenderState(p->dev, RS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    quad(p->dev, 32.0f, 32.0f, 224.0f, 224.0f, 0.5f, 0xffffffff, 1.0f, 1.0f);
}

typedef struct {
    const char *name;
    void (*draw)(path_t *);
} scene_t;

static const scene_t scenes[] = {
    {"gouraud", scene_gouraud},
    {"flat", scene_flat},
    {"specular", scene_specular},
    {"fog", scene_fog},
    {"blend", scene_blend},
    {"zbuffer", scene_zbuffer},
    {"point", scene_point},
    {"modulate", scene_texture_modulate},
    {"perspective", scene_perspective},
    {"bilinear", scene_bilinear},
    {"alphatest", scene_alphatest},
    {"texalpha", scene_texalpha_blend},
};
#define NSCENES ((int)(sizeof scenes / sizeof scenes[0]))

/* 565 pixels of the target, W*H of them, into `out`. */
static int read_target(path_t *p, unsigned short *out)
{
    DDSURFACEDESC2_T sd;
    int y;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    if (FAILED_HR(p->rt->lpVtbl->Lock(p->rt, 0, &sd,
                                      DDLOCK_WAIT | DDLOCK_READONLY, 0))) {
        SAY("  %s: target Lock failed\n", p->name);
        return 0;
    }
    for (y = 0; y < H; y++)
        memcpy(out + y * W, (char *)sd.lpSurface + y * sd.lPitch, W * 2);
    p->rt->lpVtbl->Unlock(p->rt, 0);
    return 1;
}

static int render(path_t *p, const scene_t *s, unsigned short *out)
{
    IDirect3DDevice7 *d = p->dev;
    HRESULT_T hr;
    baseline(d);
    hr = d->lpVtbl->Clear(d, 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                          0xff000000, 1.0f, 0);
    if (FAILED_HR(hr))
        SAY("  %s: Clear failed: %08lx\n", p->name, (unsigned long)hr);
    if (FAILED_HR(d->lpVtbl->BeginScene(d))) {
        SAY("  %s: BeginScene failed\n", p->name);
        return 0;
    }
    s->draw(p);
    d->lpVtbl->EndScene(d);
    return read_target(p, out);
}

static void ppm(const char *scene, const char *which, const unsigned short *px)
{
    char path[260];
    FILE *f;
    int i;
    sprintf(path, "%s\\%s-%s.ppm", outdir, scene, which);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        unsigned v = px[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
        rgb[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
        rgb[2] = (unsigned char)((v & 31) * 255 / 31);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int chan_diff(unsigned a, unsigned b)
{
    int ar = ((a >> 11) & 31) * 255 / 31, br = ((b >> 11) & 31) * 255 / 31;
    int ag = ((a >> 5) & 63) * 255 / 63, bg = ((b >> 5) & 63) * 255 / 63;
    int ab = (a & 31) * 255 / 31, bb = (b & 31) * 255 / 31;
    int d = ar > br ? ar - br : br - ar, e;
    e = ag > bg ? ag - bg : bg - ag;
    if (e > d)
        d = e;
    e = ab > bb ? ab - bb : bb - ab;
    return e > d ? e : d;
}

static HRESULT_T fmt_cb(DDPIXELFORMAT_T *pf, void *ctx)
{
    (void)ctx;
    SAY("  texture format: flags %08lx bits %lu r %08lx g %08lx b %08lx a "
        "%08lx\n",
        (unsigned long)pf->dwFlags, (unsigned long)pf->dwRGBBitCount,
        (unsigned long)pf->dwRBitMask, (unsigned long)pf->dwGBitMask,
        (unsigned long)pf->dwBBitMask, (unsigned long)pf->dwRGBAlphaBitMask);
    return 1; /* D3DENUMRET_OK */
}

static unsigned short img_hw[W * H], img_sw[W * H];

int main(int argc, char **argv)
{
    HMODULE lib;
    DIRECTDRAWCREATEEX_FN create;
    DDSURFACEDESC2_T mode;
    HRESULT_T hr;
    path_t hw, sw;
    int i, failed = 0;

    if (argc > 1)
        outdir = argv[1];
    SAY("d3dcheck: loading ddraw.dll\n");
    lib = LoadLibraryA("ddraw.dll");
    if (!lib) {
        SAY("no ddraw.dll\n");
        return 2;
    }
    create = (DIRECTDRAWCREATEEX_FN)GetProcAddress(lib, "DirectDrawCreateEx");
    if (!create) {
        SAY("no DirectDrawCreateEx\n");
        return 2;
    }
    hr = create(0, (void **)&dd, &IID_IDirectDraw7, 0);
    SAY("DirectDrawCreateEx: %08lx\n", (unsigned long)hr);
    if (FAILED_HR(hr))
        return 2;
    hr = dd->lpVtbl->SetCooperativeLevel(dd, 0, DDSCL_NORMAL);
    SAY("SetCooperativeLevel: %08lx\n", (unsigned long)hr);
    memset(&mode, 0, sizeof mode);
    mode.dwSize = sizeof mode;
    dd->lpVtbl->GetDisplayMode(dd, &mode);
    SAY("display: %lux%lu, %lu bpp\n", (unsigned long)mode.dwWidth,
        (unsigned long)mode.dwHeight,
        (unsigned long)mode.ddpfPixelFormat.dwRGBBitCount);
    if (mode.ddpfPixelFormat.dwRGBBitCount != 16) {
        SAY("the desktop must be in 16-bit colour\n");
        return 2;
    }
    hr = dd->lpVtbl->QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d);
    SAY("QueryInterface(IDirect3D7): %08lx\n", (unsigned long)hr);
    if (FAILED_HR(hr))
        return 2;

    memset(&hw, 0, sizeof hw);
    memset(&sw, 0, sizeof sw);
    hw.name = "hal";
    hw.hal = 1;
    sw.name = "rgb";
    sw.hal = 0;
    if (!open_path(&hw) || !open_path(&sw))
        return 2;
    SAY("hal: texture formats\n");
    hw.dev->lpVtbl->EnumTextureFormats(hw.dev, fmt_cb, 0);

    for (i = 0; i < NSCENES; i++) {
        const scene_t *s = &scenes[i];
        int p, diff = 0, maxd = 0;
        SAY("scene %s: hal\n", s->name);
        if (!render(&hw, s, img_hw)) {
            failed++;
            continue;
        }
        SAY("scene %s: rgb\n", s->name);
        if (!render(&sw, s, img_sw)) {
            failed++;
            continue;
        }
        for (p = 0; p < W * H; p++) {
            int d = chan_diff(img_hw[p], img_sw[p]);
            if (d > maxd)
                maxd = d;
            if (d > TOL)
                diff++;
        }
        ppm(s->name, "hal", img_hw);
        ppm(s->name, "rgb", img_sw);
        SAY("RESULT %-12s %s differing %6d of %d, max %3d\n", s->name,
            diff == 0 ? "same " : diff <= EDGE_PIXELS ? "close" : "DIFF ",
            diff, W * H, maxd);
        if (diff > EDGE_PIXELS)
            failed++;
    }
    SAY("DONE %d of %d scenes differ\n", failed, NSCENES);
    return failed ? 1 : 0;
}
