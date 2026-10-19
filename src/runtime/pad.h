#pragma once
#include <stdint.h>

/* The game's emulated buttons (0x008A5C68), named by the key the game binds
 * to each by default (0x004CB778). docs/controls.md says what each does. */
#define PAD_I      0x000001u   /* C up */
#define PAD_K      0x000002u   /* C down */
#define PAD_J      0x000004u   /* C left */
#define PAD_L_KEY  0x000008u   /* C right */
#define PAD_W      0x000010u
#define PAD_C      0x000020u   /* also the middle mouse button */
#define PAD_TAB    0x000040u
#define PAD_Q      0x000080u
#define PAD_D      0x000100u
#define PAD_A_KEY  0x000200u   /* also mouse button 4 */
#define PAD_SPACE  0x000400u   /* also gates the mouse aiming accumulator */
#define PAD_Z_KEY  0x000800u   /* also the right mouse button, joystick button 2 */
#define PAD_X_KEY  0x001000u   /* also the left mouse button, joystick button 1 */
#define PAD_START  0x002000u   /* Esc, Enter, F1 */
#define PAD_DUP    0x004000u   /* the arrows: the stick, digitally */
#define PAD_DDOWN  0x008000u
#define PAD_DLEFT  0x010000u
#define PAD_DRIGHT 0x020000u
#define PAD_W2     0x040000u
#define PAD_S2     0x080000u
#define PAD_A2     0x100000u

/* N64 names, as far as docs/controls.md has confirmed them. */
#define PAD_CUP    PAD_I
#define PAD_CDOWN  PAD_K
#define PAD_CLEFT  PAD_J
#define PAD_CRIGHT PAD_L_KEY
#define PAD_Z      PAD_X_KEY
#define PAD_A      PAD_Z_KEY
#define PAD_R      PAD_SPACE
#define PAD_B      PAD_A_KEY
#define PAD_L      PAD_C

extern int pad_log;
int pad_arg(int argc, char** argv, int i);   /* args consumed, 0 if not ours */
void pad_init(void);
void sote_pad_frame(void);
