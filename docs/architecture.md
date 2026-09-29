# Architecture

## The game

Bunghole in One is two PE images, both MSVC 5 (linker 5.10), no packer and no
copy protection:

| Image | Base | Code | What it is |
|---|---|---|---|
| `GOLF.EXE` | `0x00400000` | 157 KB | The Illusions engine: window, DirectDraw, DirectSound, DirectInput, DirectShow video, a script VM, and **244 named exports** (`artcntrlPlaceActor`, `scrmgrGetObjectArtThread`, `krndictAddID`...) |
| `00170001.DLL` | `0x10000000` | 110 KB | The game module. Imports **93** of the engine's exports back from `Golf.exe`, and exports two functions: `specmainModuleInit` / `specmainModuleUninit` |

The engine reads its install paths from
`HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\Golf.exe`
("Hard Drive Path", "CD-ROM Path"), loads the game module by resource ID
(`%08lx.DLL`, the "Special Code Module Loader") with `LoadLibraryA`, and enters
it through `GetProcAddress("specmainModuleInit")`. The module registers its
handlers with the engine (`krndictAddID`, a table of id/function-pointer pairs
in its `.data`), and from then on the engine calls into it through those
pointers.

## The pipeline

```
game\disc\GOLF.EXE ─┐                                     ┌─ src\recomp\gen\recomp_000N.c
                    ├─ tools\catalog.py ─ work\functions_*.json ─ run_lift.py ─┤  recomp_dispatch.c
game\disc\*.DLL ────┘   (pcrecomp disasm32)                (pcrecomp lift32) └─ recomp_funcs.h
                                                                   │
src\runtime\host.c, softdd.c + pcrecomp runtime\native32 ──────────┴─ build.cmd ─ build\bunghole.exe
```

1. **`tools/disc.py`** copies the CD's files into `game\disc`, from a drive, a
   folder or a BIN/CUE/ISO (pcrecomp's `iso_peek` walker).
2. **`tools/catalog.py`** runs pcrecomp `disasm32` once per image. The entry
   point and exports are seeds; the rest is recursive descent, prologues and a
   data scan.
3. **`run_lift.py`** lifts **every** catalogued function of both images with
   pcrecomp `generate` + `lift32` into one tree and one dispatch table. There
   is no closure limit: 1,944 functions is small. A direct branch to an address
   no catalog has becomes an entry of the next round, so every `RECOMP_ITAIL`
   resolves.
4. **`build.cmd`** compiles the generated C, the host and pcrecomp's
   `native32`, `image_loader` and `recomp_trace` into one 32-bit executable,
   linked at `0x60000000` so both guest bases are free.

## At run time

`build\bunghole.exe` is a 32-bit process holding both original images at their
own bases, mapped by hand, and running only lifted code (the guest `.text`
sections are mapped non-executable).

- **Guest → Windows.** Lifted `call [iat]` goes through the dispatch table
  first and then native32's bridge, which calls the real function with the
  guest's stack slots. 183 imports go to real Windows this way.
- **DLL → engine.** The DLL's 93 imports from `Golf.exe` are bound to the
  engine's *export VAs*, which are in the dispatch table, so they land on
  lifted engine code. That binding is native32's guest-module support
  ([toolkit.md](toolkit.md)).
- **Engine → DLL.** `LoadLibraryA` and `GetProcAddress` for the module are
  shimmed to the already-mapped image; loading runs its lifted `DllMain` once.
- **Windows → guest.** Window procedures, DirectShow and DirectDraw enumeration
  callbacks fault on the non-executable guest code and native32's handler
  enters the lifted function.
- **Display.** `DirectDrawCreate` returns softdd, a software DirectDraw in the
  host; the real display mode is never changed. [host.md](host.md).

Everything the host changes about the game's view of the world is in
`src/runtime/host.c`, and why is in [host.md](host.md).
