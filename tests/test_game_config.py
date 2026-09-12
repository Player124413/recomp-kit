"""tools/game_config.py loads a game directory; tools/gen_game_config.py renders it."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


game_config = load_module("game_config")
gen_game_config = load_module("gen_game_config")


class LoadTests(unittest.TestCase):
    def test_populous_loads_with_every_required_key(self):
        cfg = game_config.load(ROOT / "games/populous")
        self.assertEqual(cfg["game"]["id"], "populous")
        self.assertEqual(cfg["game"]["executable"], "D3DPopTB.exe")
        self.assertEqual(cfg["game"]["entry_point"], 0x0055D6C0)
        self.assertEqual(cfg["game"]["image_base"], 0x00400000)
        self.assertIn("simulation_turn", cfg["globals"])
        self.assertEqual(cfg["globals"]["simulation_turn"]["addr"], 0x0089D188)

    def test_missing_key_is_an_error_naming_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            (game / "game.toml").write_text('[game]\nid = "x"\n')
            (game / "globals.toml").write_text("")
            with self.assertRaises(ValueError) as caught:
                game_config.load(game)
            self.assertIn("game.toml", str(caught.exception))
            self.assertIn("sha256", str(caught.exception))


class RenderTests(unittest.TestCase):
    def setUp(self):
        self.cfg = game_config.load(ROOT / "games/populous")
        self.header = gen_game_config.render_header(self.cfg)
        self.cmake = gen_game_config.render_cmake(self.cfg)

    def test_identity_macros(self):
        self.assertIn('#define RECOMP_GAME_ID "populous"', self.header)
        self.assertIn('#define RECOMP_APP_NAME "PopRecomp"', self.header)
        self.assertIn('#define RECOMP_EXECUTABLE "D3DPopTB.exe"', self.header)
        self.assertIn('#define RECOMP_EXE_SHA256 "815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd"',
                      self.header)
        self.assertIn('#define RECOMP_GUEST_ROOT "C:\\\\Populous"', self.header)
        self.assertIn("#define RECOMP_ENTRY_POINT 0x0055d6c0u", self.header)
        self.assertIn("#define RECOMP_IMAGE_BASE 0x00400000u", self.header)

    def test_globals_macros(self):
        self.assertIn("#define RECOMP_GLOBAL_SIMULATION_TURN_ADDR 0x0089d188u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_STRIDE 179u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_COUNT 2000u", self.header)

    def test_hook_lists_render_as_brace_lists_with_a_count(self):
        cfg = dict(self.cfg, hooks={"pair": [0x10, 0x20], "one": 0x30})
        header = gen_game_config.render_header(cfg)
        self.assertIn("#define RECOMP_HOOK_PAIR_COUNT 2", header)
        self.assertIn("#define RECOMP_HOOK_PAIR {0x00000010u, 0x00000020u}", header)
        self.assertIn("#define RECOMP_HOOK_ONE 0x00000030u", header)

    def test_cmake_fragment(self):
        self.assertIn('set(RECOMP_APP_NAME "PopRecomp")', self.cmake)
        self.assertIn('set(RECOMP_GAME_NAME "Populous: The Beginning")', self.cmake)
        self.assertIn("set(RECOMP_IMAGE_BASE 0x00400000u)", self.cmake)


if __name__ == "__main__":
    unittest.main()
