/*
 * The controls layer: modern input on top of the game's own.
 *
 * The PC port keeps the N64 game's controller. Each frame sub_0041C900 reads
 * DirectInput into a flat table of input channels, maps the user's bindings
 * onto 21 emulated buttons, and builds one stick from the joystick axes, the
 * mouse and the keys; sub_00491D70 then packs that into the struct the game
 * reads (docs/controls.md):
 *
 *   0x008A5C60  float  stick x   (-0.87 .. 0.87, right +)
 *   0x008A5C64  float  stick y   (-0.87 .. 0.87, up +)
 *   0x008A5C68  u32    buttons   (PAD_* below)
 *
 * run_lift.py's PATCHES calls sote_pad_frame() right after that, so this file
 * only ever adjusts three values the game was going to read anyway; every
 * rule about what a button does stays the game's.
 *
 * What it adds:
 *   - XInput gamepads (any modern controller): left stick with a radial dead
 *     zone and a linear response instead of the N64 curve, which damps the
 *     first 40% of the stick to 40% speed; right stick as the C buttons; the
 *     triggers, bumpers and face buttons as the N64's.
 *   - Scripted input for headless runs (--pad, --stick).
 * Settings: controls.ini beside sote.exe, optional (docs/controls.md).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "pad.h"

#define PAD_STICK_X 0x008A5C60u
#define PAD_STICK_Y 0x008A5C64u
#define PAD_BUTTONS 0x008A5C68u
#define STICK_MAX   0.87f            /* the game's own clamp (0x004BCBEC) */

/* ---- XInput, loaded at run time (xinput1_4 on Windows 8+, 9_1_0 on 7) ---- */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xpad_t;
typedef struct { DWORD packet; xpad_t pad; } xstate_t;
typedef DWORD (WINAPI *xget_t)(DWORD, xstate_t*);
static xget_t g_xget;
enum { XB_UP = 0x1, XB_DOWN = 0x2, XB_LEFT = 0x4, XB_RIGHT = 0x8, XB_START = 0x10, XB_BACK = 0x20,
       XB_LTHUMB = 0x40, XB_RTHUMB = 0x80, XB_LB = 0x100, XB_RB = 0x200, XB_A = 0x1000, XB_B = 0x2000,
       XB_X = 0x4000, XB_Y = 0x8000 };

/* ---- settings ------------------------------------------------------------ */
static struct {
    float deadzone, curve, rs_threshold, trigger_threshold;
    uint32_t map[16];                  /* XInput button bit index -> PAD_* mask */
    uint32_t lt, rt, rs_up, rs_down, rs_left, rs_right;
} g_cfg;

/* Defaults: the N64 layout on an Xbox pad, as the game's own key defaults
 * group them (docs/controls.md has the table and how it was checked). */
static void defaults(void) {
    g_cfg.deadzone = 0.15f;
    g_cfg.curve = 1.0f;
    g_cfg.rs_threshold = 0.5f;
    g_cfg.trigger_threshold = 0.25f;
    g_cfg.map[0] = PAD_DUP;  g_cfg.map[1] = PAD_DDOWN; g_cfg.map[2] = PAD_DLEFT; g_cfg.map[3] = PAD_DRIGHT;
    g_cfg.map[4] = PAD_START;                         /* Start */
    g_cfg.map[5] = PAD_START;                         /* Back */
    g_cfg.map[8] = PAD_L;                             /* LB */
    g_cfg.map[9] = PAD_R;                             /* RB */
    g_cfg.map[12] = PAD_A;                            /* A */
    g_cfg.map[13] = PAD_B;                            /* B */
    g_cfg.map[14] = PAD_B;                            /* X */
    g_cfg.map[15] = PAD_CUP;                          /* Y */
    g_cfg.lt = PAD_R;
    g_cfg.rt = PAD_Z;
    g_cfg.rs_up = PAD_CUP, g_cfg.rs_down = PAD_CDOWN, g_cfg.rs_left = PAD_CLEFT, g_cfg.rs_right = PAD_CRIGHT;
}

static float ini_float(const char* path, const char* key, float def) {
    char buf[32], d[32];
    _snprintf(d, sizeof d - 1, "%g", def), d[sizeof d - 1] = 0;
    GetPrivateProfileStringA("pad", key, d, buf, sizeof buf, path);
    return (float)atof(buf);
}

static uint32_t ini_mask(const char* path, const char* key, uint32_t def) {
    char buf[32];
    GetPrivateProfileStringA("pad", key, "", buf, sizeof buf, path);
    return buf[0] ? strtoul(buf, NULL, 0) : def;
}

static void load_settings(void) {
    static const char* names[16] = { "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back",
                                     "lthumb", "rthumb", "lb", "rb", 0, 0, "a", "b", "x", "y" };
    char path[MAX_PATH], *slash;
    defaults();
    GetModuleFileNameA(NULL, path, sizeof path);
    slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy(slash + 1, "controls.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    g_cfg.deadzone = ini_float(path, "deadzone", g_cfg.deadzone);
    g_cfg.curve = ini_float(path, "curve", g_cfg.curve);
    g_cfg.rs_threshold = ini_float(path, "right_stick_threshold", g_cfg.rs_threshold);
    g_cfg.trigger_threshold = ini_float(path, "trigger_threshold", g_cfg.trigger_threshold);
    for (int i = 0; i < 16; i++)
        if (names[i]) g_cfg.map[i] = ini_mask(path, names[i], g_cfg.map[i]);
    g_cfg.lt = ini_mask(path, "lt", g_cfg.lt);
    g_cfg.rt = ini_mask(path, "rt", g_cfg.rt);
    g_cfg.rs_up = ini_mask(path, "rs_up", g_cfg.rs_up);
    g_cfg.rs_down = ini_mask(path, "rs_down", g_cfg.rs_down);
    g_cfg.rs_left = ini_mask(path, "rs_left", g_cfg.rs_left);
    g_cfg.rs_right = ini_mask(path, "rs_right", g_cfg.rs_right);
    fprintf(stderr, "[pad] settings from %s\n", path);
}

/* A stick, as the game wants it: zero inside the dead zone, then rising from
 * zero (no step at its edge) to STICK_MAX at full deflection, shaped by
 * `curve` (1 = linear). Radial, so a diagonal is not cut short. */
static void shape(float x, float y, float* ox, float* oy) {
    float m = sqrtf(x * x + y * y), t;
    if (m <= g_cfg.deadzone) { *ox = *oy = 0; return; }
    t = (fminf(m, 1.0f) - g_cfg.deadzone) / (1.0f - g_cfg.deadzone);
    t = powf(t, g_cfg.curve) * STICK_MAX;
    *ox = x / m * t, *oy = y / m * t;
}

static float axis(SHORT v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

/* ---- scripted input (headless runs) --------------------------------------
 * --pad MASK@T[+D]: hold PAD_* bits from T seconds for D (default 0.2).
 * --stick X,Y@T[+D]: hold the stick there. T counts from the first frame. */
#define MAX_EVENTS 128
static struct { uint32_t mask; float x, y; int stick; double t, d; } g_ev[MAX_EVENTS];
static int g_nev;
static double g_t0, g_freq;

static double now(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / g_freq;
}

static int parse_event(const char* s, int stick) {
    const char* at = strchr(s, '@');
    if (!at || g_nev >= MAX_EVENTS) return 0;
    if (stick) {
        if (sscanf(s, "%f,%f", &g_ev[g_nev].x, &g_ev[g_nev].y) != 2) return 0;
    } else {
        g_ev[g_nev].mask = strtoul(s, NULL, 0);
    }
    g_ev[g_nev].stick = stick;
    g_ev[g_nev].t = atof(at + 1);
    g_ev[g_nev].d = strchr(at, '+') ? atof(strchr(at, '+') + 1) : 0.2;
    g_nev++;
    return 1;
}

int pad_log;

int pad_arg(int argc, char** argv, int i) {
    if (i + 1 < argc && !strcmp(argv[i], "--pad") && parse_event(argv[i + 1], 0)) return 2;
    if (i + 1 < argc && !strcmp(argv[i], "--stick") && parse_event(argv[i + 1], 1)) return 2;
    if (!strcmp(argv[i], "--pad-log")) return pad_log = 1;
    return 0;
}

void pad_init(void) {
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    LARGE_INTEGER f;
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    g_xget = x ? (xget_t)GetProcAddress(x, "XInputGetState") : NULL;
    QueryPerformanceFrequency(&f);
    g_freq = (double)f.QuadPart;
    load_settings();
    fprintf(stderr, "[pad] XInput %s, %d scripted events\n", g_xget ? "ready" : "not found", g_nev);
}

/* Called by the lifted game once a frame, after it has built its pad. */
void sote_pad_frame(void) {
    float sx = MEMF(PAD_STICK_X), sy = MEMF(PAD_STICK_Y);
    uint32_t b = MEM32(PAD_BUTTONS), game_b = b;
    double t;
    if (!g_t0) g_t0 = now();
    t = now() - g_t0;

    if (g_xget) {
        xstate_t st;
        for (DWORD i = 0; i < 4; i++) {
            if (g_xget(i, &st)) continue;
            const xpad_t* p = &st.pad;
            float lx, ly, rx = axis(p->rx), ry = axis(p->ry);
            shape(axis(p->lx), axis(p->ly), &lx, &ly);
            if (lx || ly) sx = lx, sy = ly;      /* else the keys and mouse keep the stick */
            for (int k = 0; k < 16; k++)
                if (p->buttons & (1u << k)) b |= g_cfg.map[k];
            if (p->lt > g_cfg.trigger_threshold * 255) b |= g_cfg.lt;
            if (p->rt > g_cfg.trigger_threshold * 255) b |= g_cfg.rt;
            if (ry > g_cfg.rs_threshold) b |= g_cfg.rs_up;
            if (ry < -g_cfg.rs_threshold) b |= g_cfg.rs_down;
            if (rx < -g_cfg.rs_threshold) b |= g_cfg.rs_left;
            if (rx > g_cfg.rs_threshold) b |= g_cfg.rs_right;
            break;                           /* the first connected pad is player one */
        }
    }
    for (int i = 0; i < g_nev; i++) {
        if (t < g_ev[i].t || t >= g_ev[i].t + g_ev[i].d) continue;
        if (g_ev[i].stick) sx = g_ev[i].x, sy = g_ev[i].y;
        else b |= g_ev[i].mask;
    }
    if (pad_log && b != game_b) fprintf(stderr, "[pad] %.2fs buttons %06X -> %06X\n", t, game_b, b);
    MEMF(PAD_STICK_X) = sx;
    MEMF(PAD_STICK_Y) = sy;
    MEM32(PAD_BUTTONS) = b;
}
