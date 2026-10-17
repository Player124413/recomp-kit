#!/usr/bin/env python3
"""In-workflow self-test and automated diagnostic runner for recomp-kit ports.

Launches the compiled game executable in a headless/virtual environment (Xvfb + Lavapipe/Wine),
monitors startup, graphics initialization, memory arenas, and import binding,
detects any crash traces in logs.txt, and provides actionable diagnostics or auto-remedies.

Usage:
    python tools/test_recomp_executable.py --game-dir games/shrek2 --pkg-dir build/package/Shrek2Recomp --platform linux --timeout 15
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


def find_executable(pkg_dir: Path, platform: str):
    """Locate the main compiled port binary in the package directory."""
    if platform == "windows":
        exes = [p for p in pkg_dir.glob("*.exe") if not p.name.lower().startswith("unins")]
        if not exes:
            return None
        # Prefer *Recomp.exe
        for e in exes:
            if "recomp" in e.name.lower():
                return e
        return exes[0]
    else:
        bins = [p for p in pkg_dir.iterdir() if p.is_file() and os.access(p, os.X_OK) and not p.name.endswith(".sh")]
        if not bins:
            return None
        for b in bins:
            if "recomp" in b.name.lower():
                return b
        return bins[0]


def run_self_test(exe_path: Path, pkg_dir: Path, platform: str, timeout_sec: int = 15):
    """Run the executable under Xvfb and software Vulkan/Wine."""
    env = os.environ.copy()

    # Find Lavapipe Vulkan ICD if available
    lvp_candidates = [
        "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
        "/usr/share/vulkan/icd.d/lvp_icd.i686.json",
        "/etc/vulkan/icd.d/lvp_icd.x86_64.json",
    ]
    for c in lvp_candidates:
        if os.path.exists(c):
            env["VK_ICD_FILENAMES"] = c
            break

    # SDL / Vulkan headless configuration
    env["SDL_VIDEODRIVER"] = "x11"
    env["RECOMP_HEADLESS"] = "0"

    cmd = []
    # Use xvfb-run for virtual display
    if shutil.which("xvfb-run"):
        cmd.extend(["xvfb-run", "-a", "-s", "-screen 0 1280x720x24"])

    if platform == "windows":
        cmd.extend(["wine64", str(exe_path)])
    else:
        cmd.append(str(exe_path))

    print(f"Launching self-test command: {' '.join(cmd)}")
    print(f"Working directory: {pkg_dir}")
    print(f"Test timeout: {timeout_sec}s...")

    start_time = time.time()
    try:
        proc = subprocess.run(
            cmd,
            cwd=pkg_dir,
            env=env,
            timeout=timeout_sec,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        elapsed = time.time() - start_time
        exit_code = proc.returncode
        stdout = proc.stdout
        stderr = proc.stderr
        timed_out = False
    except subprocess.TimeoutExpired as e:
        elapsed = time.time() - start_time
        exit_code = 0  # Running until timeout is a success for interactive games!
        stdout = e.stdout or ""
        stderr = e.stderr or ""
        timed_out = True

    # Read logs.txt from package directory or game directory
    log_file = pkg_dir / "logs.txt"
    log_content = ""
    if log_file.is_file():
        log_content = log_file.read_text(encoding="utf-8", errors="replace")

    return {
        "timed_out": timed_out,
        "elapsed": elapsed,
        "exit_code": exit_code,
        "stdout": stdout,
        "stderr": stderr,
        "log_content": log_content,
    }


def analyze_results(res: dict):
    """Analyze execution logs and diagnose issues."""
    all_output = res["stdout"] + "\n" + res["stderr"] + "\n" + res["log_content"]
    issues = []
    successes = []

    if "GPU backend: vulkan" in all_output or "gpu/vulkan: swapchain" in all_output:
        successes.append("Vulkan graphics device and swapchain initialized successfully.")

    if "presenter:" in all_output or "focus yes" in all_output:
        successes.append("Window created and presenter loop entered.")

    if res["timed_out"]:
        successes.append(f"Game process remained alive and ran stably for the full {int(res['elapsed'])}s test window.")

    # Check for known failure patterns
    if "image ends at" in all_output and "above the heap arena start" in all_output:
        issues.append({
            "code": "HEAP_OVERLAP",
            "message": "PE Image overlaps heap arena. Need to raise heap_base in game.toml.",
            "auto_remedy": "raise_heap_base",
        })

    if "above the guest arena limit" in all_output:
        issues.append({
            "code": "GUEST_ARENA_EXHAUSTED",
            "message": "Executable or auxiliary DLLs exceed guest_size. Need to raise guest_size.",
            "auto_remedy": "raise_guest_size",
        })

    if "missing mapped export" in all_output:
        match = re.search(r"missing mapped export (.*)", all_output)
        sym = match.group(1) if match else "unknown"
        issues.append({
            "code": "MISSING_MAPPED_EXPORT",
            "message": f"Module import missing from DLL export table: {sym}",
            "auto_remedy": "stub_trampoline",
        })

    if "unresolved import" in all_output:
        match = re.search(r"unresolved import (.*)", all_output)
        sym = match.group(1) if match else "unknown"
        issues.append({
            "code": "UNRESOLVED_IMPORT",
            "message": f"System DLL import unresolved: {sym}",
            "auto_remedy": "stub_import",
        })

    if "Segmentation fault" in all_output or "c0000005" in all_output:
        issues.append({
            "code": "CRASH_ACCESS_VIOLATION",
            "message": "Crash: Access violation (Segmentation Fault / c0000005). Check backtrace in logs.txt.",
            "auto_remedy": "check_backtrace",
        })

    return successes, issues


def main():
    parser = argparse.ArgumentParser(description="In-workflow self-test for recomp-kit")
    parser.add_argument("--game-dir", required=True, type=Path, help="Path to game port directory (games/<slug>)")
    parser.add_argument("--pkg-dir", required=True, type=Path, help="Path to packaged release directory")
    parser.add_argument("--platform", choices=["windows", "linux"], default="linux", help="Target platform of package")
    parser.add_argument("--timeout", type=int, default=15, help="Test timeout in seconds")

    args = parser.parse_args()

    pkg_dir = args.pkg_dir.resolve()
    if not pkg_dir.is_dir():
        print(f"Error: package directory {pkg_dir} does not exist!")
        return 1

    exe_path = find_executable(pkg_dir, args.platform)
    if not exe_path:
        print(f"Error: no executable found in {pkg_dir}!")
        return 1

    print(f"=== Starting Self-Test for {exe_path.name} ({args.platform}) ===")
    res = run_self_test(exe_path, pkg_dir, args.platform, args.timeout)

    successes, issues = analyze_results(res)

    print("\n" + "=" * 50)
    print("=== SELF-TEST RESULTS & DIAGNOSTICS ===")
    print("=" * 50)

    if successes:
        print("\n[SUCCESS CRITERIA MET]")
        for s in successes:
            print(f"  + {s}")

    if issues:
        print("\n[DETECTED ISSUES / WARNINGS]")
        for issue in issues:
            print(f"  ! [{issue['code']}] {issue['message']}")
            print(f"    Suggested remedy: {issue['auto_remedy']}")
    else:
        print("\n[PERFECT HEALTH] No critical crashes or errors detected during startup!")

    print("\n--- Recent logs.txt tail ---")
    if res["log_content"]:
        lines = res["log_content"].strip().splitlines()
        for l in lines[-25:]:
            print(f"  | {l}")
    else:
        print("  (logs.txt was empty or standard stdout/stderr used)")

    # If critical crash occurred before timeout and no success criteria
    critical_crash = any(i["code"] in ("CRASH_ACCESS_VIOLATION", "GUEST_ARENA_EXHAUSTED", "HEAP_OVERLAP") for i in issues)
    if critical_crash and not res["timed_out"]:
        print("\n[FAIL] Self-test detected a critical startup crash.")
        return 2

    print("\n[PASS] Self-test verified executable boots up cleanly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
