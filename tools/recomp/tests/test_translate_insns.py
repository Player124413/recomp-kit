"""Instruction forms the first corpus never used, checked against Unicorn.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_insns.py

Each case is a synthetic listing in the export grammar plus the machine code
it describes.  The listing goes through the translator exactly as a game
function does, the emitted C is compiled with tools/recomp/tests/harness.c
into one shared library, and the same initial registers and memory are run
through the native code and through Unicorn.  Registers, the defined
arithmetic flags and the scratch memory both sides wrote must agree.

No game is needed: the cases are their own machine code.  A translator that
cannot emit a case fails that case's test with the translator's error, and
the remaining cases still build and run.  clang must be on PATH (a skip
otherwise)."""

import ctypes as C
import os
import platform
import random
import shutil
import struct
import subprocess
import sys
import tempfile

import pytest
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_INTR, UcError
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EFLAGS, UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_FPTAG,
    UC_X86_REG_MXCSR)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402

# Guest layout shared by both engines.  The code lives in its own page range,
# the stack and scratch are zeroed before every run, and the cave holds the
# return address a RET lands on, which is where emulation stops.
CODE_BASE, CODE_SIZE = 0x0D010000, 0x00020000
STACK_BASE, STACK_SIZE = 0x0EF00000, 0x00100000
ESP_INIT = 0x0EFFFF00
SCRATCH, SCRATCH_SIZE = 0x0E100000, 0x00010000
CAVE, CAVE_SIZE = 0x0DEAC000, 0x1000
MAGIC_RET = CAVE

EFLAGS_COMPARED = 0x0CC5   # CF PF ZF SF DF OF
EFLAGS_MISC_INIT = 0x202
FPU_CW_INIT = 0x037F
FPU_TAG_INIT = 0xFFFF
CS_SELECTOR = 0x1B          # the flat user-mode code selector Windows hands a process

UC_REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
           UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
REG_NAMES = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"]


class X86(C.Structure):
    _fields_ = [
        ("r", C.c_uint32 * 8), ("eip", C.c_uint32),
        ("eflags_cf", C.c_uint32), ("eflags_zf", C.c_uint32),
        ("eflags_sf", C.c_uint32), ("eflags_of", C.c_uint32),
        ("eflags_pf", C.c_uint32), ("eflags_af", C.c_uint32),
        ("eflags_df", C.c_uint32), ("eflags_misc", C.c_uint32),
        ("st", C.c_double * 8), ("fpu_top", C.c_uint32),
        ("fpu_cw", C.c_uint16), ("fpu_sw", C.c_uint16),
        ("fpu_tag", C.c_uint16),
        ("fs_base", C.c_uint32),
    ]


# ------------------------------------------------------------------ cases --

class Case(object):
    """One synthetic function.

    `lines` are (offset, listing text) pairs; `code` is the hex of the same
    bytes.  `setup(rng)` returns a dict with optional `regs` (name -> value),
    `mem` (list of (address, bytes)) and `args` (dwords pushed above the
    return address).  `ignore` lists (address, length) ranges of scratch the
    comparison skips; `check(native, emu)` adds case-specific assertions where
    exact bytes are the wrong measure (x87 results)."""

    def __init__(self, name, addr, lines, code, setup=None, ignore=(), check=None,
                 expect_intr=None):
        self.name = name
        self.addr = addr
        self.lines = lines
        self.code = bytes.fromhex(code.replace(" ", ""))
        self.setup = setup or (lambda rng: {})
        self.ignore = ignore
        self.check = check
        self.expect_intr = expect_intr

    def listing(self):
        return "\n".join("%08x  %s" % (self.addr + off, text) for off, text in self.lines) + "\n"


def rand_regs(rng, **fixed):
    regs = {name: rng.getrandbits(32) for name in REG_NAMES if name != "ESP"}
    regs.update(fixed)
    return regs


def cmpxchg_setup(rng):
    """Exercise both the matching and nonmatching accumulator paths."""
    mem = rng.getrandbits(32)
    eax = mem if rng.random() < 0.5 else rng.getrandbits(32)
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x40, EAX=eax),
            "mem": [(SCRATCH + 0x40, struct.pack("<I", mem))]}


def cmpxchg8b_setup(rng):
    """Compare equal qwords and mismatches confined to either dword."""
    mem = rng.getrandbits(64)
    expected = mem
    if rng.random() < 0.5:
        expected ^= 1 << rng.randrange(64)
    return {"regs": rand_regs(rng, EDI=SCRATCH + 0x40,
                              EAX=expected & 0xffffffff, EDX=expected >> 32),
            "mem": [(SCRATCH + 0x40, struct.pack("<Q", mem))]}


def loop_setup(rng):
    # ECX = 0 would spin 2^32 times; the CRT never enters a LOOP that way.
    return {"regs": rand_regs(rng, ECX=rng.randint(1, 40))}


def cmps_setup(rng):
    """Two dword arrays that agree for a random prefix, ECX covering them."""
    count = rng.randint(1, 12)
    same = rng.randint(0, count)          # == count: the arrays are equal
    left = [rng.getrandbits(32) for _ in range(count)]
    right = list(left)
    if same < count:
        right[same] ^= rng.randint(1, 0xFFFFFFFF)
    a, b = SCRATCH + 0x100, SCRATCH + 0x800
    return {"regs": rand_regs(rng, ECX=count, ESI=a, EDI=b),
            "mem": [(a, struct.pack("<%dI" % count, *left)),
                    (b, struct.pack("<%dI" % count, *right))]}


def cmpsw_setup(rng):
    count = rng.randint(1, 12)
    same = rng.randint(0, count)
    left = [rng.getrandbits(16) for _ in range(count)]
    right = list(left)
    if same < count:
        right[same] ^= rng.randint(1, 0xFFFF)
    a, b = SCRATCH + 0x100, SCRATCH + 0x800
    return {"regs": rand_regs(rng, ECX=count, ESI=a, EDI=b),
            "mem": [(a, struct.pack("<%dH" % count, *left)),
                    (b, struct.pack("<%dH" % count, *right))]}


def scas_setup(rng, width):
    count = rng.randint(1, 12)
    needle = rng.getrandbits(width * 8)
    hay = [rng.getrandbits(width * 8) for _ in range(count)]
    if rng.random() < 0.7:
        hay[rng.randrange(count)] = needle
    fmt = {2: "H", 4: "I"}[width]
    a = SCRATCH + 0x100
    return {"regs": rand_regs(rng, ECX=count, EDI=a, EAX=needle | (rng.getrandbits(32) << (width * 8))),
            "mem": [(a, struct.pack("<%d%s" % (count, fmt), *hay))]}


def seg_store_setup(rng):
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x40)}


def check_seg_store(native, emu):
    # Unicorn has no GDT, so its CS is not Windows's; the translator stores
    # the selector a 32-bit Windows process sees, which is what a CONTEXT
    # record written by the CRT's exception path expects.
    assert struct.unpack("<H", bytes(native[SCRATCH + 0x40:SCRATCH + 0x42]))[0] == CS_SELECTOR


def one_double(rng, lo, hi):
    return {"args": list(struct.unpack("<II", struct.pack("<d", rng.uniform(lo, hi))))}


def fptan_setup(rng):
    return one_double(rng, -1.5, 1.5)


def fsave_setup(rng):
    # ECX names the save area; the argument is the value on the x87 stack.
    d = one_double(rng, -1e6, 1e6)
    d["regs"] = rand_regs(rng, ECX=SCRATCH + 0x1000)
    return d


def rd_double(mem, addr):
    return struct.unpack("<d", bytes(mem[addr:addr + 8]))[0]


def check_fptan(native, emu):
    esp = ESP_INIT
    for who, mem in (("native", native), ("unicorn", emu)):
        assert rd_double(mem, esp + 0xc) == 1.0, "%s: FPTAN did not push 1.0" % who
    n, u = rd_double(native, esp + 0x14), rd_double(emu, esp + 0x14)
    assert abs(n - u) <= 1e-12 * max(1.0, abs(u)), "tan: native %r unicorn %r" % (n, u)


def check_fsave(native, emu):
    area = SCRATCH + 0x1000 + 8
    # Control word, status word and tag word, then the eight registers as
    # 80-bit values.  The exception pointers in between (FIP, FCS, FDP, FDS)
    # are compared by the generic scratch check only outside the ignored
    # range: the translator does not track them.
    for lo, hi, what in ((0, 12, "environment"), (28, 108, "registers")):
        assert bytes(native[area + lo:area + hi]) == bytes(emu[area + lo:area + hi]), (
            "FSAVE %s image differs: native %s unicorn %s"
            % (what, bytes(native[area + lo:area + hi]).hex(), bytes(emu[area + lo:area + hi]).hex()))


def check_x87_constants(native, emu):
    # FLDL2E is pushed last, so it is the first value FSTP stores.
    for mem in (native, emu):
        l2e = rd_double(mem, SCRATCH + 0x100)
        ln2 = rd_double(mem, SCRATCH + 0x108)
        assert abs(l2e - 1.4426950408889634) < 1e-15, l2e
        assert abs(ln2 - 0.6931471805599453) < 1e-15, ln2


def fbstp_setup(rng):
    value = rng.randint(-999999999, 999999999)
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x200),
            "mem": [(SCRATCH + 0x210, struct.pack("<i", value))]}


CASES = [
    Case("Cleanup RET follows a nonadjacent pushed continuation", 0x0D01EC00,
         [(0, "PUSH EBP"), (1, "MOV EBP,ESP"), (3, "PUSH 0x0d01ec11"),
          (8, "LEA EAX,[EBP]"), (11, "MOV EDX,0x3"),
          (16, "RET"), (17, "POP EBP"), (18, "RET")],
         "55 89 e5 68 11 ec 01 0d 8d 45 00 ba 03 00 00 00 c3 5d c3",
         lambda rng: {"regs": rand_regs(rng)}),
    Case("Cleanup RET immediate adjusts ESP before the continuation", 0x0D01ED00,
         [(0, "PUSH 0x1234"), (5, "PUSH 0x0d01ed0e"), (10, "NOP"),
          (11, "RET 0x4"), (14, "INC EAX"), (15, "RET 0x8")],
         "68 34 12 00 00 68 0e ed 01 0d 90 c2 04 00 40 c2 08 00",
         lambda rng: {"regs": rand_regs(rng), "args": [11, 22]}),
    Case("Variable argument cleanup returns through a popped address", 0x0D01EA00,
         [(0, "POP EAX"), (1, "LEA ESP,[ESP + EDX*0x4]"), (4, "JMP EAX")],
         "58 8d 24 94 ff e0",
         lambda rng: {"regs": rand_regs(rng, EDX=3), "args": [11, 22, 33]}),
    Case("PUSH immediate RET reaches the epilogue before returning", 0x0D01E800,
         [(0, "PUSH EBP"), (1, "MOV EBP,ESP"),
          (3, "PUSH 0x0d01e809"), (8, "RET"), (9, "POP EBP"), (10, "RET")],
         "55 89 e5 68 09 e8 01 0d c3 5d c3"),
    Case("A branch to the shared RET returns to its own caller", 0x0D01E900,
         [(0, "JMP 0x0d01e90a"), (5, "PUSH 0x0d01e90b"),
          (10, "RET"), (11, "INC EAX"), (12, "RET")],
         "e9 05 00 00 00 68 0b e9 01 0d c3 40 c3"),
    Case("LOOP counts ECX down and branches while it is not zero", 0x0D010000,
         [(0x0, "XOR EAX,EAX"), (0x2, "INC EAX"), (0x3, "LOOP 0x0d010002"), (0x5, "RET")],
         "31 C0  40  E2 FD  C3", loop_setup),
    Case("INT3 is a breakpoint the runtime notes and steps over", 0x0D011000,
         [(0x0, "INT3"), (0x1, "MOV EAX,0x1234"), (0x6, "RET")],
         "CC  B8 34 12 00 00  C3", expect_intr=3),
    Case("STC and CLC set and clear the carry", 0x0D012000,
         [(0x0, "STC"), (0x1, "SETC AL"), (0x4, "CLC"), (0x5, "SETC BL"), (0x8, "RET")],
         "F9  0F 92 C0  F8  0F 92 C3  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("REPE CMPSD compares dwords until they differ", 0x0D013000,
         [(0x0, "CMPSD.REPE ES:EDI,ESI"), (0x2, "RET")],
         "F3 A7  C3", cmps_setup),
    Case("REPNE CMPSD compares dwords until they match", 0x0D014000,
         [(0x0, "CMPSD.REPNE ES:EDI,ESI"), (0x2, "RET")],
         "F2 A7  C3", cmps_setup),
    Case("CMPSW compares one word", 0x0D015000,
         [(0x0, "CMPSW ES:EDI,ESI"), (0x2, "RET")],
         "66 A7  C3", cmpsw_setup),
    Case("REPE CMPSW compares words until they differ", 0x0D016000,
         [(0x0, "CMPSW.REPE ES:EDI,ESI"), (0x3, "RET")],
         "F3 66 A7  C3", cmpsw_setup),
    Case("REPNE SCASD scans dwords for EAX", 0x0D017000,
         [(0x0, "SCASD.REPNE ES:EDI,EAX"), (0x2, "RET")],
         "F2 AF  C3", lambda rng: scas_setup(rng, 4)),
    Case("REPE SCASW scans words while they equal AX", 0x0D018000,
         [(0x0, "SCASW.REPE ES:EDI,AX"), (0x3, "RET")],
         "F3 66 AF  C3", lambda rng: scas_setup(rng, 2)),
    Case("PUSH and POP of 16-bit registers move ESP by two", 0x0D019000,
         [(0x0, "PUSH AX"), (0x2, "PUSH CX"), (0x4, "POP DX"), (0x6, "POP BX"), (0x8, "RET")],
         "66 50  66 51  66 5A  66 5B  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("MOV to memory from CS stores the code selector", 0x0D01A000,
         [(0x0, "MOV word ptr [ECX],CS"), (0x2, "RET")],
         "8C 09  C3", seg_store_setup, ignore=((SCRATCH + 0x40, 2),), check=check_seg_store),
    Case("FPTAN replaces ST(0) with its tangent and pushes 1.0", 0x0D01B000,
         [(0x0, "FLD double ptr [ESP + 0x4]"), (0x4, "FPTAN"),
          (0x6, "FSTP double ptr [ESP + 0xc]"), (0xa, "FSTP double ptr [ESP + 0x14]"),
          (0xe, "RET")],
         "DD 44 24 04  D9 F2  DD 5C 24 0C  DD 5C 24 14  C3", fptan_setup, check=check_fptan),
    Case("FSAVE writes the 108-byte state and reinitialises; FRSTOR brings it back", 0x0D01C000,
         [(0x0, "FLD1"), (0x2, "FLD double ptr [ESP + 0x4]"), (0x6, "FSAVE [ECX + 0x8]"),
          (0xa, "FLD1"), (0xc, "FSTP double ptr [ESP + 0xc]"),
          (0x10, "FRSTOR [ECX + 0x8]"), (0x13, "FSTP double ptr [ESP + 0x14]"),
          (0x17, "FSTP double ptr [ESP + 0x1c]"), (0x1b, "RET")],
         "D9 E8  DD 44 24 04  9B DD 71 08  D9 E8  DD 5C 24 0C  DD 61 08  DD 5C 24 14  DD 5C 24 1C  C3",
         fsave_setup, ignore=((SCRATCH + 0x1000 + 8 + 12, 16),), check=check_fsave),
    Case("PUSHF and POPF move the low 16 flag bits through the stack", 0x0D01E000,
         [(0x0, "STC"), (0x1, "PUSHF"), (0x3, "POP AX"), (0x5, "CLC"), (0x6, "PUSH AX"),
          (0x8, "POPF"), (0xa, "SETC BL"), (0xd, "RET")],
         "F9  66 9C  66 58  F8  66 50  66 9D  0F 92 C3  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("FNINIT empties the stack and resets the control word", 0x0D01D000,
         [(0x0, "FLD1"), (0x2, "FNINIT"), (0x4, "FNSTCW word ptr [ESP + 0x4]"),
          (0x8, "FLD1"), (0xa, "FSTP double ptr [ESP + 0xc]"), (0xe, "RET")],
         "D9 E8  DB E3  D9 7C 24 04  D9 E8  DD 5C 24 0C  C3",
         lambda rng: {"args": [0, 0, 0, 0, 0, 0]}),
    Case("CMPXCHG stores when EAX equals the destination and loads EAX otherwise", 0x0D01F000,
         [(0x0, "CMPXCHG.LOCK dword ptr [ECX],EDX"), (0x4, "SETZ BL"), (0x7, "RET")],
         "F0 0F B1 11  0F 94 C3  C3", cmpxchg_setup),
    Case("XADD exchanges and adds", 0x0D020000,
         [(0x0, "XADD.LOCK dword ptr [ECX],EDX"), (0x4, "RET")],
         "F0 0F C1 11  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x40),
                      "mem": [(SCRATCH + 0x40, struct.pack("<I", rng.getrandbits(32)))]}),
    Case("PAUSE is a hint and changes nothing", 0x0D021000,
         [(0x0, "PAUSE"), (0x2, "RET")],
         "F3 90  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("CMC complements the carry", 0x0D022000,
         [(0x0, "STC"), (0x1, "CMC"), (0x2, "SETC BL"), (0x5, "CMC"),
          (0x6, "SETC CL"), (0x9, "RET")],
         "F9  F5  0F 92 C3  F5  0F 92 C1  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("STMXCSR stores the default MXCSR", 0x0D023000,
         [(0x0, "STMXCSR dword ptr [ECX]"), (0x3, "RET")],
         "0F AE 19  C3", lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x80)}),
    Case("FLDLN2 and FLDL2E push the x87 constants", 0x0D024000,
         [(0x0, "FLDLN2"), (0x2, "FLDL2E"), (0x4, "FSTP double ptr [ECX]"),
          (0x6, "FSTP double ptr [ECX + 0x8]"), (0x9, "RET")],
         "D9 ED  D9 EA  DD 19  DD 59 08  C3", lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x100)},
         ignore=((SCRATCH + 0x100, 16),), check=check_x87_constants),
    Case("FCLEX clears the status word like FNCLEX", 0x0D025000,
         [(0x0, "FLDZ"), (0x2, "FLDZ"), (0x4, "FDIVP ST1,ST0"), (0x6, "FCLEX"),
          (0x9, "FNSTSW word ptr [ECX]"), (0xb, "FSTP double ptr [ECX + 0x8]"), (0xe, "RET")],
         "D9 EE  D9 EE  DE F9  9B DB E2  DD 39  DD 59 08  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x100)}, ignore=((SCRATCH + 0x108, 8),)),
    Case("FBSTP stores packed BCD and pops", 0x0D026000,
         [(0x0, "FILD dword ptr [ECX + 0x10]"), (0x3, "FBSTP tword ptr [ECX]"), (0x5, "RET")],
         "DB 41 10  DF 31  C3", fbstp_setup),
    Case("CMPXCHG8B compares EDX:EAX and stores ECX:EBX changing only ZF", 0x0D027000,
         # Materialize CMP's flags: Unicorn otherwise corrupts its lazy
         # flag path through CMPXCHG8B. PUSHFD after it also checks preserved AF.
         [(0x0, "CMP ESI,EBP"), (0x2, "PUSHFD"), (0x3, "POPFD"),
          (0x4, "CMPXCHG8B.LOCK qword ptr [EDI]"),
          (0x8, "PUSHFD"), (0x9, "POP ESI"), (0xa, "RET")],
         "39 EE  9C  9D  F0 0F C7 0F  9C  5E  C3", cmpxchg8b_setup),
    Case("EMMS changes no integer registers or flags", 0x0D028000,
         [(0x0, "CMP ESI,EBP"), (0x2, "EMMS"), (0x4, "RET")],
         "39 EE  0F 77  C3", lambda rng: {"regs": rand_regs(rng)}),
]


# -------------------------------------------------------------- translate --

class NoImage(object):
    """The translator's view of a program that has no bytes: every synthetic
    case is straight-line code whose listing is complete, so nothing here is
    ever consulted for real."""
    base = 0
    end = 0
    size = 0
    md = None

    def insn_end(self, addr, mnem):
        return None

    def rd32(self, va):
        return None

    def rd8(self, va):
        return None

    def is_exec(self, va):
        return False

    def relocated_pointers(self):
        return set()


class Opts(object):
    eager_flags = False


def translate_case(case):
    """The emitted C for one case, or the TranslateError it raised."""
    tr = T.Translator(NoImage(), {c.addr for c in CASES}, Opts())
    insns = T.parse_listing_text(case.listing())
    fn = T.Function(case.addr, case.name, len(case.code), insns)
    fn.measure(NoImage())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "FN(" not in text, "synthetic cases must not call anything"
    return text


# ------------------------------------------------------------------ build --

class Native(object):
    def __init__(self, lib_path):
        self.lib = C.CDLL(lib_path)
        self.lib.harness_mem.restype = C.POINTER(C.c_uint8)
        self.lib.harness_x86_size.restype = C.c_uint64
        self.lib.harness_run.argtypes = [C.c_uint32, C.POINTER(X86)]
        self.lib.harness_eflags.restype = C.c_uint32
        self.lib.harness_eflags.argtypes = [C.POINTER(X86)]
        assert self.lib.harness_x86_size() == C.sizeof(X86)
        self.mem = self.lib.harness_mem()

    def write(self, addr, data):
        C.memmove(C.addressof(self.mem.contents) + addr, data, len(data))

    def read(self, addr, n):
        return C.string_at(C.addressof(self.mem.contents) + addr, n)

    def zero(self, addr, n):
        C.memset(C.addressof(self.mem.contents) + addr, 0, n)


@pytest.fixture(scope="module")
def built():
    """Translate every case, compile the ones that translated into one shared
    library with the harness, and return (Native, {case name: error})."""
    clang = shutil.which("clang")
    if clang is None:
        pytest.skip("clang is not on PATH")
    errors, parts, ok = {}, ['#include "x86.h"', ""], []
    for case in CASES:
        try:
            parts.append("/* %s */\n%s\n" % (case.name, translate_case(case)))
            ok.append(case)
        except T.TranslateError as e:
            errors[case.name] = str(e)
    work = tempfile.mkdtemp(prefix="translate-insns-")
    with open(os.path.join(work, "synth.c"), "w") as fh:
        fh.write("\n".join(parts))
    with open(os.path.join(work, "table.c"), "w") as fh:
        fh.write('#include "x86.h"\n')
        for case in ok:
            fh.write("void fn_%08x(X86 *c);\n" % case.addr)
        fh.write("void recomp_call(X86 *c, uint32_t target) {\n    switch (target) {\n")
        for case in ok:
            fh.write("    case 0x%08xu: fn_%08x(c); return;\n" % (case.addr, case.addr))
        fh.write("    default: recomp_unknown_call(c, target); c->eip = rd32(c->r[R_ESP]); c->r[R_ESP] += 4;\n"
                 "    }\n}\n")
        # The harness calls each case from MAGIC_RET outside the code arena.
        # Like a non-entry CALL continuation, it returns without popping or
        # dispatching: the case's computed-return epilogue already cleaned ESP.
        fh.write("void recomp_jump(X86 *c, uint32_t target) {\n"
                 "    if (target == 0x%08xu) { c->eip = target; return; }\n"
                 "    recomp_call(c, target);\n}\n" % MAGIC_RET)
    lib = os.path.join(work, "libinsns" + (".dylib" if platform.system() == "Darwin" else ".so"))
    subprocess.check_call([clang, "-O1", "-g", "-std=c11", "-Wall", "-Wextra", "-Wno-unused",
                           "-fPIC", "-shared", "-I", os.path.join(ROOT, "runtime"),
                           os.path.join(HERE, "harness.c"), os.path.join(work, "synth.c"),
                           os.path.join(work, "table.c"), "-o", lib])
    return Native(lib), errors


# -------------------------------------------------------------------- run --

class Emu(object):
    def __init__(self):
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.u.mem_map(CODE_BASE, CODE_SIZE)
        self.u.mem_map(STACK_BASE, STACK_SIZE)
        self.u.mem_map(SCRATCH, SCRATCH_SIZE)
        self.u.mem_map(CAVE, CAVE_SIZE)
        self.interrupts = []
        self.u.hook_add(UC_HOOK_INTR, lambda u, intno, user: self.interrupts.append(intno))

    def reset(self, case):
        self.u.mem_write(STACK_BASE, b"\0" * STACK_SIZE)
        self.u.mem_write(SCRATCH, b"\0" * SCRATCH_SIZE)
        self.u.mem_write(case.addr, case.code)
        self.u.reg_write(UC_X86_REG_FPCW, FPU_CW_INIT)
        self.u.reg_write(UC_X86_REG_MXCSR, 0x1f80)
        self.u.reg_write(UC_X86_REG_FPSW, 0)
        self.u.reg_write(UC_X86_REG_FPTAG, FPU_TAG_INIT)
        self.u.reg_write(UC_X86_REG_EFLAGS, EFLAGS_MISC_INIT)
        self.interrupts = []


def run_once(case, native, emu, rng):
    state = case.setup(rng)
    regs = dict(rand_regs(rng), **state.get("regs", {}))
    args = state.get("args", [])
    frame = struct.pack("<I", MAGIC_RET) + struct.pack("<%dI" % len(args), *args)

    emu.reset(case)
    native.zero(STACK_BASE, STACK_SIZE)
    native.zero(SCRATCH, SCRATCH_SIZE)
    for addr, data in state.get("mem", []):
        emu.u.mem_write(addr, data)
        native.write(addr, data)
    emu.u.mem_write(ESP_INIT, frame)
    native.write(ESP_INIT, frame)

    c = X86()
    for i, name in enumerate(REG_NAMES):
        value = ESP_INIT if name == "ESP" else regs[name]
        c.r[i] = value
        emu.u.reg_write(UC_REGS[i], value)
    c.eflags_misc = EFLAGS_MISC_INIT
    c.fpu_cw = FPU_CW_INIT
    c.fpu_tag = FPU_TAG_INIT

    emu.u.emu_start(case.addr, MAGIC_RET, timeout=2 * 1000 * 1000, count=100000)
    native.lib.harness_run(case.addr, C.byref(c))

    problems = []
    for i, name in enumerate(REG_NAMES):
        u = emu.u.reg_read(UC_REGS[i])
        if c.r[i] != u:
            problems.append("%s: native %08x unicorn %08x" % (name, c.r[i], u))
    nf = native.lib.harness_eflags(C.byref(c)) & EFLAGS_COMPARED
    uf = emu.u.reg_read(UC_X86_REG_EFLAGS) & EFLAGS_COMPARED
    if nf != uf:
        problems.append("EFLAGS: native %03x unicorn %03x" % (nf, uf))
    for base, size, what in ((STACK_BASE, STACK_SIZE, "stack"), (SCRATCH, SCRATCH_SIZE, "scratch")):
        n, u = bytearray(native.read(base, size)), bytearray(emu.u.mem_read(base, size))
        for lo, length in case.ignore:
            if base <= lo < base + size:
                n[lo - base:lo - base + length] = u[lo - base:lo - base + length] = b"\0" * length
        if n != u:
            first = next(k for k in range(size) if n[k] != u[k])
            problems.append("%s differs first at %08x: native %02x unicorn %02x"
                             % (what, base + first, n[first], u[first]))
    if case.expect_intr is not None:
        assert emu.interrupts == [case.expect_intr], emu.interrupts
    assert not problems, "\n".join(problems)
    if case.check:
        native_view = _MemView(native)
        emu_view = _MemView(emu)
        case.check(native_view, emu_view)


class _MemView(object):
    """Slice guest memory of either engine by absolute address."""

    def __init__(self, engine):
        self.engine = engine

    def __getitem__(self, s):
        assert isinstance(s, slice) and s.step is None
        n = s.stop - s.start
        if isinstance(self.engine, Native):
            return self.engine.read(s.start, n)
        return bytes(self.engine.u.mem_read(s.start, n))


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_case(case, built):
    native, errors = built
    if case.name in errors:
        pytest.fail("translator: %s" % errors[case.name])
    emu = Emu()
    rng = random.Random(0x4D616A ^ case.addr)
    for _ in range(40):
        try:
            run_once(case, native, emu, rng)
        except UcError as e:
            pytest.fail("unicorn: %s" % e)


def test_segment_register_loads_do_not_reach_the_flat_model():
    """Ghidra decodes data as code now and then, and `MOV CS,[mem]` is what
    such bytes can spell.  A segment load has no meaning in the flat model
    the runtime provides: a load of CS is an invalid opcode on the CPU too,
    so it raises #UD; a load of a data segment is dropped."""
    tr = T.Translator(NoImage(), {0x0D01F000}, Opts())
    insns = T.parse_listing_text("0d01f000  MOV CS,word ptr [ECX]\n0d01f003  MOV DS,AX\n0d01f005  RET\n")
    fn = T.Function(0x0D01F000, "seg", 6, insns)
    fn.measure(NoImage())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "recomp_int(c, 6u);" in text
    assert "0x23u" not in text and "wr" not in text.split("MOV DS,AX")[1].split("\n")[0]
