#!/usr/bin/env python3
"""Inspect Shrek 2 PC game files, extract PE metadata, and configure game.toml.

Usage:
    python tools/prepare_shrek2.py --game-dir games/shrek2 --install /path/to/shrek2
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


UE2_STANDARD_DIRS = [
    "System", "Maps", "Textures", "Sounds", "Music", "Animations", "StaticMeshes", "KarmaData"
]


def parse_pe32(data: bytes):
    """Parse PE32 headers from raw bytes without external libraries."""
    if len(data) < 64 or data[:2] != b"MZ":
        return None
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 + 68 > len(data):
        return None
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        return None

    # COFF header
    num_sections = struct.unpack_from("<H", data, pe_offset + 6)[0]
    opt_header_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    opt_offset = pe_offset + 24

    # Optional header magic: 0x10b for PE32 (32-bit), 0x20b for PE32+ (64-bit)
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


def find_system_folder(install_dir: Path):
    """Locate the System folder anywhere under install_dir, case-insensitively."""
    # Check immediate children first
    for item in install_dir.iterdir():
        if item.is_dir() and item.name.lower() == "system":
            return item

    # If install_dir itself looks like the System directory (has Core.dll or Engine.dll)
    if any((install_dir / dll).is_file() for dll in ("Core.dll", "core.dll", "Engine.dll", "engine.dll")):
        return install_dir

    # Search recursively for a directory named 'system'
    for root, dirs, _ in os.walk(str(install_dir)):
        for d in dirs:
            if d.lower() == "system":
                cand = Path(root) / d
                if any((cand / f).is_file() for f in ("Game.exe", "game.exe", "Shrek2.exe", "shrek2.exe", "Core.dll", "core.dll")):
                    return cand
    return None


def flatten_tree_if_nested(install_dir: Path):
    """If the game was unpacked into a subfolder (e.g. 'Shrek 2/System'), move files to install_dir."""
    system_dir = find_system_folder(install_dir)
    if not system_dir:
        return

    # If system_dir is inside a subfolder, e.g. install_dir / "Shrek 2" / "System"
    real_game_root = system_dir.parent if system_dir != install_dir else install_dir
    if real_game_root != install_dir and real_game_root.is_relative_to(install_dir):
        print(f"Flattening nested game folder '{real_game_root.name}' into '{install_dir}'...")
        for item in list(real_game_root.iterdir()):
            target = install_dir / item.name
            if target.exists():
                if target.is_dir():
                    shutil.rmtree(target)
                else:
                    target.unlink()
            shutil.move(str(item), str(install_dir))
        # Remove parent directories up to install_dir if empty
        curr = real_game_root
        while curr != install_dir:
            try:
                curr.rmdir()
            except OSError:
                break
            curr = curr.parent

    # Special case: if install_dir itself contains the contents of System (Core.dll, Game.exe, etc.)
    # without a System folder, wrap them inside install_dir / System
    if not (install_dir / "System").is_dir() and not (install_dir / "system").is_dir():
        if any((install_dir / dll).is_file() for dll in ("Core.dll", "core.dll", "Engine.dll", "engine.dll")):
            print(f"Detected System folder contents directly in {install_dir}. Creating System/ directory...")
            sys_dest = install_dir / "System"
            sys_dest.mkdir(parents=True, exist_ok=True)
            for item in list(install_dir.iterdir()):
                if item != sys_dest and item.name != ".git":
                    shutil.move(str(item), str(sys_dest))


def normalize_casing_symlinks(install_dir: Path):
    """Create symlinks for standard UE2 folders and files to handle case-sensitive filesystems."""
    existing_dirs = {p.name.lower(): p for p in install_dir.iterdir() if p.is_dir()}
    for std_name in UE2_STANDARD_DIRS:
        std_lower = std_name.lower()
        if std_lower in existing_dirs:
            actual = existing_dirs[std_lower]
            target_link = install_dir / std_name
            if actual.name != std_name and not target_link.exists():
                try:
                    target_link.symlink_to(actual.name)
                    print(f"Created symlink: {std_name} -> {actual.name}")
                except OSError:
                    pass

    # Ensure System folder is reachable
    sys_dir = None
    for cand_name in ("System", "system", "SYSTEM"):
        if (install_dir / cand_name).is_dir():
            sys_dir = install_dir / cand_name
            break

    if sys_dir:
        # Case symlinks inside System
        existing_files = {p.name.lower(): p for p in sys_dir.iterdir() if p.is_file()}
        for target in ("Game.exe", "Core.dll", "Engine.dll", "Window.dll", "D3DDrv.dll", "DefOpenAL.dll"):
            t_lower = target.lower()
            if t_lower in existing_files:
                actual = existing_files[t_lower]
                target_path = sys_dir / target
                if actual.name != target and not target_path.exists():
                    try:
                        target_path.symlink_to(actual.name)
                        print(f"Created symlink in System: {target} -> {actual.name}")
                    except OSError:
                        pass


def find_executable(install_dir: Path):
    """Find Game.exe or Shrek2.exe in install directory or System/ subdirectory."""
    # Preferred order: System/Game.exe, then root Game.exe
    candidates = [
        install_dir / "System" / "Game.exe",
        install_dir / "System" / "Shrek2.exe",
        install_dir / "system" / "Game.exe",
        install_dir / "system" / "game.exe",
        install_dir / "Game.exe",
        install_dir / "game.exe",
        install_dir / "Shrek2.exe",
        install_dir / "shrek2.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c

    # Case-insensitive recursive search
    for path in install_dir.rglob("*"):
        if path.is_file() and path.name.lower() in ("game.exe", "shrek2.exe"):
            return path
    return None


def check_drm(sections):
    """Check for known SafeDisc or SecuROM section names or patterns."""
    drm_warnings = []
    suspicious_sections = {".securom", "~df394b", "icd", ".safedisc", ".cms_d", ".cms_t"}
    for sec in sections:
        name = sec["name"].lower().strip()
        if name in suspicious_sections or name.startswith("~df"):
            drm_warnings.append(
                f"Section '{sec['name']}' indicates retail CD DRM (SafeDisc / SecuROM)."
            )
    return drm_warnings


def update_game_toml(toml_path: Path, exe_name: str, sha256: str, image_base: int, entry_point: int, guest_size: int):
    """Update game.toml fields in-place while preserving comments and structure."""
    text = toml_path.read_text(encoding="utf-8")
    lines = text.splitlines()
    new_lines = []

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
            new_lines.append(f'developer_exe = "original/{exe_name}"')
        else:
            new_lines.append(line)

    toml_path.write_text("\n".join(new_lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, default=Path("games/shrek2"), help="Path to game directory")
    parser.add_argument("--install", type=Path, default=None, help="Path to PC game installation")
    args = parser.parse_args()

    game_dir = args.game_dir.resolve()
    toml_path = game_dir / "game.toml"
    if not toml_path.is_file():
        parser.error(f"Config file not found at {toml_path}")

    install_dir = (args.install or (game_dir / "original")).resolve()
    if not install_dir.is_dir():
        print(f"Notice: Installation directory '{install_dir}' does not exist yet.")
        print(f"Please put your PC Shrek 2 game files in '{install_dir}'.")
        return 0

    # Step 1: Flatten nested directories (e.g. archive extracted as 'Shrek 2/System/...')
    flatten_tree_if_nested(install_dir)

    # Step 2: Normalize casing with symlinks for Linux case sensitivity
    normalize_casing_symlinks(install_dir)

    # Step 3: Locate game executable
    exe_path = find_executable(install_dir)
    if not exe_path:
        print(f"\n[!] Error: No Game.exe or Shrek2.exe found under '{install_dir}'.")
        print("    Directory contents:")
        for item in sorted(install_dir.iterdir()):
            print(f"      - {item.name}{'/' if item.is_dir() else ''}")
        print("    Please provide the game files containing System/Game.exe.")
        return 1

    print(f"Found game executable: {exe_path}")
    exe_bytes = exe_path.read_bytes()
    sha256 = hashlib.sha256(exe_bytes).hexdigest()
    print(f"SHA-256: {sha256}")

    pe = parse_pe32(exe_bytes)
    if not pe:
        print("Warning: Could not parse 32-bit PE header. Make sure the file is a valid 32-bit Windows executable.")
        return 1

    print(f"PE Image Base:    0x{pe['image_base']:08x}")
    print(f"PE Entry Point:   0x{pe['entry_point']:08x}")
    print(f"PE Size of Image: 0x{pe['size_of_image']:08x}")

    drm_warnings = check_drm(pe["sections"])
    if drm_warnings:
        print("\n[!] DRM DETECTION WARNING:")
        for w in drm_warnings:
            print(f"    - {w}")
        print("    Static recompilation cannot decompile encrypted retail executables.")
        print("    Please use a decrypted / DRM-free (No-CD) Game.exe executable!\n")
    else:
        print("Executable appears to be clean (no DRM wrapper detected).")

    # Step 4: Ensure executable is available in both canonical System/Game.exe and original/Game.exe
    canonical_name = "Game.exe"
    original_root = game_dir / "original"
    system_dir = original_root / "System"
    system_dir.mkdir(parents=True, exist_ok=True)

    system_exe = system_dir / canonical_name
    root_exe = original_root / canonical_name

    # If exe_path is not already system_exe, ensure system_exe exists
    if exe_path != system_exe and not system_exe.is_file():
        try:
            system_exe.symlink_to(os.path.relpath(exe_path, system_dir))
        except OSError:
            shutil.copy2(exe_path, system_exe)

    # Ensure root_exe exists (either symlink or copy to system_exe or exe_path)
    if exe_path != root_exe and not root_exe.is_file():
        try:
            rel = os.path.relpath(system_exe if system_exe.is_file() else exe_path, original_root)
            root_exe.symlink_to(rel)
        except OSError:
            shutil.copy2(system_exe if system_exe.is_file() else exe_path, root_exe)

    # Ensure guest_size is at least 512MB for UE2 games with modules
    guest_size = max(0x20000000, pe["image_base"] + pe["size_of_image"] + 0x10000000)
    # Round to page boundary (0x1000)
    guest_size = (guest_size + 0xFFF) & ~0xFFF

    update_game_toml(
        toml_path,
        exe_name=canonical_name,
        sha256=sha256,
        image_base=pe["image_base"],
        entry_point=pe["entry_point"],
        guest_size=guest_size,
    )
    print(f"Successfully updated {toml_path} with executable metadata!")

    # Check for Unreal Engine 2 auxiliary modules
    if system_dir.is_dir():
        print("\nScanning for Unreal Engine 2 modules in System/:")
        ue2_dlls = ["Core.dll", "Engine.dll", "Window.dll", "D3DDrv.dll", "DefOpenAL.dll", "Fire.dll", "KWGame.dll", "SHGame.dll"]
        found_dlls = []
        for dll_name in ue2_dlls:
            dll_file = system_dir / dll_name
            if not dll_file.is_file():
                for f in system_dir.iterdir():
                    if f.is_file() and f.name.lower() == dll_name.lower():
                        dll_file = f
                        break
            if dll_file.is_file():
                dbytes = dll_file.read_bytes()
                dhash = hashlib.sha256(dbytes).hexdigest()
                dpe = parse_pe32(dbytes)
                base = f"0x{dpe['image_base']:08x}" if dpe else "unknown"
                size = f"0x{dpe['size_of_image']:08x}" if dpe else "unknown"
                print(f"  - Found {dll_file.name}: base={base}, size={size}, sha256={dhash[:16]}...")
                found_dlls.append(dll_name)
        print(f"Total UE2 engine modules found: {len(found_dlls)}")

    print("\nPreparation complete. Ready for translation and build.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
