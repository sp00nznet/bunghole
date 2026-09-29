#!/usr/bin/env python3
"""Bunghole in One lift driver: both guest modules -> src/recomp/gen/.

The game is two PE images and both are lifted, whole, into one dispatch table:

* GOLF.EXE      the engine (0x00400000): window, DirectDraw/DirectSound/
                DirectInput, the script VM, and 244 exported engine calls.
* 00170001.DLL  the game module (0x10000000), loaded by the engine with
                LoadLibrary and entered through `specmainModuleInit`. It
                imports 93 of the engine's exports back from `Golf.exe`.

Together they are ~1,850 functions, so there is no closure limit: every
catalogued function is lifted (The Movies needs one; this does not).
Calls between the modules go through the DLL's IAT, which the host binds to
the engine's export VAs (pcrecomp native32_bind, guest modules), so they land
on lifted code through the dispatch table like any indirect call.

    py -3 tools/catalog.py      # work/functions_golf.json, work/functions_game.json
    py -3 run_lift.py

docs/architecture.md has the whole pipeline.
"""
import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks the toolkit checkout, the same knob CMakeLists.txt has: the
# lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'tools')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

GAME = os.path.join(_HERE, 'game', 'disc')
MODULES = [('GOLF.EXE', 'golf'), ('00170001.DLL', 'game')]
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')


def lift_module(path, catalog, md, entries, out, split, stats):
    """Lift every catalogued function of one image, appending to `entries`."""
    info = analyze_pe(path)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    cat = json.load(open(catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] %s: base=0x%08X code=0x%08X-0x%08X IAT=%d catalog=%d'
          % (os.path.basename(path), info.image_base, cs, ce, len(iat), len(byaddr)))

    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(path, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
    # Known targets become RECOMP_CALL; anything else a decoded `call` names
    # becomes RECOMP_ICALL, which reports at run time instead of failing the build.
    lifter = Lifter(iat_map=iat, lifted=set(byaddr))
    starts = {a for a in byaddr if byaddr[a].get('entry_kind') != 'alias'}
    starts -= find_splits(md, code, cs, ce, starts)

    chunk, errors, dirty = [], 0, 0

    # A direct branch that leaves a body to an address nothing catalogued is a
    # tail call the catalog missed or a jump into a neighbour's shared code.
    # Either way the target has to be dispatchable, so each round's outside
    # targets become entries of the next (The Movies' run_lift, same reason).
    todo, added = sorted(byaddr), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind = set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind)
            outside |= {t for t in behind if t not in reached}
            dirty += not clean
            try:
                lo = min(reached) if reached else addr   # a chunk can sit below the entry
                insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                                  if end > addr else ([], None))
                if leaders is not None:
                    leaders.add(addr)
                body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                        else 'void %s(void) { }\n' % name)
            except Exception as e:                  # noqa: BLE001 -- counted, not hidden
                body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
                errors += 1
            chunk.append((body, addr, name))
            entries.append((addr, name))
            if len(chunk) >= split:
                write_chunk(out, stats['files'], chunk)
                stats['files'] += 1
                chunk = []
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t}
        added += len(todo)
    if chunk:
        write_chunk(out, stats['files'], chunk)
        stats['files'] += 1
    print('[*]   lifted %d (%d outside branch targets added), %d errors, %d without a terminator'
          % (len(byaddr), added, errors, dirty))
    stats['lifted'] += len(byaddr)
    stats['errors'] += errors
    stats['no_terminator'] += dirty
    return info.image_base + info.entry_point_rva


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=GAME, help='folder holding GOLF.EXE and 00170001.DLL')
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    args = ap.parse_args()

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):                 # a smaller lift must not leave stale chunks
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))

    t0 = time.time()
    entries, oeps = [], []
    stats = {'lifted': 0, 'errors': 0, 'no_terminator': 0, 'files': 0}
    for exe, tag in MODULES:
        catalog = os.path.join(_HERE, 'work', 'functions_%s.json' % tag)
        if not os.path.exists(catalog):
            sys.exit('no catalog at %s -- run tools/catalog.py first (README, Step by step)' % catalog)
        oeps.append(lift_module(os.path.join(args.game, exe), catalog, md, entries, args.out,
                                args.split, stats))

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* Bunghole in One - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* Bunghole in One - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t bh_exe_entry_va = 0x%08Xu;\n'
                'const uint32_t bh_dll_entry_va = 0x%08Xu;\n' % (len(entries), oeps[0], oeps[1]))

    stats['lines'] = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8',
                                             errors='replace'))
                         for fn in os.listdir(args.out))
    json.dump(stats, open(STATS, 'w'), indent=1)
    print('=' * 60)
    print('  lifted %(lifted)d   errors %(errors)d   no terminator %(no_terminator)d' % stats)
    print('  %s lines of C in %d files, %.1fs' % (format(stats['lines'], ','), stats['files'],
                                                 time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
