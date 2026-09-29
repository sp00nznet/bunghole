#!/usr/bin/env python3
"""Bunghole in One conformance harness (REPO_RULES section 9).

Two fixed corpora, one pass/fail count each, compared against the committed
baseline in conformance.json; a regression fails the run:

* **Milestones**: one headless run of build/bunghole.exe with scripted input,
  scored by the lines the host prints at each stage. Boot: what the original
  does on every start, in this order. Play: through the menus to hole 1 and
  one putt, and the ball has to move: the host compares the screen after the
  putt with the screen 150 frames later, and a ball that rolls takes the
  camera with it.
* **Lift health**: from the generated tree. Lift errors, bodies with no
  terminator, and RECOMP_ITAIL targets that cannot resolve at run time (not in
  the dispatch table).

The game is not in the repo. Without game/disc and build/bunghole.exe this
skips with a message and exits 0, so it can sit in CI without the corpus.

    py -3 tools/conformance.py              # run, compare, print the table
    py -3 tools/conformance.py --update     # ...and accept the result as the baseline
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build', 'bunghole.exe')
GEN = os.path.join(ROOT, 'src', 'recomp', 'gen')
BASELINE = os.path.join(ROOT, 'conformance.json')

# (name, what the host prints when it is reached). Order is boot order. The
# intro videos run in real time, so 2,000 frames is about 70 s in: the main
# menu, where the game waits for the player.
MILESTONES = [
    ('both images mapped, every import bound', r'\[bind\] 0x10000000: .* 0 unresolved'),
    ('window created', r'\[headless\] CreateWindowExA\('),
    ('install paths read', r'\[install\] CD-ROM Path'),
    ('DirectInput mouse set up', r'\[input\] SetCooperativeLevel'),
    ('DirectDraw mode set', r'\[softdd\] SetDisplayMode\(640, 480, 16\)'),
    ('flip chain created', r'\[softdd\] primary 640x480 \+ back buffer'),
    ('first frame presented', r'\[softdd\] frame 1 presented'),
    ('game module loaded', r'\[module\] DllMain\(PROCESS_ATTACH\) -> 1'),
    ('game module entered', r'GetProcAddress\(0x10000000, "specmainModuleInit"\) -> 0x1'),
    ('100 frames (intro video)', r'\[softdd\] frame 100 presented'),
    ('1000 frames', r'\[softdd\] frame 1000 presented'),
    ('2000 frames (main menu)', r'\[softdd\] frame 2000 presented'),
    ('menu: Start the Game', r'\[input\] click at 318,198'),
    ('players: 1', r'\[input\] click at 200,237'),
    ('character: Beavis', r'\[input\] click at 192,168'),
    ('hole 1: putt', r'\[input\] drag at 320,238'),
    ('hole 1: the ball moved', r'\[play\] ([4-9]\d|100)% of the screen changed'),
    ('no fault through the putt', r'\[softdd\] 6800 frames: stopping'),
]

# Frames, not seconds: the intros run in real time and the game presents about
# 30 frames a second, so these land on the main menu, the player count, the
# character sheet and the tee of hole 1 (docs/img/ has each screen).
PLAY = ['--frames', '6800', '--click', '318,198@2050', '--click', '200,237@2400',
        '--click', '192,168@2900', '--drag', '320,238,-80,-50@5000']


def boot(seconds):
    try:
        p = subprocess.run([HOST, '--headless', '--run', '--watchdog', str(seconds)] + PLAY,
                           cwd=ROOT, capture_output=True, text=True, errors='replace',
                           timeout=seconds + 60)
        out, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out, code = (e.stdout or '') + (e.stderr or ''), 'timeout'
        out = out if isinstance(out, str) else out.decode(errors='replace')
    passed = [name for name, pat in MILESTONES if re.search(pat, out)]
    last = [l for l in out.splitlines() if l.startswith(('===', '[watchdog]', 'ICALL', 'ITAIL', '[play]'))]
    return passed, code, last[:3]


def lift_health():
    stats = json.load(open(os.path.join(ROOT, 'work', 'lift_stats.json')))
    disp = set(re.findall(r'\{ 0x([0-9A-F]{8})u,', open(os.path.join(GEN, 'recomp_dispatch.c')).read()))
    unresolved = sum(1 for fn in glob.glob(os.path.join(GEN, 'recomp_0*.c'))
                     for t in re.findall(r'RECOMP_ITAIL\(0x([0-9A-F]{8})u\)', open(fn).read())
                     if t not in disp)
    return {'lifted': stats['lifted'], 'errors': stats['errors'],
            'no_terminator': stats['no_terminator'], 'unresolved_itail': unresolved}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--update', action='store_true', help='accept this run as the baseline')
    ap.add_argument('--seconds', type=int, default=300, help='headless run limit')
    args = ap.parse_args()
    if not (os.path.exists(HOST) and os.path.isfile(os.path.join(ROOT, 'game', 'disc', 'GOLF.EXE'))):
        print('conformance: skipped -- needs game/disc (your CD) and build/bunghole.exe '
              '(README, Building from source)')
        return 0

    passed, code, last = boot(args.seconds)
    health = lift_health()
    now = {'milestones': len(passed), 'of': len(MILESTONES), **health}
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else None

    print('milestones: %d/%d  (exit %s)' % (len(passed), len(MILESTONES), code))
    for name, _ in MILESTONES:
        print('  [%s] %s' % ('x' if name in passed else ' ', name))
    for l in last:
        print('  stopped: ' + l)
    print('lift: %(lifted)d functions, %(errors)d errors, %(no_terminator)d with no '
          'terminator, %(unresolved_itail)d unresolvable ITAIL targets' % health)

    worse = []
    if base:
        if now['milestones'] < base['milestones']:
            worse.append('milestones %d -> %d' % (base['milestones'], now['milestones']))
        for k in ('errors', 'no_terminator', 'unresolved_itail'):
            if now[k] > base[k]:
                worse.append('%s %d -> %d' % (k, base[k], now[k]))
    if args.update or not base:
        json.dump(now, open(BASELINE, 'w'), indent=1)
        print('baseline written to conformance.json')
    if worse:
        print('REGRESSION: ' + '; '.join(worse))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
