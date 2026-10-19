# The F10 menu

F10 opens a menu over the game, as in the other pcrecomp ports: levels,
cheats, graphics and the window. Settings are kept in `sote.ini` beside
`sote.exe`.

| Menu | What it does |
|---|---|
| **Level** | Restart, next, previous, or any level by name. Cutscenes are skipped. |
| **Cheats** | Invincibility, infinite lives, 50 lives, all weapons and items, Kill Dash, Teleport, the debug display, and the game's own debug menu. |
| **Graphics** | Field of view (normal, wide, wider, widest), anisotropic filtering, vsync, the game's resolution, 4:3 and widescreen. |
| **Display** | Borderless fullscreen (the default), a window, or exclusive fullscreen as the game shipped. F11 or Alt+Enter switches between borderless and a window. |

Vsync, the resolution and exclusive fullscreen take effect the
next time the game starts; everything else at once.

On a gamepad, clicking both sticks opens the game's own debug menu (below),
which the pad can work: up and down choose, A or fire runs.

## Where it comes from

Most of it is the game's. Shadows of the Empire ships a debug menu
(`0x0073E678`): Get all stuff, Invincibility, Debug Display, 50 Lives, Kill
Dash, Teleport, Next Level, This Level, Prev Level, and fog and camera
settings. Its pause menu offers it only when the player's name is
`R Testers ROCK` (`sub_0040D0C0` compares the name with that, `Wampa Stompa`,
`Wolfy` and `Credits`).

The F10 menu runs those items through the game's own code: it opens the debug
menu, selects the item and presses the button that runs it, for one frame
(`menu_pad_frame`, right after the pad is packed). Pausing, sounds and
messages are then the game's.

What the debug menu lacks, the host adds in three small hooks in the lift
(`run_lift.py` `PATCHES`):

| Hook | Where | What |
|---|---|---|
| `sote_scene_request` | `sub_0044F570` has stored the scene asked for (`0x006F0CEC`) | a level jump puts its own scene in, so "This Level" goes anywhere |
| `sote_camera_frame` | `sub_00414080` has taken the camera's field of view and aspect (`0x008D6304`, `0x008D6308`) | the field of view is scaled; on a wide screen the aspect widens with it (Hor+) |
| `sote_movie_offset` | `sub_004706A0`, the cutscene blit, has its surface in `ebx` | above 1280x960 the cutscene is centred |

### Levels

The scenes are `Sdata\DATA00.DLL` to `DATA31.DLL`. The game's debug names
(`0x0073EBB0`) tell the levels from the cutscenes:

| Scene | Level |
|---|---|
| 3 | Battle of Hoth |
| 4, 5 | Escape from Echo Base |
| 6 | The Asteroid Field |
| 8, 10 | Ord Mantell Junkyard (the train, the junk heap) |
| 14, 15 | Gall Spaceport |
| 17 | Mos Eisley and Beggar's Canyon |
| 20, 21, 22 | Imperial Freighter Suprosa |
| 25 | Sewers of Imperial City |
| 26, 27 | Xizor's Palace |
| 29, 30 | Skyhook Battle (the chase, the battle) |
| 31 | Credits |

The scene running is at `0x008A5C28` (2 is the front end). A scene change is
asked for by `sub_0044F570`, which writes the scene to `0x006F0CEC` and 2 to
`0x006F0CE8`; the main loop (`sub_0044F270`) loads it.

### Resolution and widescreen

The game draws at the resolution in its settings (`Width`, `Height`), any
size: 1920x1080 and 3840x2160 work. Two things assumed 4:3 at 640x480 to
1280x960:

- **The 3D view.** The camera's aspect (`0x008D6308`) is 4/3 whatever the
  screen. The host scales it by the screen's shape, so a wide screen sees
  more to the sides rather than a stretched picture, and the game culls for
  the wider view too (it uses field of view times aspect).
- **Cutscenes.** The player scales its 640x480 frames to the screen, up to 2x,
  from the top left. Above 1280x960 the 2x picture is now centred.

The front end lays itself out for the screen's width.

### Display

The game asks for exclusive fullscreen. Steam's `Sdata\ddraw.dll` turns that
into a Direct3D 9 device, which the host makes windowed on the game's window
(`display.c`), then makes the window borderless over its monitor or a normal
window. Each frame is scaled into the window with its aspect kept and black
bars beside it.

The wrapper already asks for anisotropic minification on 3D textures, at
Direct3D's default level of 1; the menu's setting raises the level (16 by
default). The textures have no mipmaps, so the difference is modest. There is
no antialiasing: the wrapper draws into lockable render targets that stand in
for DirectDraw surfaces, and those cannot be multisampled (tried: forcing
MSAA on the back buffer left the 3D black).

DirectInput devices are made non-exclusive, so the menu has a cursor and
clicks; the game reads the mouse the same either way.

## sote.ini

```ini
[display]
window = borderless      ; borderless, window, exclusive
vsync = 1
anisotropic = 16         ; 0 off, 2 to 16

[graphics]
fov = 1                  ; times the game's: 1, 1.15, 1.3, 1.45

[cheats]
infinite_lives = 0
```

## Checking it without a screen

`--cmd NAME@T` runs a menu command T seconds into the run: `restart`, `next`,
`prev`, `level:SCENE`, `invincible`, `lives`, `inflives`, `all`, `kill`,
`teleport`, `debugdisplay`, `debugmenu`, `fov:N` (N indexes the menu's list).
With `--headless --record` that shows each one working.
