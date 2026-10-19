/*
 * Headless display: the game's frames never reach a screen.
 *
 * The game draws through DirectDraw/Direct3D (DirectX 5), and the Steam
 * release answers that with its own Sdata\ddraw.dll, a wrapper that renders
 * on a Direct3D 9 device (docs/RECON.md). The wrapper changes the display mode
 * only through that device (it imports no ChangeDisplaySettings), so headless
 * needs one seam: the wrapper's import of d3d9!Direct3DCreate9. The host
 * points it here, and every device it creates is made windowed, on the game's
 * own window, which the host keeps invisible (host.c, headless shims). Each
 * Present is a frame: counted, and with --record read back and piped to
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

long display_frames(void) { return g_frames; }

static void patch(void** slot, void* fn, void** real) {
    DWORD old;
    if (*slot == fn) return;
    *real = *slot;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, sizeof *slot, old, &old);
}

static void windowed(D3DPRESENT_PARAMETERS* pp) {
    if (!pp->Windowed)
        fprintf(stderr, "[display] fullscreen %ux%u device -> windowed, off screen\n",
                pp->BackBufferWidth, pp->BackBufferHeight);
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
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

static HRESULT WINAPI hl_Present(IDirect3DDevice9* dev, const RECT* s, const RECT* d, HWND w, const RGNDATA* r) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1 || n == 10 || n == 100 || n % 1000 == 0) fprintf(stderr, "[display] frame %ld presented\n", n);
    if (g_record) record(dev);
    return g_real_present(dev, s, d, w, r);
}

static HRESULT WINAPI hl_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_sys) { IDirect3DSurface9_Release(g_sys); g_sys = NULL; }   /* a Reset needs it gone */
    windowed(pp);
    return g_real_reset(dev, pp);
}

static HRESULT WINAPI hl_CreateDevice(IDirect3D9* d3d, UINT ad, D3DDEVTYPE t, HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    windowed(pp);
    HRESULT hr = g_real_createdev(d3d, ad, t, focus, flags, pp, out);
    fprintf(stderr, "[display] CreateDevice(%ux%u, windowed) -> 0x%08lX\n", pp->BackBufferWidth,
            pp->BackBufferHeight, hr);
    if (SUCCEEDED(hr)) {
        void** vt = *(void***)*out;
        patch(&vt[16], (void*)hl_Reset, (void**)&g_real_reset);
        patch(&vt[17], (void*)hl_Present, (void**)&g_real_present);
        if (getenv("SOTE_DRAW_LOG")) {
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

int display_headless(const char* sdata, const char* record, long frames) {
    char path[MAX_PATH];
    _snprintf(path, sizeof path - 1, "%s\\ddraw.dll", sdata);
    path[sizeof path - 1] = 0;
    HMODULE m = LoadLibraryA(path);     /* the one the guest's DDRAW.dll import then binds to */
    void** slot = m ? iat_slot(m, "d3d9.dll", "Direct3DCreate9") : NULL;
    if (!slot) {
        fprintf(stderr, "[display] %s: no d3d9!Direct3DCreate9 import to hook; headless needs Steam's wrapper\n", path);
        return 0;
    }
    patch(slot, (void*)hl_Direct3DCreate9, (void**)&g_real_create9);
    g_record = record, g_record_frames = frames;
    fprintf(stderr, "[display] headless: %s's Direct3D 9 devices are windowed, off screen\n", path);
    return 1;
}
