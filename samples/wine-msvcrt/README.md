# Wine msvcrt sample

Two experiments on a game that does not exist. `src/sample.c` is a 32-bit
Windows program of about 150 lines, built without a C runtime, that the kit
translates like any game:

1. **How much slower is the interpreter than translated code?** `bench()` runs
   once as translated code and once from a copy of its own bytes in the heap,
   which the runtime can only reach through `runtime/interp.cpp`.
2. **Can the kit run one of Wine's own i386 DLLs?** The program loads Wine's
   `msvcrt.dll` with `LoadLibraryA` and calls `strlen`, `atoi`, `qsort` and
   `sprintf` through `GetProcAddress`. The kit maps and translates it with the
   existing auxiliary-module path (`[modules.aux.msvcrt]`), the same one Siege
   of Avalon uses for `Dfx_p6s.dll`.

Nothing here is a game, and nothing here ships: the sample is a measurement.

## Running it

Wine's `msvcrt.dll` is taken from a local CrossOver 26.3 installation and is
not redistributed; `original/` and `analysis/` are ignored.

```sh
tools/prepare.py --cc <llvm-mingw>/bin/i686-w64-mingw32-clang \
    --wine-dlls /Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine/i386-windows \
    --ghidra-home <ghidra_12.1.3_PUBLIC> --java-home <jdk>
# from the kit
tools/build.py --game-dir <here> --regenerate --target headless \
    --allow-table-gaps "msvcrt strftime jump tables name 1003ccde, a block Ghidra's listing omits" \
    --allow-unmodelled "SSE2 in Wine msvcrt; reaching one is still fatal, and the point is how far startup gets"
RECOMP_EXE=<here>/original/sample.exe build/recomp/pop_headless
```

## Result 1: the interpreter costs about 10x

20,000,000 iterations of one integer kernel, run translated and then from a
copy of its own bytes in the heap, in the same process. Every row below is
five runs on an idle M-series Mac; translated code measured 27-29 ms in all of
them, and native arm64 C compiled from the same source takes 58 ms, so
translated code is native speed here.

| Interpreter | Interpreted | vs translated |
|---|---:|---:|
| As the `nfsmw` branch had it | 505 ms | 18x |
| Resolved branch targets, lazy flags | 455 ms | 16x |
| Operand fast paths, pointer walk, one case per operation | 286 ms | 10x |

(An earlier session measured 940 ms and 510 ms for the first two rows while
the machine was busy building. The table above replaces those numbers; the
machine's state moved them by nearly 2x, which is why every row here was
re-measured together, idle, against the same translated run.)

What each change did:

- **Branch targets resolved when the routine is decoded.** Taking a branch
  looked its target address up in a hash map, twice per iteration of this
  loop; each branch now carries its target's index.
- **Lazy flags.** Flags are kept as the operands that produced them and
  computed when a Jcc, a CALL or a RET reads them. Worth little on this
  kernel, which reads a flag after nearly every instruction - the worst case
  for the change.
- **Operand fast paths.** A register or an immediate operand is now an inline
  array read; only memory, which has an address to form and bounds-check,
  reaches an out-of-line function.
- **Pointer walk.** The decoded stream is walked as a pointer, with no index
  arithmetic and no bound tested per instruction: `build()` already
  guarantees every branch target is an instruction of the routine.
- **One case per operation.** Lazy flags had introduced a second switch inside
  the arithmetic case; each operation now computes its result directly.

**Fusing a compare with the branch that reads it made no measurable
difference** (290 ms against 293 ms) and was dropped rather than kept on the
theory that it should help. It is still the obvious thing to try on code with
more compare-and-branch pairs than this.

About 180 million guest instructions in 286 ms is roughly 6 cycles each.
What is left is dispatch; threading it (computed goto) is the next
experiment, and is untested here. Interpretation will not approach translated
speed - that is what the translator is for - so the useful target for an
interpreter tier is "fast enough to boot an unknown game and report what it
needs", not "fast enough to play".

The interpreter needed one addition to run the kernel at all: `TEST EAX,imm32`
and `TEST r/m32,imm32` (opcodes `A9` and `F7 /0`), which it did not decode.
Its own tests came over too: `interp_tests` (23 checks) and the Unicorn
differential test, extended to cover the new `TEST` forms; 300 random routines
match Unicorn's registers, flags and memory after every change above.

## Result 2: Wine's i386 DLLs need SSE2 from the translator

The pipeline itself works. Wine's `msvcrt.dll` (639 KB, 2,686 listed
functions) exported Ghidra listings, translated whole (2,696 functions,
15 chunks), mapped at its own base `0x10000000`, and `LoadLibraryA` ran its
entry point — the DLL's own startup executed as translated code.

It does not get as far as returning from that startup, for one reason:

- **SSE2 is ordinary code generation in this build, not a math-library
  detail.** `memset` stores through `XMM` registers from its second
  instruction, and the locale initialisation uses `MOVD`/`PUNPCKLDQ` merely to
  write two dwords. Nothing checks CPUID first, so the kit's "no SSE"
  `recomp_cpuid` does not steer around it, and the translator's SSE trap ends
  the process. 154 of 2,686 functions (5.7%) contain SSE.
  A native override for `memset` (`native/msvcrt_native.h`) gets startup past
  the first one and straight into the next, which is why per-function
  overrides are not the answer here: **the translator needs SSE2**.

Two smaller findings, both of which would have to be fixed anyway:

- **Missing imports.** Of msvcrt's imports, the kit implements 88 of 129
  `kernel32` functions and none of its 13 `ntdll` ones. The run named the ones
  it actually reached: `__wine_dbg_header`, `__wine_dbg_output` and
  `InitializeCriticalSectionEx`. The Wine debug-channel functions also have no
  known argument count, so the runtime cannot correct the stack after them.
- **A listing gap.** Three `strftime` jump tables name `1003ccde`, a block
  Ghidra does not list. `[translate] entry_points` does not adopt it, because
  it is a block inside a function rather than a function; `--allow-table-gaps`
  accepts it instead.

## What this says about basing the kit on Wine

Mapping, translating and running a Wine DLL beside a game works today, with no
new mechanism. What stands between that and using Wine's DLLs in practice is
the translator's instruction coverage, not the loader or the module design.
SSE2 is the first thing to add.

## Open: a crash this sample found in translated code

Rewriting `experiment_interp` to call each path three times and keep the
fastest (a `fastest(bench_fn, n, rounds, &result)` helper, which the compiler
inlines into one loop with an indirect call) ends the host process with a
stack overflow - `EXC_BAD_ACCESS` writing below the host stack, in
`body_00401070`, entered again and again with `entry_ = 0x40106b`. The run
gets as far as `VirtualAlloc`; a breakpoint on `recomp_unknown_call` is never
hit, so the interpreter is not involved and this is translated code
re-entering itself. It reproduces with the interpreter as committed and with
the specialised one, and not at all with the sample as it stands, whose
`experiment_interp` calls each path once.

Worth chasing: the shape - an indirect call in a loop, returning into an
address the translator recovered as an alternate entry - is ordinary
compiler output, so a game will produce it.

## Kit changes this sample needed

- `runtime/interp.{h,cpp}`: the interpreter from the `nfsmw` branch, brought
  onto this branch and reached from `recomp_unknown_call` for heap addresses
  (`main` alone follows only short thunks, in `runtime/thunks.cpp`), plus the
  `TEST` immediate forms it was missing, and the five changes in Result 1.
  `runtime/tests/interp_tests.cpp` and the Unicorn differential test came
  with it.
- `tools/build.py`: a build's `--allow-table-gaps`/`--allow-unmodelled`
  acceptances now reach auxiliary modules, which are translated from listings
  with the same gaps.
- `cmake/Translate.cmake`: an auxiliary module's translation sees the game's
  `[translate] overrides` header, so a native replacement can stand in for one
  of its functions.
