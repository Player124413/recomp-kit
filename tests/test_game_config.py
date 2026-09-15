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
    def test_windows_version_defaults_and_optional_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            stub = (ROOT / "games/stub/game.toml").read_text()
            base = "\n".join(line for line in stub.splitlines()
                             if not line.startswith("windows_version ="))
            (game / "globals.toml").write_text("")
            for version, expected in ((None, (4, 10, 2222, 1)),
                                      ("6.1", (6, 1, 7601, 2)),
                                      ("6.1.7600", (6, 1, 7600, 2)),
                                      ("5.0", (5, 0, 0, 2))):
                setting = "" if version is None else 'windows_version = "%s"\n' % version
                (game / "game.toml").write_text(base.replace("[game]\n", "[game]\n" + setting))
                cfg = game_config.load(game)
                self.assertEqual(cfg["game"]["windows_version"], version or "4.10")
                header = gen_game_config.render_header(cfg)
                for field, value in zip(("MAJOR", "MINOR", "BUILD", "PLATFORM"), expected):
                    self.assertIn("#define RECOMP_WINDOWS_%s %du" % (field, value), header)

    def test_windows_version_rejects_invalid_components(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            stub = (ROOT / "games/stub/game.toml").read_text()
            base = "\n".join(line for line in stub.splitlines()
                             if not line.startswith("windows_version ="))
            (game / "globals.toml").write_text("")
            for bad in ('"6"', '"6.1.2.3"', '"6.-1"', '"6.256"', '"6.1.32768"', '6.1', 'true'):
                (game / "game.toml").write_text(base.replace(
                    "[game]\n", "[game]\nwindows_version = %s\n" % bad))
                with self.assertRaisesRegex(ValueError, "windows_version"):
                    game_config.load(game)

    def test_function_alignment_defaults_to_16_and_can_be_overridden(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            stub = (ROOT / "games/stub/game.toml").read_text()
            base = "\n".join(line for line in stub.splitlines()
                             if not line.startswith("function_alignment ="))
            (game / "globals.toml").write_text("")
            for value, expected in ((None, 16), (4, 4)):
                setting = "" if value is None else "function_alignment = %d\n" % value
                (game / "game.toml").write_text(base.replace("[translate]\n", "[translate]\n" + setting))
                self.assertEqual(game_config.load(game)["translate"]["function_alignment"], expected)

    def test_function_alignment_must_be_a_positive_integer(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            stub = (ROOT / "games/stub/game.toml").read_text()
            base = "\n".join(line for line in stub.splitlines()
                             if not line.startswith("function_alignment ="))
            (game / "globals.toml").write_text("")
            for bad in ("0", "-4", '"four"', "4.0", "true"):
                (game / "game.toml").write_text(base.replace(
                    "[translate]\n", "[translate]\nfunction_alignment = %s\n" % bad))
                with self.assertRaisesRegex(ValueError, "function_alignment"):
                    game_config.load(game)

    def test_heap_base_defaults_to_the_kit_layout(self):
        cfg = game_config.load(ROOT / "games/stub")
        self.assertEqual(cfg["game"]["heap_base"], 0x01000000)
        self.assertIn("#define RECOMP_HEAP_BASE 0x01000000u", gen_game_config.render_header(cfg))
        self.assertIn("set(RECOMP_HEAP_BASE 0x01000000u)", gen_game_config.render_cmake(cfg))

    def test_heap_base_is_validated(self):
        cfg = game_config.load(ROOT / "games/stub")
        for bad in (0x01000010, 0x0e000000, 0x00400000):
            with self.assertRaises(ValueError):
                game_config.validate_heap_base(bad)
        self.assertEqual(game_config.validate_heap_base(0x01400000), 0x01400000)

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
            self.assertIn('#define RECOMP_KIT_DIR "%s"' % ROOT.resolve().as_posix(), header)

    def test_auxiliary_modules_and_guest_size(self):
        """[modules.aux.<key>] names a DLL the guest loads at run time that the kit
        translates as a second image at its preferred base; [game] guest_size
        grows the arena to hold it. Defaults: no modules, 0x10000000."""
        base = game_config.load(ROOT / "games/stub")
        self.assertEqual(base["aux_modules"], [])
        self.assertEqual(base["game"]["guest_size"], 0x10000000)
        self.assertIn("#define RECOMP_GUEST_SIZE 0x10000000u", gen_game_config.render_header(base))
        self.assertIn("#define RECOMP_AUX_MODULE_COUNT 0", gen_game_config.render_header(base))
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / "g"
            game.mkdir()
            text = (ROOT / "games/stub/game.toml").read_text()
            text = text.replace('guest_root = ', 'guest_size = 0x10100000\nguest_root = ', 1)
            text += ('\n[modules.aux.blit]\nname = "Blit_p6.dll"\npath = "original/Blit_p6.dll"\n'
                     'sha256 = "%s"\nbase = 0x10000000\nsize = 0x28000\nlistings = "analysis/Blit_p6.dll"\nfunction_alignment = 1\n' % ("ab" * 32))
            (game / "game.toml").write_text(text)
            (game / "globals.toml").write_text((ROOT / "games/stub/globals.toml").read_text())
            cfg = game_config.load(game)
            self.assertEqual(cfg["game"]["guest_size"], 0x10100000)
            mod = cfg["aux_modules"][0]
            self.assertEqual(mod["key"], "blit")
            self.assertEqual(mod["name"], "Blit_p6.dll")
            self.assertEqual(mod["path"], (game / "original/Blit_p6.dll").resolve())
            self.assertEqual(mod["listings_path"], (game / "analysis/Blit_p6.dll").resolve())
            self.assertEqual(mod["function_alignment"], 1)
            self.assertEqual((mod["base"], mod["size"]), (0x10000000, 0x28000))
            header = gen_game_config.render_header(cfg)
            self.assertIn("#define RECOMP_GUEST_SIZE 0x10100000u", header)
            self.assertIn("#define RECOMP_AUX_MODULE_COUNT 1", header)
            self.assertIn('{"Blit_p6.dll", "%s", "%s", 0x10000000u, 0x00028000u}' % ((game / "original/Blit_p6.dll").resolve().as_posix(), "ab" * 32),
                          header)
            cmake = gen_game_config.render_cmake(cfg)
            self.assertIn("set(RECOMP_GUEST_SIZE 0x10100000u)", cmake)
            self.assertIn("set(RECOMP_AUX_MODULES blit)", cmake)
            bad = text.replace('guest_size = 0x10100000', 'guest_size = 0x10000000')
            (game / "game.toml").write_text(bad)
            with self.assertRaises(ValueError):
                game_config.load(game)   # a module needs an arena that reaches it: checked at load

    def test_touch_keypad_knob(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            (game / "globals.toml").write_text((ROOT / "games/stub/globals.toml").read_text())
            stub = (ROOT / "games/stub/game.toml").read_text()
            base = stub[:stub.index("[touch]")] + stub[stub.index("[bundle]"):]  # without the stub's own [touch]
            (game / "game.toml").write_text(base)
            self.assertIn("#define RECOMP_TOUCH_KEYPAD_HIDDEN 0", gen_game_config.render_header(game_config.load(game)))
            (game / "game.toml").write_text(base + '\n[touch]\nkeypad = "hidden"\n')
            self.assertIn("#define RECOMP_TOUCH_KEYPAD_HIDDEN 1", gen_game_config.render_header(game_config.load(game)))
            (game / "game.toml").write_text(base + '\n[touch]\nkeypad = "sometimes"\n')
            with self.assertRaises(ValueError):
                game_config.load(game)

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
