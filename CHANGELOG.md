# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- The whole pipeline, from your CD to a running executable: `tools/disc.py`
  (drive, folder, BIN/CUE or ISO into `game\disc`), `tools/catalog.py` (pcrecomp
  `disasm32` over both images), `run_lift.py` (every function of `GOLF.EXE` and
  `00170001.DLL` into one tree: 1,944 functions, 0 lift errors) and
  `build.cmd` (32-bit MSVC host on pcrecomp `native32`).
- `src/runtime/host.c`: both guest images mapped and bound together, the game
  DLL loaded through its lifted `DllMain`, the install registry answered with
  the game folder, DirectInput given a real HINSTANCE and a non-exclusive
  mouse ([docs/host.md](docs/host.md)).
- `src/runtime/softdd.c`: a software DirectDraw (RGB565 DIB sections, flip by
  buffer swap, `IDirectDraw2` and `IDirectDrawSurface2/3` for DirectShow). The
  game never changes the real display mode.
- `--headless`, `--record out.mp4`, `--frames N`, `--scale N`, `--watchdog S`,
  `--native-trace`.
- `tools/conformance.py`: 12 boot milestones from a headless run, plus lift
  health, against `conformance.json`. First baseline: 12/12, 0 lift errors,
  0 unresolvable tail calls.
- `Setup.cmd`: the one-click route, running the same steps as the README.
- Boots through the intro videos (MTV, GT Interactive, the disclaimer, the
  title) to the main menu, recompiled.
- Plays hole 1: the player count, character select, the fly-through, the aim
  line and a putt whose ball rolls. `--click X,Y@F` and `--drag X,Y,DX,DY@F`
  script the mouse for headless runs (the game reads it buffered, several
  times a frame, so the script runs on presented frames). `--call VA ...`
  runs one lifted function on given arguments.
- Conformance: 18 milestones, including a play phase whose last check is that
  the ball moved (the screen 150 frames after the putt against the screen
  just after it).

### Toolkit
- Needs pcrecomp#7 (native32) and #16 to #20
  ([docs/toolkit.md](docs/toolkit.md)): native32's stack headroom (#16) and
  guest modules (#17), disasm32 export seeds (#18) and callees over guesses
  (#19), and flags across tail jumps (#20, the frozen ball).
