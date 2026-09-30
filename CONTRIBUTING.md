# Contributing

This is a recompilation project, which changes what a useful contribution looks
like. Three rules matter more than the rest.

## No game files, ever

Nothing from the Bunghole in One disc goes in the repository, in any form: no
executable, DLL, video, sound, sprite or data file, and nothing regenerated
from one. That means no lifted source, no disassembly listing, no function
catalog, no reconstructed header, and no test file with a lifted function in it.

The tool ships; the disc does not. Everyone brings their own copy and points
the tools at it. `.gitignore` covers `game/`, `work/` and `src/recomp/gen/`; if
something slips past it, that is a bug worth reporting on its own.

Screenshots and recordings of the game running are fine, and are the point of
the README.

## Where your code comes from

Contributions have to be your own work or under a licence compatible with MIT.
The live risk is a fix ported from a GPL or LGPL project, which would relicense
it by accident and is very hard to untangle later. The ones most likely to be
open next to this are **Wine** (LGPL: its DirectDraw and DirectInput), **DXVK**
and **dgVoodoo**-style wrappers (check each licence), and the pcrecomp
siblings' xemu-derived code (LGPL). Reading them to understand a behaviour is
fine; copying code from them is not. If something in your PR came from
somewhere, say where.

## Claims are measured, not assumed

`tools/conformance.py` is the scoreboard: a change that claims to fix or reach
something should add a milestone for it, and must not lose one. A change to the
lifter, the disassembler or the native32 runtime belongs in
[pcrecomp](https://github.com/sp00nznet/pcrecomp), with a test there
(`difftest`, a selftest); this repo takes the game-specific part.

## The usual

- Imperative commit subjects; the body explains *why* when it is not obvious.
- Community pull requests are merged with a merge commit, never squashed or
  rebased, so your commits stay yours. Anything the maintainer adds goes in
  separate commits on top.
- Comments explain the reasoning, not the syntax: a comment recording how
  something was found out is worth more than one restating the line under it.
- AI-assisted contributions are welcome, provided a human understood and
  verified the change: ran it, and can say what it does.
