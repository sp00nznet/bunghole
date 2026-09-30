# Security

## What this touches on your machine

- **Reads** your copy of the game: a CD drive, a folder, or a BIN/CUE/ISO
  (`tools/disc.py`), and copies its files into `game\disc` in this folder.
- **Writes** only inside this folder: `game\`, `work\`, `src\recomp\gen\`,
  `build\`, `setup.log`, and the `Bunghole in One` shortcut. The game thinks
  `game\disc` is its install folder, so anything it writes (saves) lands there.
- **Never writes the registry.** The game reads its install paths from
  `HKLM\...\App Paths\Golf.exe`; the host answers those reads itself, from
  memory, and passes every other registry call through unchanged.
- **Never changes the display mode.** DirectDraw is a software implementation
  inside the host (`src/runtime/softdd.c`).
- **The network**: `Setup.cmd` can download Python (winget), `pefile` and
  `capstone` (pip), the Visual Studio Build Tools (winget) and pcrecomp (git),
  and only after asking. The recompiled game itself makes no network calls.

The recompiled game runs in an ordinary user process with the same rights as
you. Its original code is mapped non-executable and never runs; only the C
generated from it does.

## Reporting a problem

Open an issue, or for anything you would rather not post publicly, use GitHub's
private vulnerability reporting on this repository.
