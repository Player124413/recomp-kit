// mss32.cpp - Miles Sound System 6 (mss32.dll) as a game imports it: 41
// stdcall exports whose decorated names carry their arity (_AIL_name@bytes).
// This file starts as an arity table so a call through any of them leaves
// the guest stack intact; samples and streams gain behaviour in the sound
// tasks, and the 3D provider API always reports "no providers", which puts a
// game on its plain 2D path.
#include "com.h"
#include "dx.h"
#include "host_api.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include "../platform/os.h"

#include <string.h>
#include <vector>
#include <iterator>

namespace {

const uint32_t SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8;

void ret0(X86 *c) {
    set_eax(c, 0);
}
void ret1(X86 *c) {
    set_eax(c, 1);
}
void ret_done(X86 *c) {
    set_eax(c, SMP_DONE);
}

#define AIL(name, bytes, fn) {"mss32.dll", "_AIL_" #name "@" #bytes, (bytes) / 4, fn}

const ImportShim g_mss32_shims[] = {
    AIL(startup, 0, ret1),
    AIL(shutdown, 0, ret0),
    AIL(set_preference, 8, ret0),
    AIL(waveOutOpen, 16, ret0),
    AIL(mem_free_lock, 4, ret0),
    AIL(file_read, 8, ret0),
    AIL(allocate_sample_handle, 4, ret0),
    AIL(release_sample_handle, 4, ret0),
    AIL(init_sample, 4, ret0),
    AIL(set_sample_file, 12, ret0),
    AIL(start_sample, 4, ret0),
    AIL(end_sample, 4, ret0),
    AIL(sample_status, 4, ret_done),
    AIL(set_sample_volume, 8, ret0),
    AIL(set_sample_pan, 8, ret0),
    AIL(set_sample_loop_count, 8, ret0),
    AIL(sample_loop_count, 4, ret0),
    AIL(set_sample_reverb, 16, ret0),
    AIL(open_stream, 12, ret0),
    AIL(start_stream, 4, ret0),
    AIL(close_stream, 4, ret0),
    AIL(stream_status, 4, ret_done),
    AIL(set_stream_volume, 8, ret0),
    AIL(stream_volume, 4, ret0),
    AIL(set_stream_loop_count, 8, ret0),
    AIL(enumerate_3D_providers, 12, ret0),
    AIL(open_3D_provider, 4, ret1),
    AIL(close_3D_provider, 4, ret0),
    AIL(set_3D_provider_preference, 12, ret0),
    AIL(3D_provider_attribute, 12, ret0),
    AIL(allocate_3D_sample_handle, 4, ret0),
    AIL(release_3D_sample_handle, 4, ret0),
    AIL(set_3D_sample_file, 8, ret0),
    AIL(start_3D_sample, 4, ret0),
    AIL(end_3D_sample, 4, ret0),
    AIL(3D_sample_status, 4, ret_done),
    AIL(set_3D_sample_volume, 8, ret0),
    AIL(set_3D_sample_loop_count, 8, ret0),
    AIL(set_3D_position, 16, ret0),
    AIL(set_3D_orientation, 28, ret0),
    AIL(3D_update_position, 8, ret0),
};

} // namespace

void mss32_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_mss32_shims, std::size(g_mss32_shims));
}
