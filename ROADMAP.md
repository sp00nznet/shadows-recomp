# Roadmap

## Next

- **Play it with a real pad and mouse.** Every control path is checked under
  scripted input in headless runs; a windowed run with a real controller has
  not happened yet (the virtual monitor was down when this was written).
- **Play through.** Only Battle of Hoth has run. Each level is a new vehicle
  and new code; Escape from Echo Base is the first on foot, where the controls
  matter most.
- **Twin-stick on foot.** The game has one stick: it turns and moves. A mode
  where the right stick turns and the left moves and strafes needs the
  on-foot movement code, not just the pad; it is the biggest thing left
  between this and modern controls.
- **Music and sound.** CD audio goes through Steam's `win32.dll` (ogg); runs
  so far were muted.
- **A conformance harness** (`tools/conformance.py`): boot to the level select,
  start Battle of Hoth, fly, pause, open CONTROLS, with milestones counted from
  the log, as Fallout Tactics' does; and a netlab `QA` line for it.
- **Faster builds.** One generated file holding the big renderer functions takes
  most of the 20 minutes; a `COLD` list (no optimisation) for those, as
  Fallout Tactics has, would cut it.

## Toolkit (pcrecomp)

- PRs for the two changes this game needed: SteamStub 3.x
  (`feat/steamstub-v3`) and `lift32`'s `fst st(i)` (`fix/lift32-fst-st-reg`).
- `--original --lifted`: running chosen functions lifted inside the original
  code breaks when one calls a shim that calls back into the game (the oracle's
  shim stubs point `g_esp` at the native stack). A native32 fix would make
  function-level bisection work in every project.
- The oracle (`oracle.c`) is now in three game repos; it belongs in
  `runtime/native32`.

- **The F10 menu, borderless window and F11 on a real screen.** All of it is
  checked headless through `--cmd`; the popup, the window placement and the
  cursor have not been seen on a monitor yet.

## Later

- The game's other name cheats: `Wampa Stompa` (`0x004CAD98`) and `Wolfy`
  (`0x004CAD94`) change button tricks and characters; what each does is not
  worked out yet, so they are not in the menu.
- Draw distance: the debug menu's fog settings (`Fog Max`, `Fog Thick`) as a
  graphics option, where the levels hold up without the fog.
- The HUD and the front end at widescreen: the front end lays itself out for
  the width; the HUD in each vehicle needs looking at.
- Rogue Squadron 3D: same SteamStub, which the toolkit now removes, but a
  different engine (Factor 5's), so a separate project.

## Out of scope

- The N64 version.
- Redistributing anything from the game, or the generated C.
