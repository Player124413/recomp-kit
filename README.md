# recomp-kit

A static recompilation kit: 32-bit x86 Windows games become native
applications for macOS, iOS, Android, Linux and Windows, with no JIT and no
emulator at run time. The design is in
`docs/superpowers/specs/2026-09-13-recomp-kit-design.md`; this milestone
(M0) consolidates the Populous: The Beginning recompilation as the first
supported game.

## Layout

| Directory | What it holds |
|---|---|
| `runtime/` | x86 semantics (`x86.h`), guest memory, PE loader, scheduler, kernel32/user32 shims |
| `dx/` | DirectDraw, Direct3D 2, DirectSound, DirectInput, QMixer shims |
| `host/` | SDL3 host, Metal/Vulkan/fake GPU backends, audio mixer, presentation |
| `platform/` | `os.h`, the only place that talks to the operating system |
| `mods/` | the mod foundation (Lua 5.4) and its native capture instruments |
| `games/<id>/` | one game: `game.toml`, `globals.toml`, plugins, artwork. No game bytes |
| `tools/` | translator, oracle, build and test scripts |
| `third_party/` | vendored Lua, TinySoundFont, volk, Vulkan headers |

## Build Populous on macOS

You need your own DRM-free `D3DPopTB.exe` with its data, Ghidra for the
listings, and Python 3.9 or later.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/python tools/setup.py --game-dir /path/to/your/populous --ghidra-home /path/to/ghidra
.venv/bin/python tools/build.py --regenerate
open build/PopRecomp.app
```

Generated code is never tracked. A build without game files links the hosts
against a stub translation: `.venv/bin/python tools/build.py --stub`. Its
outputs live under `build/stub/` so they never replace a real build.

## Run on an iPad

Requires Xcode with the iOS SDK, an Apple developer team signed in to Xcode,
a paired iPad with developer mode on, and a macOS build already regenerated.

```sh
export RECOMP_IOS_TEAM=<your team id>       # security find-identity -v -p codesigning
.venv/bin/python tools/build.py --target ios --console
```

The build stages your game directory into the app (see `[bundle].exclude` in
`games/populous/game.toml`), signs it, installs it with `devicectl` and streams
the console. Touch: tap = left click, long press = right click, drag = left
drag, two-finger drag pans, two-finger tap = Escape, three-finger tap = F10
(Options), four-finger tap toggles the keyboard. `tools/ios_logs.py` pulls the
app's Documents (saves) back to the Mac.

## Check a change

```sh
.venv/bin/python tools/test.py                  # portable Python suites
.venv/bin/python tools/format.py                # handwritten native code style
.venv/bin/python tools/check_repo.py            # nothing private is tracked
.venv/bin/python tools/check_game_literals.py   # kit code names no game
.venv/bin/python tools/test.py --native         # native suites (needs the game)
```

Every game-specific value lives in `games/<id>/`; the kit's own directories
must not name a game. `tests/test_game_literals.py` enforces that.

The Populous-specific documents carried over from the original project are
in `docs/` and in each directory's `README.md`.
