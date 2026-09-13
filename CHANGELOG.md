# Changelog

## Unreleased

- Split on-screen keypad for touch: two 8x5 halves in the bottom corners, three
  sizes, Shift/Ctrl/Alt that hold, latch or lock, HIDE/KEYS tabs, settings on the
  F10 page persisted per game; replaces the eight-key strip. `RECOMP_KEYPAD=1` forces
  it on for a desktop check.
- Switches: every environment switch the kit reads is `RECOMP_<NAME>`, read
  through one function, `recomp_env` in `platform/os.h`. The spellings from the
  kit's origin as one game's port - `POPM_<NAME>`, `POP_RECOMP_<NAME>`,
  `POP_HOST_<NAME>`, `POP_SMOKE_<NAME>`, `POP_GPU_<NAME>`, `POP_VULKAN_<NAME>`,
  `POP_REPLACE_<NAME>`, `POP_PLATFORM_TEST`, `POP_TEST_DIR`, `POP_CC`,
  `POP_BUILD_ROOT` and the rest - are gone, not aliased: `POPM_PIN_CLOCK` and
  `POP_RECOMP_PIN_CLOCK` are both `RECOMP_PIN_CLOCK`, `POPM_TEST_DIR` (mods
  tests) is `RECOMP_TEST_DIR`, `POP_TEST_DIR` (platform tests) is
  `RECOMP_PLATFORM_TEST_DIR`, and the tools' `POP_BUILD_ROOT` is
  `RECOMP_BUILD_ROOT`. `tools/test.py` and the mode probe drop a caller's
  `RECOMP_*` switches from a child's environment while keeping the names that
  locate the game and toolchain (`mode_probe.without_switches`). Smoke scripts,
  test fixtures and docs use the new names; a game repository's scripts must
  too. Still to move: the CMake variables (`POP_ROOT`, `POP_BUILD_ROOT`,
  `POP_OUT`, `POP_WARN_STRICT`, ...) and the `POPM_TESTING` compile macro,
  which share the old prefix but are not read from the environment.
- Translator: instruction forms a Visual C++ 6 executable uses that the first
  corpus did not: `LOOP`, `INT3`, `CLC`/`STC`, `PUSHF`/`POPF`, 16-bit `PUSH`/`POP`,
  word- and dword-width `CMPS`/`SCAS` with every REP prefix, `FPTAN`,
  `FSAVE`/`FNSAVE`, `FRSTOR`, `FINIT`/`FNINIT`, and segment registers as a `MOV`
  source (the flat selectors) or destination (dropped; a CS load is `#UD`).
  `x86.h` gains `x87_finit`, `x87_fnsave`, `x87_frstor` and the wider string
  compares. `tools/recomp/tests/test_translate_insns.py` checks each form against
  Unicorn on synthetic listings without a game, and runs in `tools/test.py`.
- Translator: jump tables bounded the way that compiler's hand-written `memcpy`
  bounds them - a low-bit `AND` mask whose unreachable slot holds code, a guard
  that branches to the jump after `CMP idx,N` (0..N-1) or `SUB idx,N` (-N..-1),
  and a `NEG` of a bounded index - decode exactly instead of falling back to a
  forward read that found nothing or the wrong entries
  (`tools/recomp/tests/test_jumptables.py`).
- Translator: a pushed immediate that decodes as a thunk is an entry candidate:
  the CRT's `atexit` is handed ten-byte `MOV ECX,obj / JMP dtor` stubs that no
  function-start signal accepts, and called into nothing at exit without it.
- Runtime: `game_path_resolve` uses the absolute `RECOMP_DEVELOPER_EXE` from
  wherever the app runs, and a guest root of more than one component
  (`C:\GOG Games\<name>`) resolves in `normalise_components`, so a game
  repository's build finds and opens its own game without a dialog.
- Runtime: `runtime/gdi32.cpp`, a fourth shim table: DIB sections in guest
  memory, memory DCs, `GetObjectA`, colour tables, logical palettes, `BitBlt`
  and `PatBlt` between DIBs, `GetDIBits`, text accepted and not drawn.
- Runtime: boot-path shims with their argument counts: the CRT locale probes,
  `GetEnvironmentVariableA`, `GlobalMemoryStatus`, `SetErrorMode`,
  `GetLogicalDrives`, `SHGetSpecialFolderPathA` (per-user folders under the
  guest root), `IsWindowUnicode`, `GetSystemMetrics`, `LoadCursorA`,
  `CreateIconIndirect`/`DestroyIcon`, the mixer API (no driver),
  `mciGetErrorStringA`, `VERSION.dll` (no version resource).
- dx: `CoCreateInstance` for `CLSID_DirectSound` (every other class is
  `REGDB_E_CLASSNOTREG`) and `IDirectSound::Initialize` succeeds.
- Runtime: `POPM_GUEST_ARGS` appends switches to the command line the CRT reads
  through `GetCommandLineA`, so a game's own `-debugout` or `-nointro` can be
  passed. The RaiseException diagnostic also names the class of every object a
  register points at (through MSVC RTTI) and dumps the thrown object's dwords.
- user32: `ScreenToClient`, `GetActiveWindow`, `SetFocus`.
- Translator: a pushed immediate that decodes as a thunk, a call to a callee the
  listings show never returning (recovery stops there; the emitter leaves a trap),
  and a code pointer whose bytes happen to be printable are all handled; see the
  translator tests.
- Translator: a literal transfer to a block the sweep withdrew (padding after a
  call that never returns) becomes `recomp_unknown_call(c, addr); return;` rather
  than a dangling dispatch, so a listing that ends on a `throw` translates. The
  dispatch gate now names the functions that failed to translate before it
  reports what dispatched to them (`tools/recomp/tests/test_translate_driver.py`).
- Builds take `--game-dir`: a game directory anywhere, with outputs under its own
  `build/`; paths in `game.toml` resolve from its directory. The kit ships `games/stub`
  for game-free builds and CI. Populous moves to github.com/veritr1x/populous-recomp,
  which pulls the kit in as a submodule; its smoke scripts, release notes, game docs and
  game-bound tests go with it.
- Imported the Populous recompilation from populous-recomp-checkout at
  b450dfa8bae7fe568192ef61028ed82489cba394 as the base of recomp-kit. The
  translation is no longer tracked; regenerate it with tools/build.py --regenerate.
- Kit layout: runtime/, dx/, host/, platform/, mods/ at the top level; games/populous/
  holds the game config, curated globals, plugins and artwork.
- games/<id>/game.toml drives identity, addresses and translator inputs through a
  generated game_config.h; tools/check_game_literals.py keeps game literals out of kit code.
- tools/build.py --stub and the *-stub CMake presets link the hosts without game code.
- iOS: `tools/build.py --target ios` builds, signs and installs the app on a paired iPad;
  touch mapper, fullscreen Metal window, game files seeded into Documents on first launch.
- iOS: on-screen key bar, taps that place the game's cursor and hold the click, app icon
  from the game executable, lifecycle-driven suspend of audio and presentation.
- iOS acceptance fixes: the key bar hides to a KEYS tab; a long press then lift is a
  right click and a long press then drag holds the wheel button (Populous scrolls and
  rotates with it; the game has no right-drag camera); a finger resting on a screen
  edge scrolls; settings persist (the translation's symbol table ships in the bundle
  so the mod settings store initialises); touch is cancelled cleanly on focus loss.
- Pointer hit test: the first and last three drawable pixels count as the scrolling
  edge, so an iPadOS trackpad pointer (which stops half a point short of the left
  edge) and the window's last point reach the row or column the game scrolls from.
- A GPU surface read refused around a background/foreground transition is retried
  for up to a second instead of aborting the game; the Metal device reports the first
  failed command buffer's error.

- Build with CMake presets for macOS, Linux and Windows through the unchanged
  `tools/build.py` and `tools/test.py`; the xcrun shell scripts are gone.
- Add a platform layer (`src/recomp/platform/os.h`) so the runtime, adapters
  and mod foundation compile and pass their portable tests on Linux and Windows.
- Mod plugins resolve their file extension per platform; a manifest written on
  macOS loads its `.so` or `.dll` counterpart unchanged.
- CI compiles and tests the portable layers on macOS, Ubuntu and Windows.

## 2026-09-12 — Initial native source release

- Publish the macOS native runtime, static translator, Metal renderer and C/Lua
  mod API as a standalone contributor project.
- Provide verified local game setup, build/test commands, architecture and code guides.
- Include Enhanced rendering, widescreen projection, native FPS/frame-pacing
  overlay, live Options settings and one Graphics resolution selector through 4K.
- Preserve the fixes for animation timing, audio clock behavior, focus changes,
  pointer confinement, resolution cycling and settings persistence.
- Keep original game files, generated translations and local test artifacts out of Git.

Current validation boundaries and performance limits are recorded in [Testing](docs/testing.md).
