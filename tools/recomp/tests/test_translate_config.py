"""translate.py takes its inputs from games/<id>/game.toml."""

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
    def test_configure_sets_paths_and_volatile_reads(self):
        cfg = game_config.load(ROOT / "games/populous")
        translate.configure(cfg)
        self.assertEqual(Path(translate.LISTINGS), ROOT / "analysis/decompiled/D3DPopTB.exe/functions")
        self.assertEqual(Path(translate.FUNCS_TSV), ROOT / "analysis/decompiled/D3DPopTB.exe/functions.tsv")
        self.assertEqual(Path(translate.BINARY), ROOT / "original/gog/D3DPopTB.exe")
        self.assertEqual(Path(translate.CURATED), ROOT / "games/populous/globals.toml")
        self.assertEqual(translate.ANIMATION_COUNTER, 0x897981)
        self.assertEqual(len(translate.VISUAL_ANIMATION_READS), 16)
        self.assertIn(0x468F27, translate.VISUAL_ANIMATION_READS)

    def test_visual_animation_read_rewrites_the_configured_counter(self):
        cfg = game_config.load(ROOT / "games/populous")
        cfg["translate"]["animation_counter"] = 0x1234
        cfg["translate"]["volatile_reads"] = [0x10]
        translate.configure(cfg)
        body = ["c->r[0] = rd32(0x1234u);"]
        out = translate.visual_animation_read(0x10, body)
        self.assertEqual(out, ["c->r[0] = ((uint32_t)recomp_visual_animation_tick(rd32(0x1234u)));"])


if __name__ == "__main__":
    unittest.main()
