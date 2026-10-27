#!/usr/bin/env python3
"""Shadows of the Empire lift driver: work/functions.json -> src/recomp/gen/.

Drives pcrecomp's shared `tools/lift/generate.py` + `lift32`, the same shape as
Fallout Tactics' run_lift.py:

* **Closure-limited lifting** (`generate.closure`). Lift the call-graph closure
  from the OEP and give every other catalogued function a stub that aborts
  naming itself. Bring-up uses --all; the closure is there for bisecting.
* **Extents by walking the branches** (`generate.true_extent`).

The input is the Steam exe with SteamStub 3.x removed (tools/setup.ps1 runs
pcrecomp's tools/drm/steamstub.py): the wrapped file's .text is AES ciphertext,
and the switch tables MSVC keeps in .text with it.

    py -3 run_lift.py --all
    py -3 run_lift.py --roots 0x004AE550 --max 20000
"""
import argparse
import filecmp
import json
import os
import shutil
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks the toolkit checkout, the same knob CMakeLists.txt has: the
# lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, 'work', 'pcrecomp')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, closure, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

EXE_NAME = 'Shadows.unwrapped.exe'
EXE = os.path.join(_HERE, 'game', 'Sdata', EXE_NAME)
CATALOG = os.path.join(_HERE, 'work', 'functions.json')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')

# Functions whose body is the host's instead of the lift: the host defines
# sote_hook_<name>(void) and it runs like an import shim (arguments from g_esp,
# pops its own return address). src/runtime/host.c.
HOOKS = {}

# Lines of C added after one lifted instruction (see apply_patches): the input
# layer's hooks into the game's own control code. docs/controls.md.
PATCHES = {
    # sub_0044F270's call to sub_00491D70, which packs the game's pad: from
    # here the frame's stick and buttons are final, and the controls layer
    # (src/runtime/pad.c) adjusts them before anything reads them.
    0x0044F45B: '{ extern void sote_pad_frame(void); sote_pad_frame(); } /* controls: run_lift.py PATCHES */',
    # sub_004A93F0 has just read the keyboard, joysticks and mouse into the
    # channel tables (held at 0x00810378, presses at 0x0080EE88). Menus read
    # keys straight from there (sub_0041EEC0: Enter), so the controls layer
    # adds the gamepad's menu keys here, before anything reads them.
    0x004A9494: '{ extern void sote_keys_frame(void); sote_keys_frame(); } /* controls: run_lift.py PATCHES */',
    # sub_0044F570 (a scene change is asked for) has stored the scene at
    # 0x006F0CEC: the F10 menu's level jump puts its own in (src/runtime/menu.c).
    0x0044F5C4: '{ extern void sote_scene_request(void); sote_scene_request(); } /* menu: run_lift.py PATCHES */',
    # sub_00414080 (the camera, each frame) has taken its caller's field of
    # view and aspect (0x008D6304/08), before it uses them: the menu's field
    # of view is applied here.
    0x004140AA: '{ extern void sote_camera_frame(void); sote_camera_frame(); } /* menu: run_lift.py PATCHES */',
    # sub_004706A0, the cutscene player's frame blit, has its surface in ebx.
    # It scales 640x480 to the screen, but by 2x at most, from the top left:
    # above 1280x960 the picture is moved to the middle.
    0x004706BF: '{ extern uint32_t sote_movie_offset(void); ebx += sote_movie_offset(); } /* menu: run_lift.py PATCHES */',
}

# The Steam release's Shadows.exe (1997-12-12): its PE timestamp. Unwrapping
# keeps it, so it names the build either way. The official 1.1 patch's exe
# (for the CD release) is the same build (docs/RECON.md, "CD release").
EXE_STAMP = 0x34909499


def apply_patches(body, patches):
    """Add each patch's C after the line that lifts its instruction."""
    for va, c in patches.items():
        tag = '/* 0x%08X:' % va
        k = body.find(tag)
        if k != -1:
            e = body.index('\n', k)
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


def sync_dir(src, dst):
    """Move src's files over dst's where they differ, drop dst's others,
    remove src. The number of files changed."""
    os.makedirs(dst, exist_ok=True)
    changed = 0
    keep = set(os.listdir(src))
    for fn in os.listdir(dst):
        if fn not in keep and fn.startswith('recomp_'):
            os.remove(os.path.join(dst, fn))
    for fn in keep:
        new, old = os.path.join(src, fn), os.path.join(dst, fn)
        if os.path.exists(old) and filecmp.cmp(new, old, shallow=False):
            continue
        os.replace(new, old)
        changed += 1
    shutil.rmtree(src)
    return changed


def write_target_header(out, entry):
    """recomp_target.h: what the host needs to know about the binary it runs."""
    with open(os.path.join(out, 'recomp_target.h'), 'w', newline='\n') as f:
        f.write('/* Generated by run_lift.py: the binary this build runs. */\n#pragma once\n')
        f.write('#define SOTE_EXE_NAME "%s"\n' % EXE_NAME)
        f.write('#define SOTE_EXE_STAMP 0x%08Xu   /* its PE timestamp: the build lifted */\n' % EXE_STAMP)
        f.write('#define SOTE_EXE_ENTRY 0x%08Xu   /* the OEP: an exe still wrapped has another */\n' % entry)


def exe_stamp(path):
    """The PE header's TimeDateStamp: which build of the game an exe is."""
    with open(path, 'rb') as f:
        head = f.read(4096)
    nt = int.from_bytes(head[0x3C:0x40], 'little')
    return int.from_bytes(head[nt + 8:nt + 12], 'little')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--catalog', default=CATALOG)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--roots', default='', help='comma-separated extra root VAs')
    ap.add_argument('--max', type=int, default=3000, help='closure size cap')
    ap.add_argument('--all', action='store_true', help='lift every function')
    ap.add_argument('--split', type=int, default=100,
                    help='functions per .c file: small enough that every builder core has one')
    ap.add_argument('--seeds', default=os.path.join(_HERE, 'work', 'run_seeds.json'),
                    help='seed_from_log.py JSON of unresolved targets from runs')
    args = ap.parse_args()
    # Lifted into a scratch folder, then only the files that changed replace
    # their old copies: an untouched file keeps its time, and a rebuild after
    # a one-line patch compiles one file, not all of them.
    final_out = args.out
    args.out = final_out + '.new'
    shutil.rmtree(args.out, ignore_errors=True)
    if not os.path.exists(args.catalog):
        sys.exit('no catalog at %s -- run disasm32.py first (README, Step by step)' % args.catalog)

    stamp = exe_stamp(args.exe)
    if stamp != EXE_STAMP:
        sys.exit('%s is not the build this project supports: its PE timestamp is 0x%08X, '
                 'the Steam release\'s is 0x%08X (docs/RECON.md).' % (args.exe, stamp, EXE_STAMP))
    info = analyze_pe(args.exe)
    if any(s.name == '.bind' for s in info.sections):
        sys.exit('%s still carries SteamStub (.bind): unwrap it first (tools/setup.ps1)' % args.exe)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] base=0x%08X code=0x%08X-0x%08X IAT=%d' % (info.image_base, cs, ce, len(iat)))

    cat = json.load(open(args.catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] catalog: %d functions inside .text' % len(byaddr))

    # Entries runs found that the catalog missed. Bound each by the next known
    # entry: one handed `ce` makes the extent walk descend the whole of .text.
    want = set()
    if os.path.exists(args.seeds):
        want = {e['address'] for e in json.load(open(args.seeds))
                if cs <= e['address'] < ce and e['address'] not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [], 'entry_kind': 'start'}
    print('[*] run seeds not in the catalog: %d injected' % len(want))

    entry = info.image_base + info.entry_point_rva
    roots = [entry] + [int(x, 0) for x in args.roots.split(',') if x.strip()]
    chosen = set(byaddr) if args.all else set(closure(byaddr, roots, args.max))
    print('[*] lifting %d of %d functions (%s)'
          % (len(chosen), len(byaddr), 'all' if args.all else 'closure, cap %d' % args.max))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(args.exe, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]

    # precise carry: adc/sbb take their carry from the flag state that set it
    # (the default breaks 64-bit adds; The Movies lost its menu video to it).
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):                 # a smaller lift must not leave stale chunks
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))
    entries, chunk, idx, errors, dirty = [], [], 0, 0, 0
    ordered = sorted(byaddr)
    starts = {a for a in ordered if byaddr[a].get('entry_kind') != 'alias'}
    splits = find_splits(md, code, cs, ce, starts)
    starts -= splits          # still dispatchable; just not a wall inside its parent
    print('[*] split entries (the middle of the function before them): %d' % len(splits))
    t0 = time.time()

    def flush(force=False):
        nonlocal chunk, idx
        if chunk and (force or len(chunk) >= args.split):
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []

    def lift_one(addr, name, end, reached):
        nonlocal errors
        if addr in HOOKS:
            chunk.append(('extern void sote_hook_%s(void);\nvoid %s(void) { sote_hook_%s(); }\n'
                          % (HOOKS[addr], name, HOOKS[addr]), addr, name))
            entries.append((addr, name))
            return
        try:
            lo = min(reached) if reached else addr   # a chunk can sit below the entry
            insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                              if end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                    else 'void %s(void) { }\n' % name)
            body = apply_patches(body, {va: c for va, c in PATCHES.items() if reached and va in reached})
        except Exception as e:                      # noqa: BLE001 -- counted, not hidden
            body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
            errors += 1
        chunk.append((body, addr, name))
        entries.append((addr, name))
        if len(chunk) >= args.split:
            flush()
            print('[*]   %d/%d (%d err)' % (len(entries), len(chosen), errors), flush=True)

    # Branch and call targets outside the catalog become entries, lifted in the
    # next round, until none are new: an unresolvable RECOMP_ITAIL otherwise.
    todo, added = sorted(chosen), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called)
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            lift_one(addr, name, end, reached)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t, 'end': ce, 'calls_to': [], 'entry_kind': 'start'}
            chosen.add(t)
        added += len(todo)
    print('[*] branch and call targets outside the catalog, added as entries: %d' % added)

    stubs = [a for a in ordered if a not in chosen]
    for a in stubs:
        name = 'sub_%08X' % a
        chunk.append(('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, a), a, name))
        entries.append((a, name))
        flush()
    flush(force=True)

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* Shadows.exe - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n'
                'void recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* Shadows.exe - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t sote_entry_va = 0x%08Xu;\n' % (len(entries), entry))
        # Function starts proper: not an alias or split label inside another
        # body. Only these may take --original --lifted's trap; a trap at a
        # label breaks the original code that falls through it.
        real = sorted(a for a in starts if a in chosen)
        f.write('\nconst uint32_t sote_func_starts[] = {\n')
        for i in range(0, len(real), 8):
            f.write('    ' + ', '.join('0x%08Xu' % a for a in real[i:i + 8]) + ',\n')
        f.write('};\nconst uint32_t sote_func_start_count = %d;\n' % len(real))

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out))
    stats = {'lifted': len(chosen), 'stubs': len(stubs), 'errors': errors,
             'no_terminator': dirty, 'files': idx, 'lines': lines}
    json.dump(stats, open(STATS, 'w'), indent=1)
    write_target_header(args.out, entry)
    changed = sync_dir(args.out, final_out)
    print('=' * 60)
    print('  lifted %d   not-lifted stubs %d   errors %d   no terminator %d'
          % (len(chosen), len(stubs), errors, dirty))
    print('  %s lines of C in %d files, %.1fs; %d changed' % (format(lines, ','), idx, time.time() - t0, changed))
    print('=' * 60)


if __name__ == '__main__':
    main()
