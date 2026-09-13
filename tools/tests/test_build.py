"""Build-tool translator arguments, without invoking the translator."""

import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("build", Path(__file__).parents[1] / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


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
