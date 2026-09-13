# Changelog

## Unreleased

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
