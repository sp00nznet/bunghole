# Roadmap

## Next

- **Play a round.** Hole 1 and one putt work, headless. Next: sink it, the
  scorecard, the next holes, 2-4 players, the traps. Each is a scripted run
  (`--click`, `--drag`) and a conformance milestone once it works.
- **See the windowed path.** It runs to 1,900 frames on an offstage virtual
  monitor, but offstage's capture came back empty on this machine, so there is
  no picture of the window yet.
- **Sound.** DirectSound is created on the real device and the music is WAV
  streams; nothing has been listened to. A headless run should mute (or record)
  it.
- **Play is timed in frames.** The scripted clicks assume the intros take
  2,000 frames. A faster or slower machine that presents at a different rate
  would click early or late; a "wait for this screen" step (a hash of a
  screen region) would remove the guess.
- **Merge the toolkit PRs** (#16 to #20) so a plain pcrecomp clone builds this.

## Deferred

- **Foreground-only mouse.** The mouse is non-exclusive *background*, so the
  game sees motion while unfocused. Foreground plus a re-`Acquire` on
  activation when it matters.
- **Multiple back buffers.** softdd makes one whatever the game asks for; the
  game asks for one.
- **`Clipper::GetClipList`, `EnumSurfaces`, `DuplicateSurface`, overlays.**
  Unimplemented; they log once if anything calls them.
- **Save games** (`SAVEGAME\`) are written next to the disc files in
  `game\disc`, which is where the original expected its install folder to be.

## Out of scope

- Redistributing anything from the game, including the recompiled C.
- The installer (`SETUP.EXE`, InstallShield 3) and the DirectX 5 / DirectMedia
  redistributables on the disc: the host needs neither.
