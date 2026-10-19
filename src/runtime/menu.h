#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* What the F10 menu sets, kept in sote.ini beside sote.exe (docs/menu.md). */
typedef struct {
    int window;          /* 0 borderless fullscreen, 1 a window, 2 exclusive fullscreen (as shipped) */
    int vsync;
    int aniso;           /* anisotropic filtering, 0 off, else the level (16) */
    float fov;           /* field of view, times the game's own */
    int inf_lives;
} sote_settings_t;
extern sote_settings_t g_set;

void menu_init(int headless);
int  menu_arg(int argc, char** argv, int i);     /* args consumed, 0 if not ours */
/* the game window's messages: 1 if handled (*r is the result) */
int  menu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* r);
void menu_pad_frame(uint32_t* buttons);          /* pad.c, each frame: runs a queued command */
void menu_game_debug_menu(void);                 /* opens the game's own debug menu */
void menu_save(void);

/* run_lift.py PATCHES */
void sote_scene_request(void);
void sote_camera_frame(void);
