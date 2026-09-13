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
    def test_the_stub_game_loads_with_every_required_key(self):
        cfg = game_config.load(ROOT / "games/stub")
        self.assertEqual(cfg["game"]["id"], "stub")
        self.assertEqual(cfg["game"]["executable"], "STUB.EXE")
        self.assertEqual(cfg["game"]["entry_point"], 0x00401000)
        self.assertEqual(cfg["game"]["image_base"], 0x00400000)
        self.assertIn("simulation_turn", cfg["globals"])
        self.assertEqual(cfg["globals"]["entity_base"]["stride"], 179)

    def test_developer_paths_resolve_relative_to_the_game_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / "g"
            game.mkdir()
            (game / "game.toml").write_text((ROOT / "games/stub/game.toml").read_text())
            (game / "globals.toml").write_text((ROOT / "games/stub/globals.toml").read_text())
            cfg = game_config.load(game)
            self.assertEqual(cfg["developer_exe_path"], (game / "original/STUB.EXE").resolve())
            self.assertEqual(cfg["listings_path"], (game / "analysis/STUB.EXE").resolve())
            cmake = gen_game_config.render_cmake(cfg)
            self.assertIn('set(RECOMP_DEVELOPER_EXE "%s")' % (game / "original/STUB.EXE").resolve().as_posix(),
                          cmake)
            self.assertIn('set(RECOMP_DEVELOPER_GAME_DIR "%s")' % (game / "original").resolve().as_posix(),
                          cmake)
            header = gen_game_config.render_header(cfg)
            self.assertIn('#define RECOMP_DEVELOPER_EXE "%s"' % (game / "original/STUB.EXE").resolve().as_posix(),
                          header)
            self.assertIn('#define RECOMP_GAME_DIR "%s"' % game.resolve().as_posix(), header)

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
        self.cfg = game_config.load(ROOT / "games/stub")
        self.header = gen_game_config.render_header(self.cfg)
        self.cmake = gen_game_config.render_cmake(self.cfg)

    def test_identity_macros(self):
        self.assertIn('#define RECOMP_GAME_ID "stub"', self.header)
        self.assertIn('#define RECOMP_APP_NAME "StubRecomp"', self.header)
        self.assertIn('#define RECOMP_EXECUTABLE "STUB.EXE"', self.header)
        self.assertIn('#define RECOMP_EXE_SHA256 "%s"' % ("0" * 64), self.header)
        self.assertIn('#define RECOMP_GUEST_ROOT "C:\\\\Stub"', self.header)
        self.assertIn("#define RECOMP_ENTRY_POINT 0x00401000u", self.header)
        self.assertIn("#define RECOMP_IMAGE_BASE 0x00400000u", self.header)

    def test_globals_macros(self):
        self.assertIn("#define RECOMP_GLOBAL_SIMULATION_TURN_ADDR 0x00700000u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_STRIDE 179u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_COUNT 2000u", self.header)

    def test_hook_lists_render_as_brace_lists_with_a_count(self):
        cfg = dict(self.cfg, hooks={"pair": [0x10, 0x20], "one": 0x30})
        header = gen_game_config.render_header(cfg)
        self.assertIn("#define RECOMP_HOOK_PAIR_COUNT 2", header)
        self.assertIn("#define RECOMP_HOOK_PAIR {0x00000010u, 0x00000020u}", header)
        self.assertIn("#define RECOMP_HOOK_ONE 0x00000030u", header)

    def test_stub_hooks_render(self):
        self.assertIn("#define RECOMP_HOOK_FRAME_CLOCK_BEGIN 0x00401100u", self.header)
        self.assertIn("#define RECOMP_HOOK_CURSOR_SURFACE_PTRS_COUNT 2", self.header)
        self.assertIn("#define RECOMP_HOOK_CURSOR_SURFACE_PTRS {0x00600100u, 0x00600104u}", self.header)
        self.assertIn("#define RECOMP_HOOK_MOUSE_VTABLE 0x00600200u", self.header)

    def test_cmake_fragment(self):
        self.assertIn('set(RECOMP_APP_NAME "StubRecomp")', self.cmake)
        self.assertIn('set(RECOMP_GAME_NAME "Stub Game")', self.cmake)
        self.assertIn("set(RECOMP_IMAGE_BASE 0x00400000u)", self.cmake)
        stub = (ROOT / "games/stub").resolve()
        self.assertIn('set(RECOMP_DEVELOPER_GAME_DIR "%s")' % (stub / "original").as_posix(), self.cmake)
        self.assertIn('set(RECOMP_DEVELOPER_EXE "%s")' % (stub / "original/STUB.EXE").as_posix(), self.cmake)


if __name__ == "__main__":
    unittest.main()
