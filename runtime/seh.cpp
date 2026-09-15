// x86 registration-chain dispatch and host checkpoints for Delphi frames.
#include "seh.h"
#include "imports.h"
#include "memory.h"
#include "mods_seam.h"
#include "profile.h"
#include <cstdlib>
#include <vector>

namespace {
constexpr uint32_t END_CHAIN = 0xffffffffu;
constexpr uint32_t RECORD_BYTES = 0x50;
constexpr uint32_t CONTEXT_BYTES = 0x2cc;
constexpr uint32_t MAX_LINKS = 4096;

struct SehFrame {
    X86 *cpu;
    uint32_t registration;
    uint32_t establishing_eip;
    jmp_buf env;
    uint32_t profile_depth;
    size_t dispatch_depth;
};

// Heap records outlive guest callbacks, including those abandoned by longjmp.
// A checkpoint remembers the dispatch depth before the exception. Its landing
// releases abandoned records only AFTER the block has finished using them.
struct SehDispatch {
    X86 *cpu;
    uint32_t storage;
    uint32_t record;
    uint32_t context;
};

void release_dispatch(SehDispatch *d) {
    if (g_mem)
        heap_free(d->storage);
    delete d;
}

struct SehState {
    std::vector<SehFrame *> frames;
    std::vector<SehDispatch *> dispatches;
    uint32_t g_seh_pending_target = 0;
    X86 *pending_cpu = nullptr;
    SehFrame *landing = nullptr;
    ~SehState() {
        for (SehFrame *f : frames)
            delete f;
        for (SehDispatch *d : dispatches)
            release_dispatch(d);
    }
};
thread_local SehState state;
#ifdef POPM_TESTING
thread_local RecompSehUnhandledHook unhandled_hook = nullptr;
#endif

// Verbose traces retain guest addresses only; they distinguish an omitted
// establishment from a checkpoint retired while its registration remains live.
void trace_frames(X86 *c, const char *phase) {
    LOGV("SEH %s: pending=%08x EIP=%08x ESP=%08x live=%zu", phase, state.g_seh_pending_target,
         c->eip, c->r[R_ESP], state.frames.size());
    if (log_level() < 2)
        return;
    for (const SehFrame *f : state.frames)
        if (f->cpu == c)
            LOGV("SEH live: registration=%08x established=%08x", f->registration,
                 f->establishing_eip);
}

[[noreturn]] void invalid_chain(X86 *c, uint32_t reg, uint32_t target, const char *why) {
    LOGW("SEH: %s (registration=%08x target=%08x FS=%08x ESP=%08x)", why, reg, target, c->fs_base,
         c->r[R_ESP]);
    abort();
}

// Bounds come from this guest thread's TEB, so heap-backed worker stacks are
// checked as strictly as the main stack. Validate before any registration read.
uint32_t chain_head(X86 *c) {
    if (!c->fs_base || !gm_valid(c->fs_base, 12))
        invalid_chain(c, 0, 0, "invalid TEB");
    return rd32(c->fs_base);
}

void validate_registration(X86 *c, uint32_t reg, uint32_t target) {
    chain_head(c);
    uint32_t lo = rd32(c->fs_base + 8), hi = rd32(c->fs_base + 4);
    if (lo >= hi || !gm_valid(lo, hi - lo) || hi - lo < 8 || reg < lo || reg > hi - 8 || (reg & 3))
        invalid_chain(c, reg, target, "registration outside guest stack");
}

// Trivial automatic storage only: a handler can leave this walk by longjmp.
struct ChainWalk {
    uint32_t visited[MAX_LINKS];
    uint32_t count = 0;
    void visit(X86 *c, uint32_t reg, uint32_t target) {
        validate_registration(c, reg, target);
        if (count == MAX_LINKS)
            invalid_chain(c, reg, target, "chain exceeds 4096 links");
        for (uint32_t i = 0; i < count; ++i)
            if (visited[i] == reg)
                invalid_chain(c, reg, target, "cyclic chain");
        visited[count++] = reg;
    }
};

// Windows' x86 integer/control CONTEXT fields, always guest arena offsets.
void fill_context(X86 *c, uint32_t context) {
    wr32(context, 0x10003);     // CONTEXT_i386 | CONTROL | INTEGER
    wr32(context + 0x90, 0x3b); // FS (TEB)
    wr32(context + 0x94, 0x23); // ES
    wr32(context + 0x98, 0x23); // DS
    const int regs[] = {R_EDI, R_ESI, R_EBX, R_EDX, R_ECX, R_EAX, R_EBP};
    for (uint32_t i = 0; i < 7; ++i)
        wr32(context + 0x9c + i * 4, c->r[regs[i]]);
    wr32(context + 0xb8, rd32(c->r[R_ESP]));
    wr32(context + 0xbc, 0x1b); // CS
    wr32(context + 0xc0, x86_get_eflags(c));
    wr32(context + 0xc4, c->r[R_ESP]);
    wr32(context + 0xc8, 0x23); // SS
}

SehDispatch *new_dispatch(X86 *c, uint32_t supplied_record = 0) {
    if (supplied_record && !gm_valid(supplied_record, RECORD_BYTES))
        invalid_chain(c, 0, supplied_record, "invalid exception record");
    uint32_t storage = heap_alloc(RECORD_BYTES + CONTEXT_BYTES, true);
    if (!storage)
        invalid_chain(c, 0, 0, "cannot allocate exception records");
    SehDispatch *d = new SehDispatch{c, storage, supplied_record ? supplied_record : storage,
                                     storage + RECORD_BYTES};
    state.dispatches.push_back(d);
    fill_context(c, d->context);
    return d;
}

void finish_dispatch(SehDispatch *d) {
    if (state.dispatches.empty() || state.dispatches.back() != d)
        invalid_chain(d->cpu, 0, d->record, "dispatch ownership mismatch");
    state.dispatches.pop_back();
    release_dispatch(d);
}

uint32_t call_handler(X86 *c, uint32_t reg, SehDispatch *d) {
    uint32_t handler = rd32(reg + 4);
    if (!handler || !gm_valid(handler, 1))
        invalid_chain(c, reg, handler, "invalid handler");
    LOGV("SEH handler: registration=%08x handler=%08x flags=%08x", reg, handler,
         rd32(d->record + 4));
    uint32_t args[] = {d->record, reg, d->context, 0};
    uint32_t disposition = guest_call(c, handler, args, 4);
    if (disposition == 0) {
        LOGW("ExceptionContinueExecution requested; not supported (record=%08x code=%08x "
             "registration=%08x handler=%08x)",
             d->record, rd32(d->record), reg, handler);
        abort();
    }
    if (disposition != 1) {
        LOGW("SEH: unsupported disposition %u (record=%08x registration=%08x handler=%08x)",
             disposition, d->record, reg, handler);
        abort();
    }
    return disposition;
}
} // namespace

extern "C" {

jmp_buf *recomp_seh_frame_enter(X86 *c) {
    validate_registration(c, c->r[R_ESP], 0);
    SehFrame *f = new SehFrame{};
    f->cpu = c;
    f->registration = c->r[R_ESP];
    f->establishing_eip = c->eip;
    f->profile_depth = recomp_profile_depth();
    f->dispatch_depth = state.dispatches.size();
    state.frames.push_back(f); // individually allocated: growing the vector cannot move env
    LOGV("SEH enter: registration=%08x established=%08x ESP=%08x handler=%08x", f->registration,
         f->establishing_eip, c->r[R_ESP], rd32(f->registration + 4));
    return &f->env;
}

void recomp_seh_frame_leave(X86 *c) {
    bool removed = false;
    for (size_t i = state.frames.size(); i-- > 0;) {
        SehFrame *f = state.frames[i];
        if (f->cpu == c && f->registration < c->r[R_ESP]) {
            LOGV("SEH leave: registration=%08x established=%08x EIP=%08x ESP=%08x", f->registration,
                 f->establishing_eip, c->eip, c->r[R_ESP]);
            state.frames.erase(state.frames.begin() + i);
            delete f;
            removed = true;
        }
    }
    if (!removed)
        LOGV("SEH leave: no retired frame at ESP=%08x", c->r[R_ESP]);
}

uint32_t recomp_seh_pending_target(void) {
    return state.g_seh_pending_target;
}

// Called only for computed jumps, before even an existing dispatch-table hit.
// The dispatcher keeps its own ESP; the pending registration identifies env.
void recomp_seh_intercept(X86 *c, uint32_t target) {
    trace_frames(c, "intercept");
    uint32_t reg = state.g_seh_pending_target;
    SehFrame *landing = nullptr;
    for (size_t i = state.frames.size(); i-- > 0;)
        if (state.frames[i]->cpu == c && state.frames[i]->registration == reg) {
            landing = state.frames[i];
            break;
        }
    if (!landing || state.pending_cpu != c)
        invalid_chain(c, reg, target, "unwind target has no live checkpoint");
    state.g_seh_pending_target = 0;
    state.pending_cpu = nullptr;
    for (size_t i = state.frames.size(); i-- > 0;) {
        SehFrame *f = state.frames[i];
        if (f->cpu == c && f->registration < reg) {
            state.frames.erase(state.frames.begin() + i);
            delete f;
        }
    }
    mods_hooks_unwind_to_esp(reg);
    recomp_profile_truncate(landing->profile_depth);
    state.landing = landing;
    c->eip = target;
    longjmp(landing->env, 1);
}

void recomp_seh_land(X86 *c) {
    trace_frames(c, "landing");
    SehFrame *landing = state.landing;
    if (!landing || landing->cpu != c)
        invalid_chain(c, 0, c->eip, "landing without a checkpoint");
    size_t depth = landing->dispatch_depth;
    state.landing = nullptr;
    recomp_call(c, c->eip);
    // Normal stores in the block usually retire the checkpoint. If the block
    // did not store FS:[0], its host frame still ends here, so env must go too.
    for (size_t i = state.frames.size(); i-- > 0;)
        if (state.frames[i] == landing) {
            state.frames.erase(state.frames.begin() + i);
            delete landing;
            break;
        }
    while (state.dispatches.size() > depth) {
        SehDispatch *d = state.dispatches.back();
        state.dispatches.pop_back();
        release_dispatch(d);
    }
}

void recomp_seh_reset(X86 *c) {
    for (size_t i = state.frames.size(); i-- > 0;)
        if (!c || state.frames[i]->cpu == c) {
            if (state.landing == state.frames[i])
                state.landing = nullptr;
            delete state.frames[i];
            state.frames.erase(state.frames.begin() + i);
        }
    for (size_t i = state.dispatches.size(); i-- > 0;)
        if (!c || state.dispatches[i]->cpu == c) {
            release_dispatch(state.dispatches[i]);
            state.dispatches.erase(state.dispatches.begin() + i);
        }
    if (!c || state.pending_cpu == c) {
        state.g_seh_pending_target = 0;
        state.pending_cpu = nullptr;
    }
}

int recomp_seh_raise(X86 *c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args) {
    X86 saved = *c;
    if (nargs > 15)
        nargs = 15;
    if (nargs && (!args || !gm_valid(args, nargs * 4)))
        invalid_chain(c, 0, args, "invalid exception information");
    SehDispatch *d = new_dispatch(c);
    wr32(d->record, code);
    wr32(d->record + 4, flags);
    wr32(d->record + 12, rd32(c->r[R_ESP]));
    wr32(d->record + 16, nargs);
    for (uint32_t i = 0; i < nargs; ++i)
        wr32(d->record + 20 + i * 4, rd32(args + i * 4));
    ChainWalk walk;
    for (uint32_t reg = chain_head(c); reg != END_CHAIN;) {
        walk.visit(c, reg, 0);
        uint32_t next = rd32(reg);
        call_handler(c, reg, d);
        reg = next;
    }
    *c = saved; // retain the original raise's registers for the existing diagnostics
    int intercepted = 0;
#ifdef POPM_TESTING
    if (unhandled_hook) {
        unhandled_hook(c, d->record, d->context);
        intercepted = 1;
    }
#endif
    if (!intercepted)
        LOGW("SEH: unhandled exception record=%08x context=%08x code=%08x flags=%08x", d->record,
             d->context, code, flags);
    finish_dispatch(d);
    return intercepted;
}

// Delphi passes its own next instruction as TargetIp. Returning through the
// import shim performs that transfer; only its later computed JMP abandons
// the dispatcher and lands on the saved host checkpoint.
void recomp_seh_unwind(X86 *c, uint32_t target, uint32_t target_ip, uint32_t record,
                       uint32_t retval) {
    trace_frames(c, "unwind begin");
    SehDispatch *d = new_dispatch(c, record);
    if (!record) {
        wr32(d->record, 0xc0000027u); // STATUS_UNWIND
        wr32(d->record + 12, rd32(c->r[R_ESP]));
    }
    wr32(d->record + 4, rd32(d->record + 4) | 2u);
    ChainWalk walk;
    uint32_t reg = chain_head(c);
    uint32_t end = target ? target : END_CHAIN;
    while (reg != end) {
        if (reg == END_CHAIN)
            invalid_chain(c, reg, target, "unwind target is absent from chain");
        walk.visit(c, reg, target);
        uint32_t next = rd32(reg);
        call_handler(c, reg, d);
        wr32(c->fs_base, next);
        reg = next;
    }
    if (target)
        validate_registration(c, target, target);
    wr32(c->fs_base, end);
    state.g_seh_pending_target = target;
    state.pending_cpu = target ? c : nullptr;
    c->r[R_EAX] = retval;
    LOGV("RtlUnwind: registration=%08x target_ip=%08x retval=%08x", target, target_ip, retval);
    trace_frames(c, "unwind ready");
    finish_dispatch(d);
}

#ifdef POPM_TESTING
void recomp_seh_test_unhandled_hook(RecompSehUnhandledHook hook) {
    unhandled_hook = hook;
}
uint32_t recomp_seh_test_frame_count(X86 *c) {
    uint32_t n = 0;
    for (SehFrame *f : state.frames)
        if (f->cpu == c)
            ++n;
    return n;
}
#endif
} // extern "C"
