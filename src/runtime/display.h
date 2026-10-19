#pragma once
#include <stdint.h>

/* Hook Steam's Sdata\ddraw.dll, which renders on Direct3D 9: its devices
 * follow the F10 menu's display settings (menu.h), and headless ones are
 * windowed on the invisible game window. With `record` set each frame is
 * recorded, and the run exits after `frames` recorded frames (0: never).
 * 1 on success. */
int display_init(const char* sdata, int headless, const char* record, long frames);
void display_place(void);          /* the game window as the settings say: borderless or a window */
long display_frames(void);
float display_aspect(void);        /* the back buffer's width / height, 0 before a device */
long display_flat_percent(void);   /* SOTE_DRAW_LOG: 3D vertices on the middle row, -1 if none */
void display_record_close(void);
