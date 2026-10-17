#!/usr/bin/env python3
"""Package PC release artifacts for Windows or Linux (universal across any game).

Automatically bundles full game files when original/ is <= 5.5 GB, and switches to
lightweight binaries/configs-only bundling (.exe, .dll, .ini, etc.) when > 5.5 GB.

Usage:
    python3 tools/create_pc_package.py --platform windows --pkg-dir build/windows/package/Shrek2Recomp --game-dir games/shrek2 --bundle-assets --out-archive Shrek2Recomp-PC-Windows.zip
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import sys
import tarfile
import zipfile

LIGHTWEIGHT_EXTENSIONS = {
    ".exe",
    ".dll",
    ".ini",
    ".cfg",
    ".int",
    ".def",
    ".u",
    ".asi",
    ".flt",
    ".manifest",
    ".xml",
    ".json",
    ".txt",
    ".lua",
    ".bat",
    ".sh",
}

MAX_LIGHTWEIGHT_FILE_BYTES = 64 * 1024 * 1024  # 64 MB


def parse_game_toml(game_dir: Path) -> dict:
    info = {
        "title": "Shrek 2: The Game",
        "slug": "shrek2",
        "executable": "Game.exe",
    }
    toml_path = game_dir / "game.toml"
    if not toml_path.is_file():
        return info
    text = toml_path.read_text(encoding="utf-8", errors="ignore")
    for key in ("title", "slug", "executable"):
        m = re.search(rf'^\s*{key}\s*=\s*"([^"]+)"', text, re.MULTILINE)
        if m:
            info[key] = m.group(1)
    return info


def detect_recomp_binary(pkg_dir: Path, platform: str) -> str:
    if platform == "windows":
        exes = sorted(
            p.name
            for p in pkg_dir.iterdir()
            if p.is_file() and p.name.lower().endswith("recomp.exe")
        )
        if exes:
            return exes[0]
        return "Shrek2Recomp.exe"
    else:
        bins = sorted(
            p.name
            for p in pkg_dir.iterdir()
            if p.is_file()
            and not p.name.endswith((".sh", ".txt", ".spv", ".so", ".dll", ".exe", ".ini"))
        )
        if bins:
            return bins[0]
        return "Shrek2Recomp"


def compute_dir_size(directory: Path) -> int:
    total = 0
    if not directory.is_dir():
        return 0
    for root, _, files in os.walk(str(directory)):
        for f in files:
            if f.startswith("."):
                continue
            fp = Path(root) / f
            try:
                total += fp.stat().st_size
            except OSError:
                pass
    return total


def copy_lightweight_files(src_dir: Path, dst_dir: Path) -> int:
    copied = 0
    for root, _, files in os.walk(str(src_dir)):
        for f in files:
            if f.startswith("."):
                continue
            src_file = Path(root) / f
            ext = src_file.suffix.lower()
            try:
                fsize = src_file.stat().st_size
            except OSError:
                continue
            if ext in LIGHTWEIGHT_EXTENSIONS and fsize <= MAX_LIGHTWEIGHT_FILE_BYTES:
                rel = src_file.relative_to(src_dir)
                dest_file = dst_dir / rel
                dest_file.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src_file, dest_file)
                copied += 1
    return copied


def build_batch_script(title: str, recomp_exe: str, game_exe: str) -> str:
    return f"""@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

title {title} (PC Recomp)

echo ========================================================
echo   Starting {title} (PC Recomp)
echo ========================================================

set "EXE_PATH="
if exist "%~dp0System\\{game_exe}" (
    set "EXE_PATH=%~dp0System\\{game_exe}"
) else if exist "%~dp0Binaries\\{game_exe}" (
    set "EXE_PATH=%~dp0Binaries\\{game_exe}"
) else if exist "%~dp0Binaries\\Win32\\{game_exe}" (
    set "EXE_PATH=%~dp0Binaries\\Win32\\{game_exe}"
) else if exist "%~dp0Bin32\\{game_exe}" (
    set "EXE_PATH=%~dp0Bin32\\{game_exe}"
) else if exist "%~dp0bin\\{game_exe}" (
    set "EXE_PATH=%~dp0bin\\{game_exe}"
) else if exist "%~dp0{game_exe}" (
    set "EXE_PATH=%~dp0{game_exe}"
) else if exist "%~dp0game\\System\\{game_exe}" (
    set "EXE_PATH=%~dp0game\\System\\{game_exe}"
) else if exist "%~dp0game\\Binaries\\{game_exe}" (
    set "EXE_PATH=%~dp0game\\Binaries\\{game_exe}"
) else if exist "%~dp0game\\Binaries\\Win32\\{game_exe}" (
    set "EXE_PATH=%~dp0game\\Binaries\\Win32\\{game_exe}"
) else if exist "%~dp0game\\{game_exe}" (
    set "EXE_PATH=%~dp0game\\{game_exe}"
)

if defined EXE_PATH (
    echo [INFO] Detected game files at: !EXE_PATH!
    set "EXE_ARG=--exe "!EXE_PATH!""
) else (
    echo [INFO] No {game_exe} detected in current folder.
    echo [INFO] Opening launcher / file picker...
    set "EXE_ARG="
)

echo [INFO] Output and errors are logged to: logs.txt
echo.

"%~dp0{recomp_exe}" !EXE_ARG! %*
set EXIT_CODE=!errorlevel!

if !EXIT_CODE! neq 0 (
    echo.
    echo ========================================================
    echo [ERROR] Game exited with code !EXIT_CODE!.
    echo ========================================================
    if exist "%~dp0logs.txt" (
        echo --- Contents of logs.txt ---
        type "%~dp0logs.txt"
        echo ----------------------------
    ) else (
        echo logs.txt was not created.
    )
    echo.
    echo Press any key to exit...
    pause >nul
)
"""


def build_shell_script(recomp_bin: str, game_exe: str) -> str:
    return f"""#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"

EXE_PATH=""
for candidate in \\
    "System/{game_exe}" \\
    "Binaries/{game_exe}" \\
    "Binaries/Win32/{game_exe}" \\
    "Bin32/{game_exe}" \\
    "bin/{game_exe}" \\
    "{game_exe}" \\
    "game/System/{game_exe}" \\
    "game/Binaries/{game_exe}" \\
    "game/Binaries/Win32/{game_exe}" \\
    "game/{game_exe}"; do
    if [ -f "$candidate" ]; then
        EXE_PATH="$candidate"
        break
    fi
done

EXE_ARG=""
if [ -n "$EXE_PATH" ]; then
    echo "[INFO] Detected game files at: $EXE_PATH"
    EXE_ARG="--exe $EXE_PATH"
fi

set +e
./{recomp_bin} $EXE_ARG "$@" 2>&1 | tee -a logs.txt
EXIT_CODE=${{PIPESTATUS[0]}}

if [ "$EXIT_CODE" -ne 0 ]; then
    echo "[ERROR] Exited with code $EXIT_CODE. See logs.txt for details."
fi
exit $EXIT_CODE
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=["windows", "linux"], required=True)
    parser.add_argument("--pkg-dir", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, default=Path("games/shrek2"))
    parser.add_argument("--bundle-assets", action="store_true")
    parser.add_argument(
        "--max-full-bundle-gb",
        type=float,
        default=5.5,
        help="Max size in GB to bundle all assets; above this (6 GB+), only .exe/.dll/.ini are bundled.",
    )
    parser.add_argument("--out-archive", type=Path, required=True)
    args = parser.parse_args()

    pkg_dir = args.pkg_dir.resolve()
    game_dir = args.game_dir.resolve()
    out_archive = args.out_archive.resolve()
    original_dir = game_dir / "original"

    if not pkg_dir.is_dir():
        print(f"Error: Package directory does not exist: {pkg_dir}", file=sys.stderr)
        return 1

    info = parse_game_toml(game_dir)
    game_exe = info["executable"]
    game_title = info["title"]
    game_slug = info["slug"]
    recomp_bin = detect_recomp_binary(pkg_dir, args.platform)

    # Bundle game assets if requested
    if args.bundle_assets and original_dir.is_dir():
        orig_bytes = compute_dir_size(original_dir)
        orig_gb = orig_bytes / (1024 ** 3)
        max_bytes = int(args.max_full_bundle_gb * (1024 ** 3))

        if orig_bytes <= max_bytes:
            print(
                f"Game files size is {orig_gb:.2f} GB (<= {args.max_full_bundle_gb:.1f} GB limit). "
                f"Bundling full game from {original_dir} into {pkg_dir}..."
            )
            for item in original_dir.iterdir():
                if item.name.startswith("."):
                    continue
                dest = pkg_dir / item.name
                if item.is_dir():
                    print(f"  Copying directory {item.name}/...")
                    if dest.exists():
                        shutil.rmtree(dest)
                    shutil.copytree(item, dest, dirs_exist_ok=True)
                elif item.is_file():
                    print(f"  Copying file {item.name}...")
                    shutil.copy2(item, dest)
        else:
            print(
                f"Game files size is {orig_gb:.2f} GB (> {args.max_full_bundle_gb:.1f} GB limit). "
                f"Bundling ONLY lightweight binaries & configs (.exe, .dll, .ini, etc.)..."
            )
            copied = copy_lightweight_files(original_dir, pkg_dir)
            print(f"  Copied {copied} lightweight binary/config files (skipped heavy assets).")

    # Pre-configure profile/game-path.txt if executable exists in package
    profile_dir = pkg_dir / "profile"
    profile_dir.mkdir(parents=True, exist_ok=True)
    game_path_file = profile_dir / "game-path.txt"

    for rel_cand in (
        f"System/{game_exe}",
        f"Binaries/{game_exe}",
        f"Binaries/Win32/{game_exe}",
        f"Bin32/{game_exe}",
        f"bin/{game_exe}",
        game_exe,
    ):
        if (pkg_dir / rel_cand).is_file():
            game_path_file.write_text(f"{rel_cand}\n", encoding="utf-8")
            print(f"Configured profile/game-path.txt -> {rel_cand}")
            break

    # Add launcher scripts
    if args.platform == "windows":
        bat_name = "Play-Shrek2.bat" if game_slug == "shrek2" else f"Play-{pkg_dir.name}.bat"
        bat_file = pkg_dir / bat_name
        batch_script = build_batch_script(game_title, recomp_bin, game_exe)
        bat_file.write_bytes(batch_script.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))
        print(f"Created launcher script: {bat_file}")
    elif args.platform == "linux":
        sh_name = "start-shrek2.sh" if game_slug == "shrek2" else f"start-{game_slug}.sh"
        sh_file = pkg_dir / sh_name
        shell_script = build_shell_script(recomp_bin, game_exe)
        sh_file.write_text(shell_script, encoding="utf-8")
        sh_file.chmod(0o755)
        binary = pkg_dir / recomp_bin
        if binary.is_file():
            binary.chmod(0o755)
        print(f"Created launcher script: {sh_file}")

    # Create release archive
    out_archive.parent.mkdir(parents=True, exist_ok=True)
    if out_archive.exists():
        out_archive.unlink()

    arcname_root = pkg_dir.name
    pkg_bytes = compute_dir_size(pkg_dir)
    # Use fast compression for large multi-GB bundles (> 500 MB) so 1-5 GB games archive quickly
    zip_level = 1 if pkg_bytes > 500 * 1024 * 1024 else 6
    print(
        f"Creating archive: {out_archive} containing {arcname_root}/ "
        f"({pkg_bytes / (1024 ** 2):.1f} MB, compresslevel={zip_level}) ..."
    )

    if args.platform == "windows" or out_archive.suffix.lower() == ".zip":
        with zipfile.ZipFile(out_archive, "w", zipfile.ZIP_DEFLATED, compresslevel=zip_level) as zf:
            for root, dirs, files in os.walk(str(pkg_dir)):
                for file in files:
                    file_path = Path(root) / file
                    rel = file_path.relative_to(pkg_dir)
                    zf.write(file_path, arcname=f"{arcname_root}/{rel.as_posix()}")
    else:
        with tarfile.open(out_archive, "w:gz", compresslevel=zip_level) as tf:
            tf.add(pkg_dir, arcname=arcname_root)

    print(f"Successfully created archive: {out_archive} ({out_archive.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
