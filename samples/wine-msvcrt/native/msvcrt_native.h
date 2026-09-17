// msvcrt_native.h - native replacements for Wine msvcrt functions the kit
// cannot translate, named by game.toml's [translate] overrides.
//
// Wine's i386 msvcrt is built with SSE2 on and checks no CPU feature before
// using it, so memset stores through XMM registers from its second
// instruction. The kit's translator turns SSE into a trap (recomp_unmodelled)
// and recomp_cpuid advertises no SSE, so the first memset the DLL's own
// startup runs would end the process. memset is also the only string or
// memory function in this build that uses SSE at all; the rest of the SSE in
// it is the math library, which this sample never calls.
#pragma once
#include "x86.h"

// memset(dest, c, n), cdecl, returning dest. 0x10047630 in the pinned
// CrossOver 26.3 msvcrt.dll (see game.toml for its hash).
static void msvcrt_native_memset(X86 *c) {
    uint32_t sp = c->r[R_ESP];
    uint32_t ret = rd32(sp);
    uint32_t dest = rd32(sp + 4), fill = rd32(sp + 8), n = rd32(sp + 12);
    if (dest && dest < GUEST_SIZE && n <= GUEST_SIZE - dest)
        for (uint32_t i = 0; i < n; ++i)
            wr8(dest + i, (uint8_t)fill);
    c->r[R_EAX] = dest;
    c->r[R_ESP] = sp + 4u; // cdecl: the caller pops the arguments
    c->eip = ret;
    recomp_return(c);
}

#define FN_10047630 msvcrt_native_memset
