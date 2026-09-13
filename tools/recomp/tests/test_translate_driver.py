"""Driver-level rules of the translator that need no game to check.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_driver.py
"""
import os
import sys

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
        "CALL_FN(00500000);",
        "}",
    ]
    out = T.retarget_withdrawn(body, {0x0061E9EF})
    assert out[3] == "    CALL_FN(0064cf1f);"           # a live callee is untouched
    assert out[5].startswith("recomp_unknown_call(c, 0x0061e9efu); return;")
    assert out[5].endswith("/* fall-through past the listing */")
    assert out[6] == "case 0x0061e9efu: recomp_unknown_call(c, 0x0061e9efu); return;"
    assert out[7] == "recomp_unknown_call(c, 0x0061e9efu); return;"
    assert out[8] == "CALL_FN(00500000);"
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
    img.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    return img


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


def test_a_wild_jump_is_not_an_entry_candidate():
    # MSVC's three-byte NOP padding `8d 49 00` decodes at its last byte as a
    # JMP to nowhere; an immediate landing there names nothing.
    pad = 0x00401082
    img = synthetic_image({pad - 2: b"\x8d\x49\x00\xe9\x00\x00\x00\xf5"})
    assert not img.plausible_immediate_target(pad + 1)
