"""Delphi checkpoints and omitted landing blocks, without a game image."""
import json
import struct
import sys

import pytest

from test_translate_driver import synthetic_image
from test_translate_insns import Case, T, translate_case

BASE = 0x0D02A000


def seh_case(typed=False, absolute=False, default=False):
    stub, routine, handler = BASE + 0x40, BASE + 0xa0, BASE + 0x80
    fs = "0x0" if absolute else "EAX"
    lines, code = [], bytearray()

    def add(text, raw):
        lines.append((len(code), text))
        code.extend(bytes.fromhex(raw))

    add("XOR EAX,EAX", "31c0")
    add("PUSH EBP", "55")
    add("PUSH 0x%x" % stub, (b"\x68" + struct.pack("<I", stub)).hex())
    add("PUSH dword ptr FS:[%s]" % fs, "64ff3500000000" if absolute else "64ff30")
    add("MOV dword ptr FS:[%s],ESP" % fs, "64892500000000" if absolute else "648920")
    add("POP EDX", "5a")
    add("POP ECX", "59")
    add("POP ECX", "59")
    add("MOV dword ptr FS:[%s],EDX" % fs, "64891500000000" if absolute else "648910")
    end = BASE + len(code)
    add("RET", "c3")
    code.extend(b"\xcc" * (0xa6 - len(code)))
    code[0x40:0x45] = b"\xe9" + struct.pack("<i", routine - stub - 5)
    landing = handler if typed else stub + 5
    if typed:
        code[0x45:0x51] = struct.pack("<III", 1, routine, handler)
        if default:
            code[0x45:0x59] = struct.pack("<IIIII", 2, routine, handler, 0, BASE + 0x90)
            code[0x90:0x9a] = b"\xb8\x2b\x00\x00\x00\xe9" + struct.pack("<i", end - BASE - 0x9a)
    off = landing - BASE
    # Rejoin an instruction in the listed body through an alternate entry.
    code[off:off + 10] = b"\xb8\x2a\x00\x00\x00\xe9" + struct.pack("<i", end - landing - 10)
    code[0xa0:0xa6] = b"\xb8\x01\x00\x00\x00\xc3"
    return Case("seh_frame", BASE, lines, code.hex()), landing, routine


@pytest.mark.parametrize("absolute", [False, True])
def test_checkpoints_live_in_the_establishing_function(absolute):
    case, _, _ = seh_case(absolute=absolute)
    text = translate_case(case)
    assert "recomp_seh_frame_enter(c)" in text
    assert "if (setjmp(*b_)) { recomp_seh_land(c); return; }" in text
    assert "recomp_seh_frame_leave(c)" in text


def test_other_teb_writes_do_not_create_checkpoints():
    case, _, _ = seh_case()
    case.lines = [(off, text.replace("FS:[EAX]", "FS:[0x4]")) for off, text in case.lines]
    assert "recomp_seh_" not in translate_case(case)


@pytest.mark.parametrize("reg,zeroed,checkpoint", [("EDX", True, True), ("EDX", False, False),
                                                 ("ESP", True, False)])
def test_frame_establishment_through_edx_needs_a_zero_base(reg, zeroed, checkpoint):
    stub = BASE + 0x40
    lines, code = [], bytearray()

    def add(text, raw):
        lines.append((len(code), text))
        code.extend(raw)

    r = 2 if reg == "EDX" else 4
    sib = b"\x24" if reg == "ESP" else b""
    add(("XOR" if zeroed else "OR") + " %s,%s" % (reg, reg),
        bytes([0x31 if zeroed else 0x09, 0xc0 + 9 * r]))
    add("PUSH EBP", b"\x55")
    add("PUSH 0x%x" % stub, b"\x68" + struct.pack("<I", stub))
    add("PUSH dword ptr FS:[%s]" % reg, bytes([0x64, 0xff, 0x30 + r]) + sib)
    add("MOV dword ptr FS:[%s],ESP" % reg, bytes([0x64, 0x89, 0x20 + r]) + sib)
    add("XOR EAX,EAX", b"\x31\xc0")
    add("POP EDX", b"\x5a")
    add("POP ECX", b"\x59")
    add("POP ECX", b"\x59")
    add("MOV dword ptr FS:[EAX],EDX", b"\x64\x89\x10")
    add("RET", b"\xc3")
    # PUSH changes ESP even after it was zeroed; it cannot stay FS:[0].
    text = translate_case(Case("edx_frame", BASE, lines, code.hex()))
    assert ("recomp_seh_frame_enter(c)" in text) == checkpoint
    assert ("recomp_seh_frame_leave(c)" in text) == checkpoint


@pytest.mark.parametrize("typed,default,speculative", [
    (False, False, False), (True, False, False), (True, True, False),
    (False, False, True)])
def test_omitted_landing_is_a_structural_entry(tmp_path, monkeypatch, typed, default, speculative):
    case, landing, routine = seh_case(typed=typed, default=default)
    img = synthetic_image({BASE: case.code}, base=BASE - 0x100, size=0x1000)
    listings = tmp_path / "functions"
    listings.mkdir()
    (listings / ("%08x.asm" % BASE)).write_text(case.listing())
    (listings / ("%08x.asm" % routine)).write_text(
        "%08x  MOV EAX,0x1\n%08x  RET\n" % (routine, routine + 5))
    table = tmp_path / "functions.tsv"
    table.write_text("address\tname\tsize\n%08x\tseh_frame\t%d\n%08x\tdispatcher\t6\n" %
                     (BASE, case.lines[-1][0] + 1, routine))
    if speculative:
        # An earlier pointer guess owns some real instructions, but its own
        # beginning jumps outside the image and is pruned. The SEH block's
        # jump back into that body must retain a real, independently recovered
        # entry rather than vanish with the speculative owner.
        prefix = b"\x85\xc0\x75\x1c\xe9" + struct.pack("<i", -0x10000)
        img = synthetic_image({BASE: case.code, BASE - 0x20: prefix},
                              base=BASE - 0x100, size=0x1000)
        img.code_pointers = lambda *a, **kw: ({BASE - 0x20}, set())
        (listings / ("%08x.asm" % BASE)).unlink()
        table.write_text("address\tname\tsize\n%08x\tdispatcher\t6\n" % routine)
    binary = tmp_path / "image"
    binary.write_bytes(img.data)
    curated = tmp_path / "globals.toml"
    curated.write_text("")
    out = tmp_path / "gen"
    report = out / "translate-report.json"
    monkeypatch.setattr(T, "configure", lambda cfg: None)
    monkeypatch.setattr(T.game_config, "load", lambda path: {})
    for name, value in (("LISTINGS", listings), ("FUNCS_TSV", table),
                        ("BINARY", binary), ("CURATED", curated)):
        monkeypatch.setattr(T, name, str(value))
    monkeypatch.setattr(T, "EXTRA_ENTRY_POINTS", frozenset())
    monkeypatch.setattr(T, "Image", lambda path: img)
    monkeypatch.setattr(sys, "argv", ["translate.py", "--game", str(tmp_path),
                                     "--out", str(out), "--report", str(report), "--quiet"])
    assert T.main() == 0
    text = "\n".join(p.read_text() for p in out.glob("*.c"))
    assert "void fn_%08x(" % landing in text
    if typed:
        assert "void fn_%08x(" % (BASE + 0x45) not in text
    if default:
        assert "void fn_%08x(" % (BASE + 0x90) in text
    if speculative:
        assert "void fn_%08x(" % (BASE + case.lines[-1][0]) in text
        assert "void fn_%08x(" % (BASE - 0x20) not in text
    symbols = json.loads((out / "symbols.json").read_text())
    assert any(f["addr"] == "%08x" % landing and f["provenance"] == "seh"
               for f in symbols["functions"])
    assert next(f for f in symbols["functions"] if f["addr"] == "%08x" % routine)["provenance"] != "seh"
    assert json.loads(report.read_text())["provenance"]["seh"] >= 1
    table_text = (out / "table.c").read_text()
    jump = table_text.split("void recomp_jump(", 1)[1]
    assert "recomp_seh_intercept(c, target)" in jump
    assert jump.index("recomp_seh_intercept") < jump.index("recomp_lookup")


def test_checkpoint_trace_identifies_the_establishing_and_restoring_instructions():
    case, _, _ = seh_case()
    text = translate_case(case)
    stores = [BASE + off for off, asm in case.lines if asm.startswith("MOV dword ptr FS:")]
    for site in stores:
        assert "c->eip = 0x%xu;" % site in text
