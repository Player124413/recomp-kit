"""Load a game directory: games/<id>/game.toml plus its curated globals file.

This module is the only place that knows the schema. Python 3.9 has no
tomllib, so the pinned tomli is the fallback."""

from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

REQUIRED_GAME_KEYS = ("id", "name", "app_name", "bundle_id", "executable", "sha256",
                      "image_base", "entry_point", "guest_root", "developer_exe")

HEAP_BASE_DEFAULT = 0x01000000
HEAP_END = 0x0e000000        # runtime/x86.h GUEST_HEAP_END; the mods' heap starts there


def validate_heap_base(value):
    """The heap arena start: page aligned, above the image base, below the arena end."""
    if value % 0x1000 or not (0x00400000 < value < HEAP_END):
        raise ValueError("[game] heap_base %#x must be page aligned and between 0x00400000 and %#x" % (value, HEAP_END))
    return value


def load(game_dir):
    """Return the parsed config with `globals` merged in and `dir`/`source` recorded."""
    game_dir = Path(game_dir)
    source = game_dir / "game.toml"
    with source.open("rb") as fh:
        cfg = tomllib.load(fh)
    game = cfg.get("game", {})
    missing = [key for key in REQUIRED_GAME_KEYS if key not in game]
    if missing:
        raise ValueError("%s: missing [game] keys: %s" % (source, ", ".join(missing)))
    game["heap_base"] = validate_heap_base(int(game.get("heap_base", HEAP_BASE_DEFAULT)))
    translate = cfg.setdefault("translate", {})
    cfg.setdefault("hooks", {})
    cfg.setdefault("bundle", {}).setdefault("exclude", [])
    touch = cfg.setdefault("touch", {})
    touch.setdefault("keypad", "auto")
    if touch["keypad"] not in ("auto", "hidden"):
        raise ValueError('%s: [touch] keypad must be "auto" or "hidden", not %r' % (source, touch["keypad"]))
    globals_path = game_dir / translate.get("globals", "globals.toml")
    with globals_path.open("rb") as fh:
        cfg["globals"] = tomllib.load(fh).get("globals", {})
    cfg["dir"] = game_dir
    cfg["source"] = str(source)
    # Developer inputs live beside game.toml: a game repository holds its own
    # ignored original/ and analysis/ directories.
    cfg["developer_exe_path"] = (game_dir / game["developer_exe"]).resolve()
    cfg["listings_path"] = (game_dir / translate.get("listings", "analysis")).resolve()
    return cfg
