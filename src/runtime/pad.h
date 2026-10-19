#pragma once
#include <stdint.h>

/* The game's emulated buttons (0x008A5C68). The names are what each does
 * under the Standard control scheme, read from the game's own table of which
 * buttons perform which action (0x004C4F00, labels at 0x006EF438):
 * docs/controls.md has the full table, per vehicle. */
#define PAD_CUP     0x000001u   /* camera: C up (I) */
#define PAD_CDOWN   0x000002u   /* C down (K) */
#define PAD_CLEFT   0x000004u   /* C left (J) */
#define PAD_CRIGHT  0x000008u   /* C right (L) */
#define PAD_WEAPON  0x000010u   /* on foot: weapon */
#define PAD_DUCK    0x000020u   /* on foot: duck; speeder: harpoon; cockpit: missile */
#define PAD_CAMPOS  0x000040u   /* camera position (Tab) */
#define PAD_JETPACK 0x000080u   /* jetpack on/off; Outrider: roll */
#define PAD_RKICK   0x000100u   /* swoop bike: right kick */
#define PAD_STRAFE  0x000200u   /* on foot: strafe, activate; vehicles: brakes, decelerate */
#define PAD_AIM     0x000400u   /* on foot: aim/look; speeder: harpoon */
#define PAD_JUMP    0x000800u   /* on foot: jump, jetpack thrust; vehicles: thrust, accelerate */
#define PAD_FIRE    0x001000u   /* fire (swoop bike: reverse) */
#define PAD_START   0x002000u   /* pause: Esc, Enter, F1 */
#define PAD_DUP     0x004000u   /* the arrows: the stick, digitally */
#define PAD_DDOWN   0x008000u
#define PAD_DLEFT   0x010000u
#define PAD_DRIGHT  0x020000u
#define PAD_W2      0x040000u   /* the Traditional scheme's W */
#define PAD_LKICK   0x080000u   /* swoop bike: left kick */
#define PAD_A2      0x100000u   /* the Traditional scheme's A */

extern int pad_log;
int pad_arg(int argc, char** argv, int i);   /* args consumed, 0 if not ours */
void pad_init(void);
void sote_pad_frame(void);
void sote_keys_frame(void);
