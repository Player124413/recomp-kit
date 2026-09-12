#!/usr/bin/env python3
"""Build the native app through CMake, regenerating original-game code only when needed."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recomp"))
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402
import buildlock  # noqa: E402

# What each --target builds. `plugins` is every mod plugin the tree ships.
TARGETS = {
    "app": ["recomp_app"],
    "smoke": ["pop_smoke"],
    "headless": ["pop_headless"],
    "fixture": ["pop_fixture"],
    "gen": ["recomp_gen"],
    "plugins": ["plugins"],
}
MACOS_ONLY = {"app", "smoke", "headless"}
NEEDS_GEN = {"app", "smoke", "headless", "fixture", "gen"}


def default_preset(system=None):
    """The CMake preset for this operating system."""
    return {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}[system or platform.system()]


def preset_name(preset, config, stub=False):
    """Debug and stub builds live in their own binary directories, so they are their own presets."""
    if stub:
        return preset + "-stub"
    return preset if config == "Release" else preset + "-debug"


def archive_path(root=ROOT, system=None):
    """Where CMake writes the translated archive on this platform."""
    name = "recomp_gen.lib" if (system or platform.system()) == "Windows" else "librecomp_gen.a"
    return Path(root) / "build/recomp" / name


def cmake_tool(name):
    """Prefer the venv's pinned cmake/ctest beside this interpreter, then PATH."""
    beside = Path(sys.executable).parent / name
    if beside.exists():
        return str(beside)
    return shutil.which(name) or name


def configure(preset, extra=()):
    subprocess.run([cmake_tool("cmake"), "--preset", preset, "-DPython3_EXECUTABLE=" + sys.executable]
                   + list(extra), cwd=ROOT, check=True)


def build(preset, targets, jobs):
    subprocess.run([cmake_tool("cmake"), "--build", "--preset", preset, "--parallel", str(jobs), "--target"]
                   + list(targets), cwd=ROOT, check=True)


def publish_generated(root, translate):
    """Stage a translation, then publish gen/ and symbols.json by rename.

    `translate(stage_dir)` writes the sources and raises on failure; the
    published tree is untouched in that case. Publishing is renames only, so
    a reader under the same lock never sees half a generation."""
    root = Path(root)
    recomp = root / "build/recomp"
    recomp.mkdir(parents=True, exist_ok=True)
    gen, old = recomp / "gen", recomp / "gen.old"
    stage = recomp / ("gen.new.%d" % os.getpid())
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    try:
        translate(stage)
        # x86.h sits beside the generated sources so #include "x86.h" resolves.
        shutil.copy(root / "runtime/x86.h", stage / "x86.h")
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




def run_translator(stage, game="populous"):
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/translate.py"), "--out", str(stage),
                    "--game", str(ROOT / "games" / game),
                    "--report", str(ROOT / "build/recomp/translate-report.json")], cwd=ROOT, check=True)


def texture_pack(game):
    """Compile the redistributable material-detail layer when its inputs are newer.
    Original-game replacement textures remain optional, locally prepared pack entries.
    A game without artwork has no texture pack."""
    detail = ROOT / "build/texture-pack/terrain-detail.popt"
    artwork = ROOT / "games" / game / "assets/terrain/materials-v1.png"
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
    parser.add_argument("--target", choices=sorted(TARGETS), default="app")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--preset", default=default_preset(system), help="CMake configure preset")
    parser.add_argument("--config", choices=("Release", "Debug"), default="Release")
    parser.add_argument("--game", default="populous", help="Directory under games/ whose game.toml configures the build")
    parser.add_argument("--stub", action="store_true",
                        help="Link the hosts against a stub translation (no game code; CI's build)")
    args = parser.parse_args(argv)
    if args.stub and (args.config == "Debug" or args.regenerate):
        parser.error("--stub cannot be combined with --config Debug or --regenerate")
    if not (ROOT / "games" / args.game / "game.toml").is_file():
        parser.error("No game config at games/%s/game.toml" % args.game)
    if args.target in MACOS_ONLY and not args.stub and (system or platform.system()) != "Darwin":
        parser.error("The %s host currently builds on macOS; use --target fixture, gen or plugins elsewhere"
                     % args.target)
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    return args, parser


def main():
    """Check inputs, translate under the build lock when needed, then configure and build."""
    args, parser = parse_args(sys.argv[1:])
    cfg = game_config.load(ROOT / "games" / args.game)
    # Regenerating needs the game and its listings.
    if args.regenerate and not (ROOT / cfg["game"]["developer_exe"]).is_file():
        parser.error("Prepare your own game installation with tools/setup.py first")
    preset = preset_name(args.preset, args.config, stub=args.stub)
    try:
        with buildlock.BuildLock(ROOT, "tools/build.py"):
            if args.target in NEEDS_GEN and args.regenerate:
                if not (ROOT / cfg["translate"]["listings"] / "functions.tsv").is_file():
                    parser.error("Translation listings are missing; run tools/setup.py without --link-only")
                publish_generated(ROOT, lambda stage: run_translator(stage, args.game))
            if args.target == "app":
                texture_pack(args.game)
            configure(preset, ["-DRECOMP_GAME=" + args.game])
            build(preset, TARGETS[args.target], args.jobs)
    except subprocess.CalledProcessError as error:
        parser.exit(error.returncode or 1, "Build failed; see the compiler output above.\n")
    except TimeoutError as error:
        parser.exit(1, "%s\n" % error)


if __name__ == "__main__":
    main()
