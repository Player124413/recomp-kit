#!/usr/bin/env python3
"""Package Shrek 2 PC release artifacts for Windows or Linux.

Usage:
    python3 tools/create_pc_package.py --platform windows --pkg-dir build/windows/package/Shrek2Recomp --game-dir games/shrek2 --bundle-assets --out-archive Shrek2Recomp-PC-Windows.zip
"""

import argparse
import os
from pathlib import Path
import shutil
import sys
import tarfile
import zipfile

BATCH_SCRIPT = """@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

title Shrek 2 Recomp

echo ========================================================
echo   Starting Shrek 2: The Game (PC Recomp)
echo ========================================================

set "EXE_PATH="
if exist "%~dp0System\\Game.exe" (
    set "EXE_PATH=%~dp0System\\Game.exe"
) else if exist "%~dp0Game.exe" (
    set "EXE_PATH=%~dp0Game.exe"
) else if exist "%~dp0game\\System\\Game.exe" (
    set "EXE_PATH=%~dp0game\\System\\Game.exe"
) else if exist "%~dp0game\\Game.exe" (
    set "EXE_PATH=%~dp0game\\Game.exe"
)

if defined EXE_PATH (
    echo [INFO] Detected game files at: !EXE_PATH!
    set "EXE_ARG=--exe "!EXE_PATH!""
) else (
    echo [INFO] No Game.exe detected in current folder.
    echo [INFO] Opening launcher / file picker...
    set "EXE_ARG="
)

echo [INFO] Output and errors are logged to: logs.txt
echo.

"%~dp0Shrek2Recomp.exe" !EXE_ARG! %*
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

SHELL_SCRIPT = """#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"

EXE_PATH=""
if [ -f "System/Game.exe" ]; then
    EXE_PATH="System/Game.exe"
elif [ -f "Game.exe" ]; then
    EXE_PATH="Game.exe"
elif [ -f "game/System/Game.exe" ]; then
    EXE_PATH="game/System/Game.exe"
elif [ -f "game/Game.exe" ]; then
    EXE_PATH="game/Game.exe"
fi

EXE_ARG=""
if [ -n "$EXE_PATH" ]; then
    echo "[INFO] Detected game files at: $EXE_PATH"
    EXE_ARG="--exe $EXE_PATH"
fi

set +e
./Shrek2Recomp $EXE_ARG "$@" 2>&1 | tee -a logs.txt
EXIT_CODE=${PIPESTATUS[0]}

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
    parser.add_argument("--out-archive", type=Path, required=True)
    args = parser.parse_args()

    pkg_dir = args.pkg_dir.resolve()
    game_dir = args.game_dir.resolve()
    out_archive = args.out_archive.resolve()
    original_dir = game_dir / "original"

    if not pkg_dir.is_dir():
        print(f"Error: Package directory does not exist: {pkg_dir}", file=sys.stderr)
        return 1

    # Bundle game assets if requested
    if args.bundle_assets and original_dir.is_dir():
        print(f"Bundling game assets from {original_dir} into {pkg_dir}...")
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

    # Pre-configure profile/game-path.txt if System/Game.exe or Game.exe exists in package
    profile_dir = pkg_dir / "profile"
    profile_dir.mkdir(parents=True, exist_ok=True)
    game_path_file = profile_dir / "game-path.txt"

    if (pkg_dir / "System" / "Game.exe").is_file():
        game_path_file.write_text("System/Game.exe\n", encoding="utf-8")
        print("Configured profile/game-path.txt -> System/Game.exe")
    elif (pkg_dir / "Game.exe").is_file():
        game_path_file.write_text("Game.exe\n", encoding="utf-8")
        print("Configured profile/game-path.txt -> Game.exe")

    # Add launcher scripts
    if args.platform == "windows":
        bat_file = pkg_dir / "Play-Shrek2.bat"
        # Write batch file with CRLF line endings
        bat_file.write_bytes(BATCH_SCRIPT.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))
        print(f"Created launcher script: {bat_file}")
    elif args.platform == "linux":
        sh_file = pkg_dir / "start-shrek2.sh"
        sh_file.write_text(SHELL_SCRIPT, encoding="utf-8")
        sh_file.chmod(0o755)
        binary = pkg_dir / "Shrek2Recomp"
        if binary.is_file():
            binary.chmod(0o755)
        print(f"Created launcher script: {sh_file}")

    # Create release archive
    out_archive.parent.mkdir(parents=True, exist_ok=True)
    if out_archive.exists():
        out_archive.unlink()

    arcname_root = pkg_dir.name
    print(f"Creating archive: {out_archive} containing {arcname_root}/ ...")

    if args.platform == "windows" or out_archive.suffix.lower() == ".zip":
        with zipfile.ZipFile(out_archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for root, dirs, files in os.walk(str(pkg_dir)):
                for file in files:
                    file_path = Path(root) / file
                    rel = file_path.relative_to(pkg_dir)
                    zf.write(file_path, arcname=f"{arcname_root}/{rel.as_posix()}")
    else:
        with tarfile.open(out_archive, "w:gz") as tf:
            tf.add(pkg_dir, arcname=arcname_root)

    print(f"Successfully created archive: {out_archive} ({out_archive.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
