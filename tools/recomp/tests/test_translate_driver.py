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
