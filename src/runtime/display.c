/*
 * The display: Steam's Direct3D 9 wrapper, steered.
 *
 * The game draws through DirectDraw/Direct3D (DirectX 5), and the Steam
 * release answers that with its own Sdata\ddraw.dll, a wrapper that renders
 * on a Direct3D 9 device (docs/RECON.md). The wrapper changes the display mode
 * only through that device (it imports no ChangeDisplaySettings), so one seam
 * is enough: the wrapper's import of d3d9!Direct3DCreate9. The host points it
 * here, and every device it creates follows the F10 menu (menu.c):
 *   - borderless fullscreen (the default) or a window, instead of the
 *     exclusive fullscreen the game asks for: the device is made windowed, and
 *     each Present is scaled into the window with bars, aspect kept;
 *   - anisotropic filtering and vsync. Not MSAA: the wrapper draws into
 *     lockable render targets that stand in for DirectDraw surfaces, which
 *     cannot be multisampled, and copies them to the back buffer.
 * Headless, the device is windowed on the game's invisible window (host.c),
 * and each Present is counted and, with --record, read back and piped to
 * ffmpeg as raw BGRX (REPO_RULES 10/13: works over RDP, takes no screen).
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "menu.h"

typedef IDirect3D9* (WINAPI *create9_t)(UINT);
typedef HRESULT (WINAPI *createdev_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                      D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (WINAPI *reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (WINAPI *present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

static create9_t g_real_create9;
static createdev_t g_real_createdev;
static reset_t g_real_reset;
static present_t g_real_present;

static volatile LONG g_frames;
static const char* g_record;
static long g_record_frames;
static FILE* g_ffmpeg;
static UINT g_rec_w, g_rec_h;
static long g_recorded;
static DWORD g_next_shot;
static IDirect3DSurface9* g_sys;       /* system-memory copy of the back buffer */
static UINT g_sys_w, g_sys_h;
static int g_headless;
static HWND g_hwnd;                    /* the device's window: the game's */
static UINT g_bb_w, g_bb_h;            /* the back buffer: the game's resolution */
static volatile LONG g_placed;
static DWORD g_max_aniso;

long display_frames(void) { return g_frames; }
float display_aspect(void) { return g_bb_h ? (float)g_bb_w / (float)g_bb_h : 0.0f; }

static void patch(void** slot, void* fn, void** real) {
    DWORD old;
    if (*slot == fn) return;
    *real = *slot;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, sizeof *slot, old, &old);
}

/* The game asks for exclusive fullscreen at its resolution. Unless the
 * player chose that (as shipped), the device is windowed instead, on the
 * game's window, which display_place() makes borderless or a window. */
static void setup(D3DPRESENT_PARAMETERS* pp) {
    if (g_headless || g_set.window != 2) {
        if (!pp->Windowed)
            fprintf(stderr, "[display] fullscreen %ux%u device -> windowed%s\n", pp->BackBufferWidth,
                    pp->BackBufferHeight, g_headless ? ", off screen" : "");
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    }
    pp->PresentationInterval = !g_headless && g_set.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    g_bb_w = pp->BackBufferWidth, g_bb_h = pp->BackBufferHeight;
    if (pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;
    InterlockedExchange(&g_placed, 0);
}

/* Borderless: the window covers its monitor. A window: the game's resolution
 * if it fits in 90% of the screen, else scaled down to that, centred. The
 * picture keeps its aspect either way (hl_Present). */
void display_place(void) {
    HWND h = g_hwnd;
    MONITORINFO mi = { sizeof mi };
    if (!h || g_headless || g_set.window == 2) return;
    GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    if (g_set.window == 0) {
        RECT r = mi.rcMonitor;
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        RECT wa = mi.rcWork;
        int aw = (wa.right - wa.left) * 9 / 10, ah = (wa.bottom - wa.top) * 9 / 10;
        int w = (int)g_bb_w, hh = (int)g_bb_h;
        if (w > aw || hh > ah) {
            double k = (double)aw / w < (double)ah / hh ? (double)aw / w : (double)ah / hh;
            w = (int)(w * k), hh = (int)(hh * k);
        }
        RECT r = { 0, 0, w, hh };
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        AdjustWindowRect(&r, style, FALSE);
        SetWindowLongA(h, GWL_STYLE, style);
        SetWindowPos(h, HWND_NOTOPMOST, wa.left + ((wa.right - wa.left) - (r.right - r.left)) / 2,
                     wa.top + ((wa.bottom - wa.top) - (r.bottom - r.top)) / 2, r.right - r.left,
                     r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    fprintf(stderr, "[display] %s\n", g_set.window ? "a window" : "borderless fullscreen");
}

void display_record_close(void) {
    if (g_ffmpeg) {
        _pclose(g_ffmpeg);
        g_ffmpeg = NULL;
        fprintf(stderr, "[record] %ld frames -> %s\n", g_recorded, g_record);
    }
}

/* The back buffer, read back and written to ffmpeg at most 30 times a second.
 * The recording keeps the size it started with; a later mode is scaled into
 * it, nearest neighbour (written at another size, every frame would shear). */
static void record(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    DWORD now = GetTickCount();
    if ((LONG)(now - g_next_shot) < 0) return;
    g_next_shot = now + 33;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (g_sys && (g_sys_w != d.Width || g_sys_h != d.Height)) { IDirect3DSurface9_Release(g_sys); g_sys = NULL; }
    if (!g_sys && FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format,
                                                                      D3DPOOL_SYSTEMMEM, &g_sys, NULL))) {
        IDirect3DSurface9_Release(bb);
        return;
    }
    g_sys_w = d.Width, g_sys_h = d.Height;
    HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, g_sys);
    IDirect3DSurface9_Release(bb);
    if (FAILED(hr) || FAILED(IDirect3DSurface9_LockRect(g_sys, &lr, NULL, D3DLOCK_READONLY))) return;
    if (!g_ffmpeg) {
        char cmd[MAX_PATH * 2];
        g_rec_w = d.Width & ~1u, g_rec_h = d.Height & ~1u;
        _snprintf(cmd, sizeof cmd - 1, "ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgr0 -s %ux%u -r 30 "
                  "-i - -c:v libx264 -pix_fmt yuv420p \"%s\"", g_rec_w, g_rec_h, g_record);
        g_ffmpeg = _popen(cmd, "wb");
        fprintf(stderr, "[record] %ux%u -> %s\n", g_rec_w, g_rec_h, g_record);
    }
    /* Written as many times as 30 fps owes since the start, so the video's
     * clock is the run's: "at 22 s" in a script is 0:22 in the file, even
     * when reading the frame back is slower than the game. */
    static DWORD start;
    static uint32_t frame[4096 * 2160];
    if (!start) start = now;
    long owed = (long)((now - start) / 33.333) + 1 - g_recorded;
    for (UINT y = 0; y < g_rec_h && g_rec_w <= 4096; y++) {
        const uint32_t* src = (const uint32_t*)((const uint8_t*)lr.pBits + (size_t)(y * d.Height / g_rec_h) * lr.Pitch);
        for (UINT x = 0; x < g_rec_w; x++) frame[y * g_rec_w + x] = src[x * d.Width / g_rec_w];
    }
    IDirect3DSurface9_UnlockRect(g_sys);
    for (; g_ffmpeg && owed > 0; owed--) {
        fwrite(frame, 4, (size_t)g_rec_w * g_rec_h, g_ffmpeg);
        if (++g_recorded == g_record_frames) break;
    }
    if (g_ffmpeg && g_recorded == g_record_frames) {
        display_record_close();
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

/* SOTE_DRAW_LOG=1: draw calls per second and a sample vertex, to compare a
 * lifted run with --original (docs/bringup.md). */
typedef HRESULT (WINAPI *dpup_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (WINAPI *dipup_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                  D3DFORMAT, const void*, UINT);
static dpup_t g_real_dpup;
static dipup_t g_real_dipup;
static volatile LONG g_draws, g_prims;

/* SOTE_WATCH_SY=1: a hardware write watch on a drawn vertex's y, set once
 * the game is drawing 3D; the next write to it names the lifted function that
 * computes it (how the projection bug was found, docs/bringup.md). */
extern uint32_t g_cur_func;
static volatile uint32_t g_watch;
#define WATCH_SET 0xE0000001u

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS* ep) {
    CONTEXT* c = ep->ContextRecord;
    if (ep->ExceptionRecord->ExceptionCode == WATCH_SET) {
        c->Dr0 = g_watch;
        c->Dr7 = 1 | (1u << 16) | (3u << 18);     /* L0, write, 4 bytes */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (c->Dr6 & 1)) {
        static int hits;
        fprintf(stderr, "[watch] y at 0x%08X = %g written in lifted sub_%08X\n", g_watch,
                *(volatile float*)(uintptr_t)g_watch, g_cur_func);
        c->Dr6 = 0;
        if (++hits >= 6) c->Dr7 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* SOTE_DUMP=ADDR:N[,ADDR:N...]: every 2 s of 3D drawing, print N floats
 * from each guest ADDR, four to a line, skipping a block that is all zero or
 * an identity matrix: the same dump from a lifted run and an --original one
 * shows which stage a value first goes wrong in. */
static void dump_once(void) {
    static DWORD next;
    char spec[512], *p;
    const char* e = getenv("SOTE_DUMP");
    if (!e || (LONG)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 2000;
    strncpy(spec, e, sizeof spec - 1), spec[sizeof spec - 1] = 0;
    for (p = strtok(spec, ","); p; p = strtok(NULL, ",")) {
        uint32_t a = strtoul(p, NULL, 0);
        int n = strchr(p, ':') ? atoi(strchr(p, ':') + 1) : 16, dull = 1;
        const float* b = (const float*)(uintptr_t)a;
        for (int i = 0; i < n; i++)
            if (b[i] != (n == 16 && i % 5 == 0 ? 1.0f : 0.0f)) dull = 0;
        if (dull) continue;
        for (int i = 0; i < n; i += 4) {
            const float* f = (const float*)(uintptr_t)(a + 4 * i);
            fprintf(stderr, "[dump] %08X: %12g %12g %12g %12g\n", a + 4 * i, f[0], f[1], f[2], f[3]);
        }
    }
}

static void sample(const void* v, UINT stride) {
    static DWORD next;
    const float* f = (const float*)v;
    static long n3d;
    if (f[3] != 1.0f && ++n3d > 5000) dump_once();    /* into the attract demo */
    if (!g_watch && f[3] != 1.0f && getenv("SOTE_WATCH_SY")) {
        /* SOTE_WATCH_SY=ADDR: watch *(ADDR) + 4 instead (a guest vertex list) */
        uint32_t at = strtoul(getenv("SOTE_WATCH_SY"), NULL, 0);
        g_watch = at > 1 ? *(volatile uint32_t*)(uintptr_t)at + 4 : (uint32_t)(uintptr_t)(f + 1);
        AddVectoredExceptionHandler(1, watch_veh);
        RaiseException(WATCH_SET, 0, 0, NULL);
        fprintf(stderr, "[watch] watching y at 0x%08X\n", g_watch);
    }
    if ((LONG)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 1000;
    fprintf(stderr, "[draw] frame %ld: %ld draws, %ld prims/s; vertex (%g, %g, %g, %g) stride %u\n",
            g_frames, g_draws, g_prims, f[0], f[1], f[2], f[3], stride);
    g_draws = g_prims = 0;
}

/* 3D vertices drawn, and how many sat on the screen's middle row: the flat-y
 * lift bug's signature (docs/bringup.md). */
static volatile LONG g_v3d, g_vflat;

long display_flat_percent(void) { return g_v3d ? (long)(100.0 * g_vflat / g_v3d) : -1; }

static void count_flat(const void* v, UINT stride, UINT nverts) {
    for (UINT i = 0; i < nverts && i < 64; i++) {
        const float* f = (const float*)((const uint8_t*)v + i * stride);
        if (f[3] == 1.0f) continue;
        InterlockedIncrement(&g_v3d);
        if (f[1] >= 239.5f && f[1] <= 241.0f) InterlockedIncrement(&g_vflat);
    }
}

static HRESULT WINAPI hl_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride) {
    count_flat(v, stride, t == D3DPT_TRIANGLELIST ? n * 3 : n + 2);
    InterlockedIncrement(&g_draws);
    InterlockedExchangeAdd(&g_prims, (LONG)n);
    sample(v, stride);
    return g_real_dpup(dev, t, n, v, stride);
}

static HRESULT WINAPI hl_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT lo, UINT nv,
                                                UINT n, const void* idx, D3DFORMAT f, const void* v, UINT stride) {
    count_flat(v, stride, nv);
    InterlockedIncrement(&g_draws);
    InterlockedExchangeAdd(&g_prims, (LONG)n);
    sample(v, stride);
    return g_real_dipup(dev, t, lo, nv, n, idx, f, v, stride);
}

/* Anisotropic filtering at the level the menu set (the device's most, if
 * lower), for every texture the wrapper filters. */
typedef HRESULT (WINAPI *sampler_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
static sampler_t g_real_sampler;

static HRESULT WINAPI hl_SetSamplerState(IDirect3DDevice9* dev, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
    static DWORD seen[4][3] = { { 9, 9, 9 }, { 9, 9, 9 }, { 9, 9, 9 }, { 9, 9, 9 } };
    if (t >= D3DSAMP_MAGFILTER && t <= D3DSAMP_MIPFILTER && s < 4 && seen[s][t - D3DSAMP_MAGFILTER] != v &&
        getenv("SOTE_DRAW_LOG")) {
        seen[s][t - D3DSAMP_MAGFILTER] = v;
        fprintf(stderr, "[display] frame %ld sampler %lu: %s = %lu\n", g_frames, s,
                t == D3DSAMP_MAGFILTER ? "mag" : t == D3DSAMP_MINFILTER ? "min" : "mip", v);
    }
    /* the wrapper asks for anisotropic minification itself, at the default
     * level of 1; the 2D layer is point sampled and stays so */
    if (t == D3DSAMP_MINFILTER && (v == D3DTEXF_LINEAR || v == D3DTEXF_ANISOTROPIC) && g_set.aniso && g_max_aniso > 1) {
        g_real_sampler(dev, s, D3DSAMP_MAXANISOTROPY, (DWORD)g_set.aniso < g_max_aniso ? (DWORD)g_set.aniso : g_max_aniso);
        v = D3DTEXF_ANISOTROPIC;
    }
    return g_real_sampler(dev, s, t, v);
}

/* SOTE_DRAW_LOG: what the wrapper renders into besides the back buffer. */
typedef HRESULT (WINAPI *mktex_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                  IDirect3DTexture9**, HANDLE*);
typedef HRESULT (WINAPI *mkrt_t)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                 IDirect3DSurface9**, HANDLE*);
static mktex_t g_real_mktex;
static mkrt_t g_real_mkrt;

static HRESULT WINAPI hl_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT lv, DWORD use, D3DFORMAT f,
                                       D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh) {
    HRESULT hr = g_real_mktex(dev, w, h, lv, use, f, pool, out, sh);
    if (use & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
        fprintf(stderr, "[display] CreateTexture(%ux%u, usage 0x%lX, format %d) -> 0x%08lX\n", w, h, use, (int)f, hr);
    return hr;
}

static HRESULT WINAPI hl_CreateRenderTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                            DWORD q, BOOL lock, IDirect3DSurface9** out, HANDLE* sh) {
    HRESULT hr = g_real_mkrt(dev, w, h, f, ms, q, lock, out, sh);
    fprintf(stderr, "[display] CreateRenderTarget(%ux%u, format %d, msaa %d, lockable %d) -> 0x%08lX\n", w, h, (int)f,
            (int)ms, lock, hr);
    return hr;
}

/* Black bars beside the picture: Present only paints its own rectangle. */
static void bars(HWND h, const RECT* pic, int cw, int ch) {
    HDC dc = GetDC(h);
    HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT r[4] = { { 0, 0, cw, pic->top }, { 0, pic->bottom, cw, ch },
                  { 0, pic->top, pic->left, pic->bottom }, { pic->right, pic->top, cw, pic->bottom } };
    for (int i = 0; i < 4; i++)
        if (r[i].right > r[i].left && r[i].bottom > r[i].top) FillRect(dc, &r[i], b);
    ReleaseDC(h, dc);
}

static HRESULT WINAPI hl_Present(IDirect3DDevice9* dev, const RECT* s, const RECT* d, HWND w, const RGNDATA* r) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1 || n == 10 || n == 100 || n % 1000 == 0) fprintf(stderr, "[display] frame %ld presented\n", n);
    if (g_record) record(dev);
    if (!g_headless && g_set.window != 2 && g_hwnd && g_bb_w && g_bb_h) {
        RECT c;
        /* placed here, after the wrapper and the game have done with the
         * window for this mode */
        if (!InterlockedExchange(&g_placed, 1)) display_place();
        GetClientRect(g_hwnd, &c);
        if (c.right > 0 && c.bottom > 0) {
            double k = (double)c.right / g_bb_w < (double)c.bottom / g_bb_h ? (double)c.right / g_bb_w
                                                                             : (double)c.bottom / g_bb_h;
            int pw = (int)(g_bb_w * k + 0.5), ph = (int)(g_bb_h * k + 0.5);
            RECT pic = { (c.right - pw) / 2, (c.bottom - ph) / 2, (c.right - pw) / 2 + pw, (c.bottom - ph) / 2 + ph };
            if (pw < c.right || ph < c.bottom) bars(g_hwnd, &pic, c.right, c.bottom);
            return g_real_present(dev, NULL, &pic, NULL, NULL);
        }
    }
    return g_real_present(dev, s, d, w, r);
}

static HRESULT WINAPI hl_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_sys) { IDirect3DSurface9_Release(g_sys); g_sys = NULL; }   /* a Reset needs it gone */
    setup(pp);
    return g_real_reset(dev, pp);
}

static HRESULT WINAPI hl_CreateDevice(IDirect3D9* d3d, UINT ad, D3DDEVTYPE t, HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    D3DCAPS9 caps;
    if (!g_hwnd) g_hwnd = focus;
    g_max_aniso = SUCCEEDED(IDirect3D9_GetDeviceCaps(d3d, ad, t, &caps)) &&
                  (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) ? caps.MaxAnisotropy : 0;
    setup(pp);
    HRESULT hr = g_real_createdev(d3d, ad, t, focus, flags, pp, out);
    fprintf(stderr, "[display] CreateDevice(%ux%u, %s, anisotropic %lu of %lu, vsync %d) -> 0x%08lX\n",
            pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed ? "windowed" : "exclusive",
            g_set.aniso ? (DWORD)g_set.aniso : 0, g_max_aniso, pp->PresentationInterval == D3DPRESENT_INTERVAL_ONE, hr);
    if (SUCCEEDED(hr)) {
        void** vt = *(void***)*out;
        patch(&vt[16], (void*)hl_Reset, (void**)&g_real_reset);
        patch(&vt[17], (void*)hl_Present, (void**)&g_real_present);
        patch(&vt[69], (void*)hl_SetSamplerState, (void**)&g_real_sampler);
        if (getenv("SOTE_DRAW_LOG")) {
            patch(&vt[23], (void*)hl_CreateTexture, (void**)&g_real_mktex);
            patch(&vt[28], (void*)hl_CreateRenderTarget, (void**)&g_real_mkrt);
            patch(&vt[83], (void*)hl_DrawPrimitiveUP, (void**)&g_real_dpup);
            patch(&vt[84], (void*)hl_DrawIndexedPrimitiveUP, (void**)&g_real_dipup);
        }
    }
    return hr;
}

static IDirect3D9* WINAPI hl_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d3d = g_real_create9(sdk);
    if (d3d) patch(&(*(void***)d3d)[16], (void*)hl_CreateDevice, (void**)&g_real_createdev);
    return d3d;
}

/* The IAT slot module `m` imports `dll!fn` through, or NULL. */
static void** iat_slot(HMODULE m, const char* dll, const char* fn) {
    uint8_t* b = (uint8_t*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress); dir.Size && d->Name; d++) {
        if (_stricmp((const char*)(b + d->Name), dll)) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++)
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) &&
                !strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(b + names->u1.AddressOfData))->Name, fn))
                return (void**)&slots->u1.Function;
    }
    return NULL;
}

int display_init(const char* sdata, int headless, const char* record, long frames) {
    char path[MAX_PATH];
    _snprintf(path, sizeof path - 1, "%s\\ddraw.dll", sdata);
    path[sizeof path - 1] = 0;
    HMODULE m = LoadLibraryA(path);     /* the one the guest's DDRAW.dll import then binds to */
    void** slot = m ? iat_slot(m, "d3d9.dll", "Direct3DCreate9") : NULL;
    if (!slot) {
        fprintf(stderr, "[display] %s: no d3d9!Direct3DCreate9 import to hook (not Steam's wrapper): "
                "the display settings and headless need it\n", path);
        return 0;
    }
    patch(slot, (void*)hl_Direct3DCreate9, (void**)&g_real_create9);
    g_headless = headless, g_record = record, g_record_frames = frames;
    fprintf(stderr, "[display] %s's Direct3D 9 devices: %s\n", path,
            headless ? "windowed, off screen" : "as the F10 menu sets");
    return 1;
}
