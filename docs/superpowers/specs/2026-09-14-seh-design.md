# Delphi structured exception handling

Date: 2026-09-14. Task 12, measured against kit `9345855`.

**Status: implemented and validated on macOS; regeneration, four native suites, portable translator suites and kit checks pass. Legacy corpus-specific translator checks remain unavailable.**
The revised design below resolves the three original landing contradictions
and corrects normal retirement to use `registration < ESP`. Regeneration
also identified null default-type entries and a speculative-owner boundary;
the regressions and resolver now cover both. This document does not claim a
game run or validation on the other three presets.

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

The checkpoint belongs to the live generated function. Immediately after a
chain establishment store it emits the same two-call pattern as `_setjmp`:

```c
{ jmp_buf *b_ = recomp_seh_frame_enter(c);
  if (setjmp(*b_)) { recomp_seh_land(c); return; } }
```

`enter` allocates stable storage for the environment; it never executes
`setjmp` itself. A normal chain restoration calls `recomp_seh_frame_leave`
after the store. Retirement removes records **strictly below current ESP**;
equality is a live record and must be retained. A leave with nothing to
remove logs at verbose level and returns, since other chain-head stores can
occur in a function with frames.

`RaiseException` walks the guest registration chain through `guest_call`.
Disposition 1 searches the next registration. Disposition 0 logs
`ExceptionContinueExecution requested; not supported` with the record and
aborts: context restoration is outside this task's implemented subset.
Exhaustion preserves the existing C++ throw, return-chain, register-object
and stack-return diagnostics before aborting. Only the testing runtime can
replace unhandled abort with a recording hook.

`RtlUnwind` calls handlers before the target with flag 2 and unlinks their
registrations. Finally handlers may call their blocks as ordinary nested
subroutines; those calls do not perform a nonlocal transfer. A non-null
target is left at `FS:[0]` and stored as the thread's pending registration;
EAX receives the requested return value. The shim returns normally because
the measured Delphi TargetIp is its own return address. A null target walks
the whole chain, writes the empty-chain sentinel `0xffffffff`, clears the
pending registration and returns. A target absent from the chain aborts.

The generated `recomp_jump` checks the pending registration **before table
lookup**, including a destination already in the dispatch table. The
runtime locates its checkpoint by registration identity, independent of the
dispatcher's current ESP. It clears pending state, removes checkpoints
strictly below that registration, truncates profiling and mod-hook state,
sets guest EIP to the jump destination, and `longjmp`s to the live generated
frame. An absent checkpoint aborts with registration and destination.
`recomp_call` does not intercept; the accepting transfer is a computed jump.

`recomp_seh_land` calls the recovered block through `recomp_call(c, c->eip)`.
That block reaches any remaining listed body through an alternate entry
and executes the guest RET. When it returns, the checkpoint returns from
the establishing host frame as well. Guest registers, including the
accepting dispatcher's ESP, are not restored to a snapshot by the host jump.

## What is emitted per function

Discovery recognizes a pushed immediate followed within two instructions
by `PUSH dword ptr FS:[EAX]` or `FS:[0x0]`, followed by a contiguous
`MOV dword ptr FS:[EAX|0x0],ESP`. The immediate is a handler stub even when
its bytes are absent from every exported listing. Other TEB fields do not
match this pattern.

At each stub S, validate a five-byte `JMP rel32` into executable memory.
Classify the following bytes without recognizing or naming its target:
read `n = dword[S+5]`; for `1 <= n <= 64`, require every pair at
`S+9+8*i` and `S+13+8*i` to name initialized data or code (or null for the
default handler), then executable code, respectively. If every pair passes,
the second elements are landing entries and the count/pairs are table
storage. Otherwise S+5 is the landing.
The table bytes never become forced executable entries.

The null-type allowance is a measured adjustment to the task's initial
classifier: stubs `0x0091e551`, `0x0091e611` and `0x0091e6dd` each have two
pairs, the second with a zero type and an executable default handler. The
System consumer tests the type at `0x0080a1a6` and branches directly to
acceptance when zero. Requiring every type to lie inside a section decoded
these three tables as instructions and produced six unresolved dispatches.

The existing entry-point resolver seeds landings with structural `seh`
provenance, protecting them from speculative-block withdrawal. Stubs retain
immediate provenance. SEH provenance follows recovered continuations, not
ordinary callees or the call graph of an already listed owner. The
existing recovery implementation follows branches by recursive descent;
it stops at already owned instructions. A branch back into a listed body
therefore creates or reuses an alternate entry instead of duplicating that
body. A structural continuation whose current owner is a speculative
recovered body is recovered independently: pruning a bad guessed prefix
must not remove its valid suffix. This fixes the remaining regeneration
failure at `0x00b5aea3`, which jumps into a suffix owned by a pruned pointer
guess. Configuration entry points keep their existing precedence and
`--allow-table-gaps` behavior is unchanged.

Only a function containing a recognized frame site gains checkpoint code.
Within it, each matching FS chain-head MOV from ESP emits the store plus
`frame_enter`/`setjmp`/`land`; a matching MOV from another 32-bit register
emits the store plus `frame_leave`. Other instructions retain their existing
translation. Functions without sites do not gain checkpoint/leave calls.
The generated header includes the C-compatible SEH declarations, and the
computed-jump table calls the pending-target accessor before interception.

## Runtime records

Each host thread owns its SEH state. Individually allocated frames keep
`jmp_buf` stable even when their pointer vector grows. A frame records its
32-bit registration address, CPU-context identity, host environment,
profiling depth and exception-dispatch depth. Matching by both context and
registration prevents selecting another context's checkpoint on that host
thread. Normal retirement and landing discard only addresses strictly below
their respective ESP/registration boundary.

The corrected boundary was verified on the pinned image's two nested
establishments at `0x008811b1` and `0x008811bf`. The normal restoration at
`0x008811ee` follows three POPs: the retired inner registration is
`0x0e00efe8`, whereas both ESP and the still-live outer registration are
`0x0e00eff4`. Thus `<` retires the inner and preserves the equal outer;
the superseded `>=` predicate selected the wrong frame.

An exception dispatch owns a guest heap allocation containing its 0x50-byte
record and 0x2cc-byte context. An unwind may reference a supplied record
while owning a separate context allocation. Dispatch ownership is recorded
in thread state before callbacks, so a nonlocal transfer cannot leak an
automatic owner. A checkpoint's saved dispatch depth bounds what its landing
reclaims **after** the guest block returns; exception data remain valid
through the accepting routine's cleanup. Nested landings reclaim only their
own abandoned dispatches. A returned landing also retires its checkpoint if
no chain store already removed it: its host environment is no longer live.

The walker reads stack bounds from the current TEB, supporting the main
stack and heap-backed worker stacks. Registrations require eight aligned
bytes inside those bounds. Repeated addresses, more than 4,096 links,
invalid records, unsupported dispositions and missing unwind targets fail
with guest addresses instead of continuing with corrupted state.

Thread exit drops its frames and dispatches before publishing
completion; host thread-local destruction is a final cleanup. Context reuse
and guest-arena teardown explicitly reset owned state. A synchronous worker
return retires frames below its caller's restored ESP. The import dispatcher
copies its description into a fixed character buffer, so no local
`std::string` remains live across the shim call that can longjmp.

## Failure modes

- Do not call guest code while holding a host lock. Nonlocal transfer must
  not abandon a lock owner; the scheduler's unlocked callback convention
  remains mandatory.
- Audit C++ object lifetime across every proposed longjmp boundary as well
  as locks. The import dispatcher now uses a fixed description buffer across
  `fn(c)`; future changes must not reintroduce an automatic C++ owner there.
- An unhandled raise must log its record and preserve the current useful
  register/stack diagnostics, then abort. The test-only unhandled hook must
  not turn production unhandled exceptions into apparent success.
- Reject malformed/cyclic chains, invalid records and unrecognized landings
  with the relevant guest addresses. Do not use a stale jmp_buf or silently
  execute data to satisfy a forced landing label.
- A handler requesting continuation of a noncontinuable exception is not
  successful handling; Microsoft documents a further exception for that case.

## Testing

The original hash/stub/table/accepting-stack measurements remain reproducible
with the private local `build/task12-seh-measure.py`. The corrected normal
retirement boundary is recorded by the private
`build/task12-seh-revised-measure.py` probe. Neither script is a game run;
no image bytes or generated code are committed.

The synthetic translator regressions use
`translate_case` from `test_translate_insns.py` for checkpoint emission,
and the driver's synthetic image fixture for omitted-byte discovery. The
original helper deliberately uses `NoImage` and cannot recover machine-code
entries by itself. Both FS spellings, an unrelated TEB write, plain and
typed/default omitted blocks, speculative-owner recovery, alternate entries
and interception before lookup are covered without game inputs.

The native `seh_tests` CTest target is labelled `nogame` and builds through
the game repository's test wrapper. It checks chain order and record fields,
unwind flags/return value, normal retirement's strict inequality, exit
unwind, a real host nonlocal landing, skipped callee continuation, guest RET
state, profile/mod-hook truncation, allocation cleanup, context reuse, thread
teardown and fatal malformed-chain/unsupported-continuation/missing-checkpoint
paths. The final run reports **113 checks, 0 failures**.

Validation on 2026-09-14, from the game repository root with its Python:

- Translator tests were written and run before implementation: the initial
  SEH suite failed four tests, then passed. The null-default table,
  speculative-owner, provenance and thread-teardown regressions also failed
  before their corresponding fixes.
- `.venv/bin/python -m pytest -q kit/tools/recomp/tests
  --ignore=kit/tools/recomp/tests/test_translate.py
  --ignore=kit/tools/recomp/tests/test_eaxa.py
  --ignore=kit/tools/recomp/tests/test_translate_hooks.py`:
  **87 passed, 1 skipped**. The first two exclusions are standalone
  game-backed differential scripts, not portable pytest suites. The hook
  suite was attempted separately and produced **13 failures** because its
  paths, executable hash and expected symbols require the legacy game's
  generated corpus, which is absent in this checkout. Its assertions were
  not weakened or represented as passing.
- `.venv/bin/python build/task-k1-native.py <suite>` for each of
  `runtime_tests`, `host_tests`, `dx_tests` and `seh_tests`: **1/1 CTest passed**
  for each suite. The existing local selector now includes `seh_tests`.
- `.venv/bin/python -m pytest -q kit/tests`: **37 passed, 2 skipped**.
- `.venv/bin/python -m pytest -q tests`: **4 passed**.
- `.venv/bin/python tools/build.py --regenerate --target headless --jobs 8`:
  successful translation and headless link. The report has **38,196 entries**
  (**5,602** above the previous 32,594), **5,391 alternate entries**, and
  **8,181** entries with `seh` provenance. Provenance includes recovered
  continuations and promoted owners, so it is not a count of frame sites.
  There are no translation failures, jump-table gaps, undecoded table sites
  or stale alternate entries. `fn_008812e8` is defined in the generated C.
  Translation took **89.36 seconds**; the 166 generated-C compilation steps
  spanned **19.77 seconds** in Ninja's log. Build/link spanned **20.18 seconds**;
  the complete regeneration/build command took **110.33 seconds** wall time.

Build and test logs are private local artifacts under `build/`, not committed
inputs. Formatting, game-literal and staged repository checks also pass.

## Out of scope

MSVC `__CxxThrowException` handling remains unsupported and keeps its current
abort diagnostics. A later implementation could reuse safe guest exception
dispatch and host checkpoint infrastructure, but compiler-specific frame
decoding is separate work. Hardware-fault delivery, debugger integration,
vectored handlers and a complete Windows exception subsystem are also outside
this task. No game progress log, changelog, assets or parent-repository
submodule update is committed by this task; the orchestrator records
game-port progress.
