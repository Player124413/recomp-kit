#!/usr/bin/env python3
"""Universal game preparation and auto-tuning tool for recomp-kit.

Inspects any x86 PC game installation (Sonic Adventure DX, Shrek 2, GTA, etc.),
auto-detects the main game executable and auxiliary DLLs, extracts PE metadata,
calculates required guest memory layout, and creates/updates game.toml.

Usage:
    python tools/prepare_game.py --game-dir games/sonic --install /path/to/sonic_dx --title "Sonic Adventure DX"
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import sys

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib


IGNORE_EXE_KEYWORDS = [
    "unins", "setup", "autorun", "crash", "patch", "directx",
    "vcredist", "dxsetup", "config", "setuphlp", "launcher", "update"
]


def parse_pe32(data: bytes):
    """Parse PE32 headers from raw bytes without external dependencies."""
    if len(data) < 64 or data[:2] != b"MZ":
        return None
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 + 68 > len(data):
        return None
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        return None

    # COFF header
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    # 0x14C = IMAGE_FILE_MACHINE_I386 (32-bit x86)
    if machine != 0x14C:
        return None

    num_sections = struct.unpack_from("<H", data, pe_offset + 6)[0]
    opt_header_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    opt_offset = pe_offset + 24

    # Optional header magic: 0x10b for PE32 (32-bit)
    magic = struct.unpack_from("<H", data, opt_offset)[0]
    if magic != 0x10B:
        return None

    entry_rva = struct.unpack_from("<I", data, opt_offset + 16)[0]
    image_base = struct.unpack_from("<I", data, opt_offset + 28)[0]
    size_of_image = struct.unpack_from("<I", data, opt_offset + 56)[0]

    # Section headers
    sections = []
    sec_offset = opt_offset + opt_header_size
    for _ in range(num_sections):
        if sec_offset + 40 > len(data):
            break
        name_bytes = data[sec_offset : sec_offset + 8].rstrip(b"\0")
        name = name_bytes.decode("latin1", errors="replace")
        virt_size = struct.unpack_from("<I", data, sec_offset + 8)[0]
        virt_addr = struct.unpack_from("<I", data, sec_offset + 12)[0]
        sections.append({"name": name, "rva": virt_addr, "size": virt_size})
        sec_offset += 40

    return {
        "image_base": image_base,
        "entry_point": image_base + entry_rva,
        "size_of_image": size_of_image,
        "sections": sections,
    }


def find_executables(install_dir: Path):
    """Find and rank all 32-bit PE executables in install_dir."""
    candidates = []
    for root, _, files in os.walk(install_dir):
        for f in files:
            if f.lower().endswith(".exe"):
                p = Path(root) / f
                try:
                    data = p.read_bytes()
                    pe = parse_pe32(data)
                    if pe:
                        rel = p.relative_to(install_dir)
                        score = 0
                        name_lower = f.lower()
                        # Penalize utility / uninstaller executables
                        if any(kw in name_lower for kw in IGNORE_EXE_KEYWORDS):
                            score -= 100
                        # Reward typical game executables
                        if name_lower in ("game.exe", "sonic.exe", "sonic_dx.exe", "main.exe", "app.exe"):
                            score += 50
                        # Larger files are more likely the main game code
                        score += min(len(data) // 100000, 40)
                        # Root or System/ or bin/ are top tier
                        depth = len(rel.parts)
                        if depth <= 2:
                            score += 20
                        candidates.append({
                            "path": p,
                            "rel": rel,
                            "name": f,
                            "size": len(data),
                            "score": score,
                            "pe": pe,
                            "sha256": hashlib.sha256(data).hexdigest(),
                        })
                except Exception:
                    continue
    candidates.sort(key=lambda x: x["score"], reverse=True)
    return candidates


def scan_auxiliary_dlls(base_dir: Path):
    """Scan all 32-bit DLLs in base_dir and subdirectories."""
    dlls = []
    for root, _, files in os.walk(base_dir):
        for f in files:
            if f.lower().endswith(".dll"):
                p = Path(root) / f
                try:
                    data = p.read_bytes()
                    pe = parse_pe32(data)
                    if pe:
                        dlls.append({
                            "path": p,
                            "name": f,
                            "pe": pe,
                            "size": len(data),
                        })
                except Exception:
                    continue
    return dlls


def calculate_guest_memory(main_pe, dlls):
    """Calculate needed guest arena size and heap base."""
    max_va = main_pe["image_base"] + main_pe["size_of_image"]
    for d in dlls:
        d_end = d["pe"]["image_base"] + d["pe"]["size_of_image"]
        if d_end > max_va:
            max_va = d_end

    # Round max virtual address up to next 64 MB
    arena_needed = (max_va + 0x07FFFFFF) & ~0x03FFFFFF
    # Minimum 512 MB guest arena, default 768 MB or 1 GB if needed
    guest_size = max(0x20000000, arena_needed + 0x08000000)

    # Calculate heap base
    main_base = main_pe["image_base"]
    main_end = main_base + main_pe["size_of_image"]
    if main_base >= 0x10000000:
        # High base executable (like UE2 Game.exe at 0x10900000)
        # Heap remains at 0x01000000
        heap_base = 0x01000000
    else:
        # Normal image base (e.g. 0x00400000)
        heap_base = max(0x01000000, (main_end + 0x000FFFFF) & ~0x000FFFFF)

    return guest_size, heap_base


def create_or_update_game_toml(
    toml_path: Path,
    slug: str,
    title: str,
    bundle_id: str,
    exe_rel_path: str,
    sha256: str,
    image_base: int,
    entry_point: int,
    guest_size: int,
    heap_base: int,
    developer_exe_rel: str,
):
    """Generate or update game.toml with complete metadata and controls."""
    content = f"""# {title} recomp-kit configuration
[game]
name = "{slug}"
title = "{title}"
bundle_id = "{bundle_id}"
executable = "{exe_rel_path}"
sha256 = "{sha256}"
image_base = 0x{image_base:08x}
entry_point = 0x{entry_point:08x}
guest_root = 'C:\\{slug}'
developer_exe = "{developer_exe_rel}"
windows_version = "5.1"
guest_size = 0x{guest_size:08x}
heap_base = 0x{heap_base:08x}

[controls]
# Left stick and D-pad mapped to WASD
left_stick = "wasd"
dpad = "wasd"
# Right stick mapped to mouse
right_stick = "mouse"

# Action buttons
button_a = "key:Space"
button_b = "key:Escape"
button_x = "mouse_left"
button_y = "mouse_right"

# Bumpers / Triggers
button_l1 = "key:LCtrl"
button_r1 = "key:Return"
button_l2 = "key:LShift"
button_r2 = "mouse_left"

# Menu & Navigation
button_start = "key:Return"
button_select = "key:Tab"
"""
    toml_path.parent.mkdir(parents=True, exist_ok=True)
    toml_path.write_text(content, encoding="utf-8")
    print(f"Generated/Updated {toml_path}")


def main():
    parser = argparse.ArgumentParser(description="Universal game preparation for recomp-kit")
    parser.add_argument("--game-dir", required=True, type=Path, help="Directory for the game port (e.g. games/sonic)")
    parser.add_argument("--install", required=True, type=Path, help="Directory containing the game files")
    parser.add_argument("--exe", type=str, default="", help="Specific executable name to use (e.g. sonic.exe)")
    parser.add_argument("--slug", type=str, default="", help="Game slug identifier (e.g. sonic-dx)")
    parser.add_argument("--title", type=str, default="", help="Display title (e.g. 'Sonic Adventure DX')")
    parser.add_argument("--bundle-id", type=str, default="", help="Bundle / package ID (e.g. 'dev.recompkit.sonicdx')")

    args = parser.parse_args()

    game_dir = args.game_dir.resolve()
    install_dir = args.install.resolve()

    if not install_dir.is_dir():
        print(f"Error: install directory {install_dir} does not exist!")
        return 1

    slug = args.slug or game_dir.name
    title = args.title or slug.replace("-", " ").title()
    bundle_id = args.bundle_id or f"dev.recompkit.{slug.replace('-', '')}"

    print(f"=== Universal Recomp Preparation: {title} ({slug}) ===")
    print(f"Install directory: {install_dir}")
    print(f"Port directory:    {game_dir}")

    # Discover executables
    candidates = find_executables(install_dir)
    if not candidates:
        print(f"Error: No 32-bit x86 PE executables (.exe) found in {install_dir}!")
        return 1

    chosen = None
    if args.exe:
        for c in candidates:
            if c["name"].lower() == args.exe.lower() or str(c["rel"]).lower() == args.exe.lower():
                chosen = c
                break
        if not chosen:
            print(f"Warning: requested executable '{args.exe}' not found among candidates.")

    if not chosen:
        chosen = candidates[0]

    print(f"\nTarget Executable selected: {chosen['rel']}")
    print(f"  - Size: {chosen['size']:,} bytes")
    print(f"  - SHA256: {chosen['sha256']}")
    print(f"  - Image Base:  0x{chosen['pe']['image_base']:08x}")
    print(f"  - Entry Point: 0x{chosen['pe']['entry_point']:08x}")
    print(f"  - Size of Image: 0x{chosen['pe']['size_of_image']:08x}")

    # Scan auxiliary DLLs
    exe_dir = chosen["path"].parent
    dlls = scan_auxiliary_dlls(install_dir)
    print(f"\nScanned {len(dlls)} auxiliary 32-bit DLL modules in game directory.")

    # Calculate guest memory
    guest_size, heap_base = calculate_guest_memory(chosen["pe"], dlls)
    print(f"\nCalculated memory layout:")
    print(f"  - Guest arena size: 0x{guest_size:08x} ({guest_size // (1024*1024)} MB)")
    print(f"  - Heap base:        0x{heap_base:08x}")

    # Stage executable in games/<slug>/original/<rel_path>
    stage_orig = game_dir / "original"
    target_staged_exe = stage_orig / chosen["rel"]
    target_staged_exe.parent.mkdir(parents=True, exist_ok=True)
    if not target_staged_exe.is_file() or target_staged_exe.stat().st_size != chosen["path"].stat().st_size:
        shutil.copy2(chosen["path"], target_staged_exe)
        print(f"Copied {chosen['name']} to {target_staged_exe}")

    developer_exe_rel = str(Path("original") / chosen["rel"]).replace("\\", "/")
    exe_rel_path = str(chosen["rel"]).replace("\\", "/")

    # Create / update game.toml
    toml_path = game_dir / "game.toml"
    create_or_update_game_toml(
        toml_path=toml_path,
        slug=slug,
        title=title,
        bundle_id=bundle_id,
        exe_rel_path=exe_rel_path,
        sha256=chosen["sha256"],
        image_base=chosen["pe"]["image_base"],
        entry_point=chosen["pe"]["entry_point"],
        guest_size=guest_size,
        heap_base=heap_base,
        developer_exe_rel=developer_exe_rel,
    )

    # Create empty globals.toml if missing
    globals_path = game_dir / "globals.toml"
    if not globals_path.is_file():
        globals_path.write_text("# Game global variables (optional overrides)\n", encoding="utf-8")

    # Ensure analysis directory exists
    analysis_dir = game_dir / "analysis" / chosen["name"]
    analysis_dir.mkdir(parents=True, exist_ok=True)

    print("\n[OK] Game preparation complete!")
    print(f"Ready to run translation and build for {title}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
