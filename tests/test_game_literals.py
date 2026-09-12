"""No game-specific literal in kit code; see tools/check_game_literals.py."""

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "check_game_literals", Path(__file__).resolve().parents[1] / "tools/check_game_literals.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class GameLiteralTests(unittest.TestCase):
    def test_kit_code_has_no_game_literals(self):
        self.assertEqual(list(check.findings()), [])

    def test_comments_are_ignored(self):
        lines = dict(check.code_lines("int a; // D3DPopTB\n/* PopRecomp\n */ int b;\n"))
        self.assertNotIn("D3DPopTB", lines[1])
        self.assertNotIn("PopRecomp", lines[2])
        self.assertIn("int b;", lines[3])


if __name__ == "__main__":
    unittest.main()
