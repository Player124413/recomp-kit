#!/usr/bin/env python3
"""Pull the iOS app's Documents (saves, the seeded game directory's stamp) from the device.

    tools/ios_logs.py --device <identifier> [--out build/ios-pull]
"""

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--game", default="populous")
    parser.add_argument("--out", type=Path, default=ROOT / "build/ios-pull")
    args = parser.parse_args()
    bundle_id = game_config.load(ROOT / "games" / args.game)["game"]["bundle_id"]
    args.out.mkdir(parents=True, exist_ok=True)
    subprocess.run(["xcrun", "devicectl", "device", "copy", "from", "--device", args.device,
                    "--domain-type", "appDataContainer", "--domain-identifier", bundle_id,
                    "--source", "Documents", "--destination", str(args.out)], check=True)
    print("pulled Documents to", args.out)


if __name__ == "__main__":
    main()
