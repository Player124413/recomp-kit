"""translate.py takes its inputs from a game directory's game.toml."""

import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402

spec = importlib.util.spec_from_file_location("translate", ROOT / "tools/recomp/translate.py")
translate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(translate)


class ConfigureTests(unittest.TestCase):
    def test_function_alignment_follows_the_loaded_game(self):
        cfg = game_config.load(ROOT / "games/stub")
        cfg["translate"]["function_alignment"] = 4
        translate.configure(cfg)
        self.assertEqual(translate.FUNCTION_ALIGNMENT, 4)
        translate.configure(game_config.load(ROOT / "games/stub"))
        self.assertEqual(translate.FUNCTION_ALIGNMENT, 16)

    def test_entry_points_default_empty(self):
        cfg = game_config.load(ROOT / "games/stub")
        translate.configure(cfg)
        self.assertEqual(translate.EXTRA_ENTRY_POINTS, frozenset())

    def test_entry_points_read(self):
        cfg = game_config.load(ROOT / "games/stub")
        cfg["translate"]["entry_points"] = [0x4ab000, 0x4ac000]
        translate.configure(cfg)
        self.assertEqual(translate.EXTRA_ENTRY_POINTS, frozenset({0x4ab000, 0x4ac000}))

    def test_configure_sets_paths_and_volatile_reads(self):
        stub = (ROOT / "games/stub").resolve()
        cfg = game_config.load(stub)
        translate.configure(cfg)
        self.assertEqual(Path(translate.LISTINGS), stub / "analysis/STUB.EXE/functions")
        self.assertEqual(Path(translate.FUNCS_TSV), stub / "analysis/STUB.EXE/functions.tsv")
        self.assertEqual(Path(translate.BINARY), stub / "original/STUB.EXE")
        self.assertEqual(Path(translate.CURATED), stub / "globals.toml")
        self.assertEqual(translate.ANIMATION_COUNTER, 0x500000)
        self.assertEqual(len(translate.VISUAL_ANIMATION_READS), 0)

    def test_visual_animation_read_rewrites_the_configured_counter(self):
        cfg = game_config.load(ROOT / "games/stub")
        cfg["translate"]["animation_counter"] = 0x1234
        cfg["translate"]["volatile_reads"] = [0x10]
        translate.configure(cfg)
        body = ["c->r[0] = rd32(0x1234u);"]
        out = translate.visual_animation_read(0x10, body)
        self.assertEqual(out, ["c->r[0] = ((uint32_t)recomp_visual_animation_tick(rd32(0x1234u)));"])


if __name__ == "__main__":
    unittest.main()
