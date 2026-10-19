/*
 * The F10 menu: levels, cheats, graphics and display, around the game.
 *
 * Shadows of the Empire ships a debug menu of its own (0x0073E678: Next
 * Level, Invincibility, 50 Lives, Get all stuff...), opened from the pause
 * menu only when the player is named "R Testers ROCK" (sub_0040D0C0). Its
 * actions live in sub_00492770, which runs the selected item when Jump/Fire
 * is pressed with the menu open. Everything here that changes the game goes
 * through that code: the host opens the menu, selects the item and presses
 * the button, for one frame (menu_pad_frame), so pausing, sounds and messages
 * are the game's own. Two small hooks in the lift (run_lift.py PATCHES) add
 * what the debug menu lacks: jumping to any level (sote_scene_request) and the
 * field of view (sote_camera_frame).
 *
 * The menu is a Win32 popup on the game's window, as in the other pcrecomp
 * ports. --cmd runs the same commands on a timeline, for headless checks.
 * docs/menu.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "display.h"
#include "menu.h"

#define DBG_OPEN      0x0073E668u   /* u32: the debug menu is up */
#define DBG_ITEM      0x0073E670u   /* u16: its selected item */
#define DBG_DEBOUNCE  0x0073E6D0u   /* u8: frames before it takes another press */
#define INVINCIBLE    0x004CB600u   /* u32 */
#define DEBUG_DISPLAY 0x006EEB00u   /* u16: 2 on */
#define LIVES         0x006F03F4u   /* u32 */
#define SCENE         0x008A5C28u   /* u32: the scene running (DATAnn.DLL) */
#define SCENE_STATE   0x006F0CE8u   /* u32: 2 = a scene change is asked for (sub_0044F570) */
#define SCENE_NEXT    0x006F0CECu   /* u32: the scene asked for */
#define CAM_FOV       0x008D6304u   /* float: vertical field of view, degrees (sub_00414080) */
#define CAM_ASPECT    0x008D6308u   /* float: its aspect, 4/3 */
#define PAD_JUMP_BIT  0x00000800u   /* selects in the debug menu */

/* the debug menu's items (0x0073E678) */
enum { ITEM_ALL = 0, ITEM_INVINCIBLE = 1, ITEM_DEBUG_DISPLAY = 2, ITEM_LIVES = 3, ITEM_KILL = 5,
       ITEM_TELEPORT = 6, ITEM_THIS_LEVEL = 8 };

/* The playable scenes, from the game's own debug names (0x0073EBB0); the
 * rest are cutscenes. Names as the game's front end gives them. */
static const struct { int scene; const char* name; } k_levels[] = {
    { 3, "Battle of Hoth" },
    { 4, "Escape from Echo Base" },
    { 5, "Escape from Echo Base, part 2" },
    { 6, "The Asteroid Field" },
    { 8, "Ord Mantell Junkyard: the train" },
    { 10, "Ord Mantell Junkyard" },
    { 14, "Gall Spaceport" },
    { 15, "Gall Spaceport, part 2" },
    { 17, "Mos Eisley and Beggar's Canyon" },
    { 20, "Imperial Freighter Suprosa" },
    { 21, "Imperial Freighter Suprosa, part 2" },
    { 22, "Imperial Freighter Suprosa, part 3" },
    { 25, "Sewers of Imperial City" },
    { 26, "Xizor's Palace" },
    { 27, "Xizor's Palace, part 2" },
    { 29, "Skyhook Battle: the chase" },
    { 30, "Skyhook Battle" },
    { 31, "Credits" },
};
#define NLEVELS (int)(sizeof k_levels / sizeof k_levels[0])

static const float k_fov[] = { 1.0f, 1.15f, 1.3f, 1.45f };
static const char* const k_fov_name[] = { "Normal (the game's)", "Wide", "Wider", "Widest" };
#define NFOV 4
static const int k_res[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 960 }, { 1440, 1080 },
                                { 1600, 1200 }, { 1920, 1440 },
                                { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
#define FIRST_WIDE 7
#define NRES (int)(sizeof k_res / sizeof k_res[0])

sote_settings_t g_set = { 0, 1, 16, 1.0f, 0 };
static int g_headless;
static char g_ini[MAX_PATH];

/* ---- commands ------------------------------------------------------------ */
enum { CMD_NONE, CMD_ITEM, CMD_OPEN_DEBUG, CMD_JUMP };
static volatile LONG g_cmd = CMD_NONE, g_cmd_arg;
static volatile LONG g_jump = -1;          /* the scene sote_scene_request puts in */

static int level_index(int scene) {
    for (int i = 0; i < NLEVELS; i++)
        if (k_levels[i].scene == scene) return i;
    return -1;
}

static int in_level(void) { return level_index((int)MEM32(SCENE)) >= 0; }

static void queue(int cmd, int arg) {
    InterlockedExchange(&g_cmd_arg, arg);
    InterlockedExchange(&g_cmd, cmd);
}

static void jump_to(int scene) {
    fprintf(stderr, "[menu] level: scene %d -> %d (%s)\n", (int)MEM32(SCENE), scene,
            level_index(scene) >= 0 ? k_levels[level_index(scene)].name : "?");
    queue(CMD_JUMP, scene);
}

/* Each frame, right after the pad is packed (pad.c), before the game reads
 * it: a queued command becomes the debug menu, opened on its item with Jump
 * pressed. */
void menu_pad_frame(uint32_t* buttons) {
    LONG cmd = InterlockedExchange(&g_cmd, CMD_NONE), arg = g_cmd_arg;
    if (g_set.inf_lives && in_level() && (int32_t)MEM32(LIVES) < 5) MEM32(LIVES) = 5;
    switch (cmd) {
    case CMD_JUMP:
        if (MEM32(SCENE) == 2 || !in_level()) {   /* front end: no debug menu, ask directly */
            MEM32(SCENE_NEXT) = (uint32_t)arg;
            MEM32(SCENE_STATE) = 2;
            return;
        }
        InterlockedExchange(&g_jump, arg);
        arg = ITEM_THIS_LEVEL;
        /* fall through: "This Level", retargeted in sote_scene_request */
    case CMD_ITEM:
        if (!in_level()) { fprintf(stderr, "[menu] not in a level: item %ld skipped\n", arg); return; }
        MEM32(DBG_OPEN) = 1;
        MEM16(DBG_ITEM) = (uint16_t)arg;
        MEM8(DBG_DEBOUNCE) = 0;
        *buttons |= PAD_JUMP_BIT;
        fprintf(stderr, "[menu] the game's debug menu: item %ld\n", arg);
        break;
    case CMD_OPEN_DEBUG:
        if (in_level()) MEM32(DBG_OPEN) = 1;
        break;
    }
}

void menu_game_debug_menu(void) { queue(CMD_OPEN_DEBUG, 0); }

/* run_lift.py: sub_0044F570 (a scene change is asked for) has stored the
 * scene; a level jump puts its own in. */
void sote_scene_request(void) {
    LONG s = InterlockedExchange(&g_jump, -1);
    if (s >= 0) MEM32(SCENE_NEXT) = (uint32_t)s;
}

/* run_lift.py: sub_00414080 (each camera set-up) has taken the field of view
 * its caller asked for; the menu's is that, scaled. The game's value is
 * remembered apart from ours, so it is never scaled twice. */
void sote_camera_frame(void) {
    static float game_fov, fov, game_aspect, aspect;
    float f = MEMF(CAM_FOV), a = MEMF(CAM_ASPECT), screen = display_aspect();
    if (f != fov) game_fov = f;
    if (a != aspect) game_aspect = a;
    fov = game_fov * g_set.fov;
    /* The game's aspect is for a 4:3 screen; on a wider one the view widens
     * with it (Hor+), and so does the culling, which uses fov x aspect. */
    aspect = screen > 0 ? game_aspect * screen / (4.0f / 3.0f) : game_aspect;
    MEMF(CAM_FOV) = fov;
    MEMF(CAM_ASPECT) = aspect;
}

/* run_lift.py: sub_004706A0 blits a cutscene frame into the locked surface
 * (16-bit, pitch in pixels at 0x008D6FBC), scaled by screen/640 and
 * screen/480 but never past 2x (its stepping adds at most one pixel per
 * pixel). Above 1280x960 the 2x picture is centred, by moving the surface
 * pointer it starts from. The screen size is what sub_0046FFD0 left:
 * 0x007BBEF0 = 640 - width, 0x007BBCEC = 480 - height. */
uint32_t sote_movie_offset(void) {
    int w = 640 - (int32_t)MEM32(0x007BBEF0u), h = 480 - (int32_t)MEM32(0x007BBCECu);
    int x = w > 1280 ? (w - 1280) / 2 : 0, y = h > 960 ? (h - 960) / 2 : 0;
    return (uint32_t)(y * (int32_t)MEM32(0x008D6FBCu) + x) * 2;
}

/* ---- settings (sote.ini) ------------------------------------------------- */
static const char* const k_window[] = { "borderless", "window", "exclusive" };

static int ini_int(const char* sec, const char* key, int def) { return GetPrivateProfileIntA(sec, key, def, g_ini); }

static void ini_put(const char* sec, const char* key, const char* fmt, double v) {
    char b[32];
    _snprintf(b, sizeof b - 1, fmt, v), b[sizeof b - 1] = 0;
    WritePrivateProfileStringA(sec, key, b, g_ini);
}

void menu_save(void) {
    if (g_headless) return;                    /* a test run leaves the player's settings alone */
    WritePrivateProfileStringA("display", "window", k_window[g_set.window], g_ini);
    ini_put("display", "vsync", "%.0f", g_set.vsync);
    ini_put("display", "anisotropic", "%.0f", g_set.aniso);
    ini_put("graphics", "fov", "%g", g_set.fov);
    ini_put("cheats", "infinite_lives", "%.0f", g_set.inf_lives);
}

static void load(void) {
    char b[32], *slash;
    GetModuleFileNameA(NULL, g_ini, sizeof g_ini);
    slash = strrchr(g_ini, '\\');
    strcpy(slash ? slash + 1 : g_ini, "sote.ini");
    GetPrivateProfileStringA("display", "window", "borderless", b, sizeof b, g_ini);
    for (int i = 0; i < 3; i++)
        if (!_stricmp(b, k_window[i])) g_set.window = i;
    g_set.vsync = ini_int("display", "vsync", g_set.vsync);
    g_set.aniso = ini_int("display", "anisotropic", g_set.aniso);
    GetPrivateProfileStringA("graphics", "fov", "1", b, sizeof b, g_ini);
    g_set.fov = (float)atof(b);
    if (g_set.fov < 0.5f || g_set.fov > 2.0f) g_set.fov = 1.0f;
    g_set.inf_lives = ini_int("cheats", "infinite_lives", g_set.inf_lives);
}

/* The game's resolution is its own setting (the launcher writes it): HKCU,
 * where the host keeps the game's key (host.c). Read at start. */
#define GAME_KEY "SOFTWARE\\LucasArts Entertainment Company\\Shadows of the Empire"
static int game_res(const char* name) {
    char b[16] = "";
    DWORD n = sizeof b - 1;
    RegGetValueA(HKEY_CURRENT_USER, GAME_KEY, name, RRF_RT_REG_SZ, NULL, b, &n);
    return atoi(b);
}

static void set_game_res(int w, int h) {
    char b[16];
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, GAME_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    _snprintf(b, sizeof b - 1, "%d", w), b[sizeof b - 1] = 0;
    RegSetValueExA(k, "Width", 0, REG_SZ, (const BYTE*)b, (DWORD)strlen(b) + 1);
    _snprintf(b, sizeof b - 1, "%d", h), b[sizeof b - 1] = 0;
    RegSetValueExA(k, "Height", 0, REG_SZ, (const BYTE*)b, (DWORD)strlen(b) + 1);
    RegCloseKey(k);
}

/* ---- the popup ----------------------------------------------------------- */
enum {
    ID_RESTART = 100, ID_NEXT, ID_PREV, ID_LEVEL = 110,
    ID_INVINCIBLE = 200, ID_LIVES, ID_INF_LIVES, ID_ALL, ID_KILL, ID_TELEPORT, ID_DEBUG_DISPLAY, ID_DEBUG_MENU,
    ID_FOV = 300, ID_ANISO = 310, ID_VSYNC = 330, ID_RES = 340, ID_WINDOW = 400,
};

static void add(HMENU m, UINT id, const char* text, int checked, int enabled) {
    AppendMenuA(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text);
}

/* the next (dir 1) or previous (-1) playable scene from the one running */
static int step_level(int dir) {
    int s = (int)MEM32(SCENE), best = -1;
    for (int i = 0; i < NLEVELS; i++) {
        int c = k_levels[i].scene;
        if (dir > 0 ? c > s && (best < 0 || c < best) : c < s && c > best) best = c;
    }
    return best;
}

static void run(int cmd) {
    if (cmd == ID_RESTART) jump_to((int)MEM32(SCENE));
    else if (cmd == ID_NEXT && step_level(1) >= 0) jump_to(step_level(1));
    else if (cmd == ID_PREV && step_level(-1) >= 0) jump_to(step_level(-1));
    else if (cmd >= ID_LEVEL && cmd < ID_LEVEL + NLEVELS) jump_to(k_levels[cmd - ID_LEVEL].scene);
    else if (cmd == ID_INVINCIBLE) queue(CMD_ITEM, ITEM_INVINCIBLE);
    else if (cmd == ID_LIVES) queue(CMD_ITEM, ITEM_LIVES);
    else if (cmd == ID_ALL) queue(CMD_ITEM, ITEM_ALL);
    else if (cmd == ID_KILL) queue(CMD_ITEM, ITEM_KILL);
    else if (cmd == ID_TELEPORT) queue(CMD_ITEM, ITEM_TELEPORT);
    else if (cmd == ID_DEBUG_DISPLAY) queue(CMD_ITEM, ITEM_DEBUG_DISPLAY);
    else if (cmd == ID_DEBUG_MENU) queue(CMD_OPEN_DEBUG, 0);
    else if (cmd == ID_INF_LIVES) g_set.inf_lives = !g_set.inf_lives;
    else if (cmd >= ID_FOV && cmd < ID_FOV + NFOV) g_set.fov = k_fov[cmd - ID_FOV];
    else if (cmd == ID_ANISO) g_set.aniso = g_set.aniso ? 0 : 16;
    else if (cmd == ID_VSYNC) g_set.vsync = !g_set.vsync;
    else if (cmd >= ID_RES && cmd < ID_RES + NRES) set_game_res(k_res[cmd - ID_RES][0], k_res[cmd - ID_RES][1]);
    else if (cmd >= ID_WINDOW && cmd < ID_WINDOW + 3) {
        int was = g_set.window;
        g_set.window = cmd - ID_WINDOW;
        if (was != 2 && g_set.window != 2) display_place();   /* exclusive changes at the next start */
    }
    if (cmd) menu_save();
}

static void show(HWND h) {
    HMENU m = CreatePopupMenu(), lv = CreatePopupMenu(), ch = CreatePopupMenu(), gr = CreatePopupMenu(),
          fov = CreatePopupMenu(), res = CreatePopupMenu(), dp = CreatePopupMenu();
    int lvl = in_level(), cur = level_index((int)MEM32(SCENE));
    int rw = game_res("Width"), rh = game_res("Height");
    char b[96];
    POINT at;

    _snprintf(b, sizeof b - 1, "%s", cur >= 0 ? k_levels[cur].name : MEM32(SCENE) == 2 ? "Front end" : "Cutscene");
    b[sizeof b - 1] = 0;
    add(m, 0, b, 0, 0);
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    add(lv, ID_RESTART, "Restart level", 0, lvl);
    add(lv, ID_NEXT, "Next level", 0, step_level(1) >= 0);
    add(lv, ID_PREV, "Previous level", 0, lvl && step_level(-1) >= 0);
    AppendMenuA(lv, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < NLEVELS; i++) add(lv, ID_LEVEL + i, k_levels[i].name, i == cur, 1);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)lv, "Level");

    add(ch, ID_INVINCIBLE, "Invincibility", MEM32(INVINCIBLE) != 0, lvl);
    add(ch, ID_INF_LIVES, "Infinite lives", g_set.inf_lives, 1);
    add(ch, ID_LIVES, "50 lives", 0, lvl);
    add(ch, ID_ALL, "All weapons and items", 0, lvl);
    AppendMenuA(ch, MF_SEPARATOR, 0, NULL);
    add(ch, ID_KILL, "Kill Dash", 0, lvl);
    add(ch, ID_TELEPORT, "Teleport", 0, lvl);
    add(ch, ID_DEBUG_DISPLAY, "Debug display", MEM16(DEBUG_DISPLAY) == 2, lvl);
    add(ch, ID_DEBUG_MENU, "The game's debug menu (gamepad: both stick clicks)", 0, lvl);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)ch, "Cheats");

    for (int i = 0; i < NFOV; i++) add(fov, ID_FOV + i, k_fov_name[i], g_set.fov == k_fov[i], 1);
    AppendMenuA(gr, MF_POPUP, (UINT_PTR)fov, "Field of view");
    add(gr, ID_ANISO, "Anisotropic filtering", g_set.aniso != 0, 1);
    add(gr, ID_VSYNC, "Vsync (next start)", g_set.vsync, 1);
    for (int i = 0; i < NRES; i++) {
        if (i == FIRST_WIDE) AppendMenuA(res, MF_SEPARATOR, 0, NULL);
        _snprintf(b, sizeof b - 1, "%d x %d%s", k_res[i][0], k_res[i][1], i >= FIRST_WIDE ? " (widescreen)" : "");
        b[sizeof b - 1] = 0;
        add(res, ID_RES + i, b, rw == k_res[i][0] && rh == k_res[i][1], 1);
    }
    AppendMenuA(gr, MF_POPUP, (UINT_PTR)res, "Resolution (next start)");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)gr, "Graphics");

    add(dp, ID_WINDOW + 0, "Fullscreen, borderless\tF11", g_set.window == 0, 1);
    add(dp, ID_WINDOW + 1, "Window\tF11", g_set.window == 1, 1);
    add(dp, ID_WINDOW + 2, "Fullscreen, exclusive: as shipped (next start)", g_set.window == 2, 1);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)dp, "Display");

    /* the game hides the cursor and may hold it: shown and freed for the menu */
    int shown = 0;
    while (ShowCursor(TRUE) < 0) shown++;
    shown++;
    ClipCursor(NULL);
    GetCursorPos(&at);
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, h, NULL);
    PostMessageA(h, WM_NULL, 0, 0);
    DestroyMenu(m);
    while (shown--) ShowCursor(FALSE);
    /* the key or click that chose stays down for a moment: the game would
     * read it as its own (Enter pauses) */
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 500;) {
        if (!(GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
              GetAsyncKeyState(VK_ESCAPE) | GetAsyncKeyState(VK_SPACE)) & 0x8000) break;
        Sleep(10);
    }
    run(cmd);
}

int menu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* r) {
    *r = 0;
    if (g_headless) return 0;
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_F10) { show(h); return 1; }                 /* F10 arrives as a system key */
        if ((w == VK_F11 || (w == VK_RETURN && (l & (1 << 29)))) && g_set.window != 2) {
            g_set.window = !g_set.window;
            display_place();
            menu_save();
            return 1;
        }
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        return w == VK_F10 || w == VK_F11;                      /* not the window menu */
    case WM_ACTIVATEAPP:
        if (!w) ClipCursor(NULL);
        return 0;
    }
    return 0;
}

/* ---- --cmd NAME@T: the menu's commands on a timeline ---------------------- */
#define MAX_CMDS 32
static struct { int id; double t; } g_script[MAX_CMDS];
static int g_nscript;

static DWORD WINAPI script_thread(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_nscript; i++) {
        while ((GetTickCount() - t0) / 1000.0 < g_script[i].t) Sleep(20);
        fprintf(stderr, "[menu] %.1fs --cmd %d\n", (GetTickCount() - t0) / 1000.0, g_script[i].id);
        run(g_script[i].id);
    }
    return 0;
}

int menu_arg(int argc, char** argv, int i) {
    static const struct { const char* name; int id; } names[] = {
        { "restart", ID_RESTART }, { "next", ID_NEXT }, { "prev", ID_PREV }, { "invincible", ID_INVINCIBLE },
        { "lives", ID_LIVES }, { "inflives", ID_INF_LIVES }, { "all", ID_ALL }, { "kill", ID_KILL },
        { "teleport", ID_TELEPORT }, { "debugdisplay", ID_DEBUG_DISPLAY }, { "debugmenu", ID_DEBUG_MENU },
    };
    if (strcmp(argv[i], "--cmd") || i + 1 >= argc) return 0;
    char name[32];
    const char* at = strchr(argv[i + 1], '@');
    int id = -1, n = at ? (int)(at - argv[i + 1]) : 0;
    if (!at || n >= (int)sizeof name || g_nscript == MAX_CMDS) { fprintf(stderr, "--cmd NAME@SECONDS\n"); exit(1); }
    memcpy(name, argv[i + 1], n), name[n] = 0;
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
        if (!strcmp(name, names[k].name)) id = names[k].id;
    if (!strncmp(name, "level:", 6)) {                       /* level:SCENE */
        int li = level_index(atoi(name + 6));
        if (li >= 0) id = ID_LEVEL + li;
    }
    if (!strncmp(name, "fov:", 4))                           /* fov:N, N an index into the menu's list */
        if (atoi(name + 4) >= 0 && atoi(name + 4) < NFOV) id = ID_FOV + atoi(name + 4);
    if (id < 0) { fprintf(stderr, "--cmd %s: unknown\n", name); exit(1); }
    g_script[g_nscript].id = id, g_script[g_nscript++].t = atof(at + 1);
    return 2;
}

void menu_init(int headless) {
    g_headless = headless;
    load();
    fprintf(stderr, "[menu] F10; %s, vsync %d, anisotropic %d, fov x%g (%s)\n",
            k_window[g_set.window], g_set.vsync, g_set.aniso, g_set.fov, g_ini);
    if (g_nscript) CloseHandle(CreateThread(NULL, 0, script_thread, NULL, 0, NULL));
}
