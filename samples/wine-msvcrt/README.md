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

## Result 1: the interpreter costs about 15x

20,000,000 iterations of the same integer kernel, one M-series Mac, five runs
each, medians. Absolute times move with how busy the machine is, so the ratio
within a run is the number to read; translated and interpreted always run in
the same process, one after the other.

| Path | Time | Result |
|---|---:|---|
| Translated (in the image) | 34 ms | 3576949941 |
| Interpreted (copied to the heap), as `main` had it | 940 ms (18x) | 3576949941 |
| Interpreted, with the two changes below | 510 ms (15x) | 3576949941 |
| Native arm64 C, same kernel, for scale | 58 ms | 3576949941 |

All of them agree on the value. Translated code matches native C here.

Two changes took the interpreter from 18x to about 15x:

- **Branch targets resolved when the routine is decoded** (940 -> 710 ms).
  Taking a branch looked its target address up in a hash map, twice per
  iteration of an ordinary loop; each branch now carries its target's index.
- **Lazy flags** (710 -> ~510 ms, and only about 4% of it on this kernel).
  Flags are kept as the operands that produced them and computed when a Jcc,
  a CALL or a RET actually reads them. This kernel reads a flag after nearly
  every arithmetic instruction, which is the worst case for the change; code
  that computes more between its branches gains more.

What is left is dispatch and operand handling: about 180 million guest
instructions in 510 ms is roughly 10 cycles per instruction. Specialising
common instruction shapes at decode time and threading the dispatch would
plausibly halve that again, to somewhere around 7x. Interpretation will not
approach translated speed; that is what the translator is for.

Read it as an upper bound on what an interpreter tier costs, not a lower one:
the kernel is nothing but arithmetic and branches. Real game code spends much of its time in the kit's own native shims
(graphics, sound, files), which cost the same either way, so a whole-game
slowdown would be smaller than 18x. Nothing here has been measured on a device.

The interpreter needed one addition to run the kernel at all: `TEST EAX,imm32`
and `TEST r/m32,imm32` (opcodes `A9` and `F7 /0`), which it did not decode.
Its own tests came over too: `interp_tests` (23 checks) and the Unicorn
differential test, extended to cover the new `TEST` forms; 300 random routines
match Unicorn's registers, flags and memory.

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

## Kit changes this sample needed

- `runtime/interp.{h,cpp}`: the interpreter from the `nfsmw` branch, brought
  onto this branch and reached from `recomp_unknown_call` for heap addresses
  (`main` alone follows only short thunks, in `runtime/thunks.cpp`), plus the
  `TEST` immediate forms it was missing, resolved branch targets and lazy
  flags. `runtime/tests/interp_tests.cpp` and the Unicorn differential test
  came with it.
- `tools/build.py`: a build's `--allow-table-gaps`/`--allow-unmodelled`
  acceptances now reach auxiliary modules, which are translated from listings
  with the same gaps.
- `cmake/Translate.cmake`: an auxiliary module's translation sees the game's
  `[translate] overrides` header, so a native replacement can stand in for one
  of its functions.
