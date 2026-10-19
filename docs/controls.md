# Controls

The PC port of Shadows of the Empire keeps the N64 game's controller, and that
is most of why it feels bad on a PC:

- **The stick has the N64's response curve.** `sub_0041C900` scales the first
  40% of the stick by 0.4 (40% of the way out moves at 16% speed), then
  steepens it to reach 0.87 at full deflection. A modern pad's stick is precise
  near the centre; this throws that away.
- **The mouse is a virtual joystick.** Outside aim mode it adds one frame's raw
  mouse count times 20 to the stick, so it depends on the frame rate. Holding
  Aim/Look it moves a crosshair by the mouse *times the frame time*, which at a
  modern frame rate is almost nothing, and the crosshair stays where it was
  pushed.
- **Modern pads are not supported as such.** DirectInput joysticks are, with
  32 buttons and 4 hats, but triggers and the right stick map awkwardly, and
  the readme tells you to calibrate the joystick in the Windows control panel.

The recompilation fixes these in its own layer (`src/runtime/pad.c`) without
changing what the game does with its controls.

## How the game reads input

Every frame:

1. `sub_004A93F0` polls DirectInput (keyboard, up to two joysticks, mouse) into
   a flat table of input channels: DIK keyboard codes at 0x00..0xFF, mouse
   buttons at 0x100.., joystick buttons at 0x160.., axes in records at
   `0x008D6AD0`. Held state is at `0x00810378`, presses this frame at
   `0x0080EE88`, raw values at `0x0080F898` (the mouse's relative x and y are
   channels 0x30 and 0x31).
2. `sub_0041C900` maps the user's bindings (a table of channels, pointed to by
   `0x004CBC40`) onto 21 emulated buttons, and builds one stick from the
   joystick axes, the mouse and the arrow keys.
3. `sub_00491D70` packs that into the struct the game reads:

   | Address | Type | What |
   |---|---|---|
   | `0x008A5C60` | float | stick x, -0.87..0.87, right positive |
   | `0x008A5C64` | float | stick y, -0.87..0.87, up positive |
   | `0x008A5C68` | u32 | buttons |

Menus do not use the pad: they read keys straight from the channel table
(`sub_0041EEC0` Enter, `sub_0041EEF0`.. the arrows, `sub_0041EEA0` Esc).

The controls layer runs at two points, both lift patches in `run_lift.py`:
after the poll (`0x004A9494`, menu keys go into the channel table) and after
the pad is packed (`0x0044F45B`, stick and buttons adjusted). Everything the
game does with them afterwards is the game's own code.

## What each button does

From the game's own table (`0x004C4F00`, one bitmask of buttons per control
label; labels at `0x006EF438`), Standard scheme. The pause menu's CONTROLS page
shows the same thing for the vehicle you are in.

| Bit | Default keys | On foot | Snowspeeder | Swoop bike | Cockpit | Outrider |
|---|---|---|---|---|---|---|
| `0x000001`..`08` | I K J L | camera | camera forward, back, exterior, drop | camera | camera | camera |
| `0x000010` | W | weapon | | | | |
| `0x000020` | C, joystick 3 | duck | harpoon | | missile | missile |
| `0x000040` | Tab | camera position | camera position | camera position | camera position | camera position |
| `0x000080` | Q | jetpack on/off | | | | roll |
| `0x000100` | D | | | right kick | | |
| `0x000200` | A, joystick 4 | strafe, activate | brakes | | decelerate | |
| `0x000400` | Space | aim/look | harpoon | | | missile |
| `0x000800` | Z, joystick 2, mouse 2 | jump, jetpack thrust | thrust | throttle | accelerate | roll |
| `0x001000` | X, joystick 1, mouse 1 | fire | fire | reverse | fire | fire |
| `0x002000` | Esc, Enter, keypad Enter, F1 | pause | pause | pause | pause | pause |
| `0x004000`..`0x020000` | arrows | the stick, digitally | | | | |
| `0x080000` | (Traditional S) | | | left kick | | |

The Traditional scheme moves some of these to other keys (WASD-style) but
keeps the bits.

## Gamepad (XInput)

Any controller Windows sees as an Xbox pad: Xbox, PlayStation through Steam
Input or DS4Windows, most others.

| Pad | Does | Bit |
|---|---|---|
| Left stick | move, steer (radial dead zone, linear) | stick |
| Right stick | camera (the C buttons) | `0x1`..`0x8` |
| RT | fire | `0x1000` |
| LT | aim/look (hold) | `0x400` |
| A | jump, thrust, accelerate | `0x800` |
| B | duck, harpoon, missile | `0x20` |
| X | activate, brakes, decelerate | `0x200` |
| Y | weapon | `0x10` |
| RB | strafe (hold); swoop bike right kick | `0x200`, `0x100` |
| LB | jetpack on/off; swoop bike left kick | `0x80`, `0x80000` |
| Back | camera position | `0x40` |
| Start | pause | `0x2000` + Enter |
| D-pad | the stick, digitally; menu moves | `0x4000`..`0x20000` + arrows |

In the front end (title, level select) A selects and B goes back. In a level
the pause menu takes the D-pad and Start: every select key the menus accept is
bound to something in play (Enter pauses; mouse button 1 and joystick button 1
fire), so A cannot safely be one there. The scene running is at `0x008A5C28`:
2 is the front end, the levels are 3 (Battle of Hoth) and up
([menu.md](menu.md) has the list).

## Mouse

| Mode | The game | The recompilation |
|---|---|---|
| Steering | one frame's count x20 | stick = mouse speed (counts a second / `full_speed`) |
| Aiming (Aim/Look held) | crosshair += count x frame time x30 | crosshair += count / `aim_counts` |

Both are frame-rate independent. `steer_by_speed = 0` in `controls.ini` gives
the game's own mouse back.

## controls.ini

Optional, beside `sote.exe`. Every key is optional; these are the defaults.
Button values are the bits above, and several can be combined (`0x80|0x80000`
is written `0x80080`).

```ini
[pad]
deadzone = 0.15                ; of the stick's travel, radial
curve = 1.0                    ; 1 linear; above 1 finer near the centre
right_stick_threshold = 0.5
trigger_threshold = 0.25
a = 0x800
b = 0x20
x = 0x200
y = 0x10
lb = 0x80080
rb = 0x300
lt = 0x400
rt = 0x1000
back = 0x40
start = 0x2000
dpad_up = 0x4000
dpad_down = 0x8000
dpad_left = 0x10000
dpad_right = 0x20000
rs_up = 0x1
rs_down = 0x2
rs_left = 0x4
rs_right = 0x8

[mouse]
steer_by_speed = 1
full_speed = 1500              ; counts a second for a full turn rate
aim_counts = 300               ; counts from the centre to the edge, aiming
invert_y = 0
```

The mouse numbers depend on the mouse's resolution; they are the knob to turn.

## Checking it

- `sote --pad-selftest`: the stick shaping (dead zone, full deflection,
  diagonals).
- `--pad MASK@T[+D]`, `--stick X,Y@T[+D]`, `--key CH@T[+D]`: scripted input
  for headless runs, with `--pad-log` printing each change, the game state and
  a heartbeat. This drove the speeder in the bring-up (stick right at 155 s
  banks it right, left at 160 s banks it left).

Not checked yet, because the virtual monitor was unavailable when this was
written (docs/bringup.md): a real pad and mouse in a windowed run.
