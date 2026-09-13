// bink.cpp - RAD Game Tools' Bink (binkw32.dll) and Smacker (smackw32.dll)
// video as a game imports them. No decoder: BinkOpen and SmackOpen refuse, so
// a game takes its own "video unavailable" path and skips the cinematic. The
// arities come from the decorated names.
#include "dx.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"

#include <string.h>
#include <iterator>

namespace {

uint32_t g_error_string = 0;

void ret0(X86 *c) {
    set_eax(c, 0);
}
void ret1(X86 *c) {
    set_eax(c, 1);
}

void BinkGetError(X86 *c) {
    if (!g_error_string) {
        static const char text[] = "no video decoder";
        g_error_string = heap_alloc(sizeof text, true, 16);
        memcpy(g_mem + g_error_string, text, sizeof text);
    }
    set_eax(c, g_error_string);
}

#define BINK(name, bytes, fn) {"binkw32.dll", "_Bink" #name "@" #bytes, (bytes) / 4, fn}
#define SMACK(name, bytes, fn) {"smackw32.dll", "_Smack" #name "@" #bytes, (bytes) / 4, fn}

const ImportShim g_video_shims[] = {
    BINK(Open, 8, ret0),           BINK(OpenMiles, 4, ret0),
    BINK(SetSoundSystem, 8, ret1), BINK(DDSurfaceType, 4, ret0),
    BINK(DoFrame, 4, ret0),        BINK(NextFrame, 4, ret0),
    BINK(Wait, 4, ret0),           BINK(CopyToBuffer, 28, ret0),
    BINK(Service, 4, ret0),        BINK(GetError, 0, BinkGetError),
    BINK(Close, 4, ret0),          BINK(BufferClose, 4, ret0),
    SMACK(Open, 12, ret0),         SMACK(SoundUseMSS, 4, ret0),
    SMACK(DoFrame, 4, ret0),       SMACK(NextFrame, 4, ret0),
    SMACK(Wait, 4, ret0),          SMACK(ToBuffer, 28, ret0),
    SMACK(Close, 4, ret0),
};

} // namespace

void bink_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_video_shims, std::size(g_video_shims));
}
