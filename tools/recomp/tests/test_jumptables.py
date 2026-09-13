"""Jump-table shapes the decoder has to recognise, on synthetic functions.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_jumptables.py

Each case is a listing plus a fake image holding the table's dwords.  The
shapes come from Visual C++ 6's hand-written `memcpy`, which the first
corpus (a later compiler) never showed the decoder:

    AND EAX,0x3 / JMP [EAX*4 + T]            a mask bound; slot 0 is never
                                             reached and holds code bytes
    SUB ECX,0x4 / JC S ... S: JMP [ECX*4 + T]  the guard branches TO the jump
                                             with the index in -4..-1
    CMP ECX,0x4 / JC S ... S: JMP [ECX*4 + T]  the same with the index in 0..3
    CMP ECX,0x8 / JC S ... S: NEG ECX / JMP    the index negated after the guard

A table the decoder cannot bound falls back to reading forward while the
entries look like code, which finds nothing for a hole in slot 0 and finds
the wrong entries for a negative index, so these shapes are decoded exactly."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402

FN = 0x00500000        # the function
TABLE = 0x00500800     # where the table's dwords live
GARBAGE = 0x900064E7   # what a hole reads as: code bytes, not an address


class FakeImage(object):
    """Code at the listed addresses, a table of dwords, nothing else."""

    md = None

    def __init__(self, code_addrs, dwords):
        self.base = 0x00400000
        self.end = 0x00600000
        self.size = self.end - self.base
        self.code = set(code_addrs)
        self.dwords = dwords          # address -> dword

    def insn_end(self, addr, mnem):
        return None

    def rd32(self, va):
        return self.dwords.get(va)

    def rd8(self, va):
        return None

    def is_exec(self, va):
        return va in self.code

    def looks_like_code_start(self, va):
        return va in self.code

    def looks_like_function(self, va):
        return False

    def relocated_pointers(self):
        return {}


class Opts(object):
    eager_flags = False


def decode(lines, dwords, strict=False):
    """Translate `lines` and return the targets decoded for its JMP table."""
    listing = "\n".join("%08x  %s" % (FN + off, text) for off, text in lines) + "\n"
    insns = T.parse_listing_text(listing)
    fn = T.Function(FN, "case", 0x100, insns)
    image = FakeImage([i.addr for i in insns], dwords)
    fn.measure(image)
    tr = T.Translator(image, {FN}, Opts())
    tr.prepare(fn, strict=strict)
    sites = [(k, ins) for k, ins in enumerate(insns)
             if ins.mnem == "JMP" and "dword ptr" in ins.ops[0]]
    assert len(sites) == 1
    return sorted(tr.jumptables.get((FN, sites[0][1].addr), ())), tr


def table(entries):
    return {TABLE + 4 * k: v for k, v in entries.items()}


def test_and_mask_bound_with_a_hole_in_slot_zero():
    # LeadUpVec: `AND EAX,3` leaves 0..3, but 0 was diverted earlier and the
    # slot holds the tail of the previous instruction.
    lines = [(0x00, "AND EAX,0x3"), (0x03, "ADD ECX,EAX"),
             (0x05, "JMP dword ptr [EAX*0x4 + 0x%x]" % TABLE),
             (0x10, "RET"), (0x20, "RET"), (0x30, "RET")]
    targets, tr = decode(lines, table({0: GARBAGE, 1: FN + 0x10, 2: FN + 0x20, 3: FN + 0x30}))
    assert targets == [FN + 0x10, FN + 0x20, FN + 0x30]
    decode(lines, table({0: GARBAGE, 1: FN + 0x10, 2: FN + 0x20, 3: FN + 0x30}), strict=True)


def test_guard_branching_to_the_jump_after_sub_gives_a_negative_range():
    # TrailUpVec through `SUB ECX,4 / JC`: at the jump ECX is -4..-1, so the
    # entries sit below the displacement.
    lines = [(0x00, "SUB ECX,0x4"), (0x03, "JC 0x%08x" % (FN + 0x0c)),
             (0x05, "JMP dword ptr [ECX*0x4 + 0x%x]" % (TABLE + 0x100)),
             (0x0c, "JMP dword ptr [ECX*0x4 + 0x%x]" % TABLE),
             (0x20, "RET"), (0x30, "RET"), (0x40, "RET"), (0x50, "RET")]
    dwords = table({-4: FN + 0x20, -3: FN + 0x30, -2: FN + 0x40, -1: FN + 0x50,
                    0: GARBAGE})
    # The other JMP is a decoy the decoder must not confuse with this one.
    dwords.update({TABLE + 0x100: FN + 0x20})
    listing = "\n".join("%08x  %s" % (FN + off, text) for off, text in lines) + "\n"
    insns = T.parse_listing_text(listing)
    fn = T.Function(FN, "case", 0x100, insns)
    image = FakeImage([i.addr for i in insns], dwords)
    fn.measure(image)
    tr = T.Translator(image, {FN}, Opts())
    tr.prepare(fn)
    assert sorted(tr.jumptables.get((FN, FN + 0x0c), ())) == [FN + 0x20, FN + 0x30, FN + 0x40, FN + 0x50]
    tr.prepare(fn, strict=True)


def test_guard_branching_to_the_jump_after_cmp_bounds_from_zero():
    lines = [(0x00, "CMP ECX,0x4"), (0x03, "JC 0x%08x" % (FN + 0x0c)),
             (0x05, "MOV EAX,0x0"), (0x0a, "RET"),
             (0x0c, "JMP dword ptr [ECX*0x4 + 0x%x]" % TABLE),
             (0x20, "RET"), (0x30, "RET"), (0x40, "RET"), (0x50, "RET")]
    targets, _ = decode(lines, table({0: FN + 0x20, 1: FN + 0x30, 2: FN + 0x40, 3: FN + 0x50,
                                      4: GARBAGE}))
    assert targets == [FN + 0x20, FN + 0x30, FN + 0x40, FN + 0x50]


def test_negated_index_after_a_guard_reads_downwards():
    # `CMP ECX,8 / JC S ... S: NEG ECX / JMP [ECX*4 + T]`: 0..7 becomes -7..0.
    lines = [(0x00, "CMP ECX,0x8"), (0x03, "JC 0x%08x" % (FN + 0x0c)),
             (0x05, "MOV EAX,0x0"), (0x0a, "RET"),
             (0x0c, "NEG ECX"),
             (0x0e, "JMP dword ptr [ECX*0x4 + 0x%x]" % TABLE)] + \
            [(0x20 + 0x10 * k, "RET") for k in range(8)]
    entries = {-k: FN + 0x20 + 0x10 * k for k in range(8)}
    entries[-8] = GARBAGE
    entries[1] = GARBAGE
    targets, _ = decode(lines, table(entries))
    assert targets == [FN + 0x20 + 0x10 * k for k in range(8)]
