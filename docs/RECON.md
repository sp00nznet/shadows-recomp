# Reconnaissance

What the Steam release of Shadows of the Empire is, measured with pcrecomp's own
tools (`tools/pe/pe_analyze.py`, `tools/disasm/disasm32.py`). Everything here is
true of Steam app 560170 as installed on 2026-10-18.

## The install

| File | What it is |
|---|---|
| `SOTE.EXE` | The 1997 setup/launcher (config dialog, 424 imports). Not the game. |
| `Sdata\Shadows.exe` | **The game.** Steam's `installScript.vdf` names it as the executable. |
| `Sdata\ddraw.dll` | Added by Steam (2015): a DirectDraw-to-Direct3D 9 wrapper with D3DX, libpng and libjpeg linked in. The game's `DDRAW.dll` import binds to it. |
| `Sdata\win32.dll` | Added by Steam (2016, MinGW): a stand-in for `winmm`. The exe's `WINMM.dll` import was renamed to `WIN32.dll`; it plays the CD music from `.ogg` through `libvorbisfile-3.dll`. |
| `Sdata\DATA00.DLL`..`DATA31.DLL` | Level data packaged as DLLs: each exports `GetTheDataStub` and `GetLevelStub`, 11 KB of code (MSVC CRT) around the data. |
| `Sdata\SHADOWFF.DLL` | Force-feedback effects. |

`Install.bat` writes the game's settings to `HKLM\SOFTWARE\LucasArts Entertainment
Company\Shadows of the Empire` on Steam's first launch. A copy that was never
launched from Steam has no keys at all; the host reads them from HKCU instead
(`src/runtime/host.c`, "the registry").

## Shadows.exe

```
  Image Base:    0x00400000
  Entry Point:   0x008F6310 (RVA 0x004F6310)      <- SteamStub
  Linker:        3.00
  Timestamp:     1997-12-12 01:34:17 UTC (0x34909499)
  Code Range:    0x00401000 - 0x004B8200 (750,080 bytes)
  .bind     VA 0x004F6000  VSize 0x0001F690  [EXEC|READ]
  Imports: 109 functions from 10 DLLs
```

**SteamStub 3.x.** `.text` is AES-256-CBC ciphertext and the entry point is in
`.bind`. pcrecomp's `tools/drm/steamstub.py` removes it statically (3.x support
added for this game, pcrecomp branch `feat/steamstub-v3`):

```
SteamStub 3.x: app 560170, header 0x008F6220
  .text 0x00401000+0xB7200 decrypted (AES-256-CBC); OEP 0x004AE550
```

The OEP is MSVC's `WinMainCRTStartup` (`mov eax, fs:[0]; push ebp; mov ebp, esp;
push -1; ...`). The unwrapped file keeps the PE timestamp, so `run_lift.py` and
the host check both the timestamp and the entry point.

**Imports.** 109, small for a 3D game: `DirectDrawCreate`, `DirectDrawEnumerateA`,
`DirectInputCreateA`, `DirectSoundCreate`, 54 from KERNEL32, 35 from USER32, the
registry, and seven `WIN32.dll` (winmm) calls for CD audio and `timeGetTime`.

**Function recovery** (`disasm32.py`, 2 min 28 s):

```
[*] Functions: 2806  (thunks=10, leaves=653)
[*] Instructions: 1,041,996
[*] Byte coverage: 729,337 / 750,080 (97.2% of code range)
```

**Lift** (`run_lift.py --all`): 2,806 functions, 0 errors, 8 with no terminator,
1,977,620 lines of C in 8 files.

## Input

The PC port keeps the N64 game's control model: per-vehicle control schemes
(`1st Person Controls`, `Swoop Bike Controls`, `Snowspeeder Controls`, `Cockpit
Controls`, `Outrider Controls`), a rebinding screen ("Highlight function, Press
key or button to change control"), and an invert toggle ("Vertical control is
inverted"). Devices are DirectInput: the binding names cover two joysticks of 32
buttons and four hats each, and two mouse buttons.

`sub_004A91F0` creates DirectInput (`DirectInputCreateA(hInstance, 0x500, ...)`,
DirectX 5) after zeroing the device tables at `0x0080F968`, `0x00810378`,
`0x008D6AD0` and `0x0080F898`; `sub_0041C830` calls it and then walks the
joysticks found (0x90-byte records at `0x008D6AD0`).

## Rogue Squadron 3D (Steam app 455910)

Same SteamStub 3.x (`Rogue Squadron.EXE` unwraps with the same tool, OEP
`0x005A2200`), but a different engine: Factor 5's, linker 5.10, 1999, a
1.8 MB `.text`, and its own force-feedback library (`IFORCE2.DLL`). Nothing
here carries over beyond the toolkit; it is a separate project.
