"""Stage desktop builds and notices without copying the player's game installation."""

from pathlib import Path
import platform
import shutil
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def stage(app_binary: Path, cfg: dict, out_dir: Path, system=None) -> Path:
    """Copy the app and host resources into a folder, plus a Linux tarball.

    Only named build resources are copied. Shaders are embedded in the host;
    SoundFonts and optional replacement textures belong to the player's game.
    """
    system = system or platform.system()
    if system not in {"Linux", "Windows"}:
        raise ValueError("Desktop staging supports Linux and Windows")
    game = cfg["game"]
    app_name = game["app_name"]
    binary_name = app_name + (".exe" if system == "Windows" else "")
    staged = out_dir / app_name
    resources = staged / "resources"
    resources.mkdir(parents=True, exist_ok=True)
    files = []

    def copy(source, destination):
        shutil.copy2(source, destination)
        files.append(destination)

    copy(app_binary, staged / binary_name)
    for name in ("LICENSE", "NOTICE"):
        copy(ROOT / name, staged / name)
    copy(ROOT / "tools/recomp/baseline/classic-modes.json", resources / "classic-modes.json")
    # The translation index lives beside the binary with the desktop Ninja
    # presets, just as finish_bundle.py reads it from build/recomp on macOS.
    symbols = app_binary.parent / "symbols.json"
    if symbols.is_file():
        copy(symbols, resources / "symbols.json")
    else:
        (resources / "symbols.json").unlink(missing_ok=True)

    executable = game["executable"]
    if system == "Windows":
        launch = (f'In PowerShell, from this folder:\n'
                  f'  $env:RECOMP_EXE = "C:\\path\\to\\your game\\{executable}"\n'
                  f'  .\\{binary_name}\n')
    else:
        launch = (f'In a terminal, from this folder:\n'
                  f'  RECOMP_EXE="/path/to/your game/{executable}" ./{binary_name}\n')
    readme = staged / "README.txt"
    readme.write_text(
        f"{app_name} - {game['name']}\n\n"
        f"Use your own supported installation containing {executable} and all\n"
        "its data directories. Keep the executable in that installation: the\n"
        "host uses its parent directory as the game data root. No game files\n"
        "are included in this package.\n\n"
        + launch + "\n"
        "RECOMP_EXE names the executable, not a directory. Without an explicit\n"
        "path, the host tries the configured developer executable, then a\n"
        "previously saved executable path, then opens a file picker. It does\n"
        "not search a game/ directory beside this binary.\n\n"
        "Keep resources/ beside the app. Shaders are embedded in the binary;\n"
        "any game SoundFont stays in your installation. See LICENSE and NOTICE.\n",
        encoding="utf-8")
    files.append(readme)

    if system == "Linux":
        machine = platform.machine().lower()
        arch = {"amd64": "x86_64", "arm64": "aarch64"}.get(machine, machine)
        archive = out_dir / f"{app_name}-linux-{arch}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(staged, arcname=app_name, recursive=False)
            tar.add(resources, arcname=f"{app_name}/resources", recursive=False)
            # Never sweep a reused output folder: it may contain player files.
            for file in files:
                tar.add(file, arcname=f"{app_name}/{file.relative_to(staged).as_posix()}")
    return staged
