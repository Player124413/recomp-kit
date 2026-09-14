# Changelog

## Unreleased

- Build: default FFmpeg ON on Linux, import its major-version shared objects
  and package them beside the executable with an `$ORIGIN` rpath and notice.
  On Windows, detect MSYS2 bash/make and require a MinGW-compatible compiler;
  keep video OFF with a status message when prerequisites are missing or
  the compiler uses the MSVC ABI. Package enabled builds' DLLs and notice;
  remove staged video files on OFF without touching player files. Windows
  CI stays video OFF and Linux needs no new packages. macOS configure and
  fake-file staging checks pass; Linux/Windows builds and playback are unverified.

- Build: enable shared FFmpeg by default on iOS and Android. Cross-build
  arm64 iOS 17 dylibs with relative install names and use Xcode's Embed
  Frameworks phase to copy and sign them with the app's identity/team.
  Cross-build Android API-29 libraries and package the three unversioned
  `.so` files beside `libmain.so`; remove staged copies when video is OFF.
  Include the FFmpeg notice in mobile bundles and document all configure
  flags. Android stub/real APKs and the standalone iOS FFmpeg build pass;
  embedded iOS signatures and mobile device playback remain unverified.

- Bink: decode video through FFmpeg into the DirectDraw surface supplied by
  the game, converting YUV420P to RGB565, RGB555 or XRGB8888. Stream decoded
  audio through the shared mixer, pace frames with host time and return an
  empty error string on success. Pending host close ends the video loop;
  builds without FFmpeg keep the finished-video stub and Smacker stays refused.

- Build: fetch SHA-256-pinned FFmpeg 7.1.1 for macOS with only Bink/Smacker
  decoders and demuxers and file input. `RECOMP_VIDEO` defaults to ON on
  macOS and OFF elsewhere; OFF retains the build without FFmpeg. Link
  avformat, avcodec and avutil dynamically, bundle the three replaceable
  dylibs with relative install names and ad-hoc signatures, and ship the
  LGPL notice, source identity and build flags.

- Touch: taps press and release at the finger's position, including near a
  window edge, without a cursor nudge after release. Held fingers and drags
  retain edge snapping so holding an edge still scrolls; lifting an edge
  hold moves the cursor back inside to stop scrolling.

- Load saved host settings before symbol-table validation, so window mode and
  other profile settings survive relaunch even without a usable symbol table.
  Hide renderer and native Options rows from the fallback settings page until
  symbols are available; retain window mode, frame limit and performance overlay.

- SDL: confine a captured pointer in a plain window as well as borderless
  and fullscreen modes. Click inside to capture, hold Escape to release,
  and drag into the window's resize margin to release capture for resizing.

- Android reads game data from SDL's external files directory under `game/`,
  reads `switches.txt` beside it, and defaults to a writable `profile/` there.
  Missing data logs the expected executable and `adb push` command, then exits.
  Enable fullscreen touch/keypad behavior and background audio/presenter
  suspension; end the process after SDL teardown when the game exits.
  `--push-game` stages the configured install using `[bundle].exclude`, then
  pushes before launch, and fails clearly without a ready Android device.

- Build `--target android` through the NDK preset, then package its
  `libmain.so` with an SDLActivity subclass and the matching FetchContent
  Java sources. Add a Gradle 9.7.1 wrapper and AGP 9.1.1 templates for
  arm64-v8a, API 29 minimum, compile/target SDK 36 and required Vulkan 1.1.
  Install and launch on a ready adb device, stream logcat with `--console`,
  and skip device actions when none is attached. Stub APK packaging is
  verified on macOS; Android device execution remains unverified.

- Add `android` and `android-stub` NDK presets for arm64-v8a, API 29 and
  static libc++. Build the SDL host as `libmain.so` with static SDL3 and
  NDK Vulkan/log libraries, omit desktop tests and add an Android stub CI
  build. Read arm64 Linux/Android page-fault writes from the kernel's ESR
  signal-frame record. APK packaging and device execution are not implemented here.

- Package successful desktop app builds under `build/package`: a Linux
  folder and architecture-named tarball, or a Windows folder. Include the
  kit notices, display-mode baseline, available translation symbol index
  and launch instructions using `RECOMP_EXE`; exclude original game files.
- Build: allow the app, smoke and headless hosts on Linux and Windows,
  including translation with `--regenerate`. Only the iOS packager requires
  macOS, including stub builds.
- Miles streams decode MP3 through the decoder shared with DirectShow.
  Streams refill from the guest frame pump, support volume and loop counts,
  and remain playing until queued PCM drains.
- USER32: queue `WM_MOVE` and `WM_SIZE` after window creation and the
  corresponding `SetWindowPos` operations, plus `WM_SIZE` on the first show.
  Screen and fullscreen metrics follow the accepted DirectDraw display mode,
  retaining the caption-height deduction and 1024x768 fallback without a mode.
  Games can now size their fullscreen blit rectangles from window messages.
- DirectDraw: writes through a writable pointer retained after `Unlock` reach
  the renderer before `Blt`, `BltFast` and primary presentation. Whole-rectangle
  hashes detect the writes and share the written-lock CPU recording path;
  surfaces never locked writable keep their existing path without hashing.
- Bink: `BinkOpen` returns a 256-byte guest heap record with 640x480
  dimensions and zero frame counters, so a game skips an unavailable
  cinematic instead of treating an open failure as fatal. `BinkClose`
  frees the record; decoding and waiting remain no-ops, and `SmackOpen`
  still returns 0. No video decoder is included.
- GDI: `GetDeviceCaps` reports the accepted DirectDraw mode and depth-dependent
  palette capabilities, falling back to 640x480x8 before a mode is set.
  `GetTextExtentPointA` shares the fixed 7-pixel width and 16-pixel height of
  `GetTextMetricsA`; `SetBkColor` stores each DC's background color and returns
  its previous value, initially white. Text output remains undrawn.
- Build: define `profile_tests` only when the translation's `funcs.h` defines
  its target function `FN_00500040`, so other translations can build all
  native test binaries without that game-specific suite.
- Runtime: optional `[game] heap_base` sets the heap arena start through the
  generated config and build definitions, so images ending above 16 MB can
  load. The default remains `0x01000000`; the value must be page aligned,
  above `0x00400000` and below `0x0e000000`. A rejected image now reports
  both its end and the heap start, with the setting to raise.
- Build: `tools/build.py --regenerate --allow-table-gaps "<reason>"` passes
  the waiver and its reason to the translator. Omitting the flag keeps
  jump-table gap checks unchanged.
- Touch: a tap's synthesized click now stays pressed until the game has
  presented two frames after the press (`TouchMapper::frames_presented`, fed
  by the SDL host from the present count), as well as for the 90 ms it
  already held. A game that samples its buttons with `GetKeyState` once per
  frame and presents at 15 frames a second could miss a clock-timed press and
  release altogether; on a device that made most taps land nowhere. A game
  that stops presenting still gets its release after 400 ms.
- Touch: the edge-snap margin grows by the window's safe-area inset on each
  edge (`TouchMapper::set_edge_insets`, from `SDL_GetWindowSafeArea`). A
  finger on a tablet's top bezel arrives no closer than the status bar's far
  side, some 32 points down, so the 16-point margin never saw it and the
  game's top-edge scroll never started.
- Touch: a synthesized click's release carries the press position again. The
  mapper read the placed point back out of the action vector after pushing the
  press into it, past a reallocation; on a device the release then landed at
  0,0, so a tap pressed one button and released on another (nothing happened)
  and a game that scrolls at its edges flew to its top-left corner.
  `input_touch_tests` now checks the release position across vector capacities.
- Touch: a drag the system cancels (an edge gesture it claims) releases its
  button where the cursor was placed, not at 0,0. The release carried the
  origin as its position, the host moved the game's cursor there, and a game
  that scrolls at its edges flew to its top-left corner.
- A hardware pointer resting against a system strip (a tablet's status bar,
  which the pointer cannot enter) is placed on the edge behind it
  (`pointer_behind_strip`, with 16 points of slack for a hand pushing against
  the strip), and stays there until the pointer has come 64 points away
  (`PointerStripLatch`): iPadOS glides a pointer that touched the strip some
  30 points back down on its own, which would have left the edge after an
  instant. A game's top-edge scroll, which needs the cursor to stay put while
  it ramps up, works from a mouse on a tablet.
- The iPad host ends the process when the game exits (`ExitProcess`,
  `platform_ui_process_exit`); before, the game was gone and the app stayed
  on screen showing its last frame.
- Switches from a file: `recomp_env_apply_file` reads NAME=VALUE lines, and
  the iPad host applies `Documents/switches.txt` at start, so RECOMP_* switches
  (the pointer trace, a pinned clock) reach a device that has no shell. Put the
  file there with `xcrun devicectl device copy to ... --domain-type
  appDataContainer --domain-identifier <bundle id> --destination
  Documents/switches.txt`.
- Smoke scripts: `button <left|right|middle> <down|up>` presses or releases a
  mouse button where the pointer is and leaves it, so the moves in between
  are a drag; `click` remains a press and a scheduled release.
- VERSION.dll serves the executable's own version resource out of the mapped
  image: `GetFileVersionInfoSizeA`/`GetFileVersionInfoA` for the game's module
  name, `VerQueryValueA` for `\`, `\VarFileInfo\Translation` and
  `\StringFileInfo\<lang>\<name>` (strings narrowed in place for the A
  caller). Any other file still has none. A game's "Version" label fills in.
- DirectShow: `IFilterGraph::EnumFilters` returns an enumerator over the
  graph's (empty) filter list instead of E_NOTIMPL, so a game that lists its
  filters for its log walks nothing rather than logging a failure.
- The runtime's log lines are tagged `[recomp]`, not with a game's initials.
- File seam: a handle opened for writing on an existing file goes through the
  read tier and is promoted to the write tier by its first WriteFile or
  SetEndOfFile, continuing at the offset it had reached. Classifying the open
  itself as a write copied every archive a game opens read/write into the
  profile (one game: 590 MB per fresh profile, and a copy the watchdog cut
  short then hung the next run). A create or truncation is still a write from
  the start. `runtime_tests` covers the promotion against the seam fixture.
- Mods loader: the overlay - the profile as the writable tier - is installed
  before the symbol table is loaded and before RECOMP_NO_MODS is honoured, so a
  port with no symbol table or mods still keeps its saves, settings and logs
  out of the game's own installation. The run record creates its directory.
- DirectShow multimedia streaming, the reading side (`dx/dshow.cpp`): a game
  that plays its MP3 music through `CoCreateInstance(CLSID_AMMultiMediaStream)`
  gets IAMMultiMediaStream over the file, decoded with minimp3
  (`third_party/minimp3`, CC0). Both ways a game drives it are served: pulling
  samples (IAudioMediaStream, AMAudioData, IAudioStreamSample::Update filling
  the caller's buffer and signalling its event, MS_S_ENDOFSTREAM, Seek) and
  driving the filter graph (IGraphBuilder from GetFilterGraph, IMediaControl
  Run/Pause/Stop, IMediaEventEx with a real completion event and EC_COMPLETE,
  IMediaSeeking, IMediaPosition, IBasicAudio), where the kit streams the PCM to
  a host audio channel from the frame pump. `dx_tests` covers both against an
  MP3 tone kept as a header (`dx/tests/fixtures/tone_mp3.h`).
- COM classes register: `com_register_class` names a CLSID, a constructor and
  the interface IID_IUnknown gets, and one `CoCreateInstance` in `com.cpp`
  serves every registered class (DirectSound moved onto it); an unregistered
  class is still REGDB_E_CLASSNOTREG. `win32_create_event` and
  `win32_reset_event` let a shim own a kernel event on the guest's behalf.
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
