#!/usr/bin/env python3
"""Build the native app through CMake, regenerating original-game code only when needed.

The game is a directory holding game.toml (tools/build.py --game-dir). Its
outputs (the translation, the texture pack, the apps, the logs) go under
<game-dir>/build when the game lives outside the kit, else under the kit's
build/. The kit's own default is games/stub, a game that does not exist."""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recomp"))
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402
import buildlock  # noqa: E402

# What each --target builds. `plugins` is every mod plugin the game ships.
TARGETS = {
    "app": ["recomp_app"],
    "smoke": ["pop_smoke"],
    "headless": ["pop_headless"],
    "fixture": ["pop_fixture"],
    "gen": ["recomp_gen"],
    "plugins": ["plugins"],
    "ios": ["recomp_app"],
}
MACOS_ONLY = {"ios"}
NEEDS_GEN = {"app", "smoke", "headless", "fixture", "gen", "ios"}


def default_preset(system=None):
    """The CMake preset for this operating system."""
    return {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}[system or platform.system()]


def preset_name(preset, config, stub=False, target=None):
    """Debug, stub and iOS builds live in their own binary directories, so they are their own presets."""
    if target == "ios":
        return "ios-stub" if stub else "ios"
    if stub:
        return preset + "-stub"
    return preset if config == "Release" else preset + "-debug"


def build_root_for(game_dir, root=ROOT):
    """Outputs live beside the game when it is outside the kit, else in the kit's build/."""
    game_dir = Path(game_dir)
    try:
        game_dir.relative_to(root)
        return Path(root) / "build"
    except ValueError:
        return game_dir / "build"


def build_dir_for(build_root, preset):
    """The CMake binary directory: what the presets name inside the kit, beside the game outside it."""
    return Path(build_root) / "cmake" / preset


def archive_path(build_root, system=None):
    """Where CMake writes the translated archive on this platform."""
    name = "recomp_gen.lib" if (system or platform.system()) == "Windows" else "librecomp_gen.a"
    return Path(build_root) / "recomp" / name


def cmake_tool(name):
    """Prefer the venv's pinned cmake/ctest beside this interpreter, then PATH."""
    beside = Path(sys.executable).parent / name
    if beside.exists():
        return str(beside)
    return shutil.which(name) or name


def game_defines(game_dir, build_root):
    """The two cache paths every configure needs."""
    return ["-DRECOMP_GAME_DIR=%s" % Path(game_dir).as_posix(), "-DPOP_BUILD_ROOT=%s" % Path(build_root).as_posix()]


def configure(preset, extra=(), build_dir=None):
    """Configure a preset; `build_dir` overrides the preset's binary directory."""
    command = [cmake_tool("cmake"), "--preset", preset, "-DPython3_EXECUTABLE=" + sys.executable]
    if build_dir is not None:
        command += ["-B", str(build_dir)]
    subprocess.run(command + list(extra), cwd=ROOT, check=True)


def build(preset, targets, jobs, extra=(), build_dir=None, config="Release"):
    """`extra` goes after the targets: a leading "--" hands the rest to the native tool.

    A build directory named directly (not through the build preset) needs the
    configuration spelled out too: the Xcode generator is multi-config and
    would otherwise build Debug, whose -O0 translation overflows the guest's
    stack on the device."""
    command = [cmake_tool("cmake"), "--build"]
    command += [str(build_dir), "--config", config] if build_dir is not None else ["--preset", preset]
    command += ["--parallel", str(jobs), "--target"] + list(targets) + list(extra)
    subprocess.run(command, cwd=ROOT, check=True)


def devicectl_list():
    """Paired devices as devicectl reports them."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        path = tmp.name
    subprocess.run(["xcrun", "devicectl", "list", "devices", "--json-output", path], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(path) as fh:
        return json.load(fh)["result"]["devices"]


def pick_device(devices):
    """The one paired iPad, or exit asking for --device."""
    ipads = [d for d in devices
             if d.get("hardwareProperties", {}).get("productType", "").startswith("iPad")
             and d.get("connectionProperties", {}).get("pairingState") == "paired"]
    if len(ipads) == 1:
        return ipads[0]["identifier"]
    names = ", ".join("%s (%s)" % (d.get("deviceProperties", {}).get("name", "?"), d["identifier"]) for d in ipads)
    sys.exit("Pass --device <identifier>; paired iPads: %s" % (names or "none"))


def ios_app_bundle(app_name, build_root):
    """The signed bundle under <build root>/ios; Xcode adds a configuration directory (Release-iphoneos)."""
    found = sorted((Path(build_root) / "ios").glob("**/%s.app" % app_name))
    if not found:
        sys.exit("No %s.app under %s/ios; did the iOS build succeed?" % (app_name, build_root))
    return found[-1]


def install_and_launch(app, bundle_id, device, console):
    """Install the bundle with devicectl and launch it, optionally streaming its console."""
    subprocess.run(["xcrun", "devicectl", "device", "install", "app", "--device", device, str(app)], check=True)
    launch = ["xcrun", "devicectl", "device", "process", "launch", "--terminate-existing", "--device", device]
    if console:
        launch.append("--console")
    launch.append(bundle_id)
    subprocess.run(launch, check=True)


def publish_generated(build_root, translate):
    """Stage a translation, then publish gen/ and symbols.json by rename.

    `translate(stage_dir)` writes the sources and raises on failure; the
    published tree is untouched in that case. Publishing is renames only, so
    a reader under the same lock never sees half a generation."""
    recomp = Path(build_root) / "recomp"
    recomp.mkdir(parents=True, exist_ok=True)
    gen, old = recomp / "gen", recomp / "gen.old"
    stage = recomp / ("gen.new.%d" % os.getpid())
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    try:
        translate(stage)
        # x86.h sits beside the generated sources so #include "x86.h" resolves.
        shutil.copy(ROOT / "runtime/x86.h", stage / "x86.h")
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    shutil.rmtree(old, ignore_errors=True)
    if gen.exists():
        gen.rename(old)
    stage.rename(gen)
    symbols = gen / "symbols.json"
    if symbols.is_file():
        temporary = recomp / ("symbols.json.new.%d" % os.getpid())
        shutil.copy(symbols, temporary)
        temporary.replace(recomp / "symbols.json")
    shutil.rmtree(old, ignore_errors=True)


def run_translator(stage, game_dir, build_root, allow_table_gaps=None):
    command = [sys.executable, str(ROOT / "tools/recomp/translate.py"), "--out", str(stage),
               "--game", str(game_dir),
               "--report", str(Path(build_root) / "recomp/translate-report.json")]
    if allow_table_gaps:
        command += ["--allow-table-gaps", allow_table_gaps]
    subprocess.run(command, cwd=ROOT, check=True)


def texture_pack(game_dir, build_root):
    """Compile the redistributable material-detail layer when its inputs are newer.
    Original-game replacement textures remain optional, locally prepared pack entries.
    A game without artwork has no texture pack."""
    detail = Path(build_root) / "texture-pack/terrain-detail.popt"
    artwork = Path(game_dir) / "assets/terrain/materials-v1.png"
    compiler = ROOT / "tools/recomp/terrain_detail.py"
    if not artwork.is_file():
        return
    if (not detail.is_file() or not (detail.parent / "manifest.json").is_file()
            or detail.stat().st_mtime < max(artwork.stat().st_mtime, compiler.stat().st_mtime)):
        subprocess.run([sys.executable, str(compiler), "--source", str(artwork),
                        "--output", str(detail.parent)], cwd=ROOT, check=True)


def parse_args(argv, system=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regenerate", action="store_true", help="Regenerate and compile translated C")
    parser.add_argument("--allow-table-gaps", metavar="REASON", default=None,
                        help="Accept jump-table sites the translator cannot decode (passed to translate.py)")
    parser.add_argument("--target", choices=sorted(TARGETS), default="app")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--preset", default=default_preset(system), help="CMake configure preset")
    parser.add_argument("--config", choices=("Release", "Debug"), default="Release")
    parser.add_argument("--game-dir", type=Path, default=ROOT / "games/stub",
                        help="Absolute directory holding the game.toml this build is for (default: the kit's stub game)")
    parser.add_argument("--stub", action="store_true",
                        help="Link the hosts against a stub translation (no game code; CI's build)")
    parser.add_argument("--device", default=None, help="devicectl identifier of the iPad (ios target)")
    parser.add_argument("--team", default=os.environ.get("RECOMP_IOS_TEAM", ""),
                        help="Apple team id for automatic signing (ios target; default $RECOMP_IOS_TEAM)")
    parser.add_argument("--no-install", action="store_true", help="Build the iOS app without installing it")
    parser.add_argument("--console", action="store_true", help="After launching on the device, stream its console")
    args = parser.parse_args(argv)
    if args.target == "ios" and not args.stub and not args.team:
        parser.error("--target ios needs --team or RECOMP_IOS_TEAM")
    if args.stub and (args.config == "Debug" or args.regenerate):
        parser.error("--stub cannot be combined with --config Debug or --regenerate")
    if not args.game_dir.is_absolute():
        parser.error("--game-dir must be absolute: %s" % args.game_dir)
    if not (args.game_dir / "game.toml").is_file():
        parser.error("No game config at %s/game.toml" % args.game_dir)
    if args.target == "plugins" and not (args.game_dir / "mods/CMakeLists.txt").is_file():
        parser.error("%s has no mods/CMakeLists.txt; nothing to build for --target plugins" % args.game_dir)
    if args.target in MACOS_ONLY and (system or platform.system()) != "Darwin":
        parser.error("The iOS packager runs on macOS")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    args.build_root = build_root_for(args.game_dir)
    return args, parser


def main():
    """Check inputs, translate under the build lock when needed, then configure and build."""
    args, parser = parse_args(sys.argv[1:])
    cfg = game_config.load(args.game_dir)
    # Regenerating needs the game and its listings.
    if args.regenerate and not cfg["developer_exe_path"].is_file():
        parser.error("Prepare your own game installation with tools/setup.py first")
    preset = preset_name(args.preset, args.config, stub=args.stub, target=args.target)
    build_dir = build_dir_for(args.build_root, preset)
    defines = game_defines(args.game_dir, args.build_root)
    try:
        # The lock lives at <build root>/recomp/.lock: BuildLock joins build/recomp/.lock onto its argument.
        with buildlock.BuildLock(args.build_root.parent, "tools/build.py"):
            if args.target in NEEDS_GEN and args.regenerate:
                if not (cfg["listings_path"] / "functions.tsv").is_file():
                    parser.error("Translation listings are missing; run tools/setup.py without --link-only")
                publish_generated(args.build_root,
                                  lambda stage: run_translator(stage, args.game_dir, args.build_root,
                                                               args.allow_table_gaps))
            if args.target == "ios":
                if not args.stub and not (args.build_root / "recomp/gen/table.c").is_file():
                    parser.error("No translation in %s/recomp/gen; run tools/build.py --regenerate on macOS first"
                                 % args.build_root)
                configure(preset, defines + ["-DRECOMP_IOS_TEAM=" + args.team], build_dir=build_dir)
                extra = ["--", "CODE_SIGNING_ALLOWED=NO"] if args.stub else ["--", "-allowProvisioningUpdates"]
                build(preset, TARGETS["ios"], args.jobs, extra, build_dir=build_dir, config="Release")
                if not args.stub and not args.no_install:
                    app = ios_app_bundle(cfg["game"]["app_name"], args.build_root)
                    device = args.device or pick_device(devicectl_list())
                    install_and_launch(app, cfg["game"]["bundle_id"], device, args.console)
            else:
                # A link-only build ships no texture pack, and its CI has no numpy.
                if args.target == "app" and not args.stub:
                    texture_pack(args.game_dir, args.build_root)
                configure(preset, defines, build_dir=build_dir)
                build(preset, TARGETS[args.target], args.jobs, build_dir=build_dir, config=args.config)
    except subprocess.CalledProcessError as error:
        parser.exit(error.returncode or 1, "Build failed; see the compiler output above.\n")
    except TimeoutError as error:
        parser.exit(1, "%s\n" % error)


if __name__ == "__main__":
    main()
