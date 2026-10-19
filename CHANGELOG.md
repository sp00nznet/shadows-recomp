# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- The Steam `Shadows.exe` lifts whole with pcrecomp: 2,806 functions, 0 errors,
  about 2 million lines of C, after SteamStub 3.x is removed statically (needs
  pcrecomp `feat/steamstub-v3`). `docs/RECON.md`.
- A 32-bit host on pcrecomp's `native32` runs it: the CRT, `WinMain`, the game's
  launcher dialog and its renderer through Steam's DirectDraw-to-Direct3D 9
  wrapper. It boots to the level select.
- The game's settings live under HKCU instead of HKLM, so nothing needs an
  administrator and a Steam install's own keys are left alone.
- `--headless`: the Direct3D 9 device is forced windowed on an invisible window,
  so a run takes no screen (works over RDP); `--record out.mp4` and `--frames N`
  capture what it draws; `--mute` silences the process.
- `--play` presses the launcher's Play (implied by `--headless`).
- The controls layer (`src/runtime/pad.c`): XInput gamepads with a radial dead
  zone and a linear stick, the right stick as the C buttons, and scripted
  `--pad`/`--stick` input for headless runs. Settings in `controls.ini`.
