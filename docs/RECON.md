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

## CD release

Asked for in issue #1. Measured 2026-10-27.

**v1.0 (the 1997 CD, September 1997): not examined.** The CD image on
archive.org (`star-wars-shadows-of-the-empire-us-1997-win`) was not
downloaded: we own the Steam copy, not the CD, so legal blocked it. Its
timestamp, entry point and `.text` are unknown.

**v1.1 (the official LucasArts update, posted 2/98): the same build as
Steam's.** `Shadowsp.exe` (1,832,670 bytes, SHA-256 `d45804cb...7b27a0`),
from Archive Team's 2013 grab of LucasArts' own FTP site (archive.org
`ftp.lucasarts.com-20130427`, `patches/pc/Shadowsp.exe`). It is a self-extracting zip
holding `SHADOWS.EXE`, `SHADOWFF.DLL` and docs. The patch's readme says to copy
`Shadows.exe` into `SDATA\`.

| | Steam, SteamStub removed | 1.1 patch `SHADOWS.EXE` |
|---|---|---|
| PE timestamp | `0x34909499` | `0x34909499` |
| Entry point | `0x004AE550` | `0x004AE550` (no DRM: no `.bind`, a plain MSVC entry) |
| Sections | `.text` `.rdata` `.data` `.idata` `.rsrc` `.reloc`, same RVAs and sizes | same |
| Imports | `WIN32.dll` (7), `DDRAW.dll` (2), ... | `WINMM.dll` (7), `DDRAW.dll` (2), ... |
| `.text` | | 27 bytes differ, in 7 places |

Those 27 bytes are Steam's own changes to 1.1:

| VA | 1.1 | Steam |
|---|---|---|
| `0x004127D2` | three calls on a dialog item | a different pair of calls (`0x3F9`, then `[0x8DE428]`) |
| `0x00464D02` | `call sub_0043D380` | nops |
| `0x00464DFC` | `jne`: stop if sound setup (`sub_0046F070`) failed | `jmp` |
| `0x00464E06` | `call sub_00491540`, the CD check | `call sub_004A6620` |
| `0x0046F878` | `je` | `jmp` |
| `0x004915E5` | `jne`: "Unknown error" and quit unless a drive is labeled `SHADOWS` | `jmp` |
| `0x004A8967` | `cmp eax, 0x40` | `cmp eax, 0x7F` (a bigger table) |

`.rdata` also differs in two float constants: 640/480 (1.1) against
720/560 (Steam) at `0x004C1A52`, and one at `0x004C4D48`. `.rsrc` differs too.

**The pipeline takes the 1.1 exe as it is.** `disasm32.py` finds the same 2,806
functions. `run_lift.py --all` lifts them with 0 errors, all six lift patches
apply, and the C differs from the Steam lift in 75 lines, at the places above.
Every hardcoded VA in `src/runtime` is the same. `tools/setup.ps1` used to
fail on it (steamstub: "not a SteamStub stub"). It now takes an exe with no
`.bind` as it is.

**What is not known yet:**
- **The display.** A CD install has no `Sdata\ddraw.dll`, Steam's Direct3D 9
  wrapper (docs/host.md, "Why the Steam wrapper"). Without it, the game binds the
  system DirectDraw and asks for a DirectX 5 HAL. `--headless` and the F10
  display settings hook only that wrapper. Nobody has tried a 1.1 lift
  against system DirectDraw or another DirectDraw wrapper.
- **The disc.** 1.1 keeps the CD check: a drive labeled `SHADOWS` must be
  there. It also plays music from the disc through the real `winmm`. A CD owner has both. We
  have neither, so no CD install has been run.
- **v1.0** without the patch: a different build until measured.

## Rogue Squadron 3D (Steam app 455910)

Same SteamStub 3.x (`Rogue Squadron.EXE` unwraps with the same tool, OEP
`0x005A2200`), but a different engine: Factor 5's, linker 5.10, 1999, a
1.8 MB `.text`, and its own force-feedback library (`IFORCE2.DLL`). Nothing
here carries over beyond the toolkit; it is a separate project.
