# Bring-up notes

What it took to get the lifted Shadows.exe from "maps and binds" to drawing the
game, in order. Each entry says what was seen, what it was, and the fix. The
host is `src/runtime/host.c`; the reasoning for its shape is `docs/host.md`.

## 1. SteamStub 3.x

```
steamstub: entry point is not a SteamStub 2.x x86 stub (prologue e800000000505351525657558b44)
```

The Steam exe is wrapped in SteamStub 3.x, which pcrecomp's `steamstub.py` did
not know. Its layout was read from the stub's own code and added to the tool
(pcrecomp `feat/steamstub-v3`); `docs/RECON.md` has the header fields. Rogue
Squadron 3D's exe unwraps with the same code.

## 2. No registry keys

Steam writes the game's settings to HKLM on first launch (`Install.bat`). A
copied install has none, and HKLM needs an administrator. The host sends the
game's `HKLM\SOFTWARE\LucasArts...` opens to HKCU; `tools/setup.ps1` writes the
values Steam would have.

## 3. Stuck in a dialog

The first run sat in `sub_00412090` making no native calls. It is
`DialogBoxParamA(102)`: the game's own launcher (Play, New Player, Restore
Player, View Trailer, Quit, display settings). `--play` presses Play; headless,
every dialog is made invisible before it is first shown.

## 4. DirectInputCreateA returned nothing

```
=== fault 0xC0000005 at 0x61DFD356
  read of 0x00000000 (null/low)
  in lifted sub_004A9730
last indirect calls (newest first):
  0x72B7C390  from 0x004B6C82  DINPUT.dll!DirectInputCreateA
```

The game passes its own hInstance, 0x00400000. native32 maps the image itself,
so the loader does not know that module and DirectInput refuses it; the game
then used the NULL interface. The host's module stands in for it.

## 5. 3D drew nothing but the sky

The attract demo of the Hoth level showed only its clear colour. `--original`
(the shipping machine code under the same host, `src/runtime/oracle.c`) drew
it, so it was the lift. `SOTE_DRAW_LOG=1` counts draw calls and samples a
vertex a second; the lifted run drew as much as the original, but its vertices
had a constant y:

```
original  vertex (304.087, 120.583, 1e-05, 100000)
lifted    vertex (332.05, 240.5, 0.823526, 0.294124)
          vertex (57.1524, 240.5, 0.996056, 0.00657307)
```

Every 3D vertex sat on the middle row. A profile (`SOTE_PROFILE=1`) showed the
display-list interpreter hot, and its per-vertex projection is
`sub_004ABA60`:

```
004ABA8C  fldz
004ABA96  fld dword ptr [0x8db24c]
004ABAA5  fdiv dword ptr [edx + 4]      ; k / z
004ABAB2  fst st(1)                     ; keep it in st(1)
004ABAB4  fmul dword ptr [edx]          ; x * k/z
...
004ABABC  fld dword ptr [edx + 8]
004ABABF  fmul st(1)                    ; y * k/z
```

`fst st(1)` was missing from the lifted C: pcrecomp's `lift32` handled only the
memory forms of `fst`, and the register form fell through and emitted nothing.
st(1) kept the 0 from `fldz`, so y was multiplied by 0. Fixed in the toolkit
(pcrecomp `fix/lift32-fst-st-reg`, difftest case `fpu.fst-st1`). The game has
two `fst st(i)`, both in this function.

How the vertex counts are read back: `--watchdog` prints the share of 3D
vertices on the middle row when `SOTE_DRAW_LOG=1` is set.

```
original                          3D vertices on the middle row: 0%
lifted, before the fst fix        3D vertices on the middle row: 38%
```

### Things tried on the way that did not find it

- A hardware watch on a drawn vertex's y (`SOTE_WATCH_SY`) named the
  execute-buffer code, which only copies: the buffers are reused.
- Dumping the matrices and vertex arrays (`SOTE_DUMP`) at the same moment of
  the deterministic demo in both runs: the matrices matched, the arrays were
  between uses.
- `--original --lifted RANGE`, which runs chosen functions lifted inside the
  original code: a single leaf works, but a set that includes a function that
  calls a shim which calls back into the game (the launcher dialog, window
  creation) corrupts the lifted stack, because the oracle's shim stubs point
  `g_esp` at the native stack. A fix belongs in native32 (ROADMAP.md).
