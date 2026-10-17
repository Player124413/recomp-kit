#!/usr/bin/env python3
"""Universal game preparation and auto-tuning tool for recomp-kit.

Inspects any x86 PC game installation (Shrek 2, Sonic Adventure DX, AVGN, Wolverine, etc.),
flattens nested archive folders, normalizes directory casing, auto-detects the main game
executable and auxiliary DLLs, extracts PE metadata, calculates required guest memory
layout, and creates or updates game.toml in full compliance with tools/game_config.py.

Usage:
    python3 tools/prepare_game.py --game-dir games/shrek2 --install games/shrek2/original --slug shrek2 --title "Shrek 2: The Game"
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402


IGNORE_EXE_KEYWORDS = (
    "unins",
    "setup",
    "autorun",
    "crash",
    "patch",
    "directx",
    "vcredist",
    "dxsetup",
    "config",
    "setuphlp",
    "launcher",
    "update",
    "redist",
    "physx_",
    "oalinst",
)

STANDARD_ENGINE_DIRS = (
    "System",
    "Binaries",
    "Maps",
    "Textures",
    "Sounds",
    "Music",
    "Animations",
    "StaticMeshes",
    "KarmaData",
    "Engine",
)

SUBDIR_NAMES_LOWER = {"system", "binaries", "bin32", "bin", "win32"}


def parse_pe32(data: bytes):
    """Parse PE32 headers from raw bytes without external dependencies."""
    if len(data) < 64 or data[:2] != b"MZ":
        return None
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 + 68 > len(data):
        return None
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        return None

    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    if machine != 0x14C:  # IMAGE_FILE_MACHINE_I386 (32-bit x86)
        return None

    num_sections = struct.unpack_from("<H", data, pe_offset + 6)[0]
    opt_header_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    opt_offset = pe_offset + 24

    magic = struct.unpack_from("<H", data, opt_offset)[0]
    if magic != 0x10B:  # PE32 (32-bit)
        return None

    entry_rva = struct.unpack_from("<I", data, opt_offset + 16)[0]
    image_base = struct.unpack_from("<I", data, opt_offset + 28)[0]
    size_of_image = struct.unpack_from("<I", data, opt_offset + 56)[0]

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


def flatten_tree_if_nested(install_dir: Path):
    """If an archive unpacked into a single wrapper subfolder (e.g. 'Shrek 2/System/...'), flatten it."""
    if not install_dir.is_dir():
        return

    while True:
        entries = [p for p in install_dir.iterdir() if not p.name.startswith(".")]
        if len(entries) == 1 and entries[0].is_dir() and entries[0].name.lower() not in SUBDIR_NAMES_LOWER:
            wrapper = entries[0]
            print(f"Flattening nested archive folder '{wrapper.name}' into '{install_dir}'...")
            for child in list(wrapper.iterdir()):
                dest = install_dir / child.name
                if dest.exists():
                    if dest.is_dir():
                        shutil.rmtree(dest)
                    else:
                        dest.unlink()
                shutil.move(str(child), str(install_dir))
            try:
                wrapper.rmdir()
            except OSError:
                break
        else:
            break

    # If UE2 System contents (Core.dll + Engine.dll) were placed directly in root without System/, wrap in System/
    if not (install_dir / "System").is_dir() and not (install_dir / "system").is_dir():
        if any((install_dir / dll).is_file() for dll in ("Core.dll", "core.dll")) and any(
            (install_dir / dll).is_file() for dll in ("Engine.dll", "engine.dll")
        ):
            print(f"Detected UE2 System files directly in {install_dir}; organizing into System/...")
            sys_dest = install_dir / "System"
            sys_dest.mkdir(parents=True, exist_ok=True)
            for item in list(install_dir.iterdir()):
                if item != sys_dest and not item.name.startswith("."):
                    shutil.move(str(item), str(sys_dest))


def normalize_casing_symlinks(install_dir: Path):
    """Create symlinks for standard engine folders so case-sensitive Linux builds resolve cleanly."""
    if not install_dir.is_dir():
        return
    existing_dirs = {p.name.lower(): p for p in install_dir.iterdir() if p.is_dir()}
    for std_name in STANDARD_ENGINE_DIRS:
        std_lower = std_name.lower()
        if std_lower in existing_dirs:
            actual = existing_dirs[std_lower]
            target_link = install_dir / std_name
            if actual.name != std_name and not target_link.exists():
                try:
                    target_link.symlink_to(actual.name)
                except OSError:
                    pass


def find_executables(install_dir: Path):
    """Find and rank all 32-bit PE executables in install_dir."""
    candidates = []
    for root, _, files in os.walk(str(install_dir)):
        for f in files:
            if not f.lower().endswith(".exe"):
                continue
            p = Path(root) / f
            try:
                data = p.read_bytes()
                pe = parse_pe32(data)
                if not pe:
                    continue
                rel = p.relative_to(install_dir)
                score = 0
                name_lower = f.lower()
                if any(kw in name_lower for kw in IGNORE_EXE_KEYWORDS):
                    score -= 100
                if name_lower in ("game.exe", "shrek2.exe", "wolverine.exe", "sonic.exe", "sonic_dx.exe", "avgn.exe"):
                    score += 50
                score += min(len(data) // 100000, 40)
                if len(rel.parts) <= 3:
                    score += 20
                candidates.append(
                    {
                        "path": p,
                        "rel": rel,
                        "name": f,
                        "size": len(data),
                        "score": score,
                        "pe": pe,
                        "sha256": hashlib.sha256(data).hexdigest(),
                    }
                )
            except Exception:
                continue
    candidates.sort(key=lambda x: x["score"], reverse=True)
    return candidates


def scan_auxiliary_dlls(base_dir: Path):
    """Scan all 32-bit DLLs in base_dir and subdirectories."""
    dlls = []
    for root, _, files in os.walk(str(base_dir)):
        for f in files:
            if not f.lower().endswith(".dll"):
                continue
            p = Path(root) / f
            try:
                data = p.read_bytes()
                pe = parse_pe32(data)
                if pe:
                    dlls.append({"path": p, "name": f, "pe": pe, "size": len(data)})
            except Exception:
                continue
    return dlls


def calculate_guest_memory(main_pe, dlls):
    """Calculate needed guest arena size and page-aligned heap base (< 0x0e000000)."""
    max_va = main_pe["image_base"] + main_pe["size_of_image"]
    for d in dlls:
        d_end = d["pe"]["image_base"] + d["pe"]["size_of_image"]
        if d_end > max_va:
            max_va = d_end

    arena_needed = (max_va + 0x08000000 + 0xFFF) & ~0xFFF
    guest_size = max(0x30000000, arena_needed)
    guest_size = min(guest_size, 0x70000000) & ~0xFFF

    main_base = main_pe["image_base"]
    main_end = main_base + main_pe["size_of_image"]
    heap_base = 0x01000000
    if main_base < 0x0E000000 and main_end > 0x01000000:
        cand_heap = (main_end + 0xFFFF) & ~0xFFFF
        if 0x00400000 < cand_heap < 0x0E000000:
            heap_base = cand_heap

    return guest_size, heap_base


def make_app_name(slug: str, title: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9]", "", title.title() if title else slug.title())
    if not cleaned:
        cleaned = "Game"
    if not cleaned.lower().endswith("recomp"):
        cleaned += "Recomp"
    return cleaned


def write_new_game_toml(
    toml_path: Path,
    slug: str,
    title: str,
    app_name: str,
    bundle_id: str,
    exe_name: str,
    sha256: str,
    image_base: int,
    entry_point: int,
    guest_size: int,
    heap_base: int,
    developer_exe_rel: str,
):
    """Write a complete, schema-compliant game.toml."""
    guest_root_name = re.sub(r"[^A-Za-z0-9_]", "", title.title() or slug.title()) or "Game"
    content = f"""# {toml_path.as_posix()} - {title} port configuration

[game]
id = "{slug}"
name = "{title}"
app_name = "{app_name}"
bundle_id = "{bundle_id}"
executable = "{exe_name}"
sha256 = "{sha256}"
image_base = 0x{image_base:08x}
entry_point = 0x{entry_point:08x}
heap_base = 0x{heap_base:08x}
guest_root = 'C:\\{guest_root_name}'
developer_exe = "{developer_exe_rel}"
windows_version = "5.1"
guest_size = 0x{guest_size:08x}

[translate]
function_alignment = 16
listings = "analysis/{exe_name}"
globals = "globals.toml"
animation_counter = 0
volatile_reads = []
entry_points = []
resumable_stacks = false

[controls]
default_layout = "pad"
pad = "mapped"

[controls.mapped]
left_stick = "wasd"
dpad = "wasd"
right_stick = "cursor"
cursor_speed = 900
cross = "mouse_left"
r1 = "mouse_left"
r2 = "key:Return"
square = "mouse_right"
l1 = "key:LCtrl"
l2 = "mouse_right"
circle = "key:Space"
triangle = "key:Space"
start = "key:Escape"
select = "key:Tab"
l3 = "none"
r3 = "none"
ps = "action:settings"

[launcher]
title = "{title}"
install_names = ["{title}", "{guest_root_name}"]
min_free_mb = 1200

[setup]
required_dirs = []

[hooks]
frame_clock_begin = 0x00000000
frame_clock_wait = 0x00000000
frame_clock_wait_clamp = 0x00000000
frame_clock_clamp_deadline = 0x00000000
frame_clock_wait_deadline = 0x00000000
cursor_surface_ptrs = [0x00000000]
mouse_vtable = 0x00000000
mouse_device_ptr = 0x00000000
mouse_device_right = 0x00000000
camera = 0x00000000
sprites = []

[mods]
builtin = "none"

[bundle]
exclude = [
    "*.pdb",
    "*.ilk",
    "*.exp",
    "*.lib",
    "DirectX*",
    "Help*",
    "Manual*",
    "*.pdf"
]
"""
    toml_path.parent.mkdir(parents=True, exist_ok=True)
    toml_path.write_text(content, encoding="utf-8")


def update_existing_game_toml(
    toml_path: Path,
    slug: str,
    title: str,
    app_name: str,
    bundle_id: str,
    exe_name: str,
    sha256: str,
    image_base: int,
    entry_point: int,
    guest_size: int,
    heap_base: int,
    developer_exe_rel: str,
):
    """Update an existing game.toml in-place if it is valid, or recreate if broken."""
    text = toml_path.read_text(encoding="utf-8")
    # If the file was previously overwritten by an old template lacking id/app_name, rewrite cleanly
    if "id =" not in text or "app_name =" not in text or "[controls.mapped]" not in text:
        write_new_game_toml(
            toml_path=toml_path,
            slug=slug,
            title=title,
            app_name=app_name,
            bundle_id=bundle_id,
            exe_name=exe_name,
            sha256=sha256,
            image_base=image_base,
            entry_point=entry_point,
            guest_size=guest_size,
            heap_base=heap_base,
            developer_exe_rel=developer_exe_rel,
        )
        return

    lines = text.splitlines()
    new_lines = []
    has_heap_base = any(line.strip().startswith("heap_base =") for line in lines)
    has_guest_size = any(line.strip().startswith("guest_size =") for line in lines)

    for line in lines:
        stripped = line.strip()
        if stripped.startswith("executable ="):
            new_lines.append(f'executable = "{exe_name}"')
        elif stripped.startswith("sha256 ="):
            new_lines.append(f'sha256 = "{sha256}"')
        elif stripped.startswith("image_base ="):
            new_lines.append(f"image_base = 0x{image_base:08x}")
        elif stripped.startswith("entry_point ="):
            new_lines.append(f"entry_point = 0x{entry_point:08x}")
        elif stripped.startswith("developer_exe ="):
            new_lines.append(f'developer_exe = "{developer_exe_rel}"')
        elif stripped.startswith("guest_size ="):
            new_lines.append(f"guest_size = 0x{guest_size:08x}")
        elif stripped.startswith("heap_base ="):
            new_lines.append(f"heap_base = 0x{heap_base:08x}")
        elif stripped.startswith("listings ="):
            new_lines.append(f'listings = "analysis/{exe_name}"')
        else:
            new_lines.append(line)
            if stripped == "[game]":
                if not has_heap_base:
                    new_lines.append(f"heap_base = 0x{heap_base:08x}")
                if not has_guest_size:
                    new_lines.append(f"guest_size = 0x{guest_size:08x}")

    toml_path.write_text("\n".join(new_lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Universal game preparation for recomp-kit")
    parser.add_argument("--game-dir", required=True, type=Path, help="Directory for the game port (e.g. games/shrek2)")
    parser.add_argument("--install", required=True, type=Path, help="Directory containing the game files")
    parser.add_argument("--exe", type=str, default="", help="Specific executable name to use (e.g. Game.exe)")
    parser.add_argument("--slug", type=str, default="", help="Game slug identifier (e.g. shrek2)")
    parser.add_argument("--title", type=str, default="", help="Display title (e.g. 'Shrek 2: The Game')")
    parser.add_argument("--bundle-id", type=str, default="", help="Bundle / package ID (e.g. 'dev.recompkit.shrek2')")

    args = parser.parse_args()

    game_dir = args.game_dir.resolve()
    install_dir = args.install.resolve()

    if not install_dir.is_dir():
        print(f"Error: install directory {install_dir} does not exist!")
        return 1

    slug = args.slug or game_dir.name
    title = args.title or slug.replace("-", " ").title()
    clean_slug = re.sub(r"[^a-z0-9]", "", slug.lower()) or "game"
    bundle_id = args.bundle_id or f"dev.recompkit.{clean_slug}"
    app_name = "Shrek2Recomp" if slug == "shrek2" else make_app_name(slug, title)

    print(f"=== Universal Recomp Preparation: {title} ({slug}) ===")
    print(f"Install directory: {install_dir}")
    print(f"Port directory:    {game_dir}")

    # Step 1: Flatten any nested wrapper folders and normalize directory casing
    flatten_tree_if_nested(install_dir)
    normalize_casing_symlinks(install_dir)

    # Step 2: Discover 32-bit x86 PE executables
    candidates = find_executables(install_dir)
    if not candidates:
        print(f"Error: No 32-bit x86 PE executables (.exe) found in {install_dir}!")
        return 1

    chosen = None
    if args.exe:
        req = Path(args.exe).name.lower()
        for c in candidates:
            if c["name"].lower() == req or str(c["rel"]).lower() == args.exe.lower():
                chosen = c
                break
        if not chosen:
            print(f"Warning: requested executable '{args.exe}' not found; selecting best match automatically.")

    if not chosen:
        chosen = candidates[0]

    exe_name = chosen["name"]
    # For Shrek 2, keep canonical "Game.exe" casing if game.toml expects it
    if slug == "shrek2" and exe_name.lower() == "game.exe":
        exe_name = "Game.exe"

    print(f"\nTarget Executable selected: {chosen['rel']} (executable = {exe_name})")
    print(f"  - Size:          {chosen['size']:,} bytes")
    print(f"  - SHA256:        {chosen['sha256']}")
    print(f"  - Image Base:    0x{chosen['pe']['image_base']:08x}")
    print(f"  - Entry Point:   0x{chosen['pe']['entry_point']:08x}")
    print(f"  - Size of Image: 0x{chosen['pe']['size_of_image']:08x}")

    # Step 3: Scan auxiliary DLLs and compute memory layout
    dlls = scan_auxiliary_dlls(install_dir)
    print(f"\nScanned {len(dlls)} auxiliary 32-bit DLL modules in game directory.")

    guest_size, heap_base = calculate_guest_memory(chosen["pe"], dlls)
    print(f"\nCalculated memory layout:")
    print(f"  - Guest arena size: 0x{guest_size:08x} ({guest_size // (1024 * 1024)} MB)")
    print(f"  - Heap base:        0x{heap_base:08x}")

    # Step 4: Stage executable in games/<slug>/original/ with exact canonical filename
    # Use a real file copy (not a symlink to a lowercase filename) so .resolve() preserves
    # the exact filename casing expected by Ghidra ExportProgram.java and listings_path.
    stage_orig = game_dir / "original"
    stage_orig.mkdir(parents=True, exist_ok=True)
    target_staged_exe = stage_orig / chosen["rel"]
    target_staged_exe.parent.mkdir(parents=True, exist_ok=True)
    if chosen["path"].resolve() != target_staged_exe.resolve():
        shutil.copy2(chosen["path"], target_staged_exe)

    root_exe = stage_orig / exe_name
    if root_exe.is_symlink():
        root_exe.unlink()
    if not root_exe.is_file() or root_exe.resolve() != root_exe:
        shutil.copy2(target_staged_exe, root_exe)

    if target_staged_exe.parent != stage_orig:
        canonical_in_sub = target_staged_exe.parent / exe_name
        if canonical_in_sub.is_symlink():
            canonical_in_sub.unlink()
        if not canonical_in_sub.is_file():
            shutil.copy2(target_staged_exe, canonical_in_sub)

    developer_exe_rel = f"original/{exe_name}"

    # Step 5: Ensure globals.toml exists before validating game_config
    globals_path = game_dir / "globals.toml"
    if not globals_path.is_file():
        globals_path.write_text("# Game global variables (optional overrides)\n\n[globals]\n", encoding="utf-8")

    # Step 6: Create or update game.toml
    toml_path = game_dir / "game.toml"
    if toml_path.is_file():
        update_existing_game_toml(
            toml_path=toml_path,
            slug=slug,
            title=title,
            app_name=app_name,
            bundle_id=bundle_id,
            exe_name=exe_name,
            sha256=chosen["sha256"],
            image_base=chosen["pe"]["image_base"],
            entry_point=chosen["pe"]["entry_point"],
            guest_size=guest_size,
            heap_base=heap_base,
            developer_exe_rel=developer_exe_rel,
        )
    else:
        write_new_game_toml(
            toml_path=toml_path,
            slug=slug,
            title=title,
            app_name=app_name,
            bundle_id=bundle_id,
            exe_name=exe_name,
            sha256=chosen["sha256"],
            image_base=chosen["pe"]["image_base"],
            entry_point=chosen["pe"]["entry_point"],
            guest_size=guest_size,
            heap_base=heap_base,
            developer_exe_rel=developer_exe_rel,
        )

    # Step 7: Clean up any empty analysis/<exe_name> directory so Ghidra / setup.py can populate or link it cleanly
    analysis_dir = game_dir / "analysis" / exe_name
    if analysis_dir.is_dir() and not analysis_dir.is_symlink() and not (analysis_dir / "functions.tsv").is_file():
        shutil.rmtree(analysis_dir, ignore_errors=True)

    # Step 8: Validate against tools/game_config.py so setup.py / build.py can never fail on schema errors
    cfg = game_config.load(game_dir)
    for req_dir in cfg.get("setup", {}).get("required_dirs", ()):
        (stage_orig / req_dir).mkdir(parents=True, exist_ok=True)
    print(f"\n[OK] Verified {toml_path} with tools/game_config.py (id={cfg['game']['id']}, app_name={cfg['game']['app_name']}).")
    print(f"Ready to run Ghidra export and build for {title}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
