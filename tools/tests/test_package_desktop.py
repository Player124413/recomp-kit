"""Desktop packages use synthetic binaries and never need a game installation."""

import importlib.util
from pathlib import Path
import stat
import subprocess
import tarfile
from unittest.mock import patch

import pytest

spec = importlib.util.spec_from_file_location("package_desktop", Path(__file__).parents[1] / "package_desktop.py")
package_desktop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_desktop)


def test_stage_layout(tmp_path):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    with patch("platform.machine", return_value="x86_64"):
        out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux")
    assert (out / "StubRecomp").is_file()
    assert "STUB.EXE" in (out / "README.txt").read_text()
    assert (out / "LICENSE").is_file()
    assert (tmp_path / "out" / "StubRecomp-linux-x86_64.tar.gz").is_file()


@pytest.mark.parametrize("machine,arch", [("x86_64", "x86_64"), ("AMD64", "x86_64"),
                                         ("aarch64", "aarch64"), ("arm64", "aarch64")])
def test_linux_archive_contents(tmp_path, monkeypatch, machine, arch):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    exe.chmod(0o755)
    (tmp_path / "symbols.json").write_text('{"functions": []}')
    (tmp_path / "STUB.EXE").write_bytes(b"private game")
    (tmp_path / "sound.sf2").write_bytes(b"private SoundFont")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    monkeypatch.setattr(package_desktop.platform, "machine", lambda: machine)
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux")
    expected = {"StubRecomp", "LICENSE", "NOTICE", "README.txt", "resources",
                "resources/classic-modes.json", "resources/symbols.json"}
    assert {p.relative_to(out).as_posix() for p in out.rglob("*")} == expected
    assert (out / "StubRecomp").read_bytes() == exe.read_bytes()
    assert stat.S_IMODE((out / "StubRecomp").stat().st_mode) == stat.S_IMODE(exe.stat().st_mode)
    for name in ("LICENSE", "NOTICE"):
        assert (out / name).read_bytes() == (package_desktop.ROOT / name).read_bytes()
    assert (out / "resources/classic-modes.json").read_bytes() == (
        package_desktop.ROOT / "tools/recomp/baseline/classic-modes.json").read_bytes()
    assert (out / "resources/symbols.json").read_bytes() == (tmp_path / "symbols.json").read_bytes()
    readme = (out / "README.txt").read_text()
    assert 'RECOMP_EXE="/path/to/your game/STUB.EXE" ./StubRecomp' in readme
    assert "parent directory as the game data root" in readme
    assert "RECOMP_DATA=" not in readme
    # Repackaging must preserve a player's files without adding them to the archive.
    (out / "player.sav").write_bytes(b"private save")
    package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux")
    assert (out / "player.sav").read_bytes() == b"private save"
    with tarfile.open(tmp_path / "out" / f"StubRecomp-linux-{arch}.tar.gz") as tar:
        assert set(tar.getnames()) == {"StubRecomp"} | {f"StubRecomp/{p}" for p in expected}
        assert tar.extractfile("StubRecomp/StubRecomp").read() == b"\x7fELF"
        assert tar.getmember("StubRecomp/StubRecomp").mode == stat.S_IMODE(exe.stat().st_mode)


def test_windows_folder(tmp_path):
    exe = tmp_path / "recomp_app.exe"
    exe.write_bytes(b"MZ")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Windows")
    assert (out / "StubRecomp.exe").read_bytes() == b"MZ"
    assert (out / "NOTICE").is_file()
    assert (out / "LICENSE").is_file()
    assert (out / "resources/classic-modes.json").is_file()
    assert not (out / "resources/symbols.json").exists()
    assert not list((tmp_path / "out").glob("*.tar.gz"))
    readme = (out / "README.txt").read_text()
    assert '$env:RECOMP_EXE = "C:\\path\\to\\your game\\STUB.EXE"' in readme
    assert ".\\StubRecomp.exe" in readme


@pytest.mark.parametrize("system,target,stub,packages", [
    ("Linux", "app", False, True), ("Windows", "app", False, True),
    ("Darwin", "app", False, False), ("Linux", "app", True, False),
    ("Windows", "app", True, False), ("Linux", "smoke", False, False),
    ("Windows", "headless", False, False),
])
def test_build_packages_only_successful_desktop_apps(tmp_path, monkeypatch, system, target, stub, packages):
    spec = importlib.util.spec_from_file_location("build", Path(__file__).parents[1] / "build.py")
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    (tmp_path / "game.toml").touch()
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    argv = ["build.py", "--game-dir", str(tmp_path), "--target", target] + (["--stub"] if stub else [])
    monkeypatch.setattr(build.sys, "argv", argv)
    monkeypatch.setattr(build.platform, "system", lambda: system)
    monkeypatch.setattr(build.game_config, "load", lambda path: cfg)
    monkeypatch.setattr(build, "texture_pack", lambda *args: None)
    monkeypatch.setattr(build, "configure", lambda *args, **kw: None)
    binary = tmp_path / "build/recomp" / ("StubRecomp.exe" if system == "Windows" else "StubRecomp")
    calls = []

    def fake_build(*args, **kwargs):
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(b"fake native binary")
        calls.append("built")

    def fake_stage(app_binary, config, out_dir, system=None):
        assert calls == ["built"]
        assert app_binary == binary and app_binary.is_file()
        assert config == cfg
        assert out_dir == tmp_path / "build/package"
        assert system in {"Linux", "Windows"}
        calls.append("packaged")
        return out_dir / "StubRecomp"

    monkeypatch.setattr(build, "build", fake_build)
    monkeypatch.setattr(build.package_desktop, "stage", fake_stage)
    build.main()
    assert calls == (["built", "packaged"] if packages else ["built"])

    def failed_build(*args, **kwargs):
        raise subprocess.CalledProcessError(7, "cmake")

    calls.clear()
    monkeypatch.setattr(build, "build", failed_build)
    with pytest.raises(SystemExit) as error:
        build.main()
    assert error.value.code == 7
    assert not calls
