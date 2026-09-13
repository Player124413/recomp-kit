# Contributing

Start with a small change you can explain and verify. Useful areas include input
and window behavior, rendering correctness, audio scheduling, documentation,
portable tests and mod examples. Open an issue before a large architecture change.

## Prerequisites

- Python 3.9 or later; create `.venv` and install `requirements-dev.txt`.
- Native builds on macOS: Apple Silicon, Xcode Command Line Tools and Git. CMake
  and Ninja come from `requirements-dev.txt`.
- Portable-layer builds on Linux: clang and lld (`apt-get install clang lld`).
- Portable-layer builds on Windows: LLVM's clang, a Visual Studio developer
  command prompt for the Windows SDK, and `tools/build.py --target fixture`
  or `tools/test.py --compile-only`. Linux and Windows build and test the
  runtime, adapters and mod foundation only; no game host exists for them yet.
- First translation: [Ghidra 12.1.3](https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.3_build).
- A Java runtime compatible with that Ghidra distribution. The documented setup
  was tested with OpenJDK 26.0.1; set `JAVA_HOME` to the JDK directory.
- Your own supported installation of the game you are building. Its
  `game.toml` names the executable and its SHA-256; the loader refuses other
  binaries because translated addresses and data layouts are tied to that
  image. Do not bypass the hash to add support for another version.

## Prepare a game installation

Building the app, running it, the game-backed suites and regenerating the
translation all require this step; generated code is never tracked. Every
command below takes `--game-dir /abs/path/to/<game>` when run from the kit;
a game repository's `tools/*.py` wrappers pass it for you.

```sh
.venv/bin/python tools/setup.py \
  --game-dir /abs/path/to/the/game/repository \
  --install "/path/to/your/installed game" \
  --ghidra-home "/path/to/ghidra_12.1.3_PUBLIC" \
  --java-home "/path/to/your/jdk/Contents/Home"
```

Setup verifies the executable against `game.toml`, links the installation
where `developer_exe` points (ignored `<game>/original/...`), fetches the
annotation metadata `[setup]` names, and exports translation inputs into
ignored `<game>/analysis/`. It does not download the game. Existing links to
another installation and dirty metadata checkouts are preserved and reported.
The first export can take several minutes.

If inputs already exist, `--link-only` validates the game link without running
Ghidra. `GHIDRA_HOME` and `JAVA_HOME` can supply the tool paths instead of flags.

## Build and run

```sh
.venv/bin/python tools/build.py --game-dir /abs/path/to/<game> --jobs 8
open /abs/path/to/<game>/build/<AppName>.app
```

The first build translates and compiles the original functions. Subsequent builds
reuse that archive and rebuild the handwritten host. After editing instruction
translation or its C helpers, regenerate explicitly:

```sh
.venv/bin/python tools/build.py --regenerate --jobs 8
```

`--target smoke` builds the offscreen scripted host, `--target headless` the
minimal boot host, `--target fixture` the parity fixture and `--target plugins`
every mod plugin. `--preset` and `--config Debug` pick the CMake preset; the
CMake tree lives in `<build root>/cmake/<preset>` and every artifact keeps its
documented path under that build root: the game's `build/` when the game lives
outside the kit, the kit's `build/` for the stub. Your default writable profile
stays there too. `POPM_PROFILE_DIR` selects a separate profile for an
interactive run. Keep the app in the checkout; moving it requires explicitly
configuring its game path.

## Add a game

Make a repository for it with `game.toml` and `globals.toml` at its root,
following `games/stub/` for the keys and populous-recomp for a complete
example, and add this kit as a submodule at `kit/`. Build with
`kit/tools/build.py --game-dir "$PWD"`. Nothing under `runtime/`, `dx/`,
`host/` or `platform/` may name your game; put addresses under `[hooks]` and
use the generated `RECOMP_HOOK_*` macros.

## Check your change

```sh
.venv/bin/python tools/test.py           # No game files required
.venv/bin/python tools/format.py         # Check handwritten C/C++/Objective-C
.venv/bin/python tools/test.py --native  # Runtime, DirectX and Metal tests on macOS; portable suites everywhere
.venv/bin/python tools/test.py --mods    # Build the app first; real game-backed mod tests
.venv/bin/python tools/test.py --gameplay # Build the app first; isolated native Options/gameplay run
```

The test runner describes missing prerequisites rather than silently skipping a
requested suite. Detailed limits and the manual input/audio checklist are in
[Testing](docs/testing.md). Save files and logs from tests use ignored scratch
profiles; do not attach original game files or personal saves to issues.

## Code conventions

- Use descriptive names in handwritten code and keep functions focused on one job.
- Comment each major function's purpose, significant inputs/outputs, ownership,
  failure behavior and thread assumptions. Explain unusual arithmetic or layout
  constraints where they occur. Avoid comments that merely restate the name.
- Run `.venv/bin/python tools/format.py --write` for native code. Do not format
  `third_party/` or generated game code; their upstream/generated layout is intentional.
- Keep guest addresses as 32-bit values. Use the memory helpers rather than
  casting guest addresses into host pointers. See [Architecture](docs/architecture.md).
- Preserve simulation timing independently of render rate. Document whether a
  measurement describes simulation, GPU completion or frames actually displayed.
- Add focused regression coverage for behavior changes. A screenshot or counter
  alone does not establish playable-game correctness.
- Keep generated output, original game files, SDKs, credentials and private run
  artifacts out of commits. `tools/check_repo.py` checks tracked publication inputs.

## Submit a pull request

Fork the repository, create a descriptive branch, and keep the change focused.
Describe the problem, resulting behavior, relevant implementation decision and
what you tested. Include a screenshot for UI changes and system/resolution details
for performance reports. Update **Unreleased** in [CHANGELOG.md](CHANGELOG.md)
for user-visible changes. Pull requests run checks without publishing game assets.

Contributions to the handwritten implementation use [LICENSE](LICENSE). Preserve
upstream notices; do not add assets or source you do not have permission to contribute.
