# recomp-kit

A static recompilation kit: 32-bit x86 Windows games become native
applications for macOS, iOS, Android, Linux and Windows, with no JIT and no
emulator at run time. The design is in
`docs/superpowers/specs/2026-09-13-recomp-kit-design.md`. Populous: The
Beginning is the first supported game; it lives in its own repository,
[populous-recomp](https://github.com/veritr1x/populous-recomp), which pulls
this kit in as a submodule.

## Layout

| Directory | What it holds |
|---|---|
| `runtime/` | x86 semantics (`x86.h`), guest memory, PE loader, scheduler, kernel32/user32 shims |
| `dx/` | DirectDraw, Direct3D 2, DirectSound, DirectInput, QMixer shims |
| `host/` | SDL3 host, Metal/Vulkan/fake GPU backends, audio mixer, presentation |
| `platform/` | `os.h`, the only place that talks to the operating system |
| `mods/` | the mod foundation (Lua 5.4) and its native capture instruments |
| `games/stub/` | a game that does not exist: the values game-free builds and CI configure with |
| `tools/` | translator, oracle, build and test scripts |
| `third_party/` | vendored Lua, TinySoundFont, volk, Vulkan headers |

## Games live in their own repositories

A game repository holds what is the game's and nothing of the kit's:

```
<game>/
  kit/            this repository, as a git submodule
  game.toml       identity, addresses, translator inputs (see games/stub/game.toml)
  globals.toml    curated symbols
  core/ tests/    game-specific headers the mods and tests use
  mods/           the game's plugins, examples and smoke probe (optional)
  assets/         artwork the texture pack is compiled from (optional)
  smoke/          the game's smoke scripts (optional)
  original/       ignored: your own installation, linked by tools/setup.py
  analysis/       ignored: Ghidra listings, exported by tools/setup.py
  build/          ignored: the translation, the texture pack, the apps, the logs
```

Every kit tool takes `--game-dir <absolute path>`; the game repository's own
`tools/build.py` is a four-line wrapper that passes it. Paths in `game.toml`
(`developer_exe`, `translate.listings`) are relative to `game.toml`'s
directory. Outputs go under `<game>/build` when the game lives outside the
kit, else under the kit's `build/`.

## Build a game on desktop

`tools/build.py --target app` selects the `macos`, `linux` or `windows`
CMake preset and builds `recomp_app`. `--regenerate` runs the Python
translator on all three platforms. The iOS packager runs on macOS,
including `--target ios --stub` builds. See [Contributing](CONTRIBUTING.md)
for platform prerequisites; the commands below use a macOS shell.

From the game repository, with Python 3.9 or later, Ghidra for the first
translation, and your own copy of the game:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r kit/requirements-dev.txt
.venv/bin/python tools/setup.py --install /path/to/the/installed/game --ghidra-home /path/to/ghidra
.venv/bin/python tools/build.py --regenerate
open build/<AppName>.app
```

From the kit itself the same commands take `--game-dir /abs/path/to/<game>`.
Generated code is never tracked. A build without game files links the hosts
against a stub translation of the stub game: `.venv/bin/python tools/build.py
--stub`; its outputs live under `build/stub/` so they never replace a real
build.

## Run on an iPad

Requires Xcode with the iOS SDK, an Apple developer team signed in to Xcode,
a paired iPad with developer mode on, and a macOS build already regenerated.

```sh
export RECOMP_IOS_TEAM=<your team id>       # security find-identity -v -p codesigning
.venv/bin/python tools/build.py --target ios --console
```

The build stages the game directory into the app (see `[bundle].exclude` in
the game's `game.toml`), signs it, installs it with `devicectl` and streams
the console. Touch: tap = left click, long press then lift = right click, long
press then drag = wheel-button drag, a hold on a screen edge scrolls, drag =
left drag, two-finger drag pans, two-finger tap = Escape, three-finger tap =
F10 (Options), four-finger tap toggles the system keyboard. An on-screen split
keyboard sits in the bottom corners when no hardware keyboard is attached:
HIDE/KEYS tabs per half, Shift/Ctrl/Alt hold to chord, tap to latch, double
tap to lock; size and visibility on the F10 page, persisted per game
(`[touch] keypad = "hidden"` in `game.toml` starts it hidden).
RECOMP_* switches reach the device through `Documents/switches.txt` (NAME=VALUE
lines), copied in with `xcrun devicectl device copy to --domain-type
appDataContainer --domain-identifier <bundle id>`. `tools/ios_logs.py` pulls
the app's Documents (saves) back to the Mac.

## Check a change

```sh
.venv/bin/python tools/test.py                  # portable Python suites, on the stub game
.venv/bin/python tools/format.py                # handwritten native code style
.venv/bin/python tools/check_repo.py            # nothing private is tracked
.venv/bin/python tools/check_game_literals.py   # kit code names no game
.venv/bin/python tools/test.py --native         # native suites; game-labelled ones skip on the stub
.venv/bin/python tools/test.py --game-dir /abs/<game> --native   # the same against a real game
```

Nothing under `runtime/`, `dx/`, `host/` or `platform/` may name a game;
`tests/test_game_literals.py` enforces that. Game-specific documents live
with their game.
