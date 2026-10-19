/*
 * Star Wars: Shadows of the Empire - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and machine lock; see its header). What is here is only what is specific to
 * this game: where the image goes, the command line, the registry, and the
 * fault report. Its shape is Fallout Tactics' host, trimmed to what this game
 * has needed so far; docs/host.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000..0x008F6000 is
 * free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"
#include "display.h"
#include "pad.h"
#include "menu.h"
#include "oracle.h"
#include "recomp_target.h"   /* the binary this build runs: run_lift.py */

extern const uint32_t sote_entry_va;    /* recomp_dispatch.c */
extern const uint32_t sote_func_starts[], sote_func_start_count;

#define SOTE_IMAGE_BASE 0x00400000u
/* A NULL module is the process exe, which is the host; mean the guest. */
#define GUEST_MODULE(h) ((h) ? (h) : SOTE_IMAGE_BASE)

static DWORD g_watchdog_s;
static int g_original;                 /* --original: the shipping machine code (oracle.c) */

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ---- the guest's identity ------------------------------------------------
 * The guest is Shadows.exe in Sdata\, not this host. Its hInstance (resources,
 * window classes) comes from GetModuleHandleA(NULL), and it finds its files
 * relative to GetModuleFileNameA. */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 1024];

static void shim_GetModuleHandleA(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)ARG(0)) : SOTE_IMAGE_BASE;
    g_esp += 4 + 1 * 4;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == SOTE_IMAGE_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            uint32_t k = n < size ? n : size - 1;
            memcpy(out, g_guest_exe, k);
            out[k] = 0;
            n = k;
        }
        g_eax = n;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size);
    }
    g_esp += 4 + 3 * 4;
}

static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

static void shim_OutputDebugStringA(void) {
    const char* s = gstr(ARG(0));
    size_t n = strlen(s);
    fprintf(stderr, "[ods] %s%s", s, n && s[n - 1] == '\n' ? "" : "\n");
    g_esp += 4 + 1 * 4;
}

/* The game opens with its own launcher, dialog 102 ("Main Menu": Play, New
 * Player, Restore Player, View Trailer, Quit, and the display settings), and
 * may warn about missing hardware features (dialog 103: Continue, Quit).
 * Every dialog procedure is wrapped: headless, the dialog is made invisible
 * before it is first shown; with --play the launcher's Play (1000) and the
 * warning's Continue (IDOK) are pressed as soon as each is up. */
#define DLG_LAUNCHER 102
#define DLG_HARDWARE 103
#define ID_PLAY 1000
static int g_headless, g_autoplay;
static uint32_t g_dlg_pending;         /* the guest procedure of the dialog being created */

static INT_PTR CALLBACK hl_dlgproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    uint32_t guest = (uint32_t)(uintptr_t)GetPropA(h, "sote.dlgproc");
    if (!guest) guest = g_dlg_pending;
    if (m == WM_INITDIALOG) {
        SetPropA(h, "sote.dlgproc", (HANDLE)(uintptr_t)guest);
        if (g_headless) {
            SetWindowLongA(h, GWL_EXSTYLE, GetWindowLongA(h, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                           WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
            SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
        }
    }
    /* guest code: the call faults on the non-executable page and native32
     * runs the lifted body */
    INT_PTR r = ((DLGPROC)(uintptr_t)guest)(h, m, w, l);
    if (m == WM_INITDIALOG && g_autoplay) {
        int id = (int)(intptr_t)GetPropA(h, "sote.dlgid");
        if (id == DLG_LAUNCHER) PostMessageA(h, WM_COMMAND, MAKEWPARAM(ID_PLAY, BN_CLICKED), 0);
        if (id == DLG_HARDWARE) PostMessageA(h, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        fprintf(stderr, "[dialog] %d up%s\n", id,
                id == DLG_LAUNCHER ? ": pressing Play" : id == DLG_HARDWARE ? ": pressing Continue" : "");
    }
    return r;
}

static int g_dlg_creating;             /* resource ID, for hl_dlgproc's WM_INITDIALOG */

static LRESULT CALLBACK dlg_cbt(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_CREATEWND && g_dlg_creating) {
        SetPropA((HWND)w, "sote.dlgid", (HANDLE)(intptr_t)g_dlg_creating);
        g_dlg_creating = 0;
    }
    return CallNextHookEx(NULL, code, w, l);
}

static void shim_DialogBoxParamA(void) {
    uint32_t saved = g_dlg_pending;
    uint32_t name = ARG(1);
    HHOOK hook = SetWindowsHookExA(WH_CBT, dlg_cbt, NULL, GetCurrentThreadId());
    g_dlg_pending = ARG(3);
    g_dlg_creating = name < 0x10000 ? (int)name : -1;
    g_eax = (uint32_t)DialogBoxParamA((HINSTANCE)(uintptr_t)GUEST_MODULE(ARG(0)),
                                      (LPCSTR)(uintptr_t)name, (HWND)(uintptr_t)ARG(2),
                                      ARG(3) ? hl_dlgproc : NULL, (LPARAM)ARG(4));
    if (hook) UnhookWindowsHookEx(hook);
    g_dlg_pending = saved;
    g_esp += 4 + 5 * 4;
}

static void shim_MessageBoxA(void) {
    fprintf(stderr, "[messagebox] %s: %s\n", gstr(ARG(2)), gstr(ARG(1)));
    g_eax = (uint32_t)MessageBoxA((HWND)(uintptr_t)ARG(0), (LPCSTR)(uintptr_t)ARG(1),
                                  (LPCSTR)(uintptr_t)ARG(2), ARG(3));
    g_esp += 4 + 4 * 4;
}

/* ---- the registry --------------------------------------------------------
 * The game keeps its paths and options under HKLM\SOFTWARE\LucasArts
 * Entertainment Company\Shadows of the Empire. Steam's Install.bat writes them
 * on first launch, and HKLM needs an administrator. The recomp keeps them under
 * HKCU instead (tools/setup.ps1 writes them), so the game reads and saves its
 * settings with no elevation and without touching a Steam install's keys. */
static HKEY reg_root(uint32_t key, const char* sub) {
    if ((HKEY)(uintptr_t)key == HKEY_LOCAL_MACHINE && sub && !_strnicmp(sub, "SOFTWARE\\LucasArts", 18))
        return HKEY_CURRENT_USER;
    return (HKEY)(uintptr_t)key;
}

static void shim_RegOpenKeyExA(void) {
    const char* sub = (const char*)(uintptr_t)ARG(1);
    g_eax = (uint32_t)RegOpenKeyExA(reg_root(ARG(0), sub), sub, ARG(2), ARG(3), (PHKEY)(uintptr_t)ARG(4));
    if (getenv("SOTE_REG_LOG")) fprintf(stderr, "[reg] open %s -> %u\n", gstr(ARG(1)), g_eax);
    g_esp += 4 + 5 * 4;
}

static void shim_RegCreateKeyExA(void) {
    const char* sub = (const char*)(uintptr_t)ARG(1);
    g_eax = (uint32_t)RegCreateKeyExA(reg_root(ARG(0), sub), sub, ARG(2), (LPSTR)(uintptr_t)ARG(3), ARG(4),
                                      ARG(5), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(6),
                                      (PHKEY)(uintptr_t)ARG(7), (LPDWORD)(uintptr_t)ARG(8));
    g_esp += 4 + 9 * 4;
}

static void shim_RegQueryValueExA(void) {
    g_eax = (uint32_t)RegQueryValueExA((HKEY)(uintptr_t)ARG(0), (LPCSTR)(uintptr_t)ARG(1),
                                       (LPDWORD)(uintptr_t)ARG(2), (LPDWORD)(uintptr_t)ARG(3),
                                       (LPBYTE)(uintptr_t)ARG(4), (LPDWORD)(uintptr_t)ARG(5));
    if (getenv("SOTE_REG_LOG"))
        fprintf(stderr, "[reg] query %s -> %u%s%s\n", gstr(ARG(1)), g_eax, g_eax == 0 && ARG(4) ? " " : "",
                g_eax == 0 && ARG(4) && ARG(3) && MEM32(ARG(3)) == REG_SZ ? gstr(ARG(4)) : "");
    g_esp += 4 + 6 * 4;
}

/* ---- headless ------------------------------------------------------------
 * Nothing reaches a screen (REPO_RULES 13: over RDP a window lands on the
 * phone). The display side is display.c; here the game's windows are made
 * real but invisible: layered at alpha 0, click-through, never activated, off
 * the taskbar. Windows treats them as visible, so they paint and D3D presents
 * to them, and nothing appears. Fallout Tactics' host does the same. */
static HWND g_game_hwnd;
#define HL_EXSTYLE (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)

static void shim_hl_MessageBoxA(void) {
    uint32_t buttons = ARG(3) & MB_TYPEMASK;
    g_eax = (buttons == MB_YESNO || buttons == MB_YESNOCANCEL) ? IDNO : IDOK;
    fprintf(stderr, "[messagebox] -> %u: %s: %s\n", g_eax, gstr(ARG(2)), gstr(ARG(1)));
    g_esp += 4 + 4 * 4;
}

static void shim_hl_CreateWindowExA(void) {
    uint32_t style = ARG(3);
    HWND parent = (HWND)(uintptr_t)ARG(8);
    int top = !parent || !(style & WS_CHILD);
    HWND h = CreateWindowExA(ARG(0) | (top ? HL_EXSTYLE : 0), (LPCSTR)(uintptr_t)ARG(1),
                             (LPCSTR)(uintptr_t)ARG(2), style & ~WS_VISIBLE, (int)ARG(4), (int)ARG(5),
                             (int)ARG(6), (int)ARG(7), parent, (HMENU)(uintptr_t)ARG(9),
                             (HINSTANCE)(uintptr_t)ARG(10), (LPVOID)(uintptr_t)ARG(11));
    if (h && top) SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    if (h && (style & WS_VISIBLE)) ShowWindow(h, SW_SHOWNOACTIVATE);
    fprintf(stderr, "[headless] CreateWindowExA(\"%s\", %dx%d) -> invisible hwnd %p\n",
            gstr(ARG(2)), (int)ARG(6), (int)ARG(7), (void*)h);
    if (!g_game_hwnd && top) g_game_hwnd = h;
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

/* A top-level window keeps the invisible bits whatever style the game (or
 * the ddraw wrapper's subclass) sets. */
static void shim_hl_SetWindowLongA(void) {
    HWND h = (HWND)(uintptr_t)ARG(0);
    int i = (int)ARG(1);
    LONG v = (LONG)ARG(2);
    if (i == GWL_EXSTYLE && !(GetWindowLongA(h, GWL_STYLE) & WS_CHILD)) v |= HL_EXSTYLE;
    g_eax = (uint32_t)SetWindowLongA(h, i, v);
    g_esp += 4 + 3 * 4;
}

static void shim_hl_ShowWindow(void) {
    HWND h = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    g_esp += 4 + 2 * 4;
    g_eax = (uint32_t)ShowWindow(h, cmd == SW_HIDE ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (h == g_game_hwnd && cmd != SW_HIDE) {     /* the activation a fullscreen window gets */
        SendMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
        SendMessageA(h, WM_ACTIVATE, WA_ACTIVE, 0);
    }
}

static void shim_hl_window_cmd(void) {   /* SetForegroundWindow, BringWindowToTop, SetActiveWindow */
    g_eax = (uint32_t)(uintptr_t)g_game_hwnd;
    g_esp += 4 + 1 * 4;
}

static void shim_hl_focus_query(void) {  /* GetActiveWindow, GetForegroundWindow, GetFocus */
    g_eax = (uint32_t)(uintptr_t)g_game_hwnd;
    g_esp += 4;
}

/* Every window class the game registers gets this procedure in front of its
 * own. On a real screen it gives the F10 menu its keys. Headless, the game's
 * procedure sees every activation as "active": a hidden window is never the
 * foreground one, and a game that pauses on losing the focus would otherwise
 * freeze at a moment that depends on timing. */
#define MAX_CLASSES 8
static struct { ATOM atom; uint32_t proc; } g_classes[MAX_CLASSES];
static int g_nclasses;

static LRESULT CALLBACK hl_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ATOM atom = (ATOM)GetClassLongA(h, GCW_ATOM);
    uint32_t proc = 0;
    LRESULT r;
    for (int i = 0; i < g_nclasses; i++)
        if (g_classes[i].atom == atom) proc = g_classes[i].proc;
    if (!g_headless) {                       /* a real window: the F10 menu's keys first (menu.c) */
        if (menu_wndproc(h, m, w, l, &r)) return r;
    } else if (m == WM_ACTIVATEAPP || m == WM_NCACTIVATE) w = TRUE;
    else if (m == WM_ACTIVATE) w = MAKEWPARAM(WA_ACTIVE, HIWORD(w));
    else if (m == WM_KILLFOCUS) return 0;
    /* guest code: Windows' call faults on the non-executable page and
     * native32 runs the lifted body */
    return proc ? CallWindowProcA((WNDPROC)(uintptr_t)proc, h, m, w, l) : DefWindowProcA(h, m, w, l);
}

static void shim_hl_RegisterClassA(void) {
    WNDCLASSA c = *(const WNDCLASSA*)(uintptr_t)ARG(0);
    uint32_t guest = (uint32_t)(uintptr_t)c.lpfnWndProc;
    c.lpfnWndProc = hl_wndproc;
    ATOM a = RegisterClassA(&c);
    if (a && g_nclasses < MAX_CLASSES) g_classes[g_nclasses].atom = a, g_classes[g_nclasses++].proc = guest;
    g_eax = a;
    g_esp += 4 + 1 * 4;
}

/* --mute: this process's audio session at zero, held every second (the game
 * opens its devices later). For test runs on a machine someone is using. */
static DWORD WINAPI mute_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        waveOutSetVolume(NULL, 0);
        Sleep(1000);
    }
}

static native32_shim_t g_headless_shims[] = {
    { "MessageBoxA", shim_hl_MessageBoxA },
    { "CreateWindowExA", shim_hl_CreateWindowExA },
    { "SetWindowLongA", shim_hl_SetWindowLongA },
    { "ShowWindow", shim_hl_ShowWindow },
    { "SetForegroundWindow", shim_hl_window_cmd },
    { "BringWindowToTop", shim_hl_window_cmd },
    { "SetActiveWindow", shim_hl_window_cmd },
    { "GetActiveWindow", shim_hl_focus_query },
    { "GetForegroundWindow", shim_hl_focus_query },
    { "GetFocus", shim_hl_focus_query },
};

/* On a real screen every DirectInput device is non-exclusive. An exclusive
 * mouse hides the cursor from Windows and keeps its clicks, and the F10 menu
 * needs both; the game reads the same relative motion either way. */
#define DISCL_EXCLUSIVE_    0x1
#define DISCL_NONEXCLUSIVE_ 0x2
typedef HRESULT (WINAPI *dicoop_t)(void*, HWND, DWORD);
typedef HRESULT (WINAPI *dimake_t)(void*, const GUID*, void**, void*);
static dicoop_t g_real_dicoop;
static dimake_t g_real_dimake;

static void vt_patch(void** slot, void* fn, void** real) {
    DWORD old;
    if (*slot == fn || (*real && *real != *slot)) return;   /* one saved original: one vtable */
    *real = *slot;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, sizeof *slot, old, &old);
}

static HRESULT WINAPI hl_di_SetCooperativeLevel(void* dev, HWND h, DWORD flags) {
    DWORD asked = flags;
    if (!g_headless && (flags & DISCL_EXCLUSIVE_)) flags = (flags & ~DISCL_EXCLUSIVE_) | DISCL_NONEXCLUSIVE_;
    HRESULT hr = g_real_dicoop(dev, h, flags);
    fprintf(stderr, "[input] SetCooperativeLevel(0x%lX -> 0x%lX) -> 0x%08lX\n", asked, flags, hr);
    return hr;
}

static HRESULT WINAPI hl_di_CreateDevice(void* di, const GUID* g, void** out, void* outer) {
    HRESULT hr = g_real_dimake(di, g, out, outer);
    if (SUCCEEDED(hr) && *out) vt_patch(&(*(void***)*out)[13], (void*)hl_di_SetCooperativeLevel, (void**)&g_real_dicoop);
    return hr;
}

/* DirectInputCreateA checks that hinst is a module the loader knows; the
 * guest's (0x00400000, mapped by native32) is not, and the game then used the
 * NULL interface it got back. The host's module stands in for it. */
static void shim_DirectInputCreateA(void) {
    typedef HRESULT (WINAPI *dic_t)(HINSTANCE, DWORD, void**, void*);
    static dic_t real;
    void** out = (void**)(uintptr_t)ARG(2);
    if (!real) real = (dic_t)GetProcAddress(LoadLibraryA("dinput.dll"), "DirectInputCreateA");
    HRESULT hr = real ? real(GetModuleHandleA(NULL), ARG(1), out, (void*)(uintptr_t)ARG(3)) : E_FAIL;
    fprintf(stderr, "[input] DirectInputCreateA(version 0x%X) -> 0x%08lX\n", ARG(1), hr);
    if (SUCCEEDED(hr) && *out) vt_patch(&(*(void***)*out)[3], (void*)hl_di_CreateDevice, (void**)&g_real_dimake);
    g_eax = (uint32_t)hr;
    g_esp += 4 + 4 * 4;
}

static native32_shim_t g_shims[] = {
    { "DirectInputCreateA", shim_DirectInputCreateA },
    { "RegisterClassA", shim_hl_RegisterClassA },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "OutputDebugStringA", shim_OutputDebugStringA },
    { "DialogBoxParamA", shim_DialogBoxParamA },
    { "MessageBoxA", shim_MessageBoxA },
    { "RegOpenKeyExA", shim_RegOpenKeyExA },
    { "RegCreateKeyExA", shim_RegCreateKeyExA },
    { "RegQueryValueExA", shim_RegQueryValueExA },
};

/* ---- diagnostics --------------------------------------------------------- */

recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X  (called from 0x%08X)\n", va, g_cur_func);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. The report goes out through WriteFile from a
 * static buffer, not stdio: a fault while another thread holds the CRT's
 * stderr lock otherwise ends with no report at all (The Movies). */
static char g_crash_buf[4096];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (g_original) return EXCEPTION_CONTINUE_SEARCH;   /* its own SEH handles them; oracle.c logs */
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* SOTE_PROFILE=1: which lifted function is running, sampled every
 * millisecond, the most frequent printed when the watchdog ends the run
 * (Fallout Tactics' host has the same). */
#define PROF_SLOTS 65536
static struct { uint32_t va, n; } g_prof[PROF_SLOTS];
static DWORD WINAPI profiler(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    for (;;) {
        uint32_t va = g_cur_func;
        for (uint32_t h = (va * 2654435761u) >> 16, k = 0; va && k < PROF_SLOTS; k++, h = (h + 1) & (PROF_SLOTS - 1))
            if (g_prof[h].va == va || !g_prof[h].va) { g_prof[h].va = va; g_prof[h].n++; break; }
        Sleep(1);
    }
}
static int prof_cmp(const void* a, const void* b) {
    return (int)((const uint32_t*)b)[1] - (int)((const uint32_t*)a)[1];
}
static void profile_report(void) {
    static uint32_t top[PROF_SLOTS][2];
    int n = 0;
    uint64_t total = 0;
    for (int i = 0; i < PROF_SLOTS; i++)
        if (g_prof[i].va) top[n][0] = g_prof[i].va, top[n][1] = g_prof[i].n, total += g_prof[i].n, n++;
    if (!n) return;
    qsort(top, n, sizeof top[0], prof_cmp);
    for (int i = 0; i < n && i < 30; i++)
        fprintf(stderr, "[profile] sub_%08X %6u  %4.1f%%\n", top[i][0], top[i][1], 100.0 * top[i][1] / total);
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, display_frames());
    if (display_flat_percent() >= 0)
        fprintf(stderr, "[watchdog] 3D vertices on the middle row: %ld%%\n", display_flat_percent());
    native32_dump_icalls(8);
    profile_report();
    display_record_close();
    recomp_trace_flush();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* --lifted VA|LO-HI[,...] (with --original): these lifted entries run lifted
 * inside the original code, each with every function it calls. A range takes
 * every lifted entry in it, so halving ranges bisects a lift bug down to the
 * function that has it. */
static uint32_t g_lift[4096];
static int g_nlift;

static void lifted_arg(const char* s) {
    char buf[1024], *p;
    strncpy(buf, s, sizeof buf - 1), buf[sizeof buf - 1] = 0;
    for (p = strtok(buf, ","); p; p = strtok(NULL, ",")) {
        char* dash = strchr(p, '-');
        if (*p == '!') {               /* !VA: stays original (one that crosses into a shim's callbacks) */
            uint32_t va = strtoul(p + 1, NULL, 0);
            for (int k = 0; k < g_nlift; k++)
                if (g_lift[k] == va) g_lift[k--] = g_lift[--g_nlift];
            continue;
        }
        uint32_t lo = strtoul(p, NULL, 0), hi = dash ? strtoul(dash + 1, NULL, 0) : lo + 1;
        for (uint32_t k = 0; k < sote_func_start_count && g_nlift < 4096; k++)
            if (sote_func_starts[k] >= lo && sote_func_starts[k] < hi) g_lift[g_nlift++] = sote_func_starts[k];
    }
}

static uint32_t pe_field(const char* path, int which) {   /* 0: timestamp, 1: entry RVA */
    uint8_t head[4096] = { 0 };
    FILE* f = fopen(path, "rb");
    if (f) { fread(head, 1, sizeof head, f); fclose(f); }
    uint32_t nt = *(const uint32_t*)(head + 0x3C);
    if (nt > sizeof head - 0x40) return 0;
    return *(const uint32_t*)(head + nt + (which ? 0x28 : 8));
}

int main(int argc, char** argv) {
    const char* game = "game";
    char game_full[MAX_PATH], sdata[MAX_PATH];
    int run = 0;
    const char* rec = NULL;
    long rec_frames = 0;
    /* stderr unbuffered: the log is the evidence, and the game leaves through
     * ExitProcess, which flushes nothing with the C runtime linked in (/MT). */
    setvbuf(stderr, NULL, _IONBF, 0);
    /* Everything after -- is the game's own command line. */
    const char* guest_args = "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--")) continue;
        static char ga[1024];
        for (int k = i + 1; k < argc; k++) {
            strncat(ga, " ", sizeof ga - strlen(ga) - 1);
            strncat(ga, argv[k], sizeof ga - strlen(ga) - 1);
        }
        guest_args = ga;
        argc = i;
        break;
    }
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (!n) n = pad_arg(argc, argv, i);
        if (!n) n = menu_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = g_autoplay = 1;
        else if (!strcmp(argv[i], "--play")) g_autoplay = 1;
        else if (!strcmp(argv[i], "--original")) g_original = 1;
        else if (!strcmp(argv[i], "--lifted") && i + 1 < argc) lifted_arg(argv[++i]);
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) rec_frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--mute")) CloseHandle(CreateThread(NULL, 0, mute_thread, NULL, 0, NULL));
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: sote [--run] [--original] [--play] [--headless [--record out.mp4] [--frames N]] [--mute] [--game DIR]\n"
                   "            [--pad MASK@T[+D]] [--stick X,Y@T[+D]] [--pad-log] [--cmd NAME@T]\n"
                   "            [--watchdog S] [--native-trace] [--callbacks] [-- game args]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    if (rec && !g_headless) { fprintf(stderr, "--record needs --headless\n"); return 1; }
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    if (rec) {                         /* the run enters Sdata\ */
        static char rec_full[MAX_PATH];
        GetFullPathNameA(rec, MAX_PATH, rec_full, NULL);
        rec = rec_full;
    }
    _snprintf(sdata, sizeof sdata - 1, "%s\\Sdata", game_full);
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\Shadows.exe", sdata);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"%s", g_guest_exe, guest_args);
    char exe_full[MAX_PATH];
    _snprintf(exe_full, sizeof exe_full - 1, "%s\\" SOTE_EXE_NAME, sdata);

    /* Sdata\ holds Steam's DDRAW.dll (a Direct3D 9 wrapper) and WIN32.dll (its
     * winmm stand-in that plays the music from .ogg): the guest's imports of
     * both bind from there, ahead of System32. */
    SetDllDirectoryA(sdata);

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    printf("Shadows of the Empire recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    /* The lifted C is one build's: another exe crashes in game. */
    if (pe_field(exe_full, 0) != SOTE_EXE_STAMP ||
        SOTE_IMAGE_BASE + pe_field(exe_full, 1) != SOTE_EXE_ENTRY) {
        fprintf(stderr, "%s is not the unwrapped Steam Shadows.exe this host was lifted from "
                "(README, Getting Started: tools\\setup.ps1 makes it).\n", exe_full);
        return 1;
    }
    uint32_t span = native32_map(exe_full, SOTE_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, SOTE_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", exe_full, SOTE_IMAGE_BASE, SOTE_IMAGE_BASE + span);
    /* Headless shims first: the bind takes the first of a name. */
    static native32_shim_t shims[sizeof g_headless_shims / sizeof g_headless_shims[0] +
                                 sizeof g_shims / sizeof g_shims[0]];
    int nshims = 0;
    menu_init(g_headless);              /* sote.ini: what display_init's devices follow */
    if (!display_init(sdata, g_headless, rec, rec_frames) && g_headless) return 1;
    if (g_headless) {
        for (size_t k = 0; k < sizeof g_headless_shims / sizeof g_headless_shims[0]; k++)
            shims[nshims++] = g_headless_shims[k];
    }
    for (size_t k = 0; k < sizeof g_shims / sizeof g_shims[0]; k++) shims[nshims++] = g_shims[k];
    if (g_original) {
        /* The shipping machine code under the same host and shims
         * (oracle.c): the reference a lifted run is compared against. The
         * controls layer is a patch in the lift, so it is not in this run. */
        if (!SetCurrentDirectoryA(sdata)) { fprintf(stderr, "cannot enter %s\n", sdata); return 1; }
        if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        oracle_lift(g_lift, g_nlift);
        return oracle_run(exe_full, SOTE_IMAGE_BASE, shims, nshims, NULL, 0);
    }
    if (native32_bind(SOTE_IMAGE_BASE, shims, nshims)) return 1;

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", sote_entry_va);
        return 0;
    }
    /* The game opens its data relative to Sdata\. */
    if (!SetCurrentDirectoryA(sdata)) { fprintf(stderr, "cannot enter %s\n", sdata); return 1; }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    pad_init();
    if (getenv("SOTE_PROFILE")) CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", sote_entry_va);
    fflush(stdout);
    native32_call_guest(sote_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
