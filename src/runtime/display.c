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
    static uint32_t row[4096];
    for (UINT y = 0; g_ffmpeg && y < g_rec_h && g_rec_w <= 4096; y++) {
        const uint32_t* src = (const uint32_t*)((const uint8_t*)lr.pBits + (size_t)(y * d.Height / g_rec_h) * lr.Pitch);
        for (UINT x = 0; x < g_rec_w; x++) row[x] = src[x * d.Width / g_rec_w];
        fwrite(row, 4, g_rec_w, g_ffmpeg);
    }
    IDirect3DSurface9_UnlockRect(g_sys);
    if (g_ffmpeg && ++g_recorded == g_record_frames) {
        display_record_close();
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
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
