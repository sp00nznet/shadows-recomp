#pragma once
#include <stdint.h>

/* Headless: hook Steam's Sdata\ddraw.dll so its Direct3D 9 devices are
 * windowed on the (invisible) game window; record each frame when `record`
 * is set, and exit after `frames` recorded frames (0: never). 1 on success. */
int display_headless(const char* sdata, const char* record, long frames);
long display_frames(void);
void display_record_close(void);
