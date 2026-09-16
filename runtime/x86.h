/* x86.h - guest CPU state and instruction helpers for the static recompiler.
 *
 * Emitted/owned by tools/recomp/translate.py (Task 1).  The generated C in
 * build/recomp/gen/ includes this header, and so does the runtime in
 * runtime/.  Field names are fixed by the m1-recomp plan's
 * "Global constraints" section; do not rename them.
 *
 * Conventions
 *   - All guest pointers are uint32_t.  Guest address `a` lives at g_mem + a.
 *   - Flag fields hold 0 or 1.  The translator only stores a flag when a
 *     later instruction may read it (per-function liveness), so a flag field
 *     is only meaningful where the original x86 flag was live.
 *   - x87 registers are `double` (plan ruling; not 80-bit).
 */
#ifndef RECOMP_X86_H
#define RECOMP_X86_H

#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <math.h>
#ifndef M_LN2 /* glibc and MSVC hide the M_ constants without feature macros */
#define M_LN2 0.693147180559945309417
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- memory */

#ifndef GUEST_SIZE
#define GUEST_SIZE 0x10000000u /* 256 MB arena; RECOMP_GUEST_SIZE grows it for auxiliary modules */
#endif

extern uint8_t *g_mem;

/* Guest memory layout (see plan Global constraints, as corrected):
 *   0x00400000 + SizeOfImage  image (preferred base, no relocation).  The end
 *                             is a PE header value, not a constant; ask the
 *                             loader (loader_image_limit()) for it.
 *   GUEST_HEAP_BASE..0x0e000000  heap arena (default 0x01000000)
 *   0x0f000000                stack top, grows down (1 MB)
 *   0x0fe00000                TEB (FS base)
 *   0x0ff00000 + 16*i         import shim trampoline for import i
 */
/* The build passes the game's image base; this default serves the test harness. */
#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x00400000u
#endif
/* The build passes the game's heap start ([game] heap_base); the default
 * serves the test harness and games whose image ends below 16 MB. */
#ifndef GUEST_HEAP_BASE
#define GUEST_HEAP_BASE 0x01000000u
#endif
#define GUEST_HEAP_END 0x0e000000u
#define GUEST_STACK_TOP 0x0f000000u
#define GUEST_TEB_BASE 0x0fe00000u
#define GUEST_SHIM_BASE 0x0ff00000u
#define GUEST_SHIM_END 0x10000000u
#define GUEST_SHIM_STRIDE 16u
#define GUEST_RETURN_SENTINEL 0x0fdfff00u

/* Little-endian host (ARM64) matches the guest, so memcpy is a plain load. */
static inline uint8_t rd8(uint32_t a) {
    return g_mem[a];
}
static inline uint16_t rd16(uint32_t a) {
    uint16_t v;
    memcpy(&v, g_mem + a, 2);
    return v;
}
static inline uint32_t rd32(uint32_t a) {
    uint32_t v;
    memcpy(&v, g_mem + a, 4);
    return v;
}
static inline uint64_t rd64(uint32_t a) {
    uint64_t v;
    memcpy(&v, g_mem + a, 8);
    return v;
}
/* A guest-memory watchpoint.  RECOMP_WATCH=<hex address>[:<length>] reports
 * every write that touches those bytes together with the host backtrace, which
 * names the generated function and so the guest routine doing the writing.
 * This is the instrument for a stray write: the corrupted value says what the
 * damage is, never who did it, and the writer is usually nowhere near the
 * reader.  Unarmed - which is always, unless the switch is set - g_watch_len is
 * zero and a write costs one compare the branch predictor never takes. */
extern uint32_t g_watch_base;
extern uint32_t g_watch_len;
void recomp_watch_hit(uint32_t addr, uint32_t n, uint64_t value);
static inline void recomp_watch(uint32_t a, uint32_t n, uint64_t v) {
    if (g_watch_len != 0 && a < g_watch_base + g_watch_len && g_watch_base < a + n)
        recomp_watch_hit(a, n, v);
}

static inline void wr8(uint32_t a, uint8_t v) {
    g_mem[a] = v;
    recomp_watch(a, 1, v);
}
static inline void wr16(uint32_t a, uint16_t v) {
    memcpy(g_mem + a, &v, 2);
    recomp_watch(a, 2, v);
}
static inline void wr32(uint32_t a, uint32_t v) {
    memcpy(g_mem + a, &v, 4);
    recomp_watch(a, 4, v);
}
static inline void wr64(uint32_t a, uint64_t v) {
    memcpy(g_mem + a, &v, 8);
    recomp_watch(a, 8, v);
}
static inline float rdf32(uint32_t a) {
    float v;
    memcpy(&v, g_mem + a, 4);
    return v;
}
static inline double rdf64(uint32_t a) {
    double v;
    memcpy(&v, g_mem + a, 8);
    return v;
}
static inline void wrf32(uint32_t a, float v) {
    memcpy(g_mem + a, &v, 4);
}
static inline void wrf64(uint32_t a, double v) {
    memcpy(g_mem + a, &v, 8);
}

/* 80-bit x87 extended precision.  x87 registers are `double` here (plan
 * ruling), so a tbyte load rounds to 64-bit and a store re-expands. */
static inline double rdf80(uint32_t a) {
    uint64_t m = rd64(a);
    uint16_t se = rd16(a + 8);
    int exp = se & 0x7fffu;
    double v;
    if (exp == 0x7fff)
        v = (m << 1) ? (double)NAN : (double)INFINITY;
    else if (!exp && !m)
        v = 0.0;
    else
        v = ldexp((double)m, exp - 16383 - 63);
    return (se & 0x8000u) ? -v : v;
}

static inline void wrf80(uint32_t a, double v) {
    uint64_t m;
    uint16_t se;
    if (isnan(v)) {
        m = 0xc000000000000000ull;
        se = 0x7fffu;
    } else if (isinf(v)) {
        m = 0x8000000000000000ull;
        se = v < 0 ? 0xffffu : 0x7fffu;
    } else if (v == 0.0) {
        m = 0;
        se = signbit(v) ? 0x8000u : 0u;
    } else {
        int e, sign = signbit(v);
        double f = frexp(sign ? -v : v, &e); /* 0.5 <= f < 1 */
        m = (uint64_t)ldexp(f, 64);
        se = (uint16_t)((e - 1 + 16383) & 0x7fff);
        if (sign)
            se |= 0x8000u;
    }
    wr64(a, m);
    wr16(a + 8, se);
}

/* FBSTP: 18 packed BCD digits, little-endian, sign in bit 7 of byte 9. Rounds
 * to nearest even as the default control word does. Out-of-range values store
 * the BCD indefinite (0xffff c000 0000 0000 0000), as the hardware does. */
static inline void wrbcd80(uint32_t a, double v) {
    uint8_t out[10];
    memset(out, 0, sizeof out);
    double r = nearbyint(v);
    if (!(fabs(r) < 1e18)) {
        static const uint8_t indefinite[10] = {0, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0xff};
        memcpy(g_mem + a, indefinite, 10);
        return;
    }
    uint64_t m = (uint64_t)fabs(r);
    for (int i = 0; i < 9; ++i) {
        uint8_t lo = (uint8_t)(m % 10);
        m /= 10;
        uint8_t hi = (uint8_t)(m % 10);
        m /= 10;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    if (r < 0 || (r == 0 && signbit(v)))
        out[9] = 0x80;
    memcpy(g_mem + a, out, 10);
}

/* ------------------------------------------------------------- cpu state */

/* Register indices into X86.r */
enum { R_EAX = 0, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };

struct X86 {
    uint32_t r[8]; /* EAX ECX EDX EBX ESP EBP ESI EDI */
    uint32_t eip;
    uint32_t eflags_cf, eflags_zf, eflags_sf, eflags_of, eflags_pf, eflags_af, eflags_df;
    /* Every EFLAGS bit other than CF PF AF ZF SF DF OF, so PUSHFD/POPFD
     * round-trip (the CPUID probe toggles the ID bit).  Initialise to 0x202. */
    uint32_t eflags_misc;
    double st[8]; /* x87 stack, physical slots */
    /* FILD integers retain all 64 mantissa bits until arithmetic replaces
     * them. The double alone cannot preserve qword copies above 2^53. */
    uint64_t st_bits[8];
    uint8_t st_exact[8];
    uint32_t fpu_top; /* ST(i) == st[(fpu_top + i) & 7] */
    uint16_t fpu_cw, fpu_sw;
    /* x87 tag word, 2 bits per PHYSICAL register st[i]: 3 = empty, else in
     * use.  Initialise to 0xffff (the post-FINIT all-empty state). */
    uint16_t fpu_tag;
    uint32_t fs_base;
    /* SSE2. The translator models the data-movement subset only -
     * loads, stores, a dword broadcast - which is all a Delphi runtime's
     * FillChar and Move use, and byte order is then the only thing that has
     * to be right. There are eight in 32-bit mode. CPUID advertises no SSE,
     * so nothing chooses one of these paths from a feature test; the RTL
     * takes them unconditionally because every CPU it supports has SSE2.
     * Held as dwords: every modelled form - load, store, MOVD, MOVQ and
     * PSHUFD's selector - addresses the register in dword lanes, and on a
     * little-endian host the byte order of a copy then takes care of
     * itself. Lane 0 is the low four bytes. */
    uint32_t xmm[8][4];
};
typedef struct X86 X86;

/* ------------------------------------------------- runtime call-outs
 * Implemented in generated table.c (recomp_call) or by runtime/.
 */

/* Indirect CALL: dispatch `target` to a translated function, an import shim,
 * or recomp_unknown_call.  Generated into build/recomp/gen/table.c. */
void recomp_call(X86 *c, uint32_t target);

/* RECOMP_WATCH_FRAME=1 reports a guest call that returns with EBP changed.
 * A routine that loses the frame pointer corrupts nothing and crashes nowhere:
 * its caller simply reads its locals from somewhere else afterwards, and the
 * damage surfaces as a wrong value far away. Off, this costs one register copy
 * and one compare per call, both of which the optimiser folds away. */
extern uint32_t recomp_frame_watch;
void recomp_frame_changed(X86 *c, uint32_t target, uint32_t before, uint32_t after);
extern const int recomp_profile_enabled;
void recomp_profile_push(uint32_t index);
void recomp_profile_pop(void);

/* Indirect JMP or tail call: dispatch `target` through the generated
 * block-entry table (every function start plus every decoded jump-table
 * target), then the import shim range.  Anything else aborts. */
void recomp_jump(X86 *c, uint32_t target);

/* Indirect JMP whose target is neither a block entry nor a shim.  Aborts with
 * the target and the jumping instruction's address. */
void recomp_unknown_jump(X86 *c, uint32_t target);

/* Provided by runtime/ ------------------------------------- */

/* target is in [GUEST_SHIM_BASE, GUEST_SHIM_END): run import shim
 * (target - GUEST_SHIM_BASE) / GUEST_SHIM_STRIDE. */
void recomp_shim_call(X86 *c, uint32_t target);
/* target is neither a translated function nor a shim.  Logs; may abort. */
void recomp_unknown_call(X86 *c, uint32_t target);
/* A popped callback sentinel can bypass translated callers. The innermost
 * guest_call owns the live host return checkpoint and its guest stack range. */
void recomp_callback_return(X86 *c);
uint32_t recomp_callback_depth(void);
void recomp_callback_truncate(uint32_t depth);
void recomp_callback_reset(X86 *c);
/* DIV/IDIV with a zero divisor or a quotient that does not fit. */
void recomp_div_error(X86 *c, uint32_t addr);

void recomp_rdtsc(X86 *c); /* -> EDX:EAX */
void recomp_cpuid(X86 *c); /* EAX in -> EAX EBX ECX EDX */
uint32_t recomp_in(X86 *c, uint32_t port, int size);
void recomp_out(X86 *c, uint32_t port, uint32_t val, int size);
void recomp_cli(X86 *c);
void recomp_sti(X86 *c);
void recomp_hlt(X86 *c);
void recomp_int(X86 *c, uint32_t vec);
/* An instruction the translator could not model, reached at run time. The
 * translation carries a trap at that address instead of refusing the whole
 * image, because a listing routinely decodes the data past a function's last
 * instruction as code. Reaching one is fatal and says where. */
void recomp_unmodelled(X86 *c, uint32_t addr);

/* ------------------------------------------------------ hook dispatch -- */

/* Three tables, generated into build/recomp/gen/table.c:
 *   recomp_func_addrs   sorted address index, immutable
 *   recomp_base_ptrs    the original for each entry, fixed at build time
 *   recomp_hook_ptrs    mutable dispatch, initially "run the base"
 * plus recomp_hooked, the per-function flag every call site reads with an
 * ACQUIRE load. The installer publishes the pointer with a release store and
 * then the flag with a release store, so a set flag always implies a visible
 * pointer. The index is passed to the dispatcher because one C function serves
 * every hooked entry. */
typedef void (*RecompHookFn)(X86 *c, uint32_t index);

/* Sorted table of every translated entry point (function starts, alternate
 * entries and jump-table block entries), from the generated table.c. */
extern const uint32_t recomp_func_addrs[];
extern const uint32_t recomp_func_count;
extern void (*const recomp_base_ptrs[])(X86 *c);
/* The untouched translation of each entry. recomp_base_ptrs holds this build's
 * FN_<addr> override where one is defined, so the two tables differ exactly
 * over the override set - which is how a run record states what was replaced,
 * instead of trusting an environment variable read after the build. */
extern void (*const recomp_raw_ptrs[])(X86 *c);
extern RecompHookFn recomp_hook_ptrs[];
extern uint8_t recomp_hooked[];

uint32_t recomp_override_count(void);
uint64_t recomp_override_hash(void);

/* Index into the three tables, or -1 when addr is not an entry. Being an entry
 * is NOT the same as being hookable: eligibility comes from symbols.json. */
int32_t recomp_index_of(uint32_t addr);
int recomp_is_call_return(uint32_t target);

/* ------------------------------------------------ auxiliary modules -- */

/* A second guest image (a DLL the game loads by name) translated by
 * `translate.py --module <key>` into its own tables. Its table.c registers
 * the module from a constructor; the main image's recomp_call/recomp_jump
 * fall back to the registry after their own table misses. The runtime
 * loader maps the module's sections at [base, end), and LoadLibrary /
 * GetProcAddress answer from the module's export directory. */
typedef struct RecompModule {
    const char *name;
    uint32_t base, end;
    const uint32_t *func_addrs;
    uint32_t func_count;
    void (*const *base_ptrs)(X86 *c);
    RecompHookFn *hook_ptrs;
    uint8_t *hooked;
    const uint32_t *call_returns;
    uint32_t call_return_count;
    const char *const *profile_names;
} RecompModule;

void recomp_module_register(const RecompModule *m);
uint32_t recomp_module_count(void);
const RecompModule *recomp_module_at(uint32_t i);
const RecompModule *recomp_module_named(const char *name);
const RecompModule *recomp_module_containing(uint32_t addr);
/* Index into the owning module's tables, or -1 when no module has addr as an entry. */
int32_t recomp_module_lookup(uint32_t target);
int recomp_module_is_call_return(uint32_t target);
/* Dispatches target through the owning module's tables; 0 when no module owns it. */
int recomp_module_call(X86 *c, uint32_t target);

/* RET has already popped EIP and applied any immediate stack adjustment.
 * A CALL continuation belongs to the pending host caller, even when it is
 * also an alternate entry. Other entries are tail calls, as in interface
 * adapters that exchange a vtable method onto the guest stack before RET.
 * Delay-load adapters also RET into resolved import shims. Unknown returns
 * retain the existing EIP/host-return behaviour. */
static inline void recomp_return(X86 *c) {
    if (c->eip == GUEST_RETURN_SENTINEL) {
        recomp_callback_return(c);
        return;
    }
    if (recomp_is_call_return(c->eip) || recomp_module_is_call_return(c->eip))
        return;
    if ((c->eip >= GUEST_SHIM_BASE && c->eip < GUEST_SHIM_END) || recomp_index_of(c->eip) >= 0 ||
        recomp_module_lookup(c->eip) >= 0)
        recomp_call(c, c->eip);
}

/* --------------------------------------------------------------- flags */

static inline uint32_t parity8(uint32_t v) {
    v &= 0xffu;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v) & 1u;
}

/* Bits x86_get_eflags/x86_set_eflags build from the individual fields; every
 * other bit lives in eflags_misc so PUSHFD/POPFD is lossless. */
#define X86_EFLAGS_SPLIT 0x00000cd5u /* CF PF AF ZF SF DF OF */

static inline uint32_t x86_get_eflags(const X86 *c) {
    return (c->eflags_misc & ~X86_EFLAGS_SPLIT) | 0x00000002u /* reserved bit 1 reads as 1 */
           | (c->eflags_cf << 0) | (c->eflags_pf << 2) | (c->eflags_af << 4) | (c->eflags_zf << 6) |
           (c->eflags_sf << 7) | (c->eflags_df << 10) | (c->eflags_of << 11);
}

static inline void x86_set_eflags(X86 *c, uint32_t v) {
    c->eflags_misc = (v & ~X86_EFLAGS_SPLIT) | 0x00000002u;
    c->eflags_cf = (v >> 0) & 1u;
    c->eflags_pf = (v >> 2) & 1u;
    c->eflags_af = (v >> 4) & 1u;
    c->eflags_zf = (v >> 6) & 1u;
    c->eflags_sf = (v >> 7) & 1u;
    c->eflags_df = (v >> 10) & 1u;
    c->eflags_of = (v >> 11) & 1u;
}

/* SAHF: AH -> SF ZF AF PF CF */
static inline void x86_sahf(X86 *c) {
    uint32_t ah = (c->r[R_EAX] >> 8) & 0xffu;
    c->eflags_cf = (ah >> 0) & 1u;
    c->eflags_pf = (ah >> 2) & 1u;
    c->eflags_af = (ah >> 4) & 1u;
    c->eflags_zf = (ah >> 6) & 1u;
    c->eflags_sf = (ah >> 7) & 1u;
}

/* ------------------------------------------------------ shifts / rotates
 * The `_f` variants also write the flags the instruction defines.  A shift
 * or rotate with a zero count leaves every flag untouched, which is why
 * these are functions rather than inline expressions.
 */
/* Every shift is done on uint32_t: a uint8_t or uint16_t operand would
 * otherwise promote to signed int and `(uint16_t)0xffff << 16` is UB even
 * though the count is masked.  SAR shifts the sign-extended value, and a
 * count >= BITS saturates to BITS-1 so the arithmetic shift stays defined. */
#define SHIFT_OPS(BITS, UT, ST)                                                                    \
    static inline UT shl##BITS(UT v, uint32_t n) {                                                 \
        return (UT)((uint32_t)v << (n & 31u));                                                     \
    }                                                                                              \
    static inline UT shr##BITS(UT v, uint32_t n) {                                                 \
        return (UT)((uint32_t)v >> (n & 31u));                                                     \
    }                                                                                              \
    static inline UT sar##BITS(UT v, uint32_t n) {                                                 \
        uint32_t k = n & 31u;                                                                      \
        if (k >= BITS)                                                                             \
            k = BITS - 1;                                                                          \
        return (UT)((int32_t)(ST)v >> k);                                                          \
    }                                                                                              \
    static inline UT shl##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = n & 31u;                                                                    \
        UT r;                                                                                      \
        if (!cnt)                                                                                  \
            return v;                                                                              \
        if (cnt <= BITS)                                                                           \
            c->eflags_cf = (uint32_t)(((uint32_t)v >> (BITS - cnt)) & 1u);                         \
        else                                                                                       \
            c->eflags_cf = 0;                                                                      \
        r = (UT)(cnt >= BITS ? 0u : ((uint32_t)v << cnt));                                         \
        c->eflags_of = ((uint32_t)(r >> (BITS - 1)) & 1u) ^ c->eflags_cf;                          \
        c->eflags_zf = (r == 0);                                                                   \
        c->eflags_sf = (uint32_t)(r >> (BITS - 1)) & 1u;                                           \
        c->eflags_pf = parity8((uint32_t)r);                                                       \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT shr##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = n & 31u;                                                                    \
        UT r;                                                                                      \
        if (!cnt)                                                                                  \
            return v;                                                                              \
        c->eflags_cf = cnt <= BITS ? (uint32_t)(((uint32_t)v >> (cnt - 1)) & 1u) : 0u;             \
        r = (UT)(cnt >= BITS ? 0u : ((uint32_t)v >> cnt));                                         \
        c->eflags_of = (uint32_t)(v >> (BITS - 1)) & 1u;                                           \
        c->eflags_zf = (r == 0);                                                                   \
        c->eflags_sf = (uint32_t)(r >> (BITS - 1)) & 1u;                                           \
        c->eflags_pf = parity8((uint32_t)r);                                                       \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT sar##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = n & 31u;                                                                    \
        UT r;                                                                                      \
        if (!cnt)                                                                                  \
            return v;                                                                              \
        if (cnt >= BITS)                                                                           \
            cnt = BITS;                                                                            \
        c->eflags_cf = (uint32_t)(((int32_t)(ST)v >> (cnt - 1)) & 1);                              \
        r = (UT)((int32_t)(ST)v >> (cnt == BITS ? BITS - 1 : cnt));                                \
        c->eflags_of = 0;                                                                          \
        c->eflags_zf = (r == 0);                                                                   \
        c->eflags_sf = (uint32_t)(r >> (BITS - 1)) & 1u;                                           \
        c->eflags_pf = parity8((uint32_t)r);                                                       \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT rol##BITS(UT v, uint32_t n) {                                                 \
        uint32_t cnt = (n & 31u) % BITS, w = (uint32_t)v;                                          \
        return cnt ? (UT)((w << cnt) | (w >> (BITS - cnt))) : v;                                   \
    }                                                                                              \
    static inline UT ror##BITS(UT v, uint32_t n) {                                                 \
        uint32_t cnt = (n & 31u) % BITS, w = (uint32_t)v;                                          \
        return cnt ? (UT)((w >> cnt) | (w << (BITS - cnt))) : v;                                   \
    }                                                                                              \
    static inline UT rol##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        UT r;                                                                                      \
        if (!(n & 31u))                                                                            \
            return v;                                                                              \
        r = rol##BITS(v, n);                                                                       \
        c->eflags_cf = (uint32_t)r & 1u;                                                           \
        c->eflags_of = ((uint32_t)(r >> (BITS - 1)) & 1u) ^ c->eflags_cf;                          \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT ror##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        UT r;                                                                                      \
        if (!(n & 31u))                                                                            \
            return v;                                                                              \
        r = ror##BITS(v, n);                                                                       \
        c->eflags_cf = (uint32_t)(r >> (BITS - 1)) & 1u;                                           \
        c->eflags_of = c->eflags_cf ^ ((uint32_t)(r >> (BITS - 2)) & 1u);                          \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT rcl##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = (n & 31u) % (BITS + 1), i;                                                  \
        for (i = 0; i < cnt; i++) {                                                                \
            uint32_t hi = (uint32_t)(v >> (BITS - 1)) & 1u;                                        \
            v = (UT)((v << 1) | c->eflags_cf);                                                     \
            c->eflags_cf = hi;                                                                     \
        }                                                                                          \
        if (n & 31u)                                                                               \
            c->eflags_of = ((uint32_t)(v >> (BITS - 1)) & 1u) ^ c->eflags_cf;                      \
        return v;                                                                                  \
    }                                                                                              \
    static inline UT rcr##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = (n & 31u) % (BITS + 1), i;                                                  \
        if (n & 31u)                                                                               \
            c->eflags_of = ((uint32_t)(v >> (BITS - 1)) & 1u) ^ c->eflags_cf;                      \
        for (i = 0; i < cnt; i++) {                                                                \
            uint32_t lo = (uint32_t)v & 1u;                                                        \
            v = (UT)(((uint32_t)v >> 1) | (c->eflags_cf << (BITS - 1)));                           \
            c->eflags_cf = lo;                                                                     \
        }                                                                                          \
        return v;                                                                                  \
    }
SHIFT_OPS(8, uint8_t, int8_t)
SHIFT_OPS(16, uint16_t, int16_t)
SHIFT_OPS(32, uint32_t, int32_t)
#undef SHIFT_OPS

/* SHLD/SHRD (32-bit forms only; the corpus has no 16-bit double shift). */
static inline uint32_t shld32_f(X86 *c, uint32_t d, uint32_t s, uint32_t n) {
    uint32_t cnt = n & 31u, r;
    if (!cnt)
        return d;
    r = (d << cnt) | (s >> (32 - cnt));
    c->eflags_cf = (d >> (32 - cnt)) & 1u;
    c->eflags_of = ((r >> 31) & 1u) ^ c->eflags_cf;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
    return r;
}
static inline uint32_t shrd32_f(X86 *c, uint32_t d, uint32_t s, uint32_t n) {
    uint32_t cnt = n & 31u, r;
    if (!cnt)
        return d;
    r = (d >> cnt) | (s << (32 - cnt));
    c->eflags_cf = (d >> (cnt - 1)) & 1u;
    c->eflags_of = ((r >> 31) & 1u) ^ ((d >> 31) & 1u);
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
    return r;
}

/* ------------------------------------------------------ multiply / divide */

static inline void mul8(X86 *c, uint8_t s) {
    uint32_t p = (uint32_t)(uint8_t)c->r[R_EAX] * s;
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | (p & 0xffffu);
    c->eflags_cf = c->eflags_of = ((p >> 8) != 0);
}
static inline void mul16(X86 *c, uint16_t s) {
    uint32_t p = (uint32_t)(uint16_t)c->r[R_EAX] * s;
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | (p & 0xffffu);
    c->r[R_EDX] = (c->r[R_EDX] & 0xffff0000u) | ((p >> 16) & 0xffffu);
    c->eflags_cf = c->eflags_of = ((p >> 16) != 0);
}
static inline void mul32(X86 *c, uint32_t s) {
    uint64_t p = (uint64_t)c->r[R_EAX] * s;
    c->r[R_EAX] = (uint32_t)p;
    c->r[R_EDX] = (uint32_t)(p >> 32);
    c->eflags_cf = c->eflags_of = (c->r[R_EDX] != 0);
}

static inline void imul8(X86 *c, uint8_t s) {
    int32_t p = (int32_t)(int8_t)c->r[R_EAX] * (int8_t)s;
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | ((uint32_t)p & 0xffffu);
    c->eflags_cf = c->eflags_of = (p != (int8_t)p);
}
static inline void imul16(X86 *c, uint16_t s) {
    int32_t p = (int32_t)(int16_t)c->r[R_EAX] * (int16_t)s;
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | ((uint32_t)p & 0xffffu);
    c->r[R_EDX] = (c->r[R_EDX] & 0xffff0000u) | (((uint32_t)p >> 16) & 0xffffu);
    c->eflags_cf = c->eflags_of = (p != (int16_t)p);
}
static inline void imul32(X86 *c, uint32_t s) {
    int64_t p = (int64_t)(int32_t)c->r[R_EAX] * (int32_t)s;
    c->r[R_EAX] = (uint32_t)p;
    c->r[R_EDX] = (uint32_t)((uint64_t)p >> 32);
    c->eflags_cf = c->eflags_of = (p != (int32_t)p);
}

/* Two/three-operand IMUL: value is the truncated product; CF/OF on overflow. */
/* The low half of a signed product is the low half of the unsigned product;
 * computing it unsigned keeps the wraparound defined. */
static inline uint32_t imul2_32(uint32_t a, uint32_t b) {
    return a * b;
}
static inline uint32_t imul2_32_f(X86 *c, uint32_t a, uint32_t b) {
    int64_t p = (int64_t)(int32_t)a * (int32_t)b;
    c->eflags_cf = c->eflags_of = (p != (int32_t)p);
    return (uint32_t)p;
}
static inline uint16_t imul2_16(uint16_t a, uint16_t b) {
    return (uint16_t)((uint32_t)a * b);
}
static inline uint16_t imul2_16_f(X86 *c, uint16_t a, uint16_t b) {
    int32_t p = (int32_t)(int16_t)a * (int16_t)b;
    c->eflags_cf = c->eflags_of = (p != (int16_t)p);
    return (uint16_t)p;
}

static inline void div8(X86 *c, uint8_t s, uint32_t at) {
    uint32_t n = (uint16_t)c->r[R_EAX], q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    q = n / s;
    if (q > 0xffu) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | (q & 0xffu) | ((n % s) << 8);
}
static inline void div16(X86 *c, uint16_t s, uint32_t at) {
    uint32_t n = ((uint32_t)(uint16_t)c->r[R_EDX] << 16) | (uint16_t)c->r[R_EAX], q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    q = n / s;
    if (q > 0xffffu) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | q;
    c->r[R_EDX] = (c->r[R_EDX] & 0xffff0000u) | (n % s);
}
static inline void div32(X86 *c, uint32_t s, uint32_t at) {
    uint64_t n = ((uint64_t)c->r[R_EDX] << 32) | c->r[R_EAX], q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    q = n / s;
    if (q > 0xffffffffu) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (uint32_t)q;
    c->r[R_EDX] = (uint32_t)(n % s);
}

static inline void idiv8(X86 *c, uint8_t s, uint32_t at) {
    int32_t n = (int16_t)c->r[R_EAX], q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    if ((int8_t)s == -1 && n == INT32_MIN) {
        recomp_div_error(c, at);
        return;
    }
    q = n / (int8_t)s;
    if (q != (int8_t)q) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | ((uint32_t)q & 0xffu) |
                  (((uint32_t)(n % (int8_t)s) & 0xffu) << 8);
}
static inline void idiv16(X86 *c, uint16_t s, uint32_t at) {
    int32_t n = (int32_t)(((uint32_t)(uint16_t)c->r[R_EDX] << 16) | (uint16_t)c->r[R_EAX]), q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    if ((int16_t)s == -1 && n == INT32_MIN) {
        recomp_div_error(c, at);
        return;
    }
    q = n / (int16_t)s;
    if (q != (int16_t)q) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | ((uint32_t)q & 0xffffu);
    c->r[R_EDX] = (c->r[R_EDX] & 0xffff0000u) | ((uint32_t)(n % (int16_t)s) & 0xffffu);
}
static inline void idiv32(X86 *c, uint32_t s, uint32_t at) {
    int64_t n = (int64_t)(((uint64_t)c->r[R_EDX] << 32) | c->r[R_EAX]), q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    if ((int32_t)s == -1 && n == INT64_MIN) {
        recomp_div_error(c, at);
        return;
    }
    q = n / (int32_t)s;
    if (q != (int32_t)q) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (uint32_t)q;
    c->r[R_EDX] = (uint32_t)(n % (int32_t)s);
}

/* -------------------------------------------------------------- bit scan */

/* With a zero source the destination is architecturally undefined; unicorn,
 * like AMD, leaves it unchanged, so the old value is passed in. */
static inline uint32_t bsr32_f(X86 *c, uint32_t old, uint32_t v) {
    c->eflags_zf = (v == 0);
    return v ? 31u - (uint32_t)__builtin_clz(v) : old;
}
static inline uint32_t bsf32_f(X86 *c, uint32_t old, uint32_t v) {
    c->eflags_zf = (v == 0);
    return v ? (uint32_t)__builtin_ctz(v) : old;
}

/* ---------------------------------------------------------- push and pop */

static inline void x86_pushad(X86 *c) {
    uint32_t sp = c->r[R_ESP];
    c->r[R_ESP] -= 32;
    wr32(sp - 4, c->r[R_EAX]);
    wr32(sp - 8, c->r[R_ECX]);
    wr32(sp - 12, c->r[R_EDX]);
    wr32(sp - 16, c->r[R_EBX]);
    wr32(sp - 20, sp);
    wr32(sp - 24, c->r[R_EBP]);
    wr32(sp - 28, c->r[R_ESI]);
    wr32(sp - 32, c->r[R_EDI]);
}
static inline void x86_popad(X86 *c) {
    uint32_t sp = c->r[R_ESP];
    c->r[R_EDI] = rd32(sp + 0);
    c->r[R_ESI] = rd32(sp + 4);
    c->r[R_EBP] = rd32(sp + 8); /* skip saved ESP at sp+12 */
    c->r[R_EBX] = rd32(sp + 16);
    c->r[R_EDX] = rd32(sp + 20);
    c->r[R_ECX] = rd32(sp + 24);
    c->r[R_EAX] = rd32(sp + 28);
    c->r[R_ESP] = sp + 32;
}

/* ---------------------------------------------------------- string ops
 * DF selects the step direction.  The REP forms use ECX as the counter.
 */
#define STRING_OPS(SUF, BITS, UT, RD, WR)                                                          \
    static inline void stos##SUF(X86 *c) {                                                         \
        WR(c->r[R_EDI], (UT)c->r[R_EAX]);                                                          \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void rep_stos##SUF(X86 *c) {                                                     \
        while (c->r[R_ECX]) {                                                                      \
            stos##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
        }                                                                                          \
    }                                                                                              \
    static inline void movs##SUF(X86 *c) {                                                         \
        WR(c->r[R_EDI], RD(c->r[R_ESI]));                                                          \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
        c->r[R_ESI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void rep_movs##SUF(X86 *c) {                                                     \
        while (c->r[R_ECX]) {                                                                      \
            movs##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
        }                                                                                          \
    }                                                                                              \
    /* The port string forms. A user-mode guest that reaches one has been      \
     * misdecoded - Windows would fault - so they exist to be translatable      \
     * rather than useful: the port shims answer, and the pointer and count     \
     * advance exactly as the other string forms do. */                   \
    static inline void ins##SUF(X86 *c) {                                                          \
        WR(c->r[R_EDI], (UT)recomp_in(c, c->r[R_EDX] & 0xffffu, BITS / 8));                        \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void rep_ins##SUF(X86 *c) {                                                      \
        while (c->r[R_ECX]) {                                                                      \
            ins##SUF(c);                                                                           \
            c->r[R_ECX]--;                                                                         \
        }                                                                                          \
    }                                                                                              \
    static inline void outs##SUF(X86 *c) {                                                         \
        recomp_out(c, c->r[R_EDX] & 0xffffu, RD(c->r[R_ESI]), BITS / 8);                           \
        c->r[R_ESI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void rep_outs##SUF(X86 *c) {                                                     \
        while (c->r[R_ECX]) {                                                                      \
            outs##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
        }                                                                                          \
    }                                                                                              \
    static inline void lods##SUF(X86 *c) {                                                         \
        UT v = RD(c->r[R_ESI]);                                                                    \
        if (BITS == 32)                                                                            \
            c->r[R_EAX] = (uint32_t)v;                                                             \
        else                                                                                       \
            c->r[R_EAX] = (c->r[R_EAX] & ~(uint32_t)((1ull << BITS) - 1)) |                        \
                          ((uint32_t)v & (uint32_t)((1ull << BITS) - 1));                          \
        c->r[R_ESI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }
STRING_OPS(b, 8, uint8_t, rd8, wr8)
STRING_OPS(w, 16, uint16_t, rd16, wr16)
STRING_OPS(d, 32, uint32_t, rd32, wr32)
#undef STRING_OPS

/* SCAS/CMPS set the flags of a SUB.  Defined out of line below the generic
 * SUB flag helper. */
static inline void x86_sub_flags8(X86 *c, uint32_t a, uint32_t b);
static inline void x86_sub_flags16(X86 *c, uint32_t a, uint32_t b);
static inline void x86_sub_flags32(X86 *c, uint32_t a, uint32_t b);

static inline void x86_sub_flags8(X86 *c, uint32_t a, uint32_t b) {
    uint32_t r = (a - b) & 0xffu;
    c->eflags_cf = ((a & 0xffu) < (b & 0xffu));
    c->eflags_of = (((a ^ b) & (a ^ r)) >> 7) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 7) & 1u;
    c->eflags_pf = parity8(r);
}
static inline void x86_sub_flags16(X86 *c, uint32_t a, uint32_t b) {
    uint32_t r = (a - b) & 0xffffu;
    c->eflags_cf = ((a & 0xffffu) < (b & 0xffffu));
    c->eflags_of = (((a ^ b) & (a ^ r)) >> 15) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 15) & 1u;
    c->eflags_pf = parity8(r);
}
static inline void x86_sub_flags32(X86 *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->eflags_cf = (a < b);
    c->eflags_of = (((a ^ b) & (a ^ r)) >> 31) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
}

static inline void scasb(X86 *c) {
    x86_sub_flags8(c, c->r[R_EAX] & 0xffu, rd8(c->r[R_EDI]));
    c->r[R_EDI] += c->eflags_df ? (uint32_t)-1 : 1u;
}
static inline void repne_scasb(X86 *c) {
    while (c->r[R_ECX]) {
        scasb(c);
        c->r[R_ECX]--;
        if (c->eflags_zf)
            break;
    }
}
static inline void repe_scasb(X86 *c) {
    while (c->r[R_ECX]) {
        scasb(c);
        c->r[R_ECX]--;
        if (!c->eflags_zf)
            break;
    }
}
static inline void cmpsb(X86 *c) {
    x86_sub_flags8(c, rd8(c->r[R_ESI]), rd8(c->r[R_EDI]));
    c->r[R_EDI] += c->eflags_df ? (uint32_t)-1 : 1u;
    c->r[R_ESI] += c->eflags_df ? (uint32_t)-1 : 1u;
}
static inline void repe_cmpsb(X86 *c) {
    while (c->r[R_ECX]) {
        cmpsb(c);
        c->r[R_ECX]--;
        if (!c->eflags_zf)
            break;
    }
}
static inline void repne_cmpsb(X86 *c) {
    while (c->r[R_ECX]) {
        cmpsb(c);
        c->r[R_ECX]--;
        if (c->eflags_zf)
            break;
    }
}

/* The same at word and dword width.  A REP-prefixed compare with ECX = 0
 * runs nothing and leaves every flag as it found it, like the byte forms. */
#define X86_CMPS_SCAS(SUF, BITS, MASK)                                                             \
    static inline void scas##SUF(X86 *c) {                                                         \
        x86_sub_flags##BITS(c, c->r[R_EAX] & (MASK), rd##BITS(c->r[R_EDI]));                       \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void cmps##SUF(X86 *c) {                                                         \
        x86_sub_flags##BITS(c, rd##BITS(c->r[R_ESI]), rd##BITS(c->r[R_EDI]));                      \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
        c->r[R_ESI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void repe_scas##SUF(X86 *c) {                                                    \
        while (c->r[R_ECX]) {                                                                      \
            scas##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
            if (!c->eflags_zf)                                                                     \
                break;                                                                             \
        }                                                                                          \
    }                                                                                              \
    static inline void repne_scas##SUF(X86 *c) {                                                   \
        while (c->r[R_ECX]) {                                                                      \
            scas##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
            if (c->eflags_zf)                                                                      \
                break;                                                                             \
        }                                                                                          \
    }                                                                                              \
    static inline void repe_cmps##SUF(X86 *c) {                                                    \
        while (c->r[R_ECX]) {                                                                      \
            cmps##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
            if (!c->eflags_zf)                                                                     \
                break;                                                                             \
        }                                                                                          \
    }                                                                                              \
    static inline void repne_cmps##SUF(X86 *c) {                                                   \
        while (c->r[R_ECX]) {                                                                      \
            cmps##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
            if (c->eflags_zf)                                                                      \
                break;                                                                             \
        }                                                                                          \
    }
X86_CMPS_SCAS(w, 16, 0xffffu)
X86_CMPS_SCAS(d, 32, 0xffffffffu)
#undef X86_CMPS_SCAS

/* -------------------------------------------------------------- x87 -----
 * st[] holds doubles.  ST(i) is st[(fpu_top + i) & 7]; a push predecrements
 * fpu_top.  Status-word bits: C0=8 C1=9 C2=10 TOP=11..13 C3=14.
 */
#define ST(c, i) ((c)->st[((c)->fpu_top + (unsigned)(i)) & 7u])

#define FTAG_VALID 0u
#define FTAG_ZERO 1u
#define FTAG_SPECIAL 2u /* NaN, infinity, denormal, unsupported */
#define FTAG_EMPTY 3u

/* The tag describes the register as x87 sees it, which is the 80-bit value.
 * A subnormal double is a NORMAL extended - the extended exponent range
 * reaches 2^-16382, far below 2^-1074 - so it tags valid, not special.  An
 * 80-bit subnormal cannot occur at all while the registers are doubles. */
static inline unsigned ftag_classify(double v) {
    /* The runtime already represents x87 registers as IEEE binary64. Read
     * the representation without aliasing or floating-point operations:
     * tagging every FPU write must not call libm or quiet a signaling NaN. */
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    bits &= UINT64_C(0x7fffffffffffffff);
    if (!bits)
        return FTAG_ZERO;
    if (bits >= UINT64_C(0x7ff0000000000000))
        return FTAG_SPECIAL;
    return FTAG_VALID; /* NORMAL and SUBNORMAL */
}

static inline unsigned ftag_of(const X86 *c, unsigned phys) {
    return (c->fpu_tag >> (2u * (phys & 7u))) & 3u;
}
static inline void ftag_put(X86 *c, unsigned phys, unsigned tag) {
    unsigned sh = 2u * (phys & 7u);
    c->fpu_tag = (uint16_t)((c->fpu_tag & ~(3u << sh)) | (tag << sh));
}
/* ST(i) is empty (nothing has been pushed into it since the last FINIT). */
static inline unsigned fempty(const X86 *c, unsigned i) {
    return ftag_of(c, (c->fpu_top + i) & 7u) == FTAG_EMPTY;
}

static inline void fpush(X86 *c, double v) {
    c->fpu_top = (c->fpu_top - 1u) & 7u;
    c->st[c->fpu_top] = v;
    c->st_bits[c->fpu_top] = 0;
    c->st_exact[c->fpu_top] = 0;
    ftag_put(c, c->fpu_top, ftag_classify(v));
}
static inline void fpush_int(X86 *c, int64_t v) {
    fpush(c, (double)v);
    c->st_bits[c->fpu_top] = (uint64_t)v;
    c->st_exact[c->fpu_top] = 1;
}
/* FLD ST(i) captures the source before pushing, including the wraparound
 * case where the destination is the same physical slot. */
static inline void fpush_st(X86 *c, unsigned i) {
    unsigned src = (c->fpu_top + i) & 7u;
    uint64_t bits = c->st_bits[src];
    uint8_t exact = c->st_exact[src];
    unsigned tag = ftag_of(c, src);
    fpush(c, c->st[src]);
    c->st_bits[c->fpu_top] = bits;
    c->st_exact[c->fpu_top] = exact;
    ftag_put(c, c->fpu_top, tag);
}
static inline double fpop(X86 *c) {
    double v = c->st[c->fpu_top];
    c->st_exact[c->fpu_top] = 0;
    ftag_put(c, c->fpu_top, FTAG_EMPTY);
    c->fpu_top = (c->fpu_top + 1u) & 7u;
    return v;
}
static inline void fdrop(X86 *c) {
    c->st_exact[c->fpu_top] = 0;
    ftag_put(c, c->fpu_top, FTAG_EMPTY);
    c->fpu_top = (c->fpu_top + 1u) & 7u;
}

/* Assign ST(i) and retag it.  Every write to an x87 register goes through
 * this, so an arithmetic result that turns out to be zero, a NaN, an infinity
 * or a denormal leaves the tag word describing what the register now holds
 * rather than what it held before. */
static inline void fset(X86 *c, unsigned i, double v) {
    unsigned phys = (c->fpu_top + i) & 7u;
    c->st[phys] = v;
    c->st_bits[phys] = 0;
    c->st_exact[phys] = 0;
    ftag_put(c, phys, ftag_classify(v));
}

/* Register moves preserve the integer representation as well as the tag. */
static inline void fcopy(X86 *c, unsigned dst, unsigned src) {
    unsigned a = (c->fpu_top + dst) & 7u, b = (c->fpu_top + src) & 7u;
    c->st[a] = c->st[b];
    c->st_bits[a] = c->st_bits[b];
    c->st_exact[a] = c->st_exact[b];
    ftag_put(c, a, ftag_of(c, b));
}

/* FXCH: the tags and exact integers travel with the values. */
static inline void fxch(X86 *c, unsigned i) {
    unsigned a = c->fpu_top & 7u, b = (c->fpu_top + i) & 7u;
    double v = c->st[a];
    unsigned t = ftag_of(c, a);
    uint64_t bits = c->st_bits[a];
    uint8_t exact = c->st_exact[a];
    c->st[a] = c->st[b];
    c->st_bits[a] = c->st_bits[b];
    c->st_exact[a] = c->st_exact[b];
    ftag_put(c, a, ftag_of(c, b));
    c->st[b] = v;
    c->st_bits[b] = bits;
    c->st_exact[b] = exact;
    ftag_put(c, b, t);
}

/* Game code compares stored float bits, so the NaN a masked invalid x87
 * operation produces has to be one fixed pattern rather than whatever the
 * host happened to carry in.  x87 silicon yields the negative QNaN
 * "indefinite" (0xfff8000000000000, or 0xffc00000 stored as a float) and
 * ARM64 yields a positive one, so normalise to the x86 pattern.  Unicorn's
 * softfloat is not self-consistent about this sign, which is why two of the
 * sweep functions are reported as known NaN-payload divergences. */
static inline double x87_indefinite(void) {
    uint64_t bits = 0xfff8000000000000ull;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
/* An x87 result that is not affected by precision control: the invalid
 * -operation NaN is still canonicalised, and the invalid flag is raised, but
 * the value keeps the register's full precision.  FRNDINT, the transcendental
 * instructions and FXCH all land here; per the SDM, PC applies only to
 * FADD/FSUB/FMUL/FDIV (and their integer and popping forms) and FSQRT. */
/* x87 raises #Z when a finite non-zero dividend meets a zero divisor.  This
 * is the one exception besides IE that the parity oracle also reports, so
 * modelling it keeps the exemption down to IE alone. */
static inline double fdivz(X86 *c, double a, double b) {
    if (b == 0.0 && a == a && !isinf(a) && a != 0.0)
        c->fpu_sw |= 0x0004u;
    return a / b;
}

static inline double fx87_exact(X86 *c, double r) {
    if (r == r)
        return r;
    c->fpu_sw |= 0x0001u; /* IE: invalid operation */
    return x87_indefinite();
}

/* An x87 result from one of the basic arithmetic instructions, which do
 * observe the precision-control field.  PC=00 rounds to single; PC=10
 * (53-bit) and PC=11 (64-bit) both land on the double we store, 64-bit
 * approximated by 53 per the plan ruling. */
static inline double fx87(X86 *c, double r) {
    if (r != r) {
        c->fpu_sw |= 0x0001u;
        return x87_indefinite();
    }
    if (((c->fpu_cw >> 8) & 3u) == 0u)
        return (double)(float)r;
    return r;
}

/* A signalling NaN has the quiet bit (mantissa MSB) clear. */
static inline int is_snan(double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    return ((b >> 52) & 0x7ffu) == 0x7ffu && (b & 0x000fffffffffffffull) != 0 &&
           ((b >> 51) & 1u) == 0;
}

/* FCOM/FUCOM: C3 C2 C0 = ZF PF CF of the comparison.  They differ only in
 * which NaNs raise the invalid-operation exception: FCOM raises on any NaN,
 * FUCOM only on a signalling one. */
static inline void fcom_common(X86 *c, double a, double b, int quiet) {
    uint16_t sw = (uint16_t)(c->fpu_sw & (uint16_t)~0x4700u);
    if (isnan(a) || isnan(b)) {
        sw |= 0x4500u; /* unordered: C3 C2 C0 */
        if (!quiet || is_snan(a) || is_snan(b))
            sw |= 0x0001u; /* IE */
    } else if (a < b)
        sw |= 0x0100u; /* C0 */
    else if (a == b)
        sw |= 0x4000u; /* C3 */
    c->fpu_sw = sw;
}
static inline void fcom(X86 *c, double a, double b) {
    fcom_common(c, a, b, 0);
}
static inline void fucom(X86 *c, double a, double b) {
    fcom_common(c, a, b, 1);
}
/* FCOMI/FUCOMI report into EFLAGS instead of the status word, with the same
 * split over which NaNs are quiet. */
static inline void fcomi_common(X86 *c, double a, double b, int quiet) {
    c->eflags_of = c->eflags_sf = c->eflags_af = 0;
    if (isnan(a) || isnan(b)) {
        c->eflags_zf = c->eflags_pf = c->eflags_cf = 1;
        if (!quiet || is_snan(a) || is_snan(b))
            c->fpu_sw |= 0x0001u;
    } else {
        c->eflags_pf = 0;
        c->eflags_zf = (a == b);
        c->eflags_cf = (a < b);
    }
}
static inline void fcomi(X86 *c, double a, double b) {
    fcomi_common(c, a, b, 0);
}
static inline void fucomi(X86 *c, double a, double b) {
    fcomi_common(c, a, b, 1);
}

/* FNSTSW/FSTSW: the architectural status word, with TOP injected. */
static inline uint16_t fstsw(const X86 *c) {
    return (uint16_t)((c->fpu_sw & (uint16_t)~0x3800u) | (uint16_t)(c->fpu_top << 11));
}
/* FXAM.  C1 is the sign bit (of a NaN too), and C3 C2 C0 encode the class:
 *   NaN 001, infinity 011, normal 010, zero 100, empty 101, denormal 110. */
static inline void fxam(X86 *c) {
    uint16_t sw = (uint16_t)(c->fpu_sw & (uint16_t)~0x4700u);
    double v = ST(c, 0);
    if (signbit(v))
        sw |= 0x0200u; /* C1 = sign */
    if (fempty(c, 0))
        sw |= 0x4100u; /* C3 C0 */
    /* Classified as the 80-bit register would be: a subnormal double is a
     * normal extended, so the denormal encoding C3 C2 = 1 1 is unreachable
     * while the registers are doubles. */
    else
        switch (fpclassify(v)) {
        case FP_NAN:
            sw |= 0x0100u;
            break; /* C0 */
        case FP_INFINITE:
            sw |= 0x0500u;
            break; /* C2 C0 */
        case FP_ZERO:
            sw |= 0x4000u;
            break; /* C3 */
        default:
            sw |= 0x0400u;
            break; /* C2, normal */
        }
    c->fpu_sw = sw;
}

/* Round per the control word's RC field (bits 10..11).
 *
 * Every mode is computed explicitly, so the result depends only on fpu_cw and
 * never on the host's ambient rounding mode.  That matters because the guest
 * control word can be restored by paths this header does not control (a
 * _longjmp reinstating a saved X86, for one), which would silently leave an
 * fesetround-based implementation rounding the wrong way.
 *
 * Nearest is ties-to-even, computed from floor and ceil rather than
 * `floor(v + 0.5)`: that addition itself rounds, and turns the exactly
 * representable 4503599627370497 into ...498.  For |v| >= 2^52 floor and ceil
 * are both v, so the tie branch returns v unchanged.
 */
static inline double fround_cw(const X86 *c, double v) {
    double lo, hi, dlo, dhi;
    switch ((c->fpu_cw >> 10) & 3u) {
    case 1:
        return floor(v);
    case 2:
        return ceil(v);
    case 3:
        return trunc(v);
    default:
        lo = floor(v);
        hi = ceil(v);
        dlo = v - lo;
        dhi = hi - v;
        if (dlo < dhi)
            return lo;
        if (dlo > dhi)
            return hi;
        return fmod(lo, 2.0) == 0.0 ? lo : hi; /* tie: to even */
    }
}

/* FLDCW.  Nothing outside fpu_cw has to change: rounding reads the field
 * directly.  Arithmetic and float stores are the one place the host's own
 * nearest-even rounding is used for the significand, which is exact whenever
 * RC = 00; this binary only ever leaves RC = 00 across the CRT's ftol, whose
 * window between the two FLDCWs contains a single FISTP and no arithmetic. */
static inline void x87_set_cw(X86 *c, uint16_t cw) {
    c->fpu_cw = cw;
}

/* FINIT/FNINIT: the post-reset state - control word 0x037f (round to nearest,
 * extended precision, every exception masked), status clear, TOP = 0, every
 * register tagged empty.  The register values themselves are left alone, as
 * on hardware; the tags make them unreadable. */
static inline void x87_finit(X86 *c) {
    x87_set_cw(c, 0x037fu);
    c->fpu_sw = 0;
    c->fpu_top = 0;
    c->fpu_tag = 0xffffu;
    memset(c->st_exact, 0, sizeof c->st_exact);
}

/* FNSAVE m108: the 32-bit protected-mode layout.  Control, status and tag
 * words each in their own dword, then the four exception pointers (FIP, FCS
 * with the opcode, FDP, FDS), which this model does not track and writes as
 * zero, then ST(0) through ST(7) as 80-bit values in stack order.  FNSAVE
 * then reinitialises the FPU, which is why the CRT pairs it with FRSTOR. */
static inline void x87_fnsave(X86 *c, uint32_t a) {
    unsigned i;
    wr32(a, c->fpu_cw);
    wr32(a + 4, fstsw(c));
    wr32(a + 8, c->fpu_tag);
    wr32(a + 12, 0);
    wr32(a + 16, 0);
    wr32(a + 20, 0);
    wr32(a + 24, 0);
    for (i = 0; i < 8; i++)
        wrf80(a + 28 + 10 * i, ST(c, i));
    x87_finit(c);
}

/* FRSTOR m108: the inverse.  TOP comes out of the saved status word and the
 * eight registers go back into the physical slots that TOP implies, so a
 * later ST(i) reads what FNSAVE wrote as ST(i). */
static inline void x87_frstor(X86 *c, uint32_t a) {
    unsigned i;
    uint16_t sw = rd16(a + 4);
    x87_set_cw(c, rd16(a));
    c->fpu_top = (sw >> 11) & 7u;
    c->fpu_sw = (uint16_t)(sw & (uint16_t)~0x3800u);
    for (i = 0; i < 8; i++)
        fset(c, i, rdf80(a + 28 + 10 * i));
    c->fpu_tag = rd16(a + 8);
}

/* FST/FSTP to a float, rounded per RC.  The host conversion rounds to
 * nearest; for the directed modes, step one ULP if it went the wrong way. */
static inline float fto_float(const X86 *c, double v) {
    unsigned rc = (c->fpu_cw >> 10) & 3u;
    float f = (float)v;
    double back;
    if (rc == 0 || v != v || isinf(v) || (double)f == v)
        return f;
    back = (double)f;
    if (rc == 1 && back > v)
        return nextafterf(f, -(float)INFINITY); /* down */
    if (rc == 2 && back < v)
        return nextafterf(f, (float)INFINITY); /* up */
    if (rc == 3 && ((v > 0 && back > v) || (v < 0 && back < v)))
        return nextafterf(f, 0.0f); /* truncate */
    return f;
}

/* FIST/FISTP: round per the control word, and store the "integer indefinite"
 * when the value does not fit (NaN, infinity, out of range) exactly as x87
 * does with the invalid-operation exception masked, raising IE. */
static inline int16_t fto_i16(X86 *c, double v) {
    double r = fround_cw(c, v);
    if (r >= -32768.0 && r <= 32767.0)
        return (int16_t)r;
    c->fpu_sw |= 0x0001u;
    return INT16_MIN;
}
static inline int32_t fto_i32(X86 *c, double v) {
    double r = fround_cw(c, v);
    if (r >= -2147483648.0 && r <= 2147483647.0)
        return (int32_t)r;
    c->fpu_sw |= 0x0001u;
    return INT32_MIN;
}
static inline int64_t fto_i64(X86 *c, double v) {
    double r = fround_cw(c, v);
    if (r >= -9223372036854775808.0 && r < 9223372036854775808.0)
        return (int64_t)r;
    c->fpu_sw |= 0x0001u;
    return INT64_MIN;
}

/* Integer stores use FILD's exact signed value until an arithmetic write.
 * Narrowing falls back to the ordinary conversion for masked overflow. */
static inline int16_t fist_i16(X86 *c) {
    int64_t v = (int64_t)c->st_bits[c->fpu_top];
    if (c->st_exact[c->fpu_top] && v >= INT16_MIN && v <= INT16_MAX)
        return (int16_t)v;
    return fto_i16(c, ST(c, 0));
}
static inline int32_t fist_i32(X86 *c) {
    int64_t v = (int64_t)c->st_bits[c->fpu_top];
    if (c->st_exact[c->fpu_top] && v >= INT32_MIN && v <= INT32_MAX)
        return (int32_t)v;
    return fto_i32(c, ST(c, 0));
}
static inline int64_t fist_i64(X86 *c) {
    if (c->st_exact[c->fpu_top])
        return (int64_t)c->st_bits[c->fpu_top];
    return fto_i64(c, ST(c, 0));
}

/* FPREM / FPREM1.
 *
 * Two things the obvious implementation gets wrong.
 *
 * When the operands' exponents differ by 64 or more, x87 does not finish: it
 * performs a PARTIAL reduction, sets C2, and expects the guest to execute the
 * instruction again.  Clearing C2 unconditionally turns that loop into a
 * single pass with a different answer.
 *
 * The quotient bits C0, C3 and C1 are bits 2, 1 and 0 of |Q|, and Q does not
 * fit anywhere: for a = 2^56, b = 3 it is 24019198012642645, and forming it
 * as a double rounds it, so the low bits are lost before they can be read.
 * They are recovered instead from two remainders - `fmod(A, B)` and
 * `fmod(A, 8B)` differ by exactly (Q mod 8) * B - which never builds a large
 * quotient at all.
 */
static inline unsigned fprem_low_bits(double A, double B) {
    double r = fmod(A, B);
    double r8 = fmod(A, 8.0 * B);
    double q = (r8 - r) / B;
    long n = lround(q);
    return (unsigned)(n & 7);
}

static inline void fprem_flags(X86 *c, unsigned bits, int incomplete) {
    uint16_t sw = (uint16_t)(c->fpu_sw & (uint16_t)~0x4700u);
    if (incomplete) {
        sw |= 0x0400u; /* C2: reduction not finished */
    } else {
        if (bits & 4)
            sw |= 0x0100u; /* C0 = quotient bit 2 */
        if (bits & 2)
            sw |= 0x4000u; /* C3 = quotient bit 1 */
        if (bits & 1)
            sw |= 0x0200u; /* C1 = quotient bit 0 */
    }
    c->fpu_sw = sw;
}

static inline double fprem_common(X86 *c, double a, double b, int ieee) {
    double A, B, r;
    int ea, eb;
    if (b == 0.0 || isnan(a) || isnan(b) || isinf(a)) {
        c->fpu_sw = (uint16_t)((c->fpu_sw & (uint16_t)~0x4700u) | 0x0001u);
        return x87_indefinite();
    }
    if (a == 0.0 || isinf(b)) { /* already reduced */
        fprem_flags(c, 0, 0);
        return a;
    }
    A = fabs(a);
    B = fabs(b);
    ea = ilogb(A);
    eb = ilogb(B);
    if (ea - eb >= 64) {
        /* Partial reduction: take out the top of the quotient only, leaving
         * the exponent difference smaller, and tell the guest to come back. */
        double scale = scalbn(B, ea - eb - 32);
        double part = trunc(A / scale);
        r = A - part * scale;
        fprem_flags(c, 0, 1);
        return a < 0 ? -r : r;
    }
    r = fmod(A, B);
    if (ieee) {
        /* Round the quotient to nearest instead of truncating: shift the
         * remainder into (-B/2, B/2]. */
        /* Compare 2r against B rather than r against B/2.  Halving is not
         * exact at the bottom of the range: for B = 2**-1074 it underflows to
         * zero, every r then reads as a tie, and FPREM1(x, x) came back as -x
         * instead of 0.  Doubling r is safe: r < B, so 2r only overflows when
         * r already exceeds B/2, which is the case that adjusts anyway. */
        double r2 = scalbn(r, 1);
        unsigned bits = fprem_low_bits(A, B);
        if (r2 > B || (r2 == B && (bits & 1))) {
            r -= B;
            bits = (bits + 1) & 7;
        }
        fprem_flags(c, bits, 0);
        return a < 0 ? -r : r;
    }
    fprem_flags(c, fprem_low_bits(A, B), 0);
    return a < 0 ? -r : r;
}

/* FSCALE: ST(0) *= 2 ** trunc(ST(1)) */
static inline double fscale(double a, double b) {
    return ldexp(a, (int)trunc(b));
}

/* ----------------------------------------------------------------- misc */

/* FXTRACT: ST(0) becomes its exponent, and its significand is pushed. */
static inline double fxtract_exponent(double v) {
    if (v == 0.0 || isnan(v) || isinf(v))
        return v == 0.0 ? -(double)INFINITY : v;
    return (double)ilogb(v);
}
static inline double fxtract_significand(double v) {
    if (v == 0.0 || isnan(v) || isinf(v))
        return v;
    return scalbn(v, -ilogb(v)); /* in [1, 2) */
}

static inline void xlat(X86 *c) {
    c->r[R_EAX] = (c->r[R_EAX] & 0xffffff00u) | rd8(c->r[R_EBX] + (c->r[R_EAX] & 0xffu));
}
static inline uint32_t bswap32(uint32_t v) {
    return __builtin_bswap32(v);
}

#ifdef __cplusplus
}
#endif
#endif /* RECOMP_X86_H */
