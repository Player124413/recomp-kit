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
