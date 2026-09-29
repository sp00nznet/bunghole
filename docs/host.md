# The host

`src/runtime/host.c` and `src/runtime/softdd.c`. The mechanism (the native
bridge, callbacks, the machine lock, guest-module binding) is pcrecomp's
`runtime/native32`; what is here is what this game needs on top of it. Each
item below was found by running the game and reading where it stopped.

## Shims: the game's view of the world

| Import | Why |
|---|---|
| `GetModuleHandleA(NULL)`, `GetModuleFileNameA`, `GetCommandLineA` | The guest is `GOLF.EXE` in the game folder, not the host: its hInstance must be `0x00400000` and its file name must be in `game\disc` |
| `LoadLibraryA`, `GetProcAddress`, `FreeLibrary` | `00170001.DLL` is already mapped and bound as a guest. Loading it runs its lifted `DllMain(PROCESS_ATTACH)` once; its exports are guest VAs (`native32_export`) |
| `RegOpenKeyExA`, `RegQueryValueExA`, `RegCloseKey` | `App Paths\Golf.exe` answers "Hard Drive Path" and "CD-ROM Path" with the game folder. Nothing is installed, and nothing touches the real registry (HKLM would need admin) |
| `DirectInputCreateA` | DirectInput checks its HINSTANCE against the loader's module list, and the guest image was mapped by hand: `0x00400000` is `E_INVALIDARG` (`0x80070057`). The host's own handle stands in |
| `IDirectInputDevice::SetCooperativeLevel` (vtable hook) | The mouse was `DISCL_EXCLUSIVE \| DISCL_FOREGROUND`. At the game's `Acquire` its window is not in the foreground yet (hidden, or still a 1x1 popup), so `Acquire` failed `E_ACCESSDENIED` (`0x80070005`) and the game quit without a word. Non-exclusive background instead; the game draws its own cursor |
| `DirectDrawCreate` | softdd, below |
| `ExitProcess` | closes the `--record` pipe first, so the mp4 is complete |

`--headless` adds `MessageBoxA` (to stderr), `CreateWindowExA` (without
`WS_VISIBLE`) and `ShowWindow` (a no-op).

The first headless run, before any of the DirectInput and DirectDraw work,
ended like this; the message box is the game's own:

```
[native] DINPUT.dll!DirectInputCreateA    (00400000 00000300 0043CB40 00000000) from sub_0041C0D8 -> 80070057
...
[messagebox] Bunghole In One: Unable to initialize DirectDraw.
Try re-installing the game, choosing YES when you are prompted
to install or update DirectX.
```

## softdd: a software DirectDraw

The engine asks for exclusive fullscreen at 640x480x16 with a flip chain:

```
[softdd] SetCooperativeLevel(01350AAA, 0x51): windowed instead
[softdd] SetDisplayMode(640, 480, 16): the desktop is left alone
[softdd] primary 640x480 + back buffer
```

Real DirectDraw would switch the desktop to 640x480 even for a hidden window.
That must never happen to a machine driven over RDP, and modern Windows
emulates 16-bit exclusive modes badly anyway. So the game never gets the real
DirectDraw:

- **Every surface is a top-down RGB565 DIB section in host memory.** The host
  is 32-bit and shares the guest's address space, so the pointer `Lock`
  returns is directly usable by lifted code; and a DIB section can be selected
  into a DC, so `GetDC` works for the engine's GDI text (`CreateFontA`,
  `TextOutA`).
- **`Flip` swaps the two buffers' memory, not the objects**: after a flip the
  game keeps drawing through the same back-buffer pointer, which must now hold
  the old front. Then the front buffer is presented.
- **Present** is `StretchDIBits` into the game's window (restyled from a popup
  to a captioned window of 640x480 times `--scale`), and/or raw `rgb565le`
  frames piped to `ffmpeg` for `--record`. The recording runs at a fixed 30 fps
  against the wall clock (slow frames held, fast ones dropped), so the video
  plays at the game's real speed.
- **The vtables are the SDK's own C declarations** (`CINTERFACE`), so every
  method has the signature and the stdcall purge the game was compiled
  against. A method nothing has needed yet logs `[softdd] unimplemented: ...`
  once and fails.
- **The video path is native.** The intros are DirectShow (`amstream`'s
  DirectDraw stream), which is real Windows code handed softdd's objects. It
  asks for `IDirectDraw2` and `IDirectDrawSurface2/3`, so both objects carry a
  second vtable pointer for those, forwarding to the same bodies. The two
  interfaces it probes and is refused (`B502D1BD...`, `BEBE595D...`, the
  multimedia-stream interfaces) are the correct answer: it is asking whether
  the object is a stream, and it is not.

## Scripted input

`--click X,Y@F` and `--drag X,Y,DX,DY@F` drive the mouse from the command
line, inside the same `GetDeviceData` hook. The game reads the mouse as
**buffered relative motion** and keeps its own cursor, so a gesture starts with
a huge negative move (the cursor clamps at the top-left) and then a move to
(X,Y), and each step is appended as the `DIDEVICEOBJECTDATA` records a real
mouse would have queued. The steps run on **presented frames**, not polls: the
game reads the mouse several times a frame, and the first version, one step
per poll, was over before the game had drawn once. The putt pressed, dragged
and released inside a single frame, and nothing happened.

A drag holds the button for 20 frames of motion. The putt in the conformance
run is `--drag 320,238,-80,-50@5000`: the ball on the tee of hole 1 is at
(320, 238), and a few pixels off it the press does not pick the ball up.

After each drag the host compares the screen with the screen 150 frames later
and prints `[play] N% of the screen changed`. A rolling ball takes the camera
with it. Before pcrecomp#20 the recording showed only the cursor and Beavis's
portrait changing after the putt (the check itself came after the fix, so
there is no "before" figure).

## Headless and recording

`--headless` never shows a window and never touches the display; with
`--record` it is the proof of a run. Over RDP it is the only mode to use
(REPO_RULES section 13). At the console, a windowed run belongs on an
[offstage](https://github.com/sp00nznet/offstage) virtual monitor:

```
offstage run -- build\bunghole.exe --run --scale 2
```
