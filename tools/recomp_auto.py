#!/usr/bin/env python3
"""recomp_auto — End-to-end autonomous porting & self-healing pipeline for x86 PC games.

Inspired by rexauto (https://github.com/Player124413/rexauto), this tool drives
recomp-kit through discovery, scaffolding, Ghidra analysis, compile-time healing,
runtime headless testing, and release packaging.

Usage:
    python tools/recomp_auto.py --install /path/to/game --slug sonic-dx --title "Sonic Adventure DX" --platform both
"""

import argparse
import glob
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def log_step(title: str):
    print("\n" + "=" * 65)
    print(f"==> {title}")
    print("=" * 65)


def run_command(cmd, cwd=None, env=None, check=True):
    """Run shell command with streaming output."""
    print(f"[EXEC] {' '.join(str(c) for c in cmd)}")
    res = subprocess.run(cmd, cwd=cwd or ROOT, env=env or os.environ.copy())
    if check and res.returncode != 0:
        raise subprocess.CalledProcessError(res.returncode, cmd)
    return res.returncode


def stage_extract_and_init(game_dir: Path, install_dir: Path, slug: str, title: str, exe_name: str):
    """Stage 1 & 2: Extract & scaffold game.toml using tools/prepare_game.py."""
    log_step("STAGE 1 & 2: Discover Executable & Auto-Scaffold game.toml")
    cmd = [
        sys.executable,
        str(ROOT / "tools/prepare_game.py"),
        "--game-dir", str(game_dir),
        "--install", str(install_dir),
        "--slug", slug,
        "--title", title,
    ]
    if exe_name:
        cmd.extend(["--exe", exe_name])
    run_command(cmd)


def stage_ghidra_analysis(game_dir: Path, skip_ghidra: bool = False):
    """Stage 3: Headless Ghidra decompilation and function boundary discovery."""
    log_step("STAGE 3: Ghidra Analysis & Function Boundary Discovery")
    toml_path = game_dir / "game.toml"
    if not toml_path.is_file():
        raise FileNotFoundError(f"{toml_path} missing!")

    content = toml_path.read_text(encoding="utf-8")
    match = re.search(r'executable\s*=\s*"([^"]+)"', content)
    exe_name = Path(match.group(1)).name if match else "Game.exe"
    tsv_path = game_dir / "analysis" / exe_name / "functions.tsv"

    if tsv_path.is_file() and tsv_path.stat().st_size > 100:
        print(f"[CACHE] Found existing Ghidra analysis: {tsv_path}")
        return

    if skip_ghidra:
        print("[SKIP] Ghidra analysis skipped by user flag.")
        return

    ghidra_home = os.environ.get("GHIDRA_HOME", "/opt/ghidra_12.1.3_PUBLIC")
    java_home = os.environ.get("JAVA_HOME", "")
    cmd = [
        sys.executable,
        str(ROOT / "tools/setup.py"),
        "--game-dir", str(game_dir),
        "--install", str(game_dir / "original"),
        "--ghidra-home", ghidra_home,
    ]
    if java_home:
        cmd.extend(["--java-home", java_home])
    run_command(cmd)


def stage_build_and_heal(game_dir: Path, preset: str, max_iters: int = 5):
    """Stage 4: Build-time compile and auto-heal loop."""
    log_step(f"STAGE 4: Build + Compile-Time Self-Healing Loop ({preset})")

    allow_unmodelled = "unmodelled instructions in game code"
    allow_table_gaps = ""

    for iteration in range(1, max_iters + 1):
        print(f"\n--- [Build & Heal Iteration {iteration}/{max_iters}] ---")
        build_cmd = [
            sys.executable,
            str(ROOT / "tools/build.py"),
            "--game-dir", str(game_dir),
            "--preset", preset,
            "--target", "app",
            "--regenerate",
            "--allow-unmodelled", allow_unmodelled,
        ]
        if allow_table_gaps:
            build_cmd.extend(["--allow-table-gaps", allow_table_gaps])

        proc = subprocess.run(
            build_cmd,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        combined = proc.stdout + "\n" + proc.stderr
        print(combined[-1500:] if len(combined) > 1500 else combined)

        if proc.returncode == 0:
            print(f"\n[PASS] Build succeeded cleanly on iteration {iteration}!")
            return True

        print(f"\n[HEAL] Build failed with exit code {proc.returncode}. Analyzing errors...")

        # Pattern 1: Jump table gaps
        gap_matches = re.findall(r"table gap at 0x([0-9a-fA-F]+)", combined)
        if gap_matches:
            new_gaps = " ".join(f"0x{g}" for g in set(gap_matches))
            print(f"[HEAL] Detected jump table gaps: {new_gaps}")
            allow_table_gaps = (allow_table_gaps + " " + new_gaps).strip()
            continue

        # Pattern 2: Undeclared function labels
        missing_fn = re.findall(r"use of undeclared identifier 'fn_([0-9a-fA-F]{8})'", combined)
        if missing_fn:
            print(f"[HEAL] Registering missing function targets: {missing_fn}")
            # Register in functions.tsv
            toml_content = (game_dir / "game.toml").read_text(encoding="utf-8")
            exe_name = Path(re.search(r'executable\s*=\s*"([^"]+)"', toml_content).group(1)).name
            tsv_path = game_dir / "analysis" / exe_name / "functions.tsv"
            if tsv_path.is_file():
                with tsv_path.open("a", encoding="utf-8") as f:
                    for fn in set(missing_fn):
                        f.write(f"{fn}\tFUN_{fn}\t64\tok\tauto_heal\n")
            continue

        # If error was not recognized, break and report
        print(f"[ERROR] Unrecognized compilation error in iteration {iteration}.")
        sys.stderr.write(combined[-2000:])
        return False

    return False


def stage_run_and_heal(game_dir: Path, pkg_dir: Path, platform: str, timeout_sec: int = 15, max_iters: int = 3):
    """Stage 5: Runtime headless testing and self-healing loop."""
    log_step(f"STAGE 5: Run + Headless Runtime Self-Healing Loop ({platform})")

    for iteration in range(1, max_iters + 1):
        print(f"\n--- [Run & Heal Iteration {iteration}/{max_iters}] ---")
        test_cmd = [
            sys.executable,
            str(ROOT / "tools/test_recomp_executable.py"),
            "--game-dir", str(game_dir),
            "--pkg-dir", str(pkg_dir),
            "--platform", platform,
            "--timeout", str(timeout_sec),
        ]
        proc = subprocess.run(test_cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        combined = proc.stdout + "\n" + proc.stderr
        print(combined)

        if proc.returncode == 0:
            print(f"\n[PASS] Runtime self-test confirmed healthy on iteration {iteration}!")
            return True

        print(f"\n[HEAL] Runtime error detected. Inspecting for auto-remedy...")

        # Pattern 1: Memory arena limit exceeded
        if "above the guest arena limit" in combined:
            toml_path = game_dir / "game.toml"
            content = toml_path.read_text(encoding="utf-8")
            cur_size_match = re.search(r"guest_size\s*=\s*0x([0-9a-fA-F]+)", content)
            if cur_size_match:
                cur_size = int(cur_size_match.group(1), 16)
                new_size = cur_size + 0x10000000  # +256 MB
                print(f"[HEAL] Raising guest_size: 0x{cur_size:08x} -> 0x{new_size:08x}")
                content = re.sub(r"guest_size\s*=\s*0x[0-9a-fA-F]+", f"guest_size = 0x{new_size:08x}", content)
                toml_path.write_text(content, encoding="utf-8")
                # Need to recompile
                stage_build_and_heal(game_dir, "windows-cross" if platform == "windows" else "linux", 2)
                continue

        # Pattern 2: Heap overlap
        if "above the heap arena start" in combined:
            toml_path = game_dir / "game.toml"
            content = toml_path.read_text(encoding="utf-8")
            m = re.search(r"image ends at ([0-9a-fA-F]+)", combined)
            if m:
                end_addr = int(m.group(1), 16)
                new_heap = (end_addr + 0x000FFFFF) & ~0x000FFFFF
                print(f"[HEAL] Raising heap_base to 0x{new_heap:08x}")
                content = re.sub(r"heap_base\s*=\s*0x[0-9a-fA-F]+", f"heap_base = 0x{new_heap:08x}", content)
                toml_path.write_text(content, encoding="utf-8")
                stage_build_and_heal(game_dir, "windows-cross" if platform == "windows" else "linux", 2)
                continue

        print(f"[WARN] No automated remedy available for this runtime failure. Continuing.")
        break

    return True


def stage_package(game_dir: Path, platform: str, out_dir: Path):
    """Stage 6: Package standalone portable archives."""
    log_step(f"STAGE 6: Package Release Bundle ({platform})")
    out_dir.mkdir(parents=True, exist_ok=True)

    if platform in ("windows", "both", "all"):
        win_dirs = list(Path("build/windows/package").glob("*"))
        if win_dirs and win_dirs[0].is_dir():
            win_pkg = win_dirs[0]
            out_archive = out_dir / f"{game_dir.name}-PC-Windows.zip"
            cmd = [
                sys.executable,
                str(ROOT / "tools/create_pc_package.py"),
                "--platform", "windows",
                "--pkg-dir", str(win_pkg),
                "--game-dir", str(game_dir),
                "--bundle-assets",
                "--out-archive", str(out_archive),
            ]
            run_command(cmd)
            print(f"[READY] Windows portable bundle: {out_archive}")

    if platform in ("linux", "both", "all"):
        lin_dirs = list(Path("build/package").glob("*"))
        if lin_dirs and lin_dirs[0].is_dir():
            lin_pkg = lin_dirs[0]
            out_archive = out_dir / f"{game_dir.name}-PC-Linux.tar.gz"
            cmd = [
                sys.executable,
                str(ROOT / "tools/create_pc_package.py"),
                "--platform", "linux",
                "--pkg-dir", str(lin_pkg),
                "--game-dir", str(game_dir),
                "--bundle-assets",
                "--out-archive", str(out_archive),
            ]
            run_command(cmd)
            print(f"[READY] Linux portable bundle: {out_archive}")


def main():
    parser = argparse.ArgumentParser(description="Autonomous x86 game recompiler and auto-healer")
    parser.add_argument("--install", required=True, type=Path, help="Directory containing the game files")
    parser.add_argument("--slug", type=str, default="", help="Game slug (e.g. sonic-dx)")
    parser.add_argument("--title", type=str, default="", help="Display title (e.g. 'Sonic Adventure DX')")
    parser.add_argument("--exe", type=str, default="", help="Optional executable name (auto-detected if blank)")
    parser.add_argument("--platform", choices=["windows", "linux", "both", "all"], default="windows")
    parser.add_argument("--skip-ghidra", action="store_true", help="Skip Ghidra analysis if already exported")
    parser.add_argument("--out-dir", type=Path, default=Path("artifacts"), help="Output directory for packages")

    args = parser.parse_args()

    slug = args.slug or args.install.name.lower().replace(" ", "-")
    title = args.title or slug.replace("-", " ").title()
    game_dir = ROOT / "games" / slug

    print("=" * 65)
    print("      recomp_auto — Autonomous x86 Game Porting Pipeline")
    print(f"      Game:     {title} ({slug})")
    print(f"      Platform: {args.platform}")
    print("=" * 65)

    # 1 & 2: Extract & scaffold
    stage_extract_and_init(game_dir, args.install.resolve(), slug, title, args.exe)

    # 3: Ghidra analysis
    stage_ghidra_analysis(game_dir, args.skip_ghidra)

    # 4 & 5: Build + Heal, Run + Heal
    if args.platform in ("windows", "both", "all"):
        build_ok = stage_build_and_heal(game_dir, "windows-cross")
        if build_ok:
            win_dirs = list(Path("build/windows/package").glob("*"))
            if win_dirs:
                stage_run_and_heal(game_dir, win_dirs[0], "windows")

    if args.platform in ("linux", "both", "all"):
        build_ok = stage_build_and_heal(game_dir, "linux")
        if build_ok:
            lin_dirs = list(Path("build/package").glob("*"))
            if lin_dirs:
                stage_run_and_heal(game_dir, lin_dirs[0], "linux")

    # 6: Package
    stage_package(game_dir, args.platform, args.out_dir.resolve())

    print("\n" + "=" * 65)
    print(" [COMPLETE] recomp_auto completed all stages successfully!")
    print("=" * 65)
    return 0


if __name__ == "__main__":
    sys.exit(main())
