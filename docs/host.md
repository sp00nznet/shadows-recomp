# The host

`build/sote.exe` (or `build-farm/sote.exe`) is a 32-bit Windows program that
runs the lifted game. It is built on pcrecomp's `runtime/native32`: the guest
image is mapped at its own base (0x00400000) in the host's address space, so a
pointer the game holds is a pointer Windows can use, and the game's imports go
straight to real Windows with no shim layer in between. Its code sections are
mapped non-executable, so when Windows calls back into the game (a window
procedure, a dialog procedure, a thread) the fetch faults and native32 runs the
lifted body instead.

## What the host adds

| Piece | File | Why |
|---|---|---|
| Guest identity | `host.c` | `GetModuleHandleA(NULL)`, `GetModuleFileNameA` and `GetCommandLineA` describe `Sdata\Shadows.exe`, not the host. |
| Registry | `host.c` | The game's HKLM key is read from HKCU (no administrator, a Steam install's keys untouched). |
| DirectInput | `host.c` | `DirectInputCreateA` gets the host's module: the guest's image is not one the loader knows. |
| Launcher | `host.c` | Every dialog procedure is wrapped: `--play` presses Play, headless dialogs are invisible. |
| Headless display | `display.c` | Steam's `Sdata\ddraw.dll` renders on Direct3D 9. Its `Direct3DCreate9` import is hooked so devices are windowed on the invisible game window, and each Present is counted and, with `--record`, piped to ffmpeg. |
| Controls | `pad.c` | Runs once a frame after the game builds its pad (a patch in `run_lift.py`); `docs/controls.md`. |
| Oracle | `oracle.c` | `--original` runs the shipping machine code under the same host, for comparison. |

## Why the Steam wrapper and not real DirectDraw

The game asks for DirectX 5's Direct3D HAL in exclusive fullscreen. Modern
Windows still has DirectDraw, but not a working DirectX 5 HAL path for this
game, which is why Steam ships the wrapper. Using it keeps the host small, and
the wrapper's own seam (one `Direct3DCreate9` import) is all headless needs.
The DLL comes from the user's install and is never redistributed.

## Command line

```
sote [--run] [--original [--lifted VA|LO-HI|!VA,...]] [--play]
     [--headless [--record out.mp4] [--frames N]] [--mute] [--game DIR]
     [--pad MASK@T[+D]] [--stick X,Y@T[+D]] [--pad-log]
     [--watchdog S] [--native-trace] [--callbacks] [-- game args]
```

Without `--run` it maps and binds the image and stops (a dry run). Debug
environment variables: `SOTE_DRAW_LOG`, `SOTE_DUMP`, `SOTE_WATCH_SY`,
`SOTE_PROFILE`, `SOTE_REG_LOG` (`docs/bringup.md` says what each found).
