// Software exception dispatch and live host checkpoints over synthetic guest frames.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../profile.h"
#include "../seh.h"
#include "../win32.h"
#include "../../platform/os.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

static int checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
        }                                                                                          \
    } while (0)

static uint32_t seen[16], flags_seen[16], visits, unhandled;
static uint32_t first_record, first_context, expected_esp, expected_flags;
static uint32_t handler_search, handler_accept, handler_continue;
static uint32_t landing_runs, after_raise, final_esp, unwound_mod_esp;
static X86 expected_cpu;
static constexpr uint32_t LANDING = 0x0d02a080;

static uint32_t invoke(X86 *c, const char *name, std::initializer_list<uint32_t> args) {
    return guest_call(c, imports_resolve("KERNEL32.dll", name), args.begin(), (int)args.size());
}

static void registration(X86 *c, uint32_t at, uint32_t next, uint32_t handler) {
    wr32(at, next);
    wr32(at + 4, handler);
    wr32(at + 8, c->r[R_EBP]);
    wr32(c->fs_base, at);
}

static void search(X86 *c) {
    uint32_t record = arg(c, 0), context = arg(c, 2);
    CHECK(arg(c, 3) == 0);
    CHECK(visits < 16);
    if (visits < 16) {
        seen[visits] = arg(c, 1);
        flags_seen[visits++] = rd32(record + 4);
    }
    if (!first_record) {
        first_record = record;
        first_context = context;
    }
    CHECK(record == first_record);
    CHECK(context != 0 && gm_valid(context, 0x2cc));
    c->r[R_EAX] = 1;
}

static void continue_execution(X86 *c) {
    c->r[R_EAX] = 0;
}

static void record_unhandled(X86 *c, uint32_t record, uint32_t context) {
    ++unhandled;
    CHECK(visits == 2);
    CHECK(record == first_record && context == first_context);
    CHECK(rd32(record) == 0x12345678);
    CHECK(rd32(record + 4) == 1);
    CHECK(rd32(record + 8) == 0);
    CHECK(rd32(record + 12) == GUEST_RETURN_SENTINEL);
    CHECK(rd32(record + 16) == 15);
    for (uint32_t i = 0; i < 15; ++i)
        CHECK(rd32(record + 20 + 4 * i) == 100 + i);
    CHECK(rd32(context) == 0x10003);
    const int regs[] = {R_EDI, R_ESI, R_EBX, R_EDX, R_ECX, R_EAX, R_EBP};
    for (uint32_t i = 0; i < 7; ++i)
        CHECK(rd32(context + 0x9c + i * 4) == expected_cpu.r[regs[i]]);
    CHECK(rd32(context + 0xb8) == GUEST_RETURN_SENTINEL);
    CHECK(rd32(context + 0xc0) == expected_flags);
    CHECK(rd32(context + 0xc4) == expected_esp);
}

// A landing restores the guest frame, unlinks it, and performs its guest RET.
static void block(X86 *c) {
    ++landing_runs;
    CHECK(heap_owns(first_record)); // the abandoned dispatcher still owns live exception data
    CHECK(rd32(first_record) == 0x12345678);
    CHECK(c->r[R_EAX] == 0xabcdef);
    CHECK(recomp_seh_pending_target() == 0);
    CHECK(recomp_seh_test_frame_count(c) == 1); // abandoned callee frame was dropped
    CHECK(c->r[R_ESP] < c->r[R_EBP]);           // still on the dispatcher's guest stack
    uint32_t reg = c->r[R_EBP];
    wr32(c->fs_base, rd32(reg));
    c->r[R_ESP] = reg + 12;
    recomp_seh_frame_leave(c);
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4;
    recomp_return(c);
    final_esp = c->r[R_ESP];
}

// These are the test's tiny dispatch tables. Calls never intercept an unwind;
// jumps intercept before lookup, including an already-known landing entry.
extern "C" int recomp_is_call_return(uint32_t target) {
    return target == GUEST_RETURN_SENTINEL;
}
extern "C" int32_t recomp_index_of(uint32_t target) {
    // A call-return can also be an entry. The landing's RET must prefer
    // its pending caller, rather than dispatch that entry a second time.
    return target == LANDING || target == GUEST_RETURN_SENTINEL ? 0 : -1;
}
extern "C" void recomp_call(X86 *c, uint32_t target) {
    if (target == LANDING) {
        block(c);
        return;
    }
    CHECK(imports_dispatch(c, target));
}
extern "C" void recomp_jump(X86 *c, uint32_t target) {
    if (recomp_seh_pending_target())
        recomp_seh_intercept(c, target);
    recomp_call(c, target);
}
extern "C" void mods_hooks_unwind_to_esp(uint32_t esp) {
    unwound_mod_esp = esp;
}

static void accept(X86 *c) {
    uint32_t reg = arg(c, 1), record = arg(c, 0);
    invoke(c, "RtlUnwind", {reg, 0, record, 0xabcdef});
    CHECK(recomp_seh_pending_target() == reg);
    CHECK(c->r[R_ESP] < reg);
    c->r[R_EBP] = reg;
    recomp_profile_push(11); // abandoned by the nonlocal transfer
    recomp_jump(c, LANDING);
    CHECK(false);
}

static void callee(X86 *c) {
    c->r[R_ESP] -= 64;
    registration(c, c->r[R_ESP], rd32(c->fs_base), handler_search);
    {
        jmp_buf *b_ = recomp_seh_frame_enter(c);
        if (setjmp(*b_)) {
            recomp_seh_land(c);
            return;
        }
    }
    invoke(c, "RaiseException", {0x12345678, 0, 0, 0});
    ++after_raise;
}

static void establishing(X86 *c) {
    c->r[R_ESP] -= 12;
    registration(c, c->r[R_ESP], 0xffffffff, handler_accept);
    {
        jmp_buf *b_ = recomp_seh_frame_enter(c);
        if (setjmp(*b_)) {
            recomp_seh_land(c);
            return;
        }
    }
    callee(c);
    ++after_raise;
}

static void clear_observations() {
    visits = unhandled = first_record = first_context = 0;
}

static void chain_walk() {
    X86 c;
    loader_init_context(&c);
    clear_observations();
    uint32_t outer = c.r[R_ESP] - 32, inner = outer - 12;
    registration(&c, outer, 0xffffffff, handler_search);
    registration(&c, inner, outer, handler_search);
    c.r[R_ESP] = inner - 32;
    for (int i = 0; i < 8; ++i)
        if (i != R_ESP)
            c.r[i] = 0x12340000u + 0x101u * i;
    x86_set_eflags(&c, 0xed7);
    uint32_t info = heap_alloc(64, true);
    for (uint32_t i = 0; i < 16; ++i)
        wr32(info + 4 * i, 100 + i);
    expected_cpu = c;
    expected_flags = x86_get_eflags(&c);
    expected_esp = c.r[R_ESP] - 20;
    uint32_t blocks = heap_stats().used_blocks;
    recomp_seh_test_unhandled_hook(record_unhandled);
    invoke(&c, "RaiseException", {0x12345678, 1, 16, info});
    CHECK(visits == 2 && seen[0] == inner && seen[1] == outer && unhandled == 1);
    CHECK(heap_stats().used_blocks == blocks);
    CHECK(rd32(c.fs_base) == inner);
    recomp_seh_test_unhandled_hook(nullptr);
    heap_free(info);
}

static void unwind_and_leave() {
    X86 c;
    loader_init_context(&c);
    clear_observations();
    uint32_t outer = c.r[R_ESP] - 12, inner = outer - 12;
    c.r[R_ESP] = outer;
    registration(&c, outer, 0xffffffff, handler_search);
    recomp_seh_frame_enter(&c);
    c.r[R_ESP] = inner;
    registration(&c, inner, outer, handler_search);
    recomp_seh_frame_enter(&c);
    // The measured normal restoration pops three words before the FS store.
    c.r[R_ESP] += 12;
    wr32(c.fs_base, outer);
    recomp_seh_frame_leave(&c);
    CHECK(recomp_seh_test_frame_count(&c) == 1);
    recomp_seh_frame_leave(&c); // equality and no-op leave preserve the outer
    CHECK(recomp_seh_test_frame_count(&c) == 1);
    c.r[R_ESP] = inner;
    registration(&c, inner, outer, handler_search);
    recomp_seh_frame_enter(&c);
    uint32_t blocks = heap_stats().used_blocks;
    CHECK(invoke(&c, "RtlUnwind", {outer, 0, 0, 0x77}) == 0x77);
    CHECK(visits == 1 && seen[0] == inner && (flags_seen[0] & 2));
    CHECK(rd32(c.fs_base) == outer && recomp_seh_pending_target() == outer);
    CHECK(heap_stats().used_blocks == blocks);
    // Unwind left stale checkpoints. A later normal restoration drops all
    // strictly below ESP, including the former inner registration.
    c.r[R_ESP] = outer + 12;
    wr32(c.fs_base, 0xffffffff);
    recomp_seh_frame_leave(&c);
    CHECK(recomp_seh_test_frame_count(&c) == 0);
    recomp_seh_reset(&c);
    clear_observations();
    c.r[R_ESP] = inner;
    registration(&c, outer, 0xffffffff, handler_search);
    registration(&c, inner, outer, handler_search);
    invoke(&c, "RtlUnwind", {0, 0, 0, 0x88});
    CHECK(visits == 2 && (flags_seen[0] & 2) && (flags_seen[1] & 2));
    CHECK(rd32(c.fs_base) == 0xffffffff && recomp_seh_pending_target() == 0);
}

static void landing() {
    X86 c;
    loader_init_context(&c);
    clear_observations();
    uint32_t before = c.r[R_ESP], blocks = heap_stats().used_blocks;
    recomp_profile_push(7);
    uint32_t depth = recomp_profile_depth();
    establishing(&c);
    CHECK(landing_runs == 1 && after_raise == 0);
    CHECK(final_esp == before + 4 && c.r[R_ESP] == final_esp);
    CHECK(c.eip == GUEST_RETURN_SENTINEL);
    CHECK(recomp_seh_test_frame_count(&c) == 0);
    CHECK(recomp_profile_depth() == depth);
    CHECK(unwound_mod_esp == before - 12);
    CHECK(heap_stats().used_blocks == blocks);
    recomp_profile_pop();
    recomp_seh_frame_enter(&c);
    loader_init_context(&c); // reusing a context must not retain its old env
    CHECK(recomp_seh_test_frame_count(&c) == 0);
}

static void fatal_case(const char *which) {
    X86 c;
    loader_init_context(&c);
    uint32_t reg = c.r[R_ESP] - 32;
    c.r[R_ESP] = reg;
    registration(&c, reg, 0xffffffff, handler_search);
    if (!strcmp(which, "cycle"))
        wr32(reg, reg);
    if (!strcmp(which, "invalid"))
        wr32(c.fs_base, HEAP_BASE);
    if (!strcmp(which, "continue"))
        wr32(reg + 4, handler_continue);
    if (!strcmp(which, "missing-target"))
        invoke(&c, "RtlUnwind", {reg + 12, 0, 0, 0});
    if (!strcmp(which, "missing-checkpoint")) {
        invoke(&c, "RtlUnwind", {reg, 0, 0, 0});
        recomp_jump(&c, LANDING);
    }
    invoke(&c, "RaiseException", {0x12345678, 0, 0, 0});
}

static void teardown() {
    X86 c;
    loader_init_context(&c);
    c.r[R_ESP] -= 12;
    registration(&c, c.r[R_ESP], 0xffffffff, handler_search);
    recomp_seh_frame_enter(&c);
    invoke(&c, "RtlUnwind", {c.r[R_ESP], 0, 0, 0});
    CHECK(recomp_seh_pending_target() != 0);
    sched_run_thread_unwind_frames();
    CHECK(recomp_seh_test_frame_count(&c) == 0);
    CHECK(recomp_seh_pending_target() == 0);
}

int main(int argc, char **argv) {
    mem_init();
    imports_init();
    handler_search = imports_alloc_trampoline("SEH.dll", "search", search, ARGC_CDECL);
    handler_accept = imports_alloc_trampoline("SEH.dll", "accept", accept, ARGC_CDECL);
    handler_continue =
        imports_alloc_trampoline("SEH.dll", "continue", continue_execution, ARGC_CDECL);
    if (argc > 1) {
        fatal_case(argv[1]);
        return 1;
    }
    chain_walk();
    unwind_and_leave();
    landing();
    teardown();
    char exe[4096];
    CHECK(os_exe_path(exe, sizeof exe) == 0);
    for (const char *which :
         {"cycle", "invalid", "continue", "missing-target", "missing-checkpoint", "unhandled"}) {
        const char *args[] = {exe, which, nullptr};
        int64_t pid = 0;
        int code = -1;
        CHECK(os_spawn(args, &pid) == 0 && os_wait(pid, &code) == 0 && code == 134);
    }
    mem_shutdown();
    printf("seh_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
