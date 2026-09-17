#!/usr/bin/env python3
"""Stage and analyse the Wine msvcrt sample (see ../README.md).

    tools/prepare.py --cc /path/to/i686-w64-mingw32-clang \\
        --wine-dlls /Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine/i386-windows \\
        --ghidra-home /path/to/ghidra_12.1.3_PUBLIC [--java-home ...]

Builds original/sample.exe from src/sample.c, copies Wine's msvcrt.dll to
original/, checks both against game.toml and exports Ghidra listings for
both into analysis/decompiled/. Then, from the kit:

    tools/build.py --game-dir <this directory> --regenerate --target headless
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT.parents[1]
GHIDRA_VERSION = "12.1.3"


def load_game_config():
    sys.path.insert(0, str(KIT / "tools"))
    import game_config
    return game_config.load(ROOT)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(path, expected):
    digest = sha256(path)
    if digest != expected:
        sys.exit("%s has SHA-256 %s; game.toml expects %s" % (path, digest, expected))


def ghidra_export(ghidra, env, project, name, image, output, listings):
    subprocess.run([
        str(ghidra / "support/analyzeHeadless"), str(project), name,
        "-import", str(image), "-deleteProject",
        "-scriptPath", str(KIT / "tools"),
        "-postScript", "ExportProgram.java", str(output),
    ], cwd=ROOT, env=env, check=True)
    if not (listings / "functions.tsv").is_file():
        sys.exit("Ghidra did not export a function index for %s" % image.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", type=Path, required=True, help="llvm-mingw's i686-w64-mingw32-clang")
    parser.add_argument("--wine-dlls", type=Path, required=True, help="Wine's lib/wine/i386-windows directory")
    parser.add_argument("--ghidra-home", type=Path, default=os.environ.get("GHIDRA_HOME"))
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    args = parser.parse_args()
    if not args.ghidra_home:
        sys.exit("Set --ghidra-home or GHIDRA_HOME to the extracted Ghidra %s directory" % GHIDRA_VERSION)
    ghidra = args.ghidra_home.expanduser().resolve()
    properties = ghidra / "Ghidra/application.properties"
    if not properties.is_file() or "application.version=%s\n" % GHIDRA_VERSION not in properties.read_text():
        sys.exit("Use Ghidra %s; set --ghidra-home to its extracted directory" % GHIDRA_VERSION)

    cfg = load_game_config()
    exe = cfg["developer_exe_path"]
    exe.parent.mkdir(parents=True, exist_ok=True)
    # No C runtime and no relocations: the kit maps the image at its base. No
    # build id or timestamp either, so game.toml's hash stays reproducible.
    subprocess.run([
        str(args.cc), "-O2", "-march=i386", "-fno-stack-protector", "-ffreestanding", "-nostdlib",
        "-Wl,--entry=_start", "-Wl,--image-base=0x400000", "-Wl,--disable-dynamicbase",
        "-Wl,--disable-reloc-section", "-Wl,--build-id=none", "-Wl,--no-insert-timestamp",
        "src/sample.c", "-lkernel32", "-o", str(exe.relative_to(ROOT)),
    ], cwd=ROOT, check=True)
    check(exe, cfg["game"]["sha256"])
    for module in cfg["aux_modules"]:
        shutil.copyfile(args.wine_dlls / module["name"], module["path"])
        check(module["path"], module["sha256"])

    env = dict(os.environ)
    if args.java_home:
        env["JAVA_HOME"] = str(args.java_home.expanduser().resolve())
        env["PATH"] = str(Path(env["JAVA_HOME"]) / "bin") + os.pathsep + env.get("PATH", "")
    env["MAXMEM"] = "4G"
    output = cfg["listings_path"].parent
    project = output.parent / "ghidra"
    project.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    ghidra_export(ghidra, env, project, "sample", exe, output, cfg["listings_path"])
    for module in cfg["aux_modules"]:
        ghidra_export(ghidra, env, project, "sample-" + module["key"], module["path"], output,
                      module["listings_path"])
    print("Listings ready. Next, from the kit: tools/build.py --game-dir %s --regenerate --target headless"
          % ROOT)


if __name__ == "__main__":
    main()
