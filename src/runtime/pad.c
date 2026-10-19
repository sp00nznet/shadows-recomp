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
 *     first 40% of the stick to 40% speed; right stick as the camera (the C
 *     buttons); every action on a sensible button, the same in every vehicle.
 *   - A mouse that steers by how fast it moves, instead of the game's own,
 *     which nudges a virtual stick that stays where it was pushed.
 *   - Menu keys from the pad (sote_keys_frame), and scripted input for
 *     headless runs (--pad, --stick, --key).
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
#include "menu.h"

#define PAD_STICK_X 0x008A5C60u
#define PAD_STICK_Y 0x008A5C64u
#define PAD_BUTTONS 0x008A5C68u
#define STICK_MAX   0.87f            /* the game's own clamp (0x004BCBEC) */
#define AIM_X       0x004CBC4Cu      /* the crosshair while Aim/Look is held (sub_0041C900) */
#define AIM_Y       0x004CBC50u

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
    int mouse;                         /* 1: mouse steers by speed; 0: the game's own */
    float mouse_speed;                 /* mouse counts a second that give full deflection */
    float mouse_aim;                   /* mouse counts from the centre to the edge, aiming */
    int mouse_invert;
} g_cfg;

/* Defaults: one action per button, the same in every vehicle, because the
 * game gives each button one meaning everywhere (Fire is 0x1000 on foot and
 * in every craft). docs/controls.md has the table this follows. */
static void defaults(void) {
    g_cfg.deadzone = 0.15f;
    g_cfg.curve = 1.0f;
    g_cfg.rs_threshold = 0.5f;
    g_cfg.trigger_threshold = 0.25f;
    g_cfg.map[0] = PAD_DUP;  g_cfg.map[1] = PAD_DDOWN; g_cfg.map[2] = PAD_DLEFT; g_cfg.map[3] = PAD_DRIGHT;
    g_cfg.map[4] = PAD_START;                         /* Start: pause */
    g_cfg.map[5] = PAD_CAMPOS;                        /* Back: camera position */
    g_cfg.map[8] = PAD_JETPACK | PAD_LKICK;           /* LB: jetpack on/off; swoop: left kick */
    g_cfg.map[9] = PAD_STRAFE | PAD_RKICK;            /* RB: strafe (hold); swoop: right kick */
    g_cfg.map[12] = PAD_JUMP;                         /* A: jump; thrust, accelerate */
    g_cfg.map[13] = PAD_DUCK;                         /* B: duck; harpoon, missile */
    g_cfg.map[14] = PAD_STRAFE;                       /* X: activate; brakes, decelerate */
    g_cfg.map[15] = PAD_WEAPON;                       /* Y: weapon */
    g_cfg.lt = PAD_AIM;                               /* LT: aim/look (hold) */
    g_cfg.rt = PAD_FIRE;                              /* RT: fire */
    g_cfg.rs_up = PAD_CUP, g_cfg.rs_down = PAD_CDOWN, g_cfg.rs_left = PAD_CLEFT, g_cfg.rs_right = PAD_CRIGHT;
    g_cfg.mouse = 1;
    g_cfg.mouse_speed = 1500.0f;
    g_cfg.mouse_aim = 300.0f;
    g_cfg.mouse_invert = 0;
}

static float ini_float_in(const char* sec, const char* path, const char* key, float def) {
    char buf[32], d[32];
    _snprintf(d, sizeof d - 1, "%g", def), d[sizeof d - 1] = 0;
    GetPrivateProfileStringA(sec, key, d, buf, sizeof buf, path);
    return (float)atof(buf);
}

static float ini_float(const char* path, const char* key, float def) { return ini_float_in("pad", path, key, def); }

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
    g_cfg.mouse = (int)ini_float_in("mouse", path, "steer_by_speed", (float)g_cfg.mouse);
    g_cfg.mouse_speed = ini_float_in("mouse", path, "full_speed", g_cfg.mouse_speed);
    g_cfg.mouse_aim = ini_float_in("mouse", path, "aim_counts", g_cfg.mouse_aim);
    g_cfg.mouse_invert = (int)ini_float_in("mouse", path, "invert_y", (float)g_cfg.mouse_invert);
    if (g_cfg.mouse_speed < 1) g_cfg.mouse_speed = 1;
    if (g_cfg.mouse_aim < 1) g_cfg.mouse_aim = 1;
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

/* --pad-selftest: the stick shaping, checked. */
static int selftest(void) {
    float x, y;
    defaults();
    shape(0.1f, 0.0f, &x, &y);   if (x != 0 || y != 0) return 1;           /* inside the dead zone */
    shape(1.0f, 0.0f, &x, &y);   if (fabsf(x - STICK_MAX) > 1e-5f) return 2;    /* full is the game's max */
    shape(0.16f, 0.0f, &x, &y);  if (x <= 0 || x > 0.02f) return 3;      /* rises from 0 at the edge */
    shape(0.7071f, 0.7071f, &x, &y);                                     /* a diagonal stays radial */
    if (fabsf(x - y) > 1e-5f || fabsf(sqrtf(x * x + y * y) - STICK_MAX) > 1e-3f) return 4;
    shape(-1.0f, 0.0f, &x, &y);  if (fabsf(x + STICK_MAX) > 1e-5f) return 5;
    if (axis(-32768) != -1.0f || axis(32767) != 1.0f) return 6;
    return 0;
}

/* ---- scripted input (headless runs) --------------------------------------
 * --pad MASK@T[+D]: hold PAD_* bits from T seconds for D (default 0.2).
 * --stick X,Y@T[+D]: hold the stick there. T counts from the first frame. */
#define MAX_EVENTS 128
static struct { uint32_t mask; float x, y; int stick; double t, d; } g_ev[MAX_EVENTS];
static int g_nev;
static struct { int ch; double t, d; } g_kev[MAX_EVENTS];
static int g_nkev;
static double g_t0, g_freq;

static double now_s(void) {
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
    if (i + 1 < argc && !strcmp(argv[i], "--key") && strchr(argv[i + 1], '@') && g_nkev < MAX_EVENTS) {
        const char* at = strchr(argv[i + 1], '@');
        g_kev[g_nkev].ch = (int)strtoul(argv[i + 1], NULL, 0);
        g_kev[g_nkev].t = atof(at + 1);
        g_kev[g_nkev].d = strchr(at, '+') ? atof(strchr(at, '+') + 1) : 0.2;
        g_nkev++;
        return 2;
    }
    if (!strcmp(argv[i], "--pad-log")) return pad_log = 1;
    if (!strcmp(argv[i], "--pad-selftest")) {
        int r = selftest();
        printf("pad selftest: %s (%d)\n", r ? "FAIL" : "ok", r);
        exit(r);
    }
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

/* ---- keys: the channel tables ---------------------------------------------
 * Menus read keys straight from the game's channel tables, not from the pad,
 * so a gamepad reaches them as keys:
 *   Start -> Enter (select in a menu; pause in play, as the N64's Start)
 *   D-pad and left stick -> the arrows (menu moves; in play they are bound to
 *                                        the stick already)
 * and in the front end only (0x008A5C28 == 2: title, level select), where no
 * key means anything else:
 *   A -> Enter, B -> Esc
 * In a level they would not be safe: every select key is bound to something
 * in play (Enter pauses; mouse button 1 and joystick button 1 fire), so there
 * the pause menu takes the D-pad and Start.
 * --key CH@T[+D] presses channel CH (a DIK_ code, 0x100+ mouse, 0x160+
 * joystick buttons) the same way, for scripts. */
#define GAME_STATE 0x008A5C28u          /* 2 front end, 3 in a level, 0 a cinematic */
#define CH_HELD    0x00810378u
#define CH_PRESSES 0x0080EE88u
#define CH_ENTER 0x1C
#define CH_ESC   0x01
#define CH_UP    0xC8
#define CH_DOWN  0xD0
#define CH_LEFT  0xCB
#define CH_RIGHT 0xCD

static uint8_t g_was[0x200];            /* last frame's injected state, for press edges */

static int pad_state(xpad_t* out) {
    xstate_t st;
    for (DWORD i = 0; g_xget && i < 4; i++)
        if (!g_xget(i, &st)) { *out = st.pad; return 1; }
    return 0;
}

static void key(uint8_t* now, int ch) { if (ch >= 0 && ch < 0x200) now[ch] = 1; }

static long g_npad, g_nkeys;            /* calls, for --pad-log's heartbeat */

static void heartbeat(double t) {
    static double next = 10;
    if (pad_log && t >= next) {
        fprintf(stderr, "[pad] %.0fs heartbeat: pad %ld, keys %ld calls\n", t, g_npad, g_nkeys);
        next = t + 10;
    }
}

void sote_keys_frame(void) {
    uint8_t now[0x200] = { 0 };
    xpad_t p;
    double t;
    if (!g_t0) g_t0 = now_s();
    t = now_s() - g_t0;
    g_nkeys++;
    heartbeat(t);
    if (pad_state(&p)) {
        float lx = axis(p.lx), ly = axis(p.ly);
        int front_end = MEM32(GAME_STATE) == 2;
        if (p.buttons & XB_START) key(now, CH_ENTER);
        if (front_end && (p.buttons & XB_A)) key(now, CH_ENTER);
        if (front_end && (p.buttons & XB_B)) key(now, CH_ESC);
        if ((p.buttons & XB_UP) || ly > 0.5f) key(now, CH_UP);
        if ((p.buttons & XB_DOWN) || ly < -0.5f) key(now, CH_DOWN);
        if ((p.buttons & XB_LEFT) || lx < -0.5f) key(now, CH_LEFT);
        if ((p.buttons & XB_RIGHT) || lx > 0.5f) key(now, CH_RIGHT);
    }
    for (int i = 0; i < g_nkev; i++)
        if (t >= g_kev[i].t && t < g_kev[i].t + g_kev[i].d) key(now, g_kev[i].ch);
    /* The poll clears the press counts each frame but not the held table:
     * DirectInput's key-up events do that, and an injected key has none. So
     * a key let go here is cleared here, or it stays down for good (it did:
     * Enter, held from the level select on, kept Start pressed in play). */
    for (int ch = 0; ch < 0x200; ch++) {
        if (now[ch]) {
            MEM32(CH_HELD + 4 * ch) = 1;
            if (!g_was[ch]) MEM32(CH_PRESSES + 4 * ch) += 1;
            if (pad_log && !g_was[ch]) fprintf(stderr, "[pad] %.2fs key 0x%X down\n", t, ch);
        } else if (g_was[ch]) {
            MEM32(CH_HELD + 4 * ch) = 0;
        }
        g_was[ch] = now[ch];
    }
}

/* Called by the lifted game once a frame, after it has built its pad. */
void sote_pad_frame(void) {
    float sx = MEMF(PAD_STICK_X), sy = MEMF(PAD_STICK_Y);
    uint32_t b = MEM32(PAD_BUTTONS), game_b = b;
    int thumbs = 0;
    double t;
    if (!g_t0) g_t0 = now_s();
    t = now_s() - g_t0;
    g_npad++;
    heartbeat(t);

    if (g_xget) {
        xstate_t st;
        for (DWORD i = 0; i < 4; i++) {
            if (g_xget(i, &st)) continue;
            const xpad_t* p = &st.pad;
            float lx, ly, rx = axis(p->rx), ry = axis(p->ry);
            thumbs = (p->buttons & (XB_LTHUMB | XB_RTHUMB)) == (XB_LTHUMB | XB_RTHUMB);
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
    /* The mouse, by speed. The game's own mouse moves a virtual stick that
     * stays where it was pushed while Aim/Look is held, and adds the raw count
     * of one frame (x20, so it depends on the frame rate) otherwise. Here the
     * stick is how fast the mouse moves, in counts a second: still is centred,
     * a flick turns hard, the same at any frame rate. DirectInput's relative
     * counts for this frame are at channels 0x30 and 0x31 (sub_004AA0F0). */
    {
        static double last;
        double dt = t - last;
        int32_t dx = (int32_t)MEM32(0x0080F958u), dy = (int32_t)MEM32(0x0080F95Cu);
        last = t;
        if (g_cfg.mouse && (dx || dy) && (b & PAD_AIM)) {
            /* Aiming: the stick is the crosshair's place, kept by the game in
             * 0x004CBC4C/50. It adds the mouse times the frame time, so at 300
             * frames a second the crosshair hardly moved; here it moves by
             * the counts themselves, aim_counts from the centre to the edge. */
            float k = STICK_MAX / g_cfg.mouse_aim, ay = (float)(g_cfg.mouse_invert ? dy : -dy);
            sx = fmaxf(-STICK_MAX, fminf(STICK_MAX, MEMF(AIM_X) + dx * k));
            sy = fmaxf(-STICK_MAX, fminf(STICK_MAX, MEMF(AIM_Y) + ay * k));
            MEMF(AIM_X) = sx, MEMF(AIM_Y) = sy;
        } else if (g_cfg.mouse && (dx || dy) && dt > 0 && dt < 0.1) {
            float mx = (float)(dx / dt / g_cfg.mouse_speed), my = (float)(-dy / dt / g_cfg.mouse_speed);
            if (g_cfg.mouse_invert) my = -my;
            sx = fmaxf(-STICK_MAX, fminf(STICK_MAX, mx * STICK_MAX));
            sy = fmaxf(-STICK_MAX, fminf(STICK_MAX, my * STICK_MAX));
        }
    }
    for (int i = 0; i < g_nev; i++) {
        if (t < g_ev[i].t || t >= g_ev[i].t + g_ev[i].d) continue;
        if (g_ev[i].stick) sx = g_ev[i].x, sy = g_ev[i].y;
        else b |= g_ev[i].mask;
    }
    if (pad_log && b != game_b) fprintf(stderr, "[pad] %.2fs buttons %06X -> %06X\n", t, game_b, b);
    if (pad_log) {   /* 0x006F0CE0 = 1 while a demo plays the pad back (sub_00491F10) */
        static uint32_t demo = 0xFFFFFFFFu, state = 0xFFFFFFFFu;
        if (MEM32(0x006F0CE0) != demo) fprintf(stderr, "[pad] %.2fs demo playback %u\n", t, demo = MEM32(0x006F0CE0));
        if (MEM32(0x008A5C28) != state) fprintf(stderr, "[pad] %.2fs state %u\n", t, state = MEM32(0x008A5C28));
    }
    {   /* both stick clicks: the game's own debug menu, which the pad can work */
        static int was;
        if (thumbs && !was) menu_game_debug_menu();
        was = thumbs;
    }
    menu_pad_frame(&b);                  /* the F10 menu's command, if one is waiting (menu.c) */
    MEMF(PAD_STICK_X) = sx;
    MEMF(PAD_STICK_Y) = sy;
    MEM32(PAD_BUTTONS) = b;
}
