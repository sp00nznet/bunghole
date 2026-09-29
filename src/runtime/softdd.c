/*
 * softdd: a software DirectDraw for the lifted game.
 *
 * The engine asks for exclusive fullscreen 640x480x16 with a flip chain
 * (SetCooperativeLevel 0x51, SetDisplayMode 640/480/16). Real DirectDraw
 * would change the desktop's display mode, which must never happen to a
 * machine driven over RDP (REPO_RULES section 13) and which modern Windows
 * emulates badly anyway. So DirectDrawCreate returns these objects instead:
 * every surface is a top-down RGB565 DIB section in host memory, which the
 * guest can Lock and write (flat memory: a host pointer is a guest pointer)
 * and which GDI can draw text into (GetDC). A Flip, or a write to a primary
 * without a back buffer, presents the front buffer: StretchDIBits into the
 * game's window, and/or raw frames piped to ffmpeg for --record.
 *
 * The vtables are the SDK's own C declarations (CINTERFACE), so every method
 * has the signature and the stdcall purge the game was compiled against;
 * a method nothing has needed yet logs itself once and fails. docs/host.md
 * has the design.
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "softdd.h"

int         softdd_headless;
int         softdd_scale = 1;
const char* softdd_record;               /* --record out.mp4, or NULL */
int         softdd_stop_after;           /* --frames N: exit after N presents */
volatile LONG softdd_frames;
void (*softdd_on_present)(const uint8_t* bits, int w, int h, int pitch, long frame);

#define UNIMPL(what) do { static int once; if (!once++) \
    fprintf(stderr, "[softdd] unimplemented: %s\n", what); } while (0)

/* ------------------------------------------------------------- objects */

typedef struct Surf {
    IDirectDrawSurfaceVtbl* lpVtbl;
    IDirectDrawSurface3Vtbl* v3;         /* the same object as IDirectDrawSurface2/3 */
    LONG      refs;
    DWORD     caps;
    int       w, h, pitch;
    uint8_t*  bits;
    HBITMAP   hbm;
    HDC       hdc;                       /* lazily, for GetDC */
    struct Surf* back;                   /* primary -> its back buffer */
    DWORD     ck_lo, ck_hi;
    int       has_ck;
    IDirectDrawPalette* pal;
    IDirectDrawClipper* clip;
} Surf;

typedef struct {
    IDirectDrawVtbl* lpVtbl;
    IDirectDraw2Vtbl* v2;                /* the same object as IDirectDraw2 */
    LONG  refs;
    HWND  hwnd;
    DWORD coop;
    int   w, h, bpp;
    Surf* primary;
} DD;

typedef struct {
    IDirectDrawPaletteVtbl* lpVtbl;
    LONG refs;
    DWORD caps;
    PALETTEENTRY e[256];
} Pal;

typedef struct {
    IDirectDrawClipperVtbl* lpVtbl;
    LONG refs;
    HWND hwnd;
} Clip;

static IDirectDrawVtbl        dd_vt;
static IDirectDrawSurfaceVtbl surf_vt;
static IDirectDrawPaletteVtbl pal_vt;
static IDirectDrawClipperVtbl clip_vt;
static IDirectDrawSurface3Vtbl surf3_vt;
static IDirectDraw2Vtbl        dd2_vt;

/* A surface pointer as either interface -> the object. DirectShow hands back
 * Surface3 pointers, the game v1 ones, and either can be a Blt source. */
static struct Surf* as_surf(void* p) {
    if (!p) return NULL;
    if (*(void**)p == &surf_vt) return (struct Surf*)p;
    if (*(void**)p == &surf3_vt) return (struct Surf*)((void**)p - 1);
    fprintf(stderr, "[softdd] a surface that is not ours: %p\n", p);
    return NULL;
}

static DD* g_dd;

static void pixfmt(DDPIXELFORMAT* pf) {
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0xF800;
    pf->dwGBitMask = 0x07E0;
    pf->dwBBitMask = 0x001F;
}

static BITMAPINFO* bmi565(int w, int h) {
    static struct { BITMAPINFOHEADER h; DWORD masks[3]; } b;
    memset(&b, 0, sizeof b);
    b.h.biSize = sizeof b.h;
    b.h.biWidth = w;
    b.h.biHeight = -h;                   /* top-down: row 0 is the top */
    b.h.biPlanes = 1;
    b.h.biBitCount = 16;
    b.h.biCompression = BI_BITFIELDS;
    b.masks[0] = 0xF800; b.masks[1] = 0x07E0; b.masks[2] = 0x001F;
    return (BITMAPINFO*)&b;
}

static Surf* surf_new(int w, int h, DWORD caps) {
    Surf* s = (Surf*)calloc(1, sizeof *s);
    s->lpVtbl = &surf_vt;
    s->v3 = &surf3_vt;
    s->refs = 1;
    s->caps = caps;
    s->w = w;
    s->h = h;
    s->pitch = (w * 2 + 3) & ~3;
    s->hbm = CreateDIBSection(NULL, bmi565(w, h), DIB_RGB_COLORS, (void**)&s->bits, NULL, 0);
    if (!s->hbm) { fprintf(stderr, "[softdd] CreateDIBSection %dx%d failed\n", w, h); exit(5); }
    return s;
}

static void surf_free(Surf* s) {
    if (s->hdc) DeleteDC(s->hdc);
    DeleteObject(s->hbm);
    free(s);
}

/* ------------------------------------------------------------- present */

static FILE* g_rec;
static DWORD g_rec_t0;
static LONG  g_rec_written;
#define REC_FPS 30

static void record_open(int w, int h) {
    char cmd[1024];
    _snprintf(cmd, sizeof cmd - 1,
              "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb565le -s %dx%d -r %d -i - "
              "-c:v libx264 -preset veryfast -pix_fmt yuv420p \"%s\"", w, h, REC_FPS, softdd_record);
    g_rec = _popen(cmd, "wb");
    if (!g_rec) { fprintf(stderr, "[softdd] cannot start ffmpeg for %s\n", softdd_record); exit(5); }
    g_rec_t0 = GetTickCount();
    fprintf(stderr, "[softdd] recording %dx%d at %d fps -> %s\n", w, h, REC_FPS, softdd_record);
}

void softdd_finish(void) {
    if (g_rec) { _pclose(g_rec); g_rec = NULL; }
}

/* The video runs at a fixed 30 fps against the wall clock: a slow frame is
 * held, a fast one is dropped, so the mp4 plays at the game's real speed. */
static void record_frame(Surf* s) {
    LONG due = (LONG)((GetTickCount() - g_rec_t0) * (DWORD)REC_FPS / 1000) + 1;
    for (; g_rec_written < due; g_rec_written++)
        for (int y = 0; y < s->h; y++)
            fwrite(s->bits + y * s->pitch, 2, s->w, g_rec);
    fflush(g_rec);
}

static void present(Surf* s) {
    LONG n = InterlockedIncrement(&softdd_frames);
    if (n == 1 || n == 10 || n == 100 || n % 1000 == 0)
        fprintf(stderr, "[softdd] frame %ld presented\n", n);
    if (softdd_on_present) softdd_on_present(s->bits, s->w, s->h, s->pitch, n);
    if (softdd_record) {
        if (!g_rec) record_open(s->w, s->h);
        record_frame(s);
    }
    if (!softdd_headless && g_dd && g_dd->hwnd) {
        RECT rc;
        HDC dc = GetDC(g_dd->hwnd);
        GetClientRect(g_dd->hwnd, &rc);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, s->w, s->h, s->bits,
                      bmi565(s->w, s->h), DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(g_dd->hwnd, dc);
    }
    if (softdd_stop_after && n >= softdd_stop_after) {
        fprintf(stderr, "[softdd] %ld frames: stopping (--frames)\n", n);
        softdd_finish();
        fflush(stderr);
        ExitProcess(0);
    }
}

/* A primary with no back buffer is drawn to directly, so every write to it
 * is a present. */
static void maybe_present(Surf* s) {
    if ((s->caps & DDSCAPS_PRIMARYSURFACE) && !s->back) present(s);
}

/* The game's window was made for a fullscreen mode: a popup, sized by the
 * mode change. Windowed, it gets a caption and a client area of the mode
 * times --scale. */
static void fit_window(DD* d) {
    if (softdd_headless || !d->hwnd || !d->w) return;
    RECT r = { 0, 0, d->w * softdd_scale, d->h * softdd_scale };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    SetWindowLongA(d->hwnd, GWL_STYLE, style);
    SetWindowLongA(d->hwnd, GWL_EXSTYLE, 0);
    AdjustWindowRect(&r, style, FALSE);
    SetWindowPos(d->hwnd, HWND_NOTOPMOST, 64, 64, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* ------------------------------------------------------------- blits */

static int clip_rect(RECT* d, RECT* s, int dw, int dh) {
    /* 1:1 blits only: shift the source with the destination. */
    if (d->left < 0) { s->left -= d->left; d->left = 0; }
    if (d->top < 0) { s->top -= d->top; d->top = 0; }
    if (d->right > dw) { s->right -= d->right - dw; d->right = dw; }
    if (d->bottom > dh) { s->bottom -= d->bottom - dh; d->bottom = dh; }
    return d->right > d->left && d->bottom > d->top;
}

static void blit(Surf* dst, RECT dr, Surf* src, RECT sr, int keyed, DWORD lo, DWORD hi) {
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (dw == sw && dh == sh) {
        if (!clip_rect(&dr, &sr, dst->w, dst->h)) return;
        if (sr.left < 0 || sr.top < 0 || sr.right > src->w || sr.bottom > src->h) {
            RECT back = sr;                        /* clip the source, then the dest to match */
            if (!clip_rect(&back, &dr, src->w, src->h)) return;
            sr = back;
        }
        dw = dr.right - dr.left;
        dh = dr.bottom - dr.top;
        int up = src == dst && dr.top > sr.top;    /* overlapping scroll: bottom-up */
        for (int k = 0; k < dh; k++) {
            int y = up ? dh - 1 - k : k;
            uint16_t* dp = (uint16_t*)(dst->bits + (dr.top + y) * dst->pitch) + dr.left;
            uint16_t* sp = (uint16_t*)(src->bits + (sr.top + y) * src->pitch) + sr.left;
            if (!keyed) { memmove(dp, sp, dw * 2); continue; }
            for (int x = 0; x < dw; x++)
                if (sp[x] < lo || sp[x] > hi) dp[x] = sp[x];
        }
        return;
    }
    /* Stretch, nearest neighbour, clipped per pixel. */
    for (int y = 0; y < dh; y++) {
        int ty = dr.top + y, fy = sr.top + y * sh / dh;
        if (ty < 0 || ty >= dst->h || fy < 0 || fy >= src->h) continue;
        uint16_t* dp = (uint16_t*)(dst->bits + ty * dst->pitch);
        uint16_t* sp = (uint16_t*)(src->bits + fy * src->pitch);
        for (int x = 0; x < dw; x++) {
            int tx = dr.left + x, fx = sr.left + x * sw / dw;
            if (tx < 0 || tx >= dst->w || fx < 0 || fx >= src->w) continue;
            if (!keyed || sp[fx] < lo || sp[fx] > hi) dp[tx] = sp[fx];
        }
    }
}

static void fill(Surf* s, RECT r, uint16_t c) {
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > s->w) r.right = s->w;
    if (r.bottom > s->h) r.bottom = s->h;
    for (int y = r.top; y < r.bottom; y++) {
        uint16_t* p = (uint16_t*)(s->bits + y * s->pitch);
        for (int x = r.left; x < r.right; x++) p[x] = c;
    }
}

static RECT full(Surf* s) { RECT r = { 0, 0, s->w, s->h }; return r; }

/* ------------------------------------------------------------- surface */

#define SELF ((Surf*)This)

static HRESULT WINAPI s_QueryInterface(IDirectDrawSurface* This, REFIID riid, LPVOID* out) {
    if (IsEqualGUID(riid, &IID_IDirectDrawSurface) || IsEqualGUID(riid, &IID_IUnknown)) {
        SELF->refs++;
        *out = This;
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IDirectDrawSurface2) || IsEqualGUID(riid, &IID_IDirectDrawSurface3)) {
        SELF->refs++;
        *out = &SELF->v3;
        return S_OK;
    }
    fprintf(stderr, "[softdd] Surface::QueryInterface {%08lX-...} refused\n", riid->Data1);
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI s_AddRef(IDirectDrawSurface* This) { return ++SELF->refs; }
static ULONG WINAPI s_Release(IDirectDrawSurface* This) {
    LONG n = --SELF->refs;
    if (n == 0) {
        if (SELF->back) surf_free(SELF->back);
        if (g_dd && g_dd->primary == SELF) g_dd->primary = NULL;
        surf_free(SELF);
    }
    return n;
}
static HRESULT WINAPI s_AddAttachedSurface(IDirectDrawSurface* This, LPDIRECTDRAWSURFACE a) {
    (void)This; (void)a; UNIMPL("Surface::AddAttachedSurface"); return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI s_AddOverlayDirtyRect(IDirectDrawSurface* This, LPRECT r) {
    (void)This; (void)r; return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI s_Blt(IDirectDrawSurface* This, LPRECT dr, LPDIRECTDRAWSURFACE src,
                           LPRECT sr, DWORD flags, LPDDBLTFX fx) {
    RECT d = dr ? *dr : full(SELF);
    if (flags & DDBLT_COLORFILL) {
        fill(SELF, d, (uint16_t)fx->dwFillColor);
    } else if (src) {
        Surf* s = as_surf(src);
        RECT r = sr ? *sr : full(s);
        if (flags & DDBLT_KEYSRCOVERRIDE)
            blit(SELF, d, s, r, 1, fx->ddckSrcColorkey.dwColorSpaceLowValue,
                 fx->ddckSrcColorkey.dwColorSpaceHighValue);
        else
            blit(SELF, d, s, r, (flags & DDBLT_KEYSRC) && s->has_ck, s->ck_lo, s->ck_hi);
    } else {
        fprintf(stderr, "[softdd] Blt flags 0x%lX with no source\n", flags);
    }
    maybe_present(SELF);
    return DD_OK;
}
static HRESULT WINAPI s_BltBatch(IDirectDrawSurface* This, LPDDBLTBATCH b, DWORD n, DWORD f) {
    (void)This; (void)b; (void)n; (void)f; UNIMPL("Surface::BltBatch"); return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI s_BltFast(IDirectDrawSurface* This, DWORD x, DWORD y, LPDIRECTDRAWSURFACE src,
                               LPRECT sr, DWORD trans) {
    Surf* s = as_surf(src);
    RECT r = sr ? *sr : full(s);
    RECT d = { (LONG)x, (LONG)y, (LONG)x + (r.right - r.left), (LONG)y + (r.bottom - r.top) };
    blit(SELF, d, s, r, (trans & DDBLTFAST_SRCCOLORKEY) && s->has_ck, s->ck_lo, s->ck_hi);
    maybe_present(SELF);
    return DD_OK;
}
static HRESULT WINAPI s_DeleteAttachedSurface(IDirectDrawSurface* This, DWORD f, LPDIRECTDRAWSURFACE a) {
    (void)This; (void)f; (void)a; UNIMPL("Surface::DeleteAttachedSurface"); return DD_OK;
}
static HRESULT WINAPI s_EnumAttachedSurfaces(IDirectDrawSurface* This, LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) {
    (void)This; (void)ctx; (void)cb; UNIMPL("Surface::EnumAttachedSurfaces"); return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI s_EnumOverlayZOrders(IDirectDrawSurface* This, DWORD f, LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) {
    (void)This; (void)f; (void)ctx; (void)cb; return DDERR_UNSUPPORTED;
}
/* Swap the memory, not the objects: after a Flip the game keeps drawing into
 * the same back-buffer pointer, which must now hold the old front. */
static HRESULT WINAPI s_Flip(IDirectDrawSurface* This, LPDIRECTDRAWSURFACE over, DWORD f) {
    (void)over; (void)f;
    Surf* b = SELF->back;
    if (b) {
        uint8_t* bits = SELF->bits; HBITMAP hbm = SELF->hbm; HDC hdc = SELF->hdc;
        SELF->bits = b->bits; SELF->hbm = b->hbm; SELF->hdc = b->hdc;
        b->bits = bits; b->hbm = hbm; b->hdc = hdc;
    }
    present(SELF);
    return DD_OK;
}
static HRESULT WINAPI s_GetAttachedSurface(IDirectDrawSurface* This, LPDDSCAPS caps, LPDIRECTDRAWSURFACE* out) {
    if (SELF->back && (caps->dwCaps & DDSCAPS_BACKBUFFER)) {
        SELF->back->refs++;
        *out = (LPDIRECTDRAWSURFACE)SELF->back;
        return DD_OK;
    }
    fprintf(stderr, "[softdd] GetAttachedSurface caps 0x%lX: none\n", caps->dwCaps);
    *out = NULL;
    return DDERR_NOTFOUND;
}
static HRESULT WINAPI s_GetBltStatus(IDirectDrawSurface* This, DWORD f) { (void)This; (void)f; return DD_OK; }
static HRESULT WINAPI s_GetCaps(IDirectDrawSurface* This, LPDDSCAPS c) { c->dwCaps = SELF->caps; return DD_OK; }
static HRESULT WINAPI s_GetClipper(IDirectDrawSurface* This, LPDIRECTDRAWCLIPPER* c) {
    *c = SELF->clip;
    return SELF->clip ? DD_OK : DDERR_NOCLIPPERATTACHED;
}
static HRESULT WINAPI s_GetColorKey(IDirectDrawSurface* This, DWORD f, LPDDCOLORKEY k) {
    (void)f;
    if (!SELF->has_ck) return DDERR_NOCOLORKEY;
    k->dwColorSpaceLowValue = SELF->ck_lo;
    k->dwColorSpaceHighValue = SELF->ck_hi;
    return DD_OK;
}
static HRESULT WINAPI s_GetDC(IDirectDrawSurface* This, HDC* out) {
    if (!SELF->hdc) SELF->hdc = CreateCompatibleDC(NULL);
    SelectObject(SELF->hdc, SELF->hbm);
    *out = SELF->hdc;
    return DD_OK;
}
static HRESULT WINAPI s_GetFlipStatus(IDirectDrawSurface* This, DWORD f) { (void)This; (void)f; return DD_OK; }
static HRESULT WINAPI s_GetOverlayPosition(IDirectDrawSurface* This, LPLONG x, LPLONG y) {
    (void)This; (void)x; (void)y; return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI s_GetPalette(IDirectDrawSurface* This, LPDIRECTDRAWPALETTE* p) {
    *p = SELF->pal;
    return SELF->pal ? DD_OK : DDERR_NOPALETTEATTACHED;
}
static HRESULT WINAPI s_GetPixelFormat(IDirectDrawSurface* This, LPDDPIXELFORMAT pf) {
    (void)This;
    pixfmt(pf);
    return DD_OK;
}
static void fill_desc(Surf* s, LPDDSURFACEDESC d) {
    DWORD size = d->dwSize ? d->dwSize : sizeof *d;
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = s->w;
    d->dwHeight = s->h;
    d->lPitch = s->pitch;
    d->ddsCaps.dwCaps = s->caps;
    if (s->back) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; d->dwBackBufferCount = 1; }
    pixfmt(&d->ddpfPixelFormat);
}
static HRESULT WINAPI s_GetSurfaceDesc(IDirectDrawSurface* This, LPDDSURFACEDESC d) {
    fill_desc(SELF, d);
    return DD_OK;
}
static HRESULT WINAPI s_Initialize(IDirectDrawSurface* This, LPDIRECTDRAW dd, LPDDSURFACEDESC d) {
    (void)This; (void)dd; (void)d; return DDERR_ALREADYINITIALIZED;
}
static HRESULT WINAPI s_IsLost(IDirectDrawSurface* This) { (void)This; return DD_OK; }
static HRESULT WINAPI s_Lock(IDirectDrawSurface* This, LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e) {
    (void)f; (void)e;
    fill_desc(SELF, d);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = SELF->bits + (r ? r->top * SELF->pitch + r->left * 2 : 0);
    return DD_OK;
}
static HRESULT WINAPI s_ReleaseDC(IDirectDrawSurface* This, HDC dc) {
    (void)dc;
    GdiFlush();
    maybe_present(SELF);
    return DD_OK;
}
static HRESULT WINAPI s_Restore(IDirectDrawSurface* This) { (void)This; return DD_OK; }
static HRESULT WINAPI s_SetClipper(IDirectDrawSurface* This, LPDIRECTDRAWCLIPPER c) {
    SELF->clip = c;
    return DD_OK;
}
static HRESULT WINAPI s_SetColorKey(IDirectDrawSurface* This, DWORD f, LPDDCOLORKEY k) {
    if (!(f & DDCKEY_SRCBLT)) { fprintf(stderr, "[softdd] SetColorKey flags 0x%lX ignored\n", f); return DD_OK; }
    SELF->has_ck = k != NULL;
    if (k) { SELF->ck_lo = k->dwColorSpaceLowValue; SELF->ck_hi = k->dwColorSpaceHighValue; }
    if (k && !(f & DDCKEY_COLORSPACE)) SELF->ck_hi = SELF->ck_lo;
    return DD_OK;
}
static HRESULT WINAPI s_SetOverlayPosition(IDirectDrawSurface* This, LONG x, LONG y) {
    (void)This; (void)x; (void)y; return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI s_SetPalette(IDirectDrawSurface* This, LPDIRECTDRAWPALETTE p) {
    SELF->pal = p;
    return DD_OK;
}
static HRESULT WINAPI s_Unlock(IDirectDrawSurface* This, LPVOID p) {
    (void)p;
    maybe_present(SELF);
    return DD_OK;
}
static HRESULT WINAPI s_UpdateOverlay(IDirectDrawSurface* This, LPRECT a, LPDIRECTDRAWSURFACE b, LPRECT c,
                                      DWORD f, LPDDOVERLAYFX fx) {
    (void)This; (void)a; (void)b; (void)c; (void)f; (void)fx; return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI s_UpdateOverlayDisplay(IDirectDrawSurface* This, DWORD f) {
    (void)This; (void)f; return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI s_UpdateOverlayZOrder(IDirectDrawSurface* This, DWORD f, LPDIRECTDRAWSURFACE r) {
    (void)This; (void)f; (void)r; return DDERR_UNSUPPORTED;
}
#undef SELF

static IDirectDrawSurfaceVtbl surf_vt = {
    s_QueryInterface, s_AddRef, s_Release, s_AddAttachedSurface, s_AddOverlayDirtyRect, s_Blt,
    s_BltBatch, s_BltFast, s_DeleteAttachedSurface, s_EnumAttachedSurfaces, s_EnumOverlayZOrders,
    s_Flip, s_GetAttachedSurface, s_GetBltStatus, s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC,
    s_GetFlipStatus, s_GetOverlayPosition, s_GetPalette, s_GetPixelFormat, s_GetSurfaceDesc,
    s_Initialize, s_IsLost, s_Lock, s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey,
    s_SetOverlayPosition, s_SetPalette, s_Unlock, s_UpdateOverlay, s_UpdateOverlayDisplay,
    s_UpdateOverlayZOrder,
};

/* ------------------------------------------------------ palette, clipper */

static HRESULT WINAPI p_QueryInterface(IDirectDrawPalette* This, REFIID r, LPVOID* o) {
    (void)This; (void)r; *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI p_AddRef(IDirectDrawPalette* This) { return ++((Pal*)This)->refs; }
static ULONG WINAPI p_Release(IDirectDrawPalette* This) {
    LONG n = --((Pal*)This)->refs;
    if (!n) free(This);
    return n;
}
static HRESULT WINAPI p_GetCaps(IDirectDrawPalette* This, LPDWORD c) { *c = ((Pal*)This)->caps; return DD_OK; }
static HRESULT WINAPI p_GetEntries(IDirectDrawPalette* This, DWORD f, DWORD b, DWORD n, LPPALETTEENTRY e) {
    (void)f;
    memcpy(e, ((Pal*)This)->e + b, n * sizeof *e);
    return DD_OK;
}
static HRESULT WINAPI p_Initialize(IDirectDrawPalette* This, LPDIRECTDRAW d, DWORD f, LPPALETTEENTRY e) {
    (void)This; (void)d; (void)f; (void)e; return DDERR_ALREADYINITIALIZED;
}
static HRESULT WINAPI p_SetEntries(IDirectDrawPalette* This, DWORD f, DWORD b, DWORD n, LPPALETTEENTRY e) {
    (void)f;
    memcpy(((Pal*)This)->e + b, e, n * sizeof *e);
    return DD_OK;
}
static IDirectDrawPaletteVtbl pal_vt = {
    p_QueryInterface, p_AddRef, p_Release, p_GetCaps, p_GetEntries, p_Initialize, p_SetEntries,
};

static HRESULT WINAPI c_QueryInterface(IDirectDrawClipper* This, REFIID r, LPVOID* o) {
    (void)This; (void)r; *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI c_AddRef(IDirectDrawClipper* This) { return ++((Clip*)This)->refs; }
static ULONG WINAPI c_Release(IDirectDrawClipper* This) {
    LONG n = --((Clip*)This)->refs;
    if (!n) free(This);
    return n;
}
static HRESULT WINAPI c_GetClipList(IDirectDrawClipper* This, LPRECT r, LPRGNDATA d, LPDWORD n) {
    (void)This; (void)r; (void)d; (void)n; UNIMPL("Clipper::GetClipList"); return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI c_GetHWnd(IDirectDrawClipper* This, HWND* w) { *w = ((Clip*)This)->hwnd; return DD_OK; }
static HRESULT WINAPI c_Initialize(IDirectDrawClipper* This, LPDIRECTDRAW d, DWORD f) {
    (void)This; (void)d; (void)f; return DDERR_ALREADYINITIALIZED;
}
static HRESULT WINAPI c_IsClipListChanged(IDirectDrawClipper* This, BOOL* b) { (void)This; *b = FALSE; return DD_OK; }
static HRESULT WINAPI c_SetClipList(IDirectDrawClipper* This, LPRGNDATA d, DWORD f) {
    (void)This; (void)d; (void)f; UNIMPL("Clipper::SetClipList"); return DD_OK;
}
static HRESULT WINAPI c_SetHWnd(IDirectDrawClipper* This, DWORD f, HWND w) { (void)f; ((Clip*)This)->hwnd = w; return DD_OK; }
static IDirectDrawClipperVtbl clip_vt = {
    c_QueryInterface, c_AddRef, c_Release, c_GetClipList, c_GetHWnd, c_Initialize,
    c_IsClipListChanged, c_SetClipList, c_SetHWnd,
};

/* ------------------------------------------------------------- DirectDraw */

#define SELF ((DD*)This)

static HRESULT WINAPI d_QueryInterface(IDirectDraw* This, REFIID riid, LPVOID* out) {
    if (IsEqualGUID(riid, &IID_IDirectDraw) || IsEqualGUID(riid, &IID_IUnknown)) {
        SELF->refs++;
        *out = This;
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IDirectDraw2)) {
        SELF->refs++;
        *out = &SELF->v2;
        return S_OK;
    }
    fprintf(stderr, "[softdd] DirectDraw::QueryInterface {%08lX-...} refused\n", riid->Data1);
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI d_AddRef(IDirectDraw* This) { return ++SELF->refs; }
static ULONG WINAPI d_Release(IDirectDraw* This) { return --SELF->refs; }   /* one per process */
static HRESULT WINAPI d_Compact(IDirectDraw* This) { (void)This; return DD_OK; }
static HRESULT WINAPI d_CreateClipper(IDirectDraw* This, DWORD f, LPDIRECTDRAWCLIPPER* out, IUnknown* u) {
    (void)This; (void)f; (void)u;
    Clip* c = (Clip*)calloc(1, sizeof *c);
    c->lpVtbl = &clip_vt;
    c->refs = 1;
    *out = (LPDIRECTDRAWCLIPPER)c;
    return DD_OK;
}
static HRESULT WINAPI d_CreatePalette(IDirectDraw* This, DWORD f, LPPALETTEENTRY e,
                                      LPDIRECTDRAWPALETTE* out, IUnknown* u) {
    (void)This; (void)u;
    Pal* p = (Pal*)calloc(1, sizeof *p);
    p->lpVtbl = &pal_vt;
    p->refs = 1;
    p->caps = f;
    if (e) memcpy(p->e, e, (f & DDPCAPS_8BIT ? 256 : 16) * sizeof *e);
    *out = (LPDIRECTDRAWPALETTE)p;
    return DD_OK;
}
static HRESULT WINAPI d_CreateSurface(IDirectDraw* This, LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out, IUnknown* u) {
    (void)u;
    DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    Surf* s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        int w = SELF->w ? SELF->w : 640, h = SELF->h ? SELF->h : 480;
        s = surf_new(w, h, caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY);
        if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount) {
            if (d->dwBackBufferCount > 1)
                fprintf(stderr, "[softdd] %lu back buffers asked, 1 made\n", d->dwBackBufferCount);
            s->back = surf_new(w, h, (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER))
                                     | DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY);
        }
        SELF->primary = s;
        fprintf(stderr, "[softdd] primary %dx%d%s\n", w, h, s->back ? " + back buffer" : "");
    } else {
        if ((d->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) != (DDSD_WIDTH | DDSD_HEIGHT)) {
            fprintf(stderr, "[softdd] CreateSurface flags 0x%lX caps 0x%lX: no size\n", d->dwFlags, caps);
            return DDERR_INVALIDPARAMS;
        }
        if ((d->dwFlags & DDSD_PIXELFORMAT) && d->ddpfPixelFormat.dwRGBBitCount != 16)
            fprintf(stderr, "[softdd] CreateSurface asks %lu bpp; made 16\n",
                    d->ddpfPixelFormat.dwRGBBitCount);
        if (!(caps & DDSCAPS_SYSTEMMEMORY)) caps |= DDSCAPS_VIDEOMEMORY;
        s = surf_new((int)d->dwWidth, (int)d->dwHeight, caps | DDSCAPS_OFFSCREENPLAIN);
    }
    if (d->dwFlags & DDSD_CKSRCBLT) {
        s->has_ck = 1;
        s->ck_lo = d->ddckCKSrcBlt.dwColorSpaceLowValue;
        s->ck_hi = d->ddckCKSrcBlt.dwColorSpaceHighValue;
    }
    *out = (LPDIRECTDRAWSURFACE)s;
    return DD_OK;
}
static HRESULT WINAPI d_DuplicateSurface(IDirectDraw* This, LPDIRECTDRAWSURFACE s, LPDIRECTDRAWSURFACE* o) {
    (void)This; (void)s; (void)o; UNIMPL("DirectDraw::DuplicateSurface"); return DDERR_UNSUPPORTED;
}
/* The callback is guest code: native32 enters it through the exec fault on
 * the guest's non-executable .text, like a window procedure. */
static HRESULT WINAPI d_EnumDisplayModes(IDirectDraw* This, DWORD f, LPDDSURFACEDESC want, LPVOID ctx,
                                         LPDDENUMMODESCALLBACK cb) {
    static const int modes[][2] = { {640, 480}, {800, 600}, {1024, 768} };
    (void)This; (void)f; (void)want;
    for (int i = 0; i < 3; i++) {
        DDSURFACEDESC d;
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d.dwWidth = modes[i][0];
        d.dwHeight = modes[i][1];
        d.lPitch = modes[i][0] * 2;
        d.dwRefreshRate = 60;
        pixfmt(&d.ddpfPixelFormat);
        if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}
static HRESULT WINAPI d_EnumSurfaces(IDirectDraw* This, DWORD f, LPDDSURFACEDESC d, LPVOID c, LPDDENUMSURFACESCALLBACK cb) {
    (void)This; (void)f; (void)d; (void)c; (void)cb; UNIMPL("DirectDraw::EnumSurfaces"); return DDERR_UNSUPPORTED;
}
static HRESULT WINAPI d_FlipToGDISurface(IDirectDraw* This) { (void)This; return DD_OK; }
static HRESULT WINAPI d_GetCaps(IDirectDraw* This, LPDDCAPS hal, LPDDCAPS hel) {
    (void)This;
    LPDDCAPS c[2] = { hal, hel };
    for (int i = 0; i < 2; i++) {
        if (!c[i]) continue;
        DWORD size = c[i]->dwSize;
        memset(c[i], 0, size);
        c[i]->dwSize = size;
        c[i]->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY
                     | DDCAPS_CANBLTSYSMEM;
        c[i]->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        c[i]->dwVidMemTotal = c[i]->dwVidMemFree = 64u << 20;
        c[i]->ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER
                             | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE
                             | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY;
    }
    return DD_OK;
}
static HRESULT WINAPI d_GetDisplayMode(IDirectDraw* This, LPDDSURFACEDESC d) {
    DWORD size = d->dwSize ? d->dwSize : sizeof *d;
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = SELF->w ? SELF->w : 640;
    d->dwHeight = SELF->h ? SELF->h : 480;
    d->lPitch = d->dwWidth * 2;
    d->dwRefreshRate = 60;
    pixfmt(&d->ddpfPixelFormat);
    return DD_OK;
}
static HRESULT WINAPI d_GetFourCCCodes(IDirectDraw* This, LPDWORD n, LPDWORD c) { (void)This; (void)c; *n = 0; return DD_OK; }
static HRESULT WINAPI d_GetGDISurface(IDirectDraw* This, LPDIRECTDRAWSURFACE* o) {
    if (!SELF->primary) return DDERR_NOTFOUND;
    SELF->primary->refs++;
    *o = (LPDIRECTDRAWSURFACE)SELF->primary;
    return DD_OK;
}
static HRESULT WINAPI d_GetMonitorFrequency(IDirectDraw* This, LPDWORD f) { (void)This; *f = 60; return DD_OK; }
static HRESULT WINAPI d_GetScanLine(IDirectDraw* This, LPDWORD l) { (void)This; *l = 0; return DD_OK; }
static HRESULT WINAPI d_GetVerticalBlankStatus(IDirectDraw* This, LPBOOL b) { (void)This; *b = TRUE; return DD_OK; }
static HRESULT WINAPI d_Initialize(IDirectDraw* This, GUID* g) { (void)This; (void)g; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI d_RestoreDisplayMode(IDirectDraw* This) { (void)This; return DD_OK; }
static HRESULT WINAPI d_SetCooperativeLevel(IDirectDraw* This, HWND w, DWORD f) {
    SELF->hwnd = w;
    SELF->coop = f;
    fprintf(stderr, "[softdd] SetCooperativeLevel(%p, 0x%lX): windowed instead\n", (void*)w, f);
    fit_window(SELF);
    return DD_OK;
}
static HRESULT WINAPI d_SetDisplayMode(IDirectDraw* This, DWORD w, DWORD h, DWORD bpp) {
    fprintf(stderr, "[softdd] SetDisplayMode(%lu, %lu, %lu): the desktop is left alone\n", w, h, bpp);
    if (bpp != 16) return DDERR_INVALIDMODE;
    SELF->w = (int)w;
    SELF->h = (int)h;
    SELF->bpp = (int)bpp;
    fit_window(SELF);
    return DD_OK;
}
/* A real vblank wait paced the game at the monitor's refresh. */
static HRESULT WINAPI d_WaitForVerticalBlank(IDirectDraw* This, DWORD f, HANDLE e) {
    (void)This; (void)f; (void)e;
    Sleep(1);
    return DD_OK;
}
#undef SELF

static IDirectDrawVtbl dd_vt = {
    d_QueryInterface, d_AddRef, d_Release, d_Compact, d_CreateClipper, d_CreatePalette,
    d_CreateSurface, d_DuplicateSurface, d_EnumDisplayModes, d_EnumSurfaces, d_FlipToGDISurface,
    d_GetCaps, d_GetDisplayMode, d_GetFourCCCodes, d_GetGDISurface, d_GetMonitorFrequency,
    d_GetScanLine, d_GetVerticalBlankStatus, d_Initialize, d_RestoreDisplayMode,
    d_SetCooperativeLevel, d_SetDisplayMode, d_WaitForVerticalBlank,
};

HRESULT softdd_create(void** out) {
    if (!g_dd) {
        g_dd = (DD*)calloc(1, sizeof *g_dd);
        g_dd->lpVtbl = &dd_vt;
        g_dd->v2 = &dd2_vt;
    }
    g_dd->refs++;
    *out = g_dd;
    fprintf(stderr, "[softdd] DirectDrawCreate -> software DirectDraw%s\n",
            softdd_headless ? " (headless)" : "");
    return DD_OK;
}

/* ------------------------------------------------ Surface3 / DirectDraw2 */

/* The newer interfaces are the same objects through a second vtable pointer
 * one slot in, forwarding to the v1 bodies; only what differs is written.
 * DirectShow's DirectDraw stream (the game's video) asks for both. */
#define S1(p) ((IDirectDrawSurface*)as_surf(p))
typedef IDirectDrawSurface3 S3;

static HRESULT WINAPI t_QueryInterface(S3* p, REFIID r, LPVOID* o) { return s_QueryInterface(S1(p), r, o); }
static ULONG WINAPI t_AddRef(S3* p) { return s_AddRef(S1(p)); }
static ULONG WINAPI t_Release(S3* p) { return s_Release(S1(p)); }
static HRESULT WINAPI t_AddAttachedSurface(S3* p, LPDIRECTDRAWSURFACE3 a) { return s_AddAttachedSurface(S1(p), S1(a)); }
static HRESULT WINAPI t_AddOverlayDirtyRect(S3* p, LPRECT r) { return s_AddOverlayDirtyRect(S1(p), r); }
static HRESULT WINAPI t_Blt(S3* p, LPRECT d, LPDIRECTDRAWSURFACE3 s, LPRECT r, DWORD f, LPDDBLTFX fx) {
    return s_Blt(S1(p), d, S1(s), r, f, fx);
}
static HRESULT WINAPI t_BltBatch(S3* p, LPDDBLTBATCH b, DWORD n, DWORD f) { return s_BltBatch(S1(p), b, n, f); }
static HRESULT WINAPI t_BltFast(S3* p, DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 s, LPRECT r, DWORD t) {
    return s_BltFast(S1(p), x, y, S1(s), r, t);
}
static HRESULT WINAPI t_DeleteAttachedSurface(S3* p, DWORD f, LPDIRECTDRAWSURFACE3 a) {
    return s_DeleteAttachedSurface(S1(p), f, S1(a));
}
static HRESULT WINAPI t_EnumAttachedSurfaces(S3* p, LPVOID c, LPDDENUMSURFACESCALLBACK cb) {
    return s_EnumAttachedSurfaces(S1(p), c, cb);
}
static HRESULT WINAPI t_EnumOverlayZOrders(S3* p, DWORD f, LPVOID c, LPDDENUMSURFACESCALLBACK cb) {
    return s_EnumOverlayZOrders(S1(p), f, c, cb);
}
static HRESULT WINAPI t_Flip(S3* p, LPDIRECTDRAWSURFACE3 o, DWORD f) { return s_Flip(S1(p), S1(o), f); }
static HRESULT WINAPI t_GetAttachedSurface(S3* p, LPDDSCAPS c, LPDIRECTDRAWSURFACE3* o) {
    LPDIRECTDRAWSURFACE s1;
    HRESULT hr = s_GetAttachedSurface(S1(p), c, &s1);
    *o = hr == DD_OK ? (LPDIRECTDRAWSURFACE3)&((Surf*)s1)->v3 : NULL;
    return hr;
}
static HRESULT WINAPI t_GetBltStatus(S3* p, DWORD f) { return s_GetBltStatus(S1(p), f); }
static HRESULT WINAPI t_GetCaps(S3* p, LPDDSCAPS c) { return s_GetCaps(S1(p), c); }
static HRESULT WINAPI t_GetClipper(S3* p, LPDIRECTDRAWCLIPPER* c) { return s_GetClipper(S1(p), c); }
static HRESULT WINAPI t_GetColorKey(S3* p, DWORD f, LPDDCOLORKEY k) { return s_GetColorKey(S1(p), f, k); }
static HRESULT WINAPI t_GetDC(S3* p, HDC* d) { return s_GetDC(S1(p), d); }
static HRESULT WINAPI t_GetFlipStatus(S3* p, DWORD f) { return s_GetFlipStatus(S1(p), f); }
static HRESULT WINAPI t_GetOverlayPosition(S3* p, LPLONG x, LPLONG y) { return s_GetOverlayPosition(S1(p), x, y); }
static HRESULT WINAPI t_GetPalette(S3* p, LPDIRECTDRAWPALETTE* l) { return s_GetPalette(S1(p), l); }
static HRESULT WINAPI t_GetPixelFormat(S3* p, LPDDPIXELFORMAT f) { return s_GetPixelFormat(S1(p), f); }
static HRESULT WINAPI t_GetSurfaceDesc(S3* p, LPDDSURFACEDESC d) { return s_GetSurfaceDesc(S1(p), d); }
static HRESULT WINAPI t_Initialize(S3* p, LPDIRECTDRAW d, LPDDSURFACEDESC s) { return s_Initialize(S1(p), d, s); }
static HRESULT WINAPI t_IsLost(S3* p) { return s_IsLost(S1(p)); }
static HRESULT WINAPI t_Lock(S3* p, LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e) { return s_Lock(S1(p), r, d, f, e); }
static HRESULT WINAPI t_ReleaseDC(S3* p, HDC d) { return s_ReleaseDC(S1(p), d); }
static HRESULT WINAPI t_Restore(S3* p) { return s_Restore(S1(p)); }
static HRESULT WINAPI t_SetClipper(S3* p, LPDIRECTDRAWCLIPPER c) { return s_SetClipper(S1(p), c); }
static HRESULT WINAPI t_SetColorKey(S3* p, DWORD f, LPDDCOLORKEY k) { return s_SetColorKey(S1(p), f, k); }
static HRESULT WINAPI t_SetOverlayPosition(S3* p, LONG x, LONG y) { return s_SetOverlayPosition(S1(p), x, y); }
static HRESULT WINAPI t_SetPalette(S3* p, LPDIRECTDRAWPALETTE l) { return s_SetPalette(S1(p), l); }
static HRESULT WINAPI t_Unlock(S3* p, LPVOID v) { return s_Unlock(S1(p), v); }
static HRESULT WINAPI t_UpdateOverlay(S3* p, LPRECT a, LPDIRECTDRAWSURFACE3 b, LPRECT c, DWORD f, LPDDOVERLAYFX fx) {
    return s_UpdateOverlay(S1(p), a, S1(b), c, f, fx);
}
static HRESULT WINAPI t_UpdateOverlayDisplay(S3* p, DWORD f) { return s_UpdateOverlayDisplay(S1(p), f); }
static HRESULT WINAPI t_UpdateOverlayZOrder(S3* p, DWORD f, LPDIRECTDRAWSURFACE3 r) {
    return s_UpdateOverlayZOrder(S1(p), f, S1(r));
}
static HRESULT WINAPI t_GetDDInterface(S3* p, LPVOID* o) {
    (void)p;
    g_dd->refs++;
    *o = g_dd;
    return DD_OK;
}
static HRESULT WINAPI t_PageLock(S3* p, DWORD f) { (void)p; (void)f; return DD_OK; }
static HRESULT WINAPI t_PageUnlock(S3* p, DWORD f) { (void)p; (void)f; return DD_OK; }
static HRESULT WINAPI t_SetSurfaceDesc(S3* p, LPDDSURFACEDESC d, DWORD f) {
    (void)p; (void)d; (void)f; UNIMPL("Surface3::SetSurfaceDesc"); return DDERR_UNSUPPORTED;
}
#undef S1

static IDirectDrawSurface3Vtbl surf3_vt = {
    t_QueryInterface, t_AddRef, t_Release, t_AddAttachedSurface, t_AddOverlayDirtyRect, t_Blt,
    t_BltBatch, t_BltFast, t_DeleteAttachedSurface, t_EnumAttachedSurfaces, t_EnumOverlayZOrders,
    t_Flip, t_GetAttachedSurface, t_GetBltStatus, t_GetCaps, t_GetClipper, t_GetColorKey, t_GetDC,
    t_GetFlipStatus, t_GetOverlayPosition, t_GetPalette, t_GetPixelFormat, t_GetSurfaceDesc,
    t_Initialize, t_IsLost, t_Lock, t_ReleaseDC, t_Restore, t_SetClipper, t_SetColorKey,
    t_SetOverlayPosition, t_SetPalette, t_Unlock, t_UpdateOverlay, t_UpdateOverlayDisplay,
    t_UpdateOverlayZOrder, t_GetDDInterface, t_PageLock, t_PageUnlock, t_SetSurfaceDesc,
};

#define D1 ((IDirectDraw*)g_dd)
typedef IDirectDraw2 D2;

static HRESULT WINAPI e_QueryInterface(D2* p, REFIID r, LPVOID* o) { (void)p; return d_QueryInterface(D1, r, o); }
static ULONG WINAPI e_AddRef(D2* p) { (void)p; return d_AddRef(D1); }
static ULONG WINAPI e_Release(D2* p) { (void)p; return d_Release(D1); }
static HRESULT WINAPI e_Compact(D2* p) { (void)p; return d_Compact(D1); }
static HRESULT WINAPI e_CreateClipper(D2* p, DWORD f, LPDIRECTDRAWCLIPPER* o, IUnknown* u) {
    (void)p; return d_CreateClipper(D1, f, o, u);
}
static HRESULT WINAPI e_CreatePalette(D2* p, DWORD f, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE* o, IUnknown* u) {
    (void)p; return d_CreatePalette(D1, f, e, o, u);
}
static HRESULT WINAPI e_CreateSurface(D2* p, LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* o, IUnknown* u) {
    (void)p; return d_CreateSurface(D1, d, o, u);
}
static HRESULT WINAPI e_DuplicateSurface(D2* p, LPDIRECTDRAWSURFACE s, LPDIRECTDRAWSURFACE* o) {
    (void)p; return d_DuplicateSurface(D1, s, o);
}
static HRESULT WINAPI e_EnumDisplayModes(D2* p, DWORD f, LPDDSURFACEDESC d, LPVOID c, LPDDENUMMODESCALLBACK cb) {
    (void)p; return d_EnumDisplayModes(D1, f, d, c, cb);
}
static HRESULT WINAPI e_EnumSurfaces(D2* p, DWORD f, LPDDSURFACEDESC d, LPVOID c, LPDDENUMSURFACESCALLBACK cb) {
    (void)p; return d_EnumSurfaces(D1, f, d, c, cb);
}
static HRESULT WINAPI e_FlipToGDISurface(D2* p) { (void)p; return d_FlipToGDISurface(D1); }
static HRESULT WINAPI e_GetCaps(D2* p, LPDDCAPS a, LPDDCAPS b) { (void)p; return d_GetCaps(D1, a, b); }
static HRESULT WINAPI e_GetDisplayMode(D2* p, LPDDSURFACEDESC d) { (void)p; return d_GetDisplayMode(D1, d); }
static HRESULT WINAPI e_GetFourCCCodes(D2* p, LPDWORD n, LPDWORD c) { (void)p; return d_GetFourCCCodes(D1, n, c); }
static HRESULT WINAPI e_GetGDISurface(D2* p, LPDIRECTDRAWSURFACE* o) { (void)p; return d_GetGDISurface(D1, o); }
static HRESULT WINAPI e_GetMonitorFrequency(D2* p, LPDWORD f) { (void)p; return d_GetMonitorFrequency(D1, f); }
static HRESULT WINAPI e_GetScanLine(D2* p, LPDWORD l) { (void)p; return d_GetScanLine(D1, l); }
static HRESULT WINAPI e_GetVerticalBlankStatus(D2* p, LPBOOL b) { (void)p; return d_GetVerticalBlankStatus(D1, b); }
static HRESULT WINAPI e_Initialize(D2* p, GUID* g) { (void)p; return d_Initialize(D1, g); }
static HRESULT WINAPI e_RestoreDisplayMode(D2* p) { (void)p; return d_RestoreDisplayMode(D1); }
static HRESULT WINAPI e_SetCooperativeLevel(D2* p, HWND w, DWORD f) { (void)p; return d_SetCooperativeLevel(D1, w, f); }
static HRESULT WINAPI e_SetDisplayMode(D2* p, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD f) {
    (void)p; (void)rate; (void)f;
    return d_SetDisplayMode(D1, w, h, bpp);
}
static HRESULT WINAPI e_WaitForVerticalBlank(D2* p, DWORD f, HANDLE e) { (void)p; return d_WaitForVerticalBlank(D1, f, e); }
static HRESULT WINAPI e_GetAvailableVidMem(D2* p, LPDDSCAPS c, LPDWORD total, LPDWORD free_) {
    (void)p; (void)c;
    if (total) *total = 64u << 20;
    if (free_) *free_ = 64u << 20;
    return DD_OK;
}
#undef D1

static IDirectDraw2Vtbl dd2_vt = {
    e_QueryInterface, e_AddRef, e_Release, e_Compact, e_CreateClipper, e_CreatePalette,
    e_CreateSurface, e_DuplicateSurface, e_EnumDisplayModes, e_EnumSurfaces, e_FlipToGDISurface,
    e_GetCaps, e_GetDisplayMode, e_GetFourCCCodes, e_GetGDISurface, e_GetMonitorFrequency,
    e_GetScanLine, e_GetVerticalBlankStatus, e_Initialize, e_RestoreDisplayMode,
    e_SetCooperativeLevel, e_SetDisplayMode, e_WaitForVerticalBlank, e_GetAvailableVidMem,
};
