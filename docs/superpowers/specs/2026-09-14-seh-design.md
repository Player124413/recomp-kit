# Delphi structured exception handling

Date: 2026-09-14. Task 12, measured against kit `9345855`.

**Status: design blocked; translator and runtime implementation not started.**
The measurements below contradict the approved task's landing contract.
This document records the requested design and the evidence that must be
resolved before implementing it. It does not claim working SEH or a game run.

## Goal

Dispatch software exceptions through the guest's x86 registration chain,
run Delphi exception/finally code, and resume the correct live translated
function without leaving abandoned host calls on its stack. Guest addresses
remain 32-bit arena offsets. Runtime code must contain no game addresses or
game identity. The intended platforms are the task's `macos`, `ios`, `linux`
and `windows` presets; Android is outside this task.

## The Delphi idiom

The private `analysis/decompiled/Siege.exe/functions/008810a0.asm` listing
contains this establishment, including the zeroing instruction immediately
before the requested excerpt:

```asm
00881135  XOR EAX,EAX
00881137  PUSH EBP
00881138  PUSH 0x8812e3
0088113d  PUSH dword ptr FS:[EAX]
00881140  MOV dword ptr FS:[EAX],ESP
00881143  MOV EAX,dword ptr [EBP + -0xc]
```

After the three pushes, ESP addresses `{next, handler, saved_ebp}`. Normal
exit pops the record and stores the former `next` into `FS:[0]`, usually
through zeroed EAX. Recognition must support both `FS:[EAX]` and `FS:[0x0]`,
without treating every write to another TEB field as a registration.

### Measurement table

These are documentation evidence, not runtime constants. Measurements were
repeated on 2026-09-14. The executable hash was verified before reading its
bytes. Names of the three System routines are inferred from their behavior;
Ghidra names them `FUN_<address>`, not recovered Delphi symbols.

| Measurement | Result | Evidence |
| --- | --- | --- |
| Executable | `Siege.exe`, file version 1.20.2.1431 | Pinned installer image |
| SHA-256 | `0c028b582632129a43ea67da6040ecc5d78a14e3bba06fcd2e4071b06a9ebd5b` | Fresh hash of `original/gog/Siege.exe` |
| `MOV dword ptr FS:[EAX],ESP` | 2,258 sites in 1,728 listed functions | All `functions/*.asm` |
| `MOV dword ptr FS:[EAX],EDX` or `ECX` | 2,679 sites | All `functions/*.asm` |
| Establishment's handler immediate | One per site, immediately before `PUSH dword ptr FS:[EAX]` | All 2,258 sites |
| Handler stubs missing from their establishing listing | 2,258 | Membership check against listing instruction addresses |
| Handler stubs missing from every `.asm` listing | 2,258 | Membership check against the union of listing instruction addresses |
| Stub encoding | All 2,258 decode as five-byte direct `JMP` | Capstone decoding of the pinned PE at the pushed addresses |
| `System.@HandleAnyException` | `0x0080a028`, 96 incoming measured stubs | Accepts an exception, adds five to the stub, jumps to that address |
| `System.@HandleOnException` | `0x0080a154`, 158 incoming measured stubs | Reads a type/handler table after the stub and jumps to a selected handler |
| `System.@HandleFinally` | `0x0080a2dc`, 2,004 incoming measured stubs | Calls the block after the stub during unwind, then returns disposition 1 |

The three System entry points themselves have `.asm` listings and rows in
`functions.tsv`. Contrary to the task's measurement instructions, the stub
JMPs cannot be found by grepping those listings: their bytes are omitted.
The pushed addresses plus PE decoding establish the target counts.

## Windows' contract

Use guest memory for the records, never host-pointer casts. The field
definitions are documented by Microsoft for
[EXCEPTION_RECORD32](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record)
and the [32-bit context layout](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-wow64_context).
The following offsets follow their 32-bit field order and sizes.

| EXCEPTION_RECORD32 field | Offset | Size |
| --- | --- | --- |
| ExceptionCode | `0x00` | 4 |
| ExceptionFlags | `0x04` | 4 |
| ExceptionRecord | `0x08` | 4; zero for a new top-level record |
| ExceptionAddress | `0x0c` | 4; guest return address of RaiseException |
| NumberParameters | `0x10` | 4; at most 15 |
| ExceptionInformation | `0x14` | 15 dwords |
| Total | | `0x50` |

| x86 CONTEXT field | Offset |
| --- | --- |
| ContextFlags | `0x00` |
| Debug-register fields | `0x04` through `0x18` |
| FloatSave | `0x1c`, 112 bytes |
| SegGs, SegFs, SegEs, SegDs | `0x8c`, `0x90`, `0x94`, `0x98` |
| Edi, Esi, Ebx, Edx | `0x9c`, `0xa0`, `0xa4`, `0xa8` |
| Ecx, Eax, Ebp, Eip | `0xac`, `0xb0`, `0xb4`, `0xb8` |
| SegCs, EFlags, Esp, SegSs | `0xbc`, `0xc0`, `0xc4`, `0xc8` |
| ExtendedRegisters | `0xcc`, 512 bytes; full record size `0x2cc` |

RaiseException should populate the integer/control state, reconstruct EFlags
with the CPU helpers, copy the bounded information array, and walk `FS:[0]`.
Each registration has `next` at +0 and `handler` at +4. An empty chain is
`0xffffffff`. Call each handler through `recomp_call` with four cdecl guest
arguments `(record, registration, context, dispatcher)` and the callback
return sentinel. `guest_call` already provides that callback mechanism.
Registration-handler disposition 0 means continue execution; 1 means search
the next record. These are not the similarly named filter-expression values.
Unsupported dispositions and malformed chains must fail diagnostically.

A continuing handler requires restoring the changed guest context. The
implementation must preserve that state across import dispatch: the current
`imports_dispatch` unconditionally increments ESP and overwrites EIP after
calling a shim. Merely restoring a context inside `seh_raise` is insufficient.

[RtlUnwind](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlunwind)
defines a target frame, continuation address, optional exception record and
return-register value. The task's intended subset invokes handlers before
the target with `EXCEPTION_UNWINDING` (2), unlinks each completed registration,
leaves `FS:[0]` at the target, sets EAX to the requested value, and returns
normally through the shim. The measured Delphi callers pass their own next
instruction as TargetIp. This subset is not an implementation of arbitrary
Windows context transfer. Null-target exit unwind and invalid target behavior
must be stated and tested rather than inferred from a successful chain walk.

## The host-stack problem

Translated calls build a host C stack in addition to the guest stack. An
exception can reach an interior block in an older guest function while host
frames for its callees and the exception dispatcher remain active. Calling
another translation wrapper does not by itself abandon those host frames.

## The setjmp-and-landing design

The approved proposal establishes the host checkpoint inside the live
generated function, as the existing `_setjmp` intrinsic requires:

```c
{ jmp_buf *b_ = recomp_seh_frame_enter(c);
  if (setjmp(*b_)) goto seh_landing_<function>; }
```

The proposed landing dispatch switches on guest EIP, with entries for the
stub and the instruction following its JMP. A normal chain restoration
calls `recomp_seh_frame_leave(c)`. An exceptional transfer asks `seh_resume`
to match ESP to a saved registration address and longjmp to that record.
The checkpoint must never be taken inside a helper that returns before use.

**This exact proposal cannot implement the measured executable.**

1. **The proposed recognizer finds none of the measured stubs.** It requires
   the handler immediate to be in `fn.addrs`, but all 2,258 stubs are absent
   even from the union of the original listings. Existing immediate recovery
   creates separate functions or alternate entries; it does not attach the
   omitted exception blocks to the establishing function. Recovery and block
   ownership need an explicit exception-aware design before checkpoints can
   land in that function.
2. **A typed stub's next bytes are data.** For example, the stub at
   `0x00b35f65`, pushed in `00b35eac.asm`, jumps to HandleOnException. Its
   next three dwords are `1`, `0x0081ea7c`, `0x00b35f76`: a count and one
   type/handler pair. The routine reads the count at stub+5, begins entries
   at stub+9, and ultimately executes `JMP dword ptr [EBX + 0x4]` at
   `0x0080a2b1`. Neither the table address nor the stub is the selected
   executable landing. All 158 typed sites require table-aware recovery;
   blindly forcing an instruction label at stub+5 would translate data.
3. **The accepting path does not restore ESP to the registration.** In
   `0080a028.asm`, the suffix following RtlUnwind loads the registration into
   EDI, loads EBP from `[EDI+8]`, changes the registration's handler, computes
   stub+5 in EBX and executes `JMP EBX` at `0x0080a12a`. It keeps its saved
   registers and exception bookkeeping on the guest stack. The intervening
   `00809f90.asm` helper is a debugger notification, not an ESP restoration.
   A focused Unicorn run of this suffix, with the proposed normal-return
   RtlUnwind contract, reaches that JMP with ESP `0x0e007fdc` and
   registration/EDI `0x0e00f000`. An equality lookup against saved ESP misses
   the correct host checkpoint.

A revision must identify the accepting registration independently of the
current guest ESP, distinguish finally callbacks that return from transfers
that abandon host frames, recover executable typed-handler destinations,
and intercept resumptions even if a target already has a dispatch-table
entry. These are changes to the specified interface and control-flow model,
not moved line numbers or a differently named helper. Implementation stops
here under the task's blocker instruction.

## What is emitted per function

Once the blockers are resolved, only functions with verified registration
establishments should receive checkpoint and landing code. Other functions
should retain their present bodies. Both accepted FS spellings require
coverage, and restore recognition must avoid unrelated TEB writes.

The requested two-case synthetic fixture is useful for the simple case but
does not model omitted blocks, typed tables or the measured accepting stack.
Each actual landing needs an instruction boundary in the owning body and a
forced label. Functions split into alternate-entry wrappers must retain a
live checkpoint in that same body. Preserve `[translate].entry_points`
seeding and `--allow-table-gaps` while changing discovery.

## Runtime records

The proposed minimum is `SehFrame { uint32_t esp; jmp_buf env; }`. Records
must have stable storage, belong to the guest context and host thread, and
be removed on normal restoration, unwind and thread/context teardown.
Recursion, nested registrations and reuse of the same stack address must
not select a dead checkpoint. Exception/context storage must survive every
guest callback that references it and be reclaimed after nonlocal transfer.

The revised design needs registration identity/dispatch state beyond the
current-ESP equality rule. It must also truncate abandoned profiling and mod
hook state, following the existing `_longjmp` integration in `runtime/cpu.cpp`.
Whether CALL_FN needs a post-call transfer check depends on the replacement
control-flow contract; it is not determined by this blocked proposal.

## Failure modes

- Do not call guest code while holding a host lock. Nonlocal transfer must
  not abandon a lock owner; the scheduler's unlocked callback convention
  remains mandatory.
- Audit C++ object lifetime across every proposed longjmp boundary as well
  as locks. In particular, `runtime/imports.cpp` currently keeps a local
  `std::string desc` alive across `fn(c)`. A new nonlocal exit through this
  path needs a safe lifetime design; a lock-only rule is insufficient.
- An unhandled raise must log its record and preserve the current useful
  register/stack diagnostics, then abort. The test-only unhandled hook must
  not turn production unhandled exceptions into apparent success.
- Reject malformed/cyclic chains, invalid records and unrecognized landings
  with the relevant guest addresses. Do not use a stale jmp_buf or silently
  execute data to satisfy a forced landing label.
- A handler requesting continuation of a noncontinuable exception is not
  successful handling; Microsoft documents a further exception for that case.

## Testing

Completed design verification only:

- Rehashed the pinned image and counted all establishment/restore sites.
- Decoded every pushed stub from the PE, confirmed its five-byte JMP and
  counted the three shared targets; checked absence from all `.asm` files.
- Decoded the example typed table and traced the System routine's consumer.
- Executed the accepting suffix in Unicorn with intercepted normal-return
  RtlUnwind, TLS lookup and debugger notification calls. This is evidence
  about stack arithmetic, not a run of the runtime or the full handler.

The reproducible measurement script and output are private local artifacts
at the game repository's ignored `build/task12-seh-measure.py` and
`build/task12-seh-measure.log`. No image bytes or generated code are committed.

Not run because implementation stopped in Step 1: the failing/passing
translator cycle, native SEH/runtime/host suites and regeneration. After the
design revision, start with the requested translator regression at
`0x0d02a000`. The existing emitted-C helper is named `translate_case` in
`test_translate_insns.py`; it can be reused without inventing another helper.
Add actual omitted-block, typed-table and nested nonlocal-transfer tests
before implementing the corresponding paths. Verify guest context resume
and normal import return behavior as distinct outcomes.

Native tests must build through the game repository's build/test tools;
add `seh_tests` to the local `build/task-k1-native.py` suite selector and
CTest with label `nogame`. Run `seh_tests`, `runtime_tests`, `host_tests`,
both translator suites and finally
`tools/build.py --regenerate --target headless --jobs 8`. Record exact
commands, exit status, translation-report entry count and compile time.
No compile time or new report count is claimed in this design-only change.

## Out of scope

MSVC `__CxxThrowException` handling remains unsupported and keeps its current
abort diagnostics. A later implementation could reuse safe guest exception
dispatch and host checkpoint infrastructure, but compiler-specific frame
decoding is separate work. Hardware-fault delivery, debugger integration,
vectored handlers and a complete Windows exception subsystem are also outside
this task. No game progress log, changelog, assets or submodule pointer is
changed by this blocked design step.
