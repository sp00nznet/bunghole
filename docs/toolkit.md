# The toolkit changes this game needed

Everything generic went to [pcrecomp](https://github.com/sp00nznet/pcrecomp)
as its own PR (REPO_RULES section 11). Until they merge, build against a
checkout that has all of them, on top of the open PRs they need:

| PR | What | How it showed up here |
|---|---|---|
| [#7](https://github.com/sp00nznet/pcrecomp/pull/7) | `runtime/native32`: the 32-bit host (not mine; The Movies' and gunman's) | the host is built on it |
| [#16](https://github.com/sp00nznet/pcrecomp/pull/16) | native32: the guest stack leaves room for the bridge's 24-slot copy | `native32_selftest` crashed on 9 of 20 runs before anything of this game ran |
| [#17](https://github.com/sp00nznet/pcrecomp/pull/17) | native32: guest modules bind to each other's exports | the DLL's 93 imports from `Golf.exe` had nowhere to point |
| [#18](https://github.com/sp00nznet/pcrecomp/pull/18) | disasm32 seeds exports | `specmainModuleInit` was not in the catalog: `ICALL: unresolved VA 0x1000F6A0` |
| [#19](https://github.com/sp00nznet/pcrecomp/pull/19) | disasm32: a callee is never dropped for a pointer-shaped guess | the `jmp [DirectInputCreateA]` thunk was dropped: `ICALL: unresolved VA 0x0041C0D8` |
| [#20](https://github.com/sp00nznet/pcrecomp/pull/20) | recomp32: flags cross calls and tail jumps | the putt counted but the ball never moved |

`Setup.cmd` checks the toolkit for each of these and names the ones missing.

## The frozen ball (#20)

The one that took longest, and the one most likely to bite another title.

The aim line drew, the stroke counted (`SCORE 1/3`), Beavis swung, and the
ball sat on the tee. Nothing faulted. A `--firsthit` trace over the game DLL
around the putt showed the stroke going through the CRT's `atan2`, `sin` and
`cos`. `--call` ran the lifted `atan2` on known inputs and it was right, so
the math itself was fine. The bug was in how `cos` reaches its body:

```
_cos:   lea  edx, [esp+4]
        call _fload          ; sets ZF for a special operand
_CIcos: push edx             ; a separate entry: compiled code calls it
        fnstcw [esp]
        je   special         ; tests _fload's ZF
```

`_CIcos` is catalogued on its own, so `_cos` reaches it with a tail
transfer, and a lifted function starts with its flags at `FK_NONE`. The `je`
read nothing, and `cos` took its special-operand path. The fix gives flags
the same hand-off at calls, tails and entries that `ret` already had.

The conformance harness now checks exactly this. After the scripted putt the
host compares the screen with the screen 150 frames later: a rolling ball
takes the camera with it, and a frozen one changes only the cursor and a
portrait.

## The thunk a table swallowed (#19)

`push 0x41c0c0` hands `SetDataFormat` a `DIDATAFORMAT` that the compiler put
in `.text`. disasm32's immediate harvest takes any code-range immediate for a
callback, decoded the table as code (`add bh, bh` straddling `0x0041C0D8`),
and dropped the real entry it overlapped: the `jmp [DirectInputCreateA]`
thunk. A wider version of the fix ("a guess never evicts anything") moved
dozens of entries each way on gunman, Hellbender, Recoil and POD with no way
to tell which side was right, so what went in is only the case with no
doubt: a guess never evicts the target of a direct call. Against IDA it
improved POD and Hellbender a little.

## Lifting both images into one table

Not a toolkit change, but the reason several of the above mattered. The
engine calls into the game module through function pointers the module
registered (`krndictAddID`), and the module calls the engine through its
IAT. With both images in one dispatch table and the IAT bound to the
engine's export VAs (#17), both directions are plain `RECOMP_ICALL`s that
land on lifted code. Nothing native from either original executable ever
runs.
