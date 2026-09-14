"""Driver-level rules of the translator that need no game to check.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_driver.py
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402


def test_a_withdrawn_block_leaves_a_trap_where_it_was_dispatched_to():
    """A function whose listing ends on a call that never returns (a C++
    throw, `exit`) falls through to padding.  The sweep recovers a block
    there, finds it dispatches nowhere and withdraws it; whatever referred to
    the block must then stop rather than call into nothing, and must not
    trip the entry-point gate.  Every spelling of a literal transfer to the
    block becomes `recomp_unknown_call(c, addr); return;`."""
    body = [
        "void fn_0061e4c0(X86 *c) {",
        "{   /* 0061e9ea CALL 0x0064cf1f */",
        "    c->r[4] -= 4; wr32(c->r[4], 0x0061e9efu);",
        "    CALL_FN(0064cf1f);",
        "}",
        "CALL_FN(0061e9ef); return;  /* fall-through past the listing */",
        "case 0x0061e9efu: CALL_FN(0061e9ef); return;",
        "c->eip = 0x0061e9eau; recomp_jump(c, 0x0061e9efu); return;",
        "c->eip = 0x61e9eau; recomp_jump(c, 0x61e9efu); return;",
        "CALL_FN(00500000);",
        "}",
    ]
    out = T.retarget_withdrawn(body, {0x0061E9EF})
    assert out[3] == "    CALL_FN(0064cf1f);"           # a live callee is untouched
    assert out[5].startswith("recomp_unknown_call(c, 0x0061e9efu); return;")
    assert out[5].endswith("/* fall-through past the listing */")
    assert out[6] == "case 0x0061e9efu: recomp_unknown_call(c, 0x0061e9efu); return;"
    assert out[7] == "recomp_unknown_call(c, 0x0061e9efu); return;"
    assert out[8] == "recomp_unknown_call(c, 0x0061e9efu); return;"   # hexlit's unpadded spelling
    assert out[9] == "CALL_FN(00500000);"
    assert "0061e9ef" not in "".join(hit for line in out for hit in
                                     __import__("re").findall(r"FN\(([0-9a-f]{8})\)", line))


def test_retargeting_nothing_is_the_identity():
    body = ["CALL_FN(00500000); return;", "recomp_jump(c, 0x00500010u); return;"]
    assert T.retarget_withdrawn(body, set()) == body


def synthetic_image(code_at, base=0x00400000, size=0x2000):
    """An Image over hand-placed bytes: {address: bytes}, one executable section."""
    import capstone
    img = T.Image.__new__(T.Image)
    img.base, img.size, img.end = base, size, base + size
    data = bytearray(size)
    for va, code in code_at.items():
        data[va - base:va - base + len(code)] = code
    img.data = bytes(data)
    img.reloc_dir = (0, 0)
    img.exec_ranges = [(base, base + size, ".text")]
    img.data_ranges = []
    img.recover_errors = []
    img.string_candidates = set()
    img.thunk_candidates = set()
    img.weak_candidates = set()
    img.interior_candidates = set()
    img.noreturn_callees = set()
    img.recover_errors = []
    img.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    return img


@pytest.mark.parametrize("code,expected", [
    (b"\xeb\xfe", True),                       # closed unconditional loop
    (b"\xc3", False),                          # ordinary return
    (b"\x75\x02\xeb\xfc\xc3", False),          # conditional return path
    (b"\xeb\x0e", False),                      # outward tail call
    (b"\xff\xe0", False),                      # computed tail call
    (b"\xe2\x0e\xeb\xfc", False),              # LOOP escapes the body
    (b"\xe3\x0e\xeb\xfc", False),              # JECXZ escapes the body
    (b"\xe8\xfb\x00\x00\x00\xeb\xf9", True), # callback inside a closed loop
])
def test_closed_loop_requires_every_return_path_to_stay_inside(code, expected):
    entry = 0x00401000
    img = synthetic_image({entry: code})
    img.md.detail = True
    insns = [img.to_insn(ci) for ci in img.md.disasm(code, entry)]
    fn = T.Function(entry, "loop", len(code), insns)
    fn.measure(img)
    assert T.Translator.closed_noreturn_loop(fn) == expected


@pytest.mark.parametrize("artifact", ["symbols.json", "translate-report.json"])
@pytest.mark.parametrize("push_ret", [False, True, "noreturn_loop"])
def test_output_records_the_loaded_image_base(tmp_path, monkeypatch, artifact, push_ret):
    """Nondefault images include omitted PUSH/RET epilogues, without pointer guesses."""
    import json
    import struct
    base, entry = 0x00600000, 0x00601000
    landing = entry + 6
    code = b"\x68" + struct.pack("<I", landing) + b"\xc3\x40\xc3" if push_ret else b"\xc3"
    loop = entry + 0x100
    if push_ret == "noreturn_loop":
        # Omitted continuation calls a shutdown loop, followed by non-code.
        code = code[:6] + b"\xe8" + struct.pack("<i", loop - landing - 5) + b"\x0f\x0b"
    img = synthetic_image({entry: code, loop: b"\xeb\xfe"}, base=base)
    img.plausible_immediate_target = lambda addr: False
    img.code_pointers = lambda *args, **kwargs: (set(), set())
    listings = tmp_path / "functions"
    listings.mkdir()
    listing = ("%08x  PUSH 0x%x\n%08x  RET\n" % (entry, landing, entry + 5)
               if push_ret else "%08x  RET\n" % entry)
    (listings / ("%08x.asm" % entry)).write_text(listing)
    table = tmp_path / "functions.tsv"
    table.write_text("address\tname\tsize\n%08x\treturn_only\t1\n" % entry)
    if push_ret == "noreturn_loop":
        (listings / ("%08x.asm" % loop)).write_text("%08x  JMP 0x%x\n" % (loop, loop))
        with table.open("a") as fh:
            fh.write("%08x\tshutdown_loop\t2\n" % loop)
    binary = tmp_path / "synthetic-image"
    binary.write_bytes(img.data)
    curated = tmp_path / "globals.toml"
    curated.write_text("")
    out = tmp_path / "gen"
    report = out / "translate-report.json"
    monkeypatch.setattr(T, "configure", lambda cfg: None)
    monkeypatch.setattr(T.game_config, "load", lambda path: {})
    monkeypatch.setattr(T, "LISTINGS", str(listings))
    monkeypatch.setattr(T, "FUNCS_TSV", str(table))
    monkeypatch.setattr(T, "BINARY", str(binary))
    monkeypatch.setattr(T, "CURATED", str(curated))
    monkeypatch.setattr(T, "Image", lambda path: img)
    monkeypatch.setattr(sys, "argv", ["translate.py", "--game", str(tmp_path),
                                     "--out", str(out), "--report", str(report), "--quiet"])
    assert T.main() == 0
    result = json.loads((out / artifact).read_text())
    assert result["image_base"] == "%08x" % base
    if push_ret:
        text = "\n".join(p.read_text() for p in out.glob("chunk_*.c"))
        assert "void fn_%08x(" % landing in text
        assert "CALL_FN(%08x); return;" % landing in text


def test_computed_returns_use_sorted_call_continuations(tmp_path, monkeypatch):
    """Direct and indirect CALL lengths name returns, not new dispatch entries."""
    import re
    import struct
    entry, callee = 0x00601000, 0x00601080
    code = (b"\xb8" + struct.pack("<I", callee) + b"\xff\xd0\xe8"
            + struct.pack("<i", callee - entry - 12)
            + b"\xff\x15\x00\x18\x60\x00\xc3")
    img = synthetic_image({entry: code, callee: b"\xc3"}, base=0x00600000)
    img.code_pointers = lambda *args, **kwargs: (set(), set())
    listings = tmp_path / "functions"
    listings.mkdir()
    (listings / ("%08x.asm" % entry)).write_text(
        "00601000  MOV EAX,0x601080\n00601005  CALL EAX\n"
        "00601007  CALL 0x601080\n0060100c  CALL dword ptr [0x601800]\n00601012  RET\n")
    (listings / ("%08x.asm" % callee)).write_text("00601080  RET\n")
    table = tmp_path / "functions.tsv"
    table.write_text("address\tname\tsize\n00601000\tcaller\t19\n00601080\tcallee\t1\n")
    binary, curated = tmp_path / "image", tmp_path / "globals.toml"
    binary.write_bytes(img.data)
    curated.write_text("")
    out = tmp_path / "gen"
    monkeypatch.setattr(T, "configure", lambda cfg: None)
    monkeypatch.setattr(T.game_config, "load", lambda path: {})
    for name, value in (("LISTINGS", listings), ("FUNCS_TSV", table),
                        ("BINARY", binary), ("CURATED", curated)):
        monkeypatch.setattr(T, name, str(value))
    monkeypatch.setattr(T, "EXTRA_ENTRY_POINTS", frozenset())
    monkeypatch.setattr(T, "Image", lambda path: img)
    monkeypatch.setattr(sys, "argv", ["translate.py", "--game", str(tmp_path),
                                     "--out", str(out), "--quiet"])
    assert T.main() == 0
    text = (out / "table.c").read_text()
    assert "int recomp_is_call_return(uint32_t target)" in text
    array = text.split("recomp_call_returns[] = {", 1)[1].split("};", 1)[0]
    assert [int(a, 16) for a in re.findall(r"0x([0-9a-f]+)u", array)] == [
        entry + 7, entry + 12, entry + 18]
    jump = text.split("void recomp_jump(", 1)[1].split("void recomp_unknown_jump(", 1)[0]
    assert jump.index("if (i >= 0)") < jump.index("recomp_is_call_return(target)")
    assert "if (recomp_is_call_return(target)) { c->eip = target; return; }" in jump
    assert jump.index("recomp_is_call_return(target)") < jump.index("recomp_unknown_jump(c, target)")
    call = text.split("void recomp_call(", 1)[1].split("void recomp_jump(", 1)[0]
    assert "recomp_is_call_return" not in call


@pytest.mark.parametrize("target,dispatch", [(0x0060100b, True),
                                           (0x00601002, False),
                                           (0x00601100, False)])
def test_return_switch_requires_pushed_instruction_boundary(target, dispatch):
    """Only a pushed instruction in this body makes RET an interior dispatch."""
    import struct
    from test_translate_insns import Opts
    entry = 0x00601000
    code = b"\x68" + struct.pack("<I", target) + b"\xb8\x2a\x00\x00\x00\xc3\xc2\x08\x00"
    img = synthetic_image({entry: code}, base=0x00600000)
    insns = T.parse_listing_text(
        "00601000  PUSH 0x%x\n00601005  MOV EAX,0x2a\n0060100a  RET\n0060100b  RET 0x8\n" % target)
    fn = T.Function(entry, "cleanup", len(code), insns)
    fn.measure(img)
    tr = T.Translator(img, {entry}, Opts())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn, [entry + 5]))
    assert ("switch (r_)" in text) == dispatch
    if dispatch:
        assert text.count("switch (r_)") == 2
        assert text.count("case 0x60100bu: goto L_0060100b;") == 2
        assert "c->r[4] += 12u;" in text
        assert text.count("default: c->eip = r_; return;") == 2
    assert "void fn_00601005(X86 *c) { body_00601000(c, 0x601005u); }" in text


@pytest.mark.parametrize("indirect", [False, True])
def test_tableless_jump_uses_all_local_instruction_labels(indirect):
    from test_translate_insns import Opts
    entry = 0x00601000
    code = b"\xff\xe0\x90\xc3" if indirect else b"\x89\xc0\x90\xc3"
    img = synthetic_image({entry: code}, base=0x00600000)
    listing = "00601000  %s\n00601002  NOP\n00601003  RET\n" % (
        "JMP EAX" if indirect else "MOV EAX,EAX")
    fn = T.Function(entry, "local_dispatch", len(code), T.parse_listing_text(listing))
    fn.measure(img)
    tr = T.Translator(img, {entry}, Opts())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert ("switch (t_)" in text) == indirect
    if indirect:
        assert "if (t_ >= 0x601000u && t_ < 0x601004u)" in text
        for addr in sorted(fn.addrs):
            assert "case %s: goto L_%08x;" % (T.hexlit(addr), addr) in text
            assert "L_%08x: ;" % addr in text
        assert "default: break;" in text
        assert text.index("default: break;") < text.index("recomp_jump(c, t_)")
        assert "case 0x601001u:" not in text  # inside an instruction uses the existing fallback


@pytest.mark.parametrize("jump_to_cleanup", [False, True])
@pytest.mark.parametrize("listed_cleanup", [False, True])
@pytest.mark.parametrize("recovered_owner", [False, True, "speculative_epilogue",
                                           "speculative_body", "zero_prefix", "config", "direct"])
def test_finally_cleanup_and_epilogue_belong_to_establishing_body(
        tmp_path, monkeypatch, jump_to_cleanup, listed_cleanup, recovered_owner,
        complete_listing=False, epilogue_handler=False, edx_frame=False):
    """Omitted normal cleanup is also callable through an alternate SEH entry."""
    import struct
    entry, stub, epilogue, helper = 0x00601000, 0x00601040, 0x00601060, 0x00601080
    dispatcher = 0x00601090
    code = bytearray(b"\x55\x89\xe5\x31\xc0\x55\x68" + struct.pack("<I", stub)
                     + b"\x64\xff\x30\x64\x89\x20\x5a\x59\x59\x64\x89\x10\x68"
                     + struct.pack("<I", epilogue))
    if edx_frame:
        code[3:5] = b"\x31\xd2"  # XOR EDX,EDX before the three frame pushes
        code[13] = 0x32           # PUSH FS:[EDX]
        code[16] = 0x22           # MOV FS:[EDX],ESP
        code[17:17] = b"\x31\xc0"  # restore later through FS:[EAX]
    cleanup = 0x00601050 if jump_to_cleanup else entry + len(code)
    if jump_to_cleanup:
        code += b"\xe9" + struct.pack("<i", cleanup - entry - len(code) - 5)
    listed_end = len(code)
    cleanup_code = b"\x90\xe8" + struct.pack("<i", helper - cleanup - 6) + b"\xc3"
    img = synthetic_image({entry: bytes(code), cleanup: cleanup_code,
                           stub: b"\xe9" + struct.pack("<i", dispatcher - stub - 5)
                                 + b"\xe9" + struct.pack("<i", cleanup - stub - 10),
                           epilogue: b"\x89\xec\x5d\xc3", helper: b"\xc3", dispatcher: b"\xc3"},
                          base=0x00600000)
    img.code_pointers = lambda *a, **kw: (set(), set())
    img.plausible_immediate_target = lambda addr: False
    img.md.detail = True
    insns = [img.to_insn(ci) for ci in img.md.disasm(bytes(code), entry)]
    if complete_listing:
        insns += [img.to_insn(ci) for ci in img.md.disasm(cleanup_code, cleanup)]
    img.md.detail = False
    listings = tmp_path / "functions"
    listings.mkdir()
    (listings / ("%08x.asm" % entry)).write_text("\n".join(
        "%08x  %s %s" % (i.addr, i.mnem, ",".join(i.ops)) for i in insns) + "\n")
    for addr in (helper, dispatcher):
        (listings / ("%08x.asm" % addr)).write_text("%08x  RET\n" % addr)
    table = tmp_path / "functions.tsv"
    table.write_text("address\tname\tsize\n%08x\testablishing\t%d\n%08x\thelper\t1\n%08x\tdispatcher\t1\n"
                     % (entry, listed_end, helper, dispatcher))
    if listed_cleanup:
        (listings / ("%08x.asm" % cleanup)).write_text(
            "%08x  NOP\n%08x  CALL 0x%x\n%08x  RET\n" % (cleanup, cleanup + 1, helper, cleanup + 6))
        with table.open("a") as fh:
            fh.write("%08x\tshared_cleanup\t7\n" % cleanup)
    if recovered_owner:
        (listings / ("%08x.asm" % entry)).unlink()
        table.write_text("\n".join(line for line in table.read_text().splitlines()
                                   if not line.startswith("%08x\t" % entry)) + "\n")
        img.code_pointers = lambda *a, **kw: ({entry}, set())
    if recovered_owner == "direct":
        # Entry protection follows a listed caller's direct edge, even though
        # the callee itself is absent from the listings.
        caller = entry + 0x100
        raw = b"\xe8" + struct.pack("<i", entry - caller - 5) + b"\xc3"
        data = bytearray(img.data)
        data[caller - img.base:caller - img.base + len(raw)] = raw
        img.data = bytes(data)
        (listings / ("%08x.asm" % caller)).write_text(
            "%08x  CALL 0x%x\n%08x  RET\n" % (caller, entry, caller + 5))
        with table.open("a") as fh:
            fh.write("%08x\tcaller\t6\n" % caller)
    if recovered_owner in ("speculative_epilogue", "speculative_body", "zero_prefix"):
        # A pointer guess owns the real epilogue, but has an invalid prefix.
        # Adopting the epilogue must not import that prefix into the real body.
        prefix = entry - 0x20
        target = epilogue if recovered_owner == "speculative_epilogue" else entry + 6
        raw = (b"\x0f\x84" + struct.pack("<i", target - prefix - 6)
               + b"\xe9" + struct.pack("<i", -0x10000))
        if recovered_owner == "zero_prefix":
            # This completely translatable block is nevertheless a data guess.
            # Its first instruction is ADD byte ptr [EAX],AL (00 00).
            raw = b"\x00\x00\xc3"
        data = bytearray(img.data)
        data[prefix - img.base:prefix - img.base + len(raw)] = raw
        img.data = bytes(data)
        img.code_pointers = lambda *a, **kw: ({prefix, entry}, set())
    if epilogue_handler:
        # A separately established handler branches into the same epilogue.
        # Resolving that structural entry must not split it out again after
        # the normal owner adopts it, or discovery never reaches a fixed point.
        other, other_stub = entry + 0x200, entry + 0x240
        raw = (b"\x31\xc0\x55\x68" + struct.pack("<I", other_stub)
               + b"\x64\xff\x30\x64\x89\x20\xc3")
        data = bytearray(img.data)
        data[other - img.base:other - img.base + len(raw)] = raw
        handler_target = epilogue + 2 if epilogue_handler == "interior" else epilogue
        if epilogue_handler == "prefix":
            handler_target = entry + 17  # restore the chain, then push and run cleanup
        data[other_stub - img.base:other_stub - img.base + 10] = (
            b"\xe9" + struct.pack("<i", dispatcher - other_stub - 5)
            + b"\xe9" + struct.pack("<i", handler_target - other_stub - 10))
        img.data = bytes(data)
        img.md.detail = True
        other_insns = [img.to_insn(ci) for ci in img.md.disasm(raw, other)]
        img.md.detail = False
        (listings / ("%08x.asm" % other)).write_text("\n".join(
            "%08x  %s %s" % (i.addr, i.mnem, ",".join(i.ops)) for i in other_insns) + "\n")
        if epilogue_handler in ("interior", "prefix"):
            img.code_pointers = lambda *a, **kw: ({entry, other}, set())
        else:
            with table.open("a") as fh:
                fh.write("%08x\tother_frame\t%d\n" % (other, len(raw)))
    binary, curated = tmp_path / "image", tmp_path / "globals.toml"
    binary.write_bytes(img.data)
    curated.write_text("")
    out = tmp_path / "gen"
    monkeypatch.setattr(T, "configure", lambda cfg: None)
    monkeypatch.setattr(T.game_config, "load", lambda path: {})
    for name, value in (("LISTINGS", listings), ("FUNCS_TSV", table),
                        ("BINARY", binary), ("CURATED", curated)):
        monkeypatch.setattr(T, name, str(value))
    seeds = {entry} if recovered_owner == "config" else set()
    if epilogue_handler is True:
        seeds.add(epilogue)
    monkeypatch.setattr(T, "EXTRA_ENTRY_POINTS", frozenset(seeds))
    monkeypatch.setattr(T, "Image", lambda path: img)
    monkeypatch.setattr(sys, "argv", ["translate.py", "--game", str(tmp_path), "--out", str(out), "--quiet"])
    assert T.main() == 0
    text = "\n".join(p.read_text() for p in out.glob("chunk_*.c"))
    assert "void fn_%08x(X86 *c) { body_%08x(c, %s); }" % (cleanup, entry, T.hexlit(cleanup)) in text
    assert text.count("void fn_%08x(" % cleanup) == 1
    body = text.split("static void body_%08x(" % entry, 1)[1].split("void fn_%08x(" % entry, 1)[0]
    assert "L_%08x: ;" % epilogue in body
    assert "case %s: goto L_%08x;" % (T.hexlit(epilogue), epilogue) in body
    assert "CALL_FN(%08x);" % helper in body
    assert "CALL_FN(%08x);" % cleanup not in body
    if recovered_owner in ("speculative_epilogue", "speculative_body", "zero_prefix"):
        assert "void fn_%08x(" % prefix not in text
    if recovered_owner:
        import json
        symbols = json.loads((out / "symbols.json").read_text())
        establishing = next(f for f in symbols["functions"] if f["addr"] == "%08x" % entry)
        if recovered_owner == "config":
            assert establishing["provenance"] == "config"
        elif recovered_owner == "direct":
            assert establishing["provenance"] == "seh"
        else:
            assert establishing["provenance"] != "seh", "SEH content cannot promote a scan guess"


def test_overlapping_cleanup_body_is_retired_when_already_in_owner(tmp_path, monkeypatch):
    test_finally_cleanup_and_epilogue_belong_to_establishing_body(
        tmp_path, monkeypatch, jump_to_cleanup=False, listed_cleanup=True,
        recovered_owner=False, complete_listing=True)


def test_zero_edx_frame_keeps_cleanup_in_its_owner(tmp_path, monkeypatch):
    test_finally_cleanup_and_epilogue_belong_to_establishing_body(
        tmp_path, monkeypatch, jump_to_cleanup=False, listed_cleanup=False,
        recovered_owner=False, complete_listing=True, edx_frame=True)


def test_adopted_epilogue_is_not_split_again_by_seh_resolution(tmp_path, monkeypatch):
    test_finally_cleanup_and_epilogue_belong_to_establishing_body(
        tmp_path, monkeypatch, jump_to_cleanup=False, listed_cleanup=False,
        recovered_owner=True, epilogue_handler=True)


def test_adopted_body_keeps_interior_handler_entries(tmp_path, monkeypatch):
    test_finally_cleanup_and_epilogue_belong_to_establishing_body(
        tmp_path, monkeypatch, jump_to_cleanup=False, listed_cleanup=False,
        recovered_owner=True, epilogue_handler="interior")


def test_retired_cleanup_prefix_remains_an_alternate_entry(tmp_path, monkeypatch):
    test_finally_cleanup_and_epilogue_belong_to_establishing_body(
        tmp_path, monkeypatch, jump_to_cleanup=False, listed_cleanup=False,
        recovered_owner=True, epilogue_handler="prefix")


def test_a_pushed_destructor_thunk_is_an_entry_candidate():
    """`atexit` is handed the address of a ten-byte `MOV ECX,obj / JMP dtor`
    thunk packed right after its initializer's RET: unaligned, not preceded
    by padding, so the function-start signals reject it, yet the CRT calls
    it at exit.  A pushed immediate that decodes as a thunk is a candidate."""
    dtor, thunk = 0x00401000, 0x0040105a
    rel = (dtor - (thunk + 10)) & 0xFFFFFFFF
    img = synthetic_image({dtor: b"\xc3",
                           thunk: bytes([0xb9, 0xd8, 0x30, 0x72, 0x00, 0xe9]) + rel.to_bytes(4, "little")})
    assert img.plausible_immediate_target(thunk)
    assert img.plausible_immediate_target(dtor)          # an aligned start after padding


@pytest.mark.parametrize("alignment,admitted", [(16, False), (4, True)])
def test_unaligned_frame_callback_uses_configured_alignment(monkeypatch, alignment, admitted):
    callback = 0x00401108
    # MOV EAX,EAX padding, then a frame with more than the thunk decoder's
    # eight instructions before its stdcall return. Only a PUSH names it.
    code = bytes.fromhex("55 8bec 6a00 53 56 33c0 33db 33f6 8b4508 5e 5b 59 5d c20400")
    img = synthetic_image({callback - 2: b"\x8b\xc0", callback: code})
    monkeypatch.setattr(T, "FUNCTION_ALIGNMENT", alignment, raising=False)
    assert img.is_exec(callback)
    assert img.recover(callback, set())
    assert not img.looks_like_thunk(callback)
    assert img.looks_like_code_start(callback) == admitted
    assert img.plausible_immediate_target(callback) == admitted
    # The stronger gate still needs its padding signal as well as alignment.
    assert not img.looks_like_function(callback)
    img.data = img.data[:callback - img.base - 1] + b"\x90" + img.data[callback - img.base:]
    assert img.looks_like_function(callback) == admitted


def test_a_wild_jump_is_not_an_entry_candidate():
    # MSVC's three-byte NOP padding `8d 49 00` decodes at its last byte as a
    # JMP to nowhere; an immediate landing there names nothing.
    pad = 0x00401082
    img = synthetic_image({pad - 2: b"\x8d\x49\x00\xe9\x00\x00\x00\xf5"})
    assert not img.plausible_immediate_target(pad + 1)


def test_recovery_stops_at_a_call_that_never_returns():
    """A block recovered from the PE follows fall-through after a CALL, and
    after a call to `__CxxThrowException` or `exit` the fall-through is
    padding and then a jump table: decoding on turns the table's bytes into
    `BOUND` and `POP ES`, the emitter refuses them, and the whole block - a
    real function a vtable names - is rejected as data.  A callee the
    listings show never returning (a function whose listing ends on the CALL)
    ends the block."""
    throw, fn = 0x00401000, 0x00401100
    rel = (throw - (fn + 5 + 5)) & 0xFFFFFFFF
    code = (b"\x68\x78\xc6\x69\x00"                  # PUSH 0x69c678
            + b"\xe8" + rel.to_bytes(4, "little")      # CALL throw
            + b"\x90"                                  # padding
            + (0x00401149).to_bytes(4, "little")        # a jump table: dwords, not code
            + (0x00401159).to_bytes(4, "little"))
    img = synthetic_image({throw: b"\xc3", fn: code})
    # Without the knowledge, the walk runs into the table.
    plain = img.recover(fn, set())
    assert plain and plain[-1].addr > fn + 10
    img.noreturn_callees = {throw}
    stopped = img.recover(fn, set())
    assert [i.addr for i in stopped] == [fn, fn + 5]


def test_a_call_that_never_returns_ends_the_block_with_a_trap():
    """After `CALL __CxxThrowException` nothing follows that the game can
    reach, and the bytes after it are padding and a switch table.  The
    emitter must not jump on to them; it leaves a trap that names the
    address, so reaching it (the callee returning after all) is reported."""
    from test_translate_insns import NoImage, Opts
    throw, fn = 0x00401000, 0x00401100
    tr = T.Translator(NoImage(), {fn, throw}, Opts())
    tr.noreturn_callees = {throw}
    insns = T.parse_listing_text("00401100  PUSH 0x69c678\n00401105  CALL 0x00401000\n")
    f = T.Function(fn, "thrower", 10, insns)
    f.measure(NoImage())
    tr.prepare(f)
    text = "\n".join(tr.translate(f))
    assert "CALL_FN(00401000);" in text
    assert "recomp_unknown_call(c, 0x40110au); return;" in text
    assert "recomp_jump(c, 0x40110au)" not in text and "CALL_FN(0040110a)" not in text


def test_a_code_pointer_spelled_in_printable_bytes_is_still_a_pointer():
    """`PUSH 0x5b2370` stores the address as bytes 70 23 5b 00: 'p', '#', '['
    and a terminator, which the string-tail test reads as the end of a
    string and drops.  The address is 16-aligned, follows padding and
    decodes: every signal of a function start.  Those outrank a text
    coincidence, which is what one printable dword is."""
    target, holder = 0x00402370, 0x00401800
    img = synthetic_image({target - 1: b"\x90\x8b\x44\x24\x04\xc3",
                           holder: b"\x68" + target.to_bytes(4, "little")},  # PUSH target
                          size=0x4000)
    assert img.looks_like_function(target)
    starts, _interior = img.code_pointers(set())
    assert target in starts
