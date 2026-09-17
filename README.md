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
| `third_party/` | vendored Lua, TinySoundFont, minimp3, stb_truetype, volk, Vulkan headers |

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

## Dependencies

SDL3 is fetched at its pinned release and linked statically; Lua,
TinySoundFont, minimp3, stb_truetype, volk and Vulkan headers are vendored with their
upstream notices. See [NOTICE](NOTICE) for licenses.

On macOS, iOS, Android and Linux, `RECOMP_VIDEO` defaults to `ON`: the first build fetches the
SHA-256-pinned FFmpeg 7.1.1 release and builds shared `avformat`, `avcodec`
and `avutil` libraries with only Bink/Smacker video and audio decoders,
Bink/Smacker demuxers and file input. The macOS app carries the three dylibs in
`Contents/Frameworks`, using `@rpath` install names and an executable rpath
of `@executable_path/../Frameworks`; each dylib is signed ad hoc before the
app. Apple system libraries/frameworks are allowed; no Homebrew libraries
are required. Bink cinematics decode through these libraries; Smacker
still refuses to open a video.

iOS cross-builds for arm64 and iOS 17.0 with the iPhoneOS SDK. Xcode embeds
the three dylibs in `Frameworks/` and signs them on copy with the app's
development identity/team; the executable uses `@executable_path/Frameworks`.
Android cross-builds with the selected NDK's arm64 API-29 compiler and
packages unversioned `libavformat.so`, `libavcodec.so` and `libavutil.so`
beside `libmain.so` under `lib/arm64-v8a/`. No Gradle packaging override is
needed. The mobile cross builds and Android APK contents are verified;
iOS embedded signatures and mobile device playback remain unverified.

Linux uses CMake's native C compiler and `--enable-pic`. The desktop packager
copies `libavformat.so.61`, `libavcodec.so.61` and `libavutil.so.59` beside
the executable, whose rpath includes `$ORIGIN`. The package also carries
`resources/ffmpeg-NOTICE.md`. FFmpeg builds from source using the existing
compiler and make; no distribution FFmpeg package is needed.

On Windows, CMake looks for MSYS2's `make` on `PATH` (or takes
`-DRECOMP_FFMPEG_MAKE=C:/msys64/usr/bin/make.exe`) and the `bash` beside it;
video defaults to ON when both are there. With the presets' clang, which
targets the MSVC ABI, FFmpeg is built by its own MSVC toolchain
(`--toolchain=msvc`), so run CMake from a Visual Studio developer shell where
`cl` and `link` are on `PATH`; a MinGW compiler gets a MinGW FFmpeg. The
three versioned DLLs are copied beside the built executables and packaged
beside the app with the notice under `resources/`. Windows CI builds FFmpeg
this way and runs the video tests.

FFmpeg is LGPL-2.1-or-later and dynamically linked. Its full license,
source URL, checksum, configure command and library replacement instructions
are in [the FFmpeg notice](third_party/ffmpeg/NOTICE.md), also shipped as
`Contents/Resources/ffmpeg-NOTICE.md` on macOS, at the iOS bundle root,
and as `assets/ffmpeg-NOTICE.md` in Android APKs. CMake `-DRECOMP_VIDEO=OFF`
disables the dependency. Existing mobile or Linux CMake caches that
explicitly have video OFF need `-DRECOMP_VIDEO=ON` once when configuring.
See [Contributing](CONTRIBUTING.md) for the CMake cache workflow and the
[notice](third_party/ffmpeg/NOTICE.md) for each platform's configure flags.

For Android, set `ANDROID_NDK_HOME`, `ANDROID_HOME` and `JAVA_HOME` for
your NDK, SDK and Android Studio JBR, then run `tools/build.py --target android`
from the prepared game repository (`--stub` needs no translation).
The APK is `build/android/app/build/outputs/apk/debug/app-debug.apk`.
`--push-game` still stages and pushes the original game data separately;
without a ready device the default build skips install and launch.

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
