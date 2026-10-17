#!/usr/bin/env python3
"""Smart game packer for recomp-kit porting.

Rule:
- Games up to ~5 GB (<= 5.5 GB): packs the ENTIRE game folder into a single .zip
  so GitHub Actions can build and bundle a 100% standalone portable port.
- Games 6 GB, 7 GB, 8 GB+ (> 5.5 GB): automatically skips heavy assets and packs
  ONLY .exe, .dll, .ini, .cfg, .int, .u, .def, etc. (preserving subfolder layout
  such as Binaries/ or System/) so upload/download takes seconds instead of hours.

Usage:
    python3 tools/pack_game_for_port.py "C:/Games/X-Men Origins Wolverine"
    python3 tools/pack_game_for_port.py "C:/Games/AVGN Adventures"
"""

import argparse
import os
from pathlib import Path
import sys
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

MAX_SINGLE_FILE_BYTES_IN_LIGHT_MODE = 64 * 1024 * 1024  # 64 MB


def get_folder_size(folder: Path) -> int:
    total = 0
    for root, _, files in os.walk(str(folder)):
        for f in files:
            if f.startswith("."):
                continue
            fp = Path(root) / f
            try:
                total += fp.stat().st_size
            except OSError:
                pass
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_folder", type=Path, help="Path to the installed game folder")
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output .zip path (defaults to <folder-name>-port-input.zip next to the folder)",
    )
    parser.add_argument(
        "--max-full-gb",
        type=float,
        default=5.5,
        help="Maximum folder size in GB to pack the full game (default: 5.5 GB, i.e. up to ~5 GB full, 6 GB+ binaries only)",
    )
    parser.add_argument(
        "--binaries-only",
        action="store_true",
        help="Force packing only .exe, .dll, and lightweight config files regardless of total size",
    )
    args = parser.parse_args()

    game_folder = args.game_folder.resolve()
    if not game_folder.is_dir():
        print(f"[ERROR] Folder does not exist: {game_folder}", file=sys.stderr)
        return 1

    total_bytes = get_folder_size(game_folder)
    total_gb = total_bytes / (1024 ** 3)
    total_mb = total_bytes / (1024 ** 2)
    threshold_bytes = int(args.max_full_gb * (1024 ** 3))

    light_mode = args.binaries_only or (total_bytes > threshold_bytes)

    if args.out:
        out_zip = args.out.resolve()
    else:
        suffix = "binaries-only.zip" if light_mode else "full-game.zip"
        out_zip = game_folder.parent / f"{game_folder.name}-{suffix}"

    print("========================================================")
    print(f"  Game Folder : {game_folder}")
    print(f"  Total Size  : {total_gb:.2f} GB ({total_mb:.0f} MB)")
    if light_mode:
        print(f"  Packing Mode: BINARIES & CONFIGS ONLY (> {args.max_full_gb:.1f} GB)")
        print("                Packing only .exe, .dll, .ini, .cfg, .int, .u, etc.")
        print("                Skipping heavy gigabyte assets!")
    else:
        print(f"  Packing Mode: FULL GAME (<= {args.max_full_gb:.1f} GB)")
        print("                Packing all game files into portable archive.")
    print(f"  Output ZIP  : {out_zip}")
    print("========================================================")

    zip_level = 1 if (not light_mode and total_bytes > 500 * 1024 * 1024) else 6
    packed_files = 0
    packed_bytes = 0

    out_zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=zip_level) as zf:
        for root, _, files in os.walk(str(game_folder)):
            for f in files:
                if f.startswith("."):
                    continue
                fp = Path(root) / f
                if fp.resolve() == out_zip:
                    continue
                try:
                    fsize = fp.stat().st_size
                except OSError:
                    continue

                if light_mode:
                    ext = fp.suffix.lower()
                    if ext not in LIGHTWEIGHT_EXTENSIONS or fsize > MAX_SINGLE_FILE_BYTES_IN_LIGHT_MODE:
                        continue

                rel = fp.relative_to(game_folder)
                zf.write(fp, arcname=rel.as_posix())
                packed_files += 1
                packed_bytes += fsize

    out_mb = out_zip.stat().st_size / (1024 ** 2)
    print(f"\n[DONE] Packed {packed_files} files ({packed_bytes / (1024 ** 2):.1f} MB raw -> {out_mb:.1f} MB ZIP)")
    print(f"Saved to: {out_zip}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
