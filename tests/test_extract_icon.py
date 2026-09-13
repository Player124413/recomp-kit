"""tools/extract_icon.py rebuilds the executable's icon group and writes the iPad icon set."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("extract_icon", ROOT / "tools/extract_icon.py")
extract_icon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract_icon)

EXE = ROOT / "original/gog/D3DPopTB.exe"


@unittest.skipUnless(EXE.is_file(), "needs the developer's game installation")
class IconTests(unittest.TestCase):
    def test_ico_has_three_frames_and_largest_is_48(self):
        ico = extract_icon.ico_bytes(EXE)
        self.assertEqual(ico[:6], b"\x00\x00\x01\x00\x03\x00")
        frame = extract_icon.largest_frame(ico)
        self.assertEqual(frame.size, (48, 48))

    def test_writes_every_ios_size(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            paths = extract_icon.write_icons(EXE, tmp)
            self.assertEqual([p.name for p in paths],
                             ["Icon-29@2x.png", "Icon-40@2x.png", "Icon-60@2x.png", "Icon-76@2x.png", "Icon-83.5@2x.png"])
            for path, (_, px) in zip(paths, extract_icon.IOS_ICONS):
                with Image.open(path) as image:
                    self.assertEqual(image.size, (px, px))
                    self.assertEqual(image.mode, "RGB")


if __name__ == "__main__":
    unittest.main()
