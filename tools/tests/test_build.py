"""Build-tool platform gates and translator arguments, without invoking the translator."""

import importlib.util
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

import pytest

spec = importlib.util.spec_from_file_location("build", Path(__file__).parents[1] / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def test_android_templates_render(tmp_path):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    out = build.android_project(tmp_path, cfg, gen_dir=tmp_path / "gen")
    manifest = (out / "app/src/main/AndroidManifest.xml").read_text()
    assert 'package="dev.recompkit.stub"' in manifest
    gradle = (out / "app/build.gradle.kts").read_text()
    assert "StubRecomp" in gradle
    assert 'applicationId = "dev.recompkit.stub"' in gradle
    assert 'resValue("string", "game_id", "stub")' in gradle
    sdl_java = tmp_path / "gen/_deps/sdl3-src/android-project/app/src/main/java"
    assert sdl_java.resolve().as_posix() in gradle
    assert "externalNativeBuild" not in gradle
    android = "{http://schemas.android.com/apk/res/android}"
    root = ET.fromstring(manifest)
    feature = root.find("uses-feature")
    assert feature.get(android + "name") == "android.hardware.vulkan.version"
    assert int(feature.get(android + "version"), 16) == (1 << 22) | (1 << 12)
    assert feature.get(android + "required") == "true"
    app = root.find("application")
    assert app.get(android + "label") == "@string/app_name"
    assert app.get(android + "requestLegacyExternalStorage") == "false"
    assert app.find("activity").get(android + "screenOrientation") == "landscape"


@pytest.mark.parametrize("system", ["Darwin", "Linux", "Windows"])
def test_android_target_selects_the_ndk_preset(system):
    for extra, preset in [([], "android"), (["--stub"], "android-stub")]:
        args, _ = build.parse_args(["--target", "android"] + extra, system=system)
        assert build.preset_name(args.preset, args.config, stub=args.stub, target=args.target) == preset
    assert build.TARGETS["android"] == ["recomp_app"]


@pytest.mark.parametrize("devices, console, actions", [
    ("", True, []),
    ("phone\toffline\nlocked\tunauthorized\n", True, []),
    ("phone\tdevice\n", False, ["install", "shell"]),
    ("phone\tdevice\n", True, ["install", "shell", "logcat"]),
])
def test_android_install_requires_a_ready_device(tmp_path, monkeypatch, devices, console, actions):
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "List of devices attached\n" + devices)

    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    monkeypatch.setattr(build.subprocess, "run", run)
    apk = tmp_path / "app-debug.apk"
    build.android_install_and_launch(apk, "dev.recompkit.stub", console=console)
    assert calls[0] == ["/sdk/adb", "devices"]
    assert [command[3] for command in calls[1:]] == actions
    if actions:
        assert calls[1] == ["/sdk/adb", "-s", "phone", "install", "-r", str(apk)]
        assert calls[2] == ["/sdk/adb", "-s", "phone", "shell", "am", "start", "-n",
                            "dev.recompkit.stub/dev.recompkit.RecompActivity"]


def test_android_install_does_not_guess_between_devices(tmp_path, monkeypatch):
    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "List of devices attached\nfirst\tdevice\nsecond\tdevice\n")

    monkeypatch.setattr(build.subprocess, "run", run)
    with pytest.raises(ValueError, match="Pass --device"):
        build.android_install_and_launch(tmp_path / "app-debug.apk", "dev.recompkit.stub")
    assert calls == [["/sdk/adb", "devices"]]


def test_app_target_allowed_on_linux():
    args, _ = build.parse_args(
        ["--target", "app", "--game-dir", str(build.ROOT / "games/stub")], system="Linux")
    assert args.preset == "linux"


def test_ios_target_still_needs_macos(capsys):
    with pytest.raises(SystemExit) as error:
        build.parse_args(
            ["--target", "ios", "--stub", "--game-dir", str(build.ROOT / "games/stub")], system="Linux")
    assert error.value.code == 2
    assert "The iOS packager runs on macOS" in capsys.readouterr().err


def test_allow_table_gaps_reaches_the_translator(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(build.subprocess, "run", lambda cmd, **kw: calls.append(cmd))
    build.run_translator(tmp_path / "stage", tmp_path, tmp_path / "build", allow_table_gaps="switch 004ab2af")
    assert "--allow-table-gaps" in calls[0]
    assert calls[0][calls[0].index("--allow-table-gaps") + 1] == "switch 004ab2af"


def test_no_table_gap_flag_by_default(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(build.subprocess, "run", lambda cmd, **kw: calls.append(cmd))
    build.run_translator(tmp_path / "stage", tmp_path, tmp_path / "build")
    assert "--allow-table-gaps" not in calls[0]
