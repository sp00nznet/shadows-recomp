# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- **The installer** (`Shadows-Recomp-Installer`): recomp-launcher with
  `launcher/launcher.json`, built by `.github/workflows/installer.yml` on
  every push (artifacts) and `v*` tag (release files). `tools/setup.ps1` takes
  `-Yes` (no questions, installs the Visual Studio Build Tools if missing),
  skips its final "Press Enter" under `RECOMP_LAUNCHER`, and finds the toolkit
  at `PCRECOMP` or `..\pcrecomp` too.
- The F10 menu (`src/runtime/menu.c`, `docs/menu.md`): level restart, next,
  previous and jump to any level; invincibility, infinite lives, 50 lives,
  all weapons and items, Kill Dash, Teleport, the debug display and the
  game's own debug menu (both stick clicks on a pad); field of view,
  anisotropic filtering, vsync, resolution; borderless
  fullscreen, a window (F11, Alt+Enter) or exclusive fullscreen. Kept in
  `sote.ini`. `--cmd NAME@T` runs the same commands in headless runs.
- Widescreen: the camera's aspect follows the screen (Hor+), and cutscenes
  are centred above 1280x960 (they stopped at 2x in the top left).
- `run_lift.py` rewrites only the generated files that changed, so a rebuild
  after a small patch compiles one file.
- The Steam `Shadows.exe` lifts whole with pcrecomp: 2,806 functions, 0 errors,
  about 2 million lines of C, after SteamStub 3.x is removed statically (needs
  pcrecomp `feat/steamstub-v3`). `docs/RECON.md`.
- A 32-bit host on pcrecomp's `native32` runs it: the CRT, `WinMain`, the game's
  launcher dialog, the title, the level select, the opening cinematic and the
  Battle of Hoth, through Steam's DirectDraw-to-Direct3D 9 wrapper.
- The game's settings live under HKCU instead of HKLM, so nothing needs an
  administrator and a Steam install's own keys are left alone.
- `--headless`: the Direct3D 9 device is forced windowed on an invisible window,
  so a run takes no screen (works over RDP); `--record out.mp4` and `--frames N`
  capture what it draws, at the run's own clock; `--mute` silences the process.
- `--play` presses the launcher's Play (implied by `--headless`).
- The controls layer (`src/runtime/pad.c`, `docs/controls.md`): XInput gamepads
  with a radial dead zone and a linear stick, the right stick as the camera,
  every action on one button in every vehicle (from the game's own action
  table); a mouse that steers by speed and aims by counts, frame-rate
  independent; menu keys from the pad; settings in `controls.ini`;
  `--pad-selftest`.
- Scripted input for headless runs: `--pad`, `--stick`, `--key`, `--pad-log`.
- `--original` runs the shipping machine code under the same host (from
  Fallout Tactics' host), and `--lifted` runs chosen functions lifted inside
  it. Debug switches `SOTE_DRAW_LOG`, `SOTE_DUMP`, `SOTE_WATCH_SY`,
  `SOTE_PROFILE`, `SOTE_REG_LOG`.
- `Setup.cmd` quick start: finds the Steam install, copies, unwraps, writes the
  settings, lifts, builds, and leaves a shortcut.

### Fixed
- 3D drew every vertex on the screen's middle row: pcrecomp's `lift32` lifted
  `fst st(i)` to nothing, and the projection keeps 1/z in st(1) that way (needs
  pcrecomp `fix/lift32-fst-st-reg`). `docs/bringup.md` 5.
- `DirectInputCreateA` refused the guest's module handle; the host passes its own.
- An injected key was never released (the poll does not clear the held table),
  which kept Start pressed in play.
