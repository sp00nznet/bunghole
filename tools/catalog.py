#!/usr/bin/env python3
"""Function catalogs for both guest modules: pcrecomp disasm32, once per image.

    py -3 tools/catalog.py            # -> work/functions_golf.json, work/functions_game.json

GOLF.EXE's 244 exports and the DLL's two are seeds automatically; nothing is
hand-seeded yet. The catalogs are derived from the game binaries, so they live
in work/ and are never committed.
"""
import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'tools')), 'tools')
GAME = os.path.join(_HERE, 'game', 'disc')

os.makedirs(os.path.join(_HERE, 'work'), exist_ok=True)     # gitignored, so absent in a fresh copy
for exe, tag in [('GOLF.EXE', 'golf'), ('00170001.DLL', 'game')]:
    out = os.path.join(_HERE, 'work', 'functions_%s.json' % tag)
    subprocess.run([sys.executable, os.path.join(TOOLS, 'disasm', 'disasm32.py'),
                    os.path.join(GAME, exe), '-o', out], check=True)
