// mss32.cpp - Miles Sound System 6 (mss32.dll) as a game imports it: 41
// stdcall exports whose decorated names carry their arity (_AIL_name@bytes).
// Samples play PCM WAVE images and streams decode MP3 through shared host
// audio channels. The 3D provider API reports "no providers" so a game can
// use its plain 2D path. Every entry preserves the guest's stdcall stack.
#include "com.h"
#include "dx.h"
#include "host_api.h"
#include "riff.h"
#include "mp3_source.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include "../platform/os.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <iterator>

namespace {

const uint32_t SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4;

int32_t miles_volume_mb(int32_t v) {
    v = std::clamp(v, 0, 127);
    return v == 0 ? -10000
                  : std::clamp((int32_t)std::lround(2000.0 * std::log10(v / 127.0)), -10000, 0);
}

int32_t miles_pan_mb(int32_t p) {
    p = std::clamp(p, 0, 127);
    return std::clamp((p - 64) * 10000 / 63, -10000, 10000);
}

struct Sample {
    uint32_t handle = 0;
    bool alive = false;
    int32_t channel = -1;
    RiffWave wave{};
    int32_t volume = 127, pan = 64;
    uint32_t loops = 1, remaining = 0;
    bool playing = false;
};
std::vector<Sample> g_samples(64);

Sample *sample_for(uint32_t handle) {
    if (!handle || handle > g_samples.size())
        return nullptr;
    Sample &s = g_samples[handle - 1];
    return s.alive ? &s : nullptr;
}

// The sample borrows its WAVE image. The host copies PCM on submission;
// Miles keeps the shared channel reserved until init or handle release.
void sample_stop(Sample &s) {
    if (s.channel >= 0)
        host_audio_stop(s.channel);
    s.playing = false;
    s.remaining = 0;
}

void sample_reset(Sample &s) {
    sample_stop(s);
    dx_free_audio_channel(s.channel);
    uint32_t handle = s.handle;
    s = Sample{};
    s.handle = handle;
    s.alive = true;
}

void sample_play(Sample &s) {
    HostAudioPlay p{};
    p.channel = s.channel;
    p.pcm = g_mem + s.wave.pcm;
    p.bytes = s.wave.pcm_bytes;
    p.sample_rate = (int32_t)s.wave.rate;
    p.channels = s.wave.channels;
    p.bits = s.wave.bits;
    p.loop = s.loops == 0 ? 1 : 0;
    p.volume = miles_volume_mb(s.volume);
    p.pan = miles_pan_mb(s.pan);
    host_audio_play(&p);
    s.playing = true;
}

// The host loop flag is boolean. Finite repeats restart at completion on
// the runtime's frame seam (also observed by status), with n total plays.
void sample_update(Sample &s) {
    if (!s.playing || host_audio_is_playing(s.channel))
        return;
    if (s.remaining > 1) {
        --s.remaining;
        sample_play(s);
    } else {
        s.remaining = 0;
        s.playing = false;
    }
}

// Use CreateFileA's read resolver, including overlays, drive mapping and
// case folding. Both NULL (ordinary Miles allocation) and FILE_READ_WITH_SIZE
// request a guest allocation here; the returned image begins with file bytes.
void ail_file_read(X86 *c) {
    set_eax(c, 0);
    std::string path = win32_host_path_op(gm_str(arg(c, 0)), WIN32_FILE_READ);
    if (path.empty())
        return;
    int fd = os_fd_open(path.c_str(), OS_O_RDONLY);
    if (fd < 0)
        return;
    OsStat st{};
    if (os_fd_stat(fd, &st) != 0 || !st.is_regular || !st.size || st.size > UINT32_MAX) {
        os_fd_close(fd);
        return;
    }
    uint32_t bytes = (uint32_t)st.size, dest = arg(c, 1);
    bool allocate = dest == 0 || dest == 0xffffffffu;
    if (allocate)
        dest = heap_alloc(bytes);
    if (!dest || !gm_valid(dest, bytes)) {
        if (allocate && dest)
            heap_free(dest);
        os_fd_close(fd);
        return;
    }
    uint32_t got = 0;
    while (got < bytes) {
        int64_t n = os_fd_read(fd, g_mem + dest + got, bytes - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    os_fd_close(fd);
    if (got != bytes) {
        if (allocate)
            heap_free(dest);
        return;
    }
    set_eax(c, dest);
}

void ail_mem_free_lock(X86 *c) {
    heap_free(arg(c, 0));
    set_eax(c, 0);
}

void ail_allocate_sample_handle(X86 *c) {
    set_eax(c, 0);
    for (uint32_t i = 0; i < g_samples.size(); ++i) {
        Sample &s = g_samples[i];
        if (s.alive)
            continue;
        s = Sample{};
        s.handle = i + 1;
        s.alive = true;
        set_eax(c, s.handle);
        return;
    }
}

void ail_release_sample_handle(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        sample_reset(*s);
        s->alive = false;
    }
    set_eax(c, 0);
}

void ail_init_sample(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0)))
        sample_reset(*s);
    set_eax(c, 0);
}

void ail_set_sample_file(X86 *c) {
    set_eax(c, 0);
    Sample *s = sample_for(arg(c, 0));
    if (!s)
        return;
    sample_stop(*s);
    s->wave = RiffWave{};
    uint32_t image = arg(c, 1);
    if (!image || !gm_valid(image, 12))
        return;
    // Miles supplies no length. Read it from RIFF, bounded by an allocation
    // when the image is at its start, and always by the guest arena.
    uint64_t bytes = (uint64_t)rd32(image + 4) + 8;
    uint32_t allocated = heap_size(image);
    if (bytes > UINT32_MAX || (allocated != UINT32_MAX && bytes > allocated))
        return;
    if (riff_parse_wave(image, (uint32_t)bytes, &s->wave))
        set_eax(c, 1);
}

void ail_start_sample(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0)); s && s->wave.pcm_bytes) {
        if (s->channel < 0)
            s->channel = dx_alloc_audio_channel();
        if (s->channel >= 0) {
            s->remaining = s->loops;
            sample_play(*s);
        }
    }
    set_eax(c, 0);
}

void ail_end_sample(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0)))
        sample_stop(*s);
    set_eax(c, 0);
}

void ail_sample_status(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    if (s)
        sample_update(*s);
    set_eax(c, !s ? SMP_FREE : s->playing ? SMP_PLAYING : SMP_DONE);
}

void ail_set_sample_volume(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        s->volume = std::clamp((int32_t)arg(c, 1), 0, 127);
        if (s->channel >= 0)
            host_audio_set_volume(s->channel, miles_volume_mb(s->volume));
    }
    set_eax(c, 0);
}

void ail_set_sample_pan(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        s->pan = std::clamp((int32_t)arg(c, 1), 0, 127);
        if (s->channel >= 0)
            host_audio_set_pan(s->channel, miles_pan_mb(s->pan));
    }
    set_eax(c, 0);
}

void ail_set_sample_loop_count(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0)))
        s->loops = arg(c, 1);
    set_eax(c, 0);
}

void ail_sample_loop_count(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    set_eax(c, s ? s->loops : 0);
}

// Stream handles own the encoded image, decoder and shared host channel.
// All state is serviced on the guest frame seam; the host copies each PCM
// submission. A refused chunk remains pending until a later frame accepts it.
struct Stream {
    bool alive = false, playing = false;
    int32_t channel = -1, volume = 127;
    uint32_t loops = 1, remaining = 1;
    uint64_t submitted = 0;
    Mp3Source source;
    std::vector<int16_t> pending;
    size_t pending_pos = 0;
};
std::vector<Stream> g_streams(64);

Stream *stream_for(uint32_t handle) {
    if (!handle || handle > g_streams.size())
        return nullptr;
    Stream &s = g_streams[handle - 1];
    return s.alive ? &s : nullptr;
}

// Read through the runtime resolver, sharing CreateFileA's overlays, drive
// mapping and case folding. Open does not play until AIL_start_stream.
void ail_open_stream(X86 *c) {
    set_eax(c, 0);
    uint32_t name = arg(c, 1);
    if (!name || !gm_valid(name, 1))
        return;
    std::string guest = gm_str(name);
    std::string path = win32_host_path_op(guest, WIN32_FILE_READ);
    int fd = path.empty() ? -1 : os_fd_open(path.c_str(), OS_O_RDONLY);
    if (fd < 0) {
        LOGW("mss32: open_stream(%s): cannot open %s", guest.c_str(), path.c_str());
        return;
    }
    OsStat st{};
    if (os_fd_stat(fd, &st) != 0 || !st.is_regular || !st.size || st.size > UINT32_MAX) {
        os_fd_close(fd);
        return;
    }
    std::vector<uint8_t> bytes((size_t)st.size);
    size_t got = 0;
    while (got < bytes.size()) {
        int64_t n = os_fd_read(fd, bytes.data() + got, bytes.size() - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    os_fd_close(fd);
    if (got != bytes.size()) {
        LOGW("mss32: open_stream(%s): short read of %s", guest.c_str(), path.c_str());
        return;
    }
    for (uint32_t i = 0; i < g_streams.size(); ++i) {
        Stream &s = g_streams[i];
        if (s.alive)
            continue;
        s = Stream{};
        if (!s.source.open(bytes)) {
            LOGW("mss32: open_stream(%s): not an MPEG audio file: %s", guest.c_str(), path.c_str());
            return;
        }
        s.channel = dx_alloc_audio_channel();
        if (s.channel < 0) {
            s = Stream{};
            return;
        }
        s.alive = true;
        LOGV("mss32: open_stream(%s): %s, %u Hz, %u channel(s)", guest.c_str(), path.c_str(),
             s.source.rate(), s.source.channels());
        set_eax(c, i + 1);
        return;
    }
}

// Keep one second of appended PCM ahead. Decoder EOF is not playback EOF:
// status stays playing until the host consumes the first play and all queues.
void stream_update(Stream &s) {
    if (!s.playing)
        return;
    uint32_t ahead = s.source.rate() * s.source.channels() * 2;
    while (host_audio_queued_bytes(s.channel) < ahead) {
        if (s.pending.empty()) {
            if (!s.source.decode_next(s.pending)) {
                if (s.loops != 0 && s.remaining <= 1)
                    break;
                if (s.remaining > 1)
                    --s.remaining;
                s.source.seek_frames(0);
                if (!s.source.decode_next(s.pending))
                    break;
            }
            s.pending_pos = 0;
        }
        uint32_t bytes = (uint32_t)(s.pending.size() - s.pending_pos) * 2;
        int32_t taken = host_audio_queue(s.channel, s.pending.data() + s.pending_pos, bytes);
        if (taken <= 0)
            return;
        s.submitted += (uint32_t)taken;
        s.pending_pos += (uint32_t)taken / 2;
        if (s.pending_pos >= s.pending.size())
            s.pending.clear();
    }
    if (s.source.drained() && s.pending.empty() &&
        host_audio_played_bytes(s.channel) >= s.submitted)
        s.playing = false;
}

// Start with one decoded frame, then convert the channel so subsequent
// frames append without restarting playback. Starting again rewinds.
void ail_start_stream(X86 *c) {
    if (Stream *s = stream_for(arg(c, 0))) {
        host_audio_stop(s->channel);
        s->playing = false;
        s->pending.clear();
        s->source.seek_frames(0);
        s->remaining = s->loops;
        if (s->source.decode_next(s->pending)) {
            HostAudioPlay p{};
            p.channel = s->channel;
            p.pcm = s->pending.data();
            p.bytes = (uint32_t)s->pending.size() * 2;
            p.sample_rate = (int32_t)s->source.rate();
            p.channels = (int32_t)s->source.channels();
            p.bits = 16;
            p.volume = miles_volume_mb(s->volume);
            host_audio_play(&p);
            s->submitted = p.bytes;
            s->pending.clear();
            s->playing = host_audio_stream(s->channel) >= 0;
            if (!s->playing)
                host_audio_stop(s->channel);
        }
    }
    set_eax(c, 0);
}

void ail_close_stream(X86 *c) {
    if (Stream *s = stream_for(arg(c, 0))) {
        host_audio_stop(s->channel);
        dx_free_audio_channel(s->channel);
        *s = Stream{};
    }
    set_eax(c, 0);
}

void ail_stream_status(X86 *c) {
    Stream *s = stream_for(arg(c, 0));
    if (s)
        stream_update(*s);
    set_eax(c, s && s->playing ? SMP_PLAYING : SMP_DONE);
}

void ail_set_stream_volume(X86 *c) {
    if (Stream *s = stream_for(arg(c, 0))) {
        s->volume = std::clamp((int32_t)arg(c, 1), 0, 127);
        host_audio_set_volume(s->channel, miles_volume_mb(s->volume));
    }
    set_eax(c, 0);
}

void ail_stream_volume(X86 *c) {
    Stream *s = stream_for(arg(c, 0));
    set_eax(c, s ? (uint32_t)s->volume : 0);
}

void ail_set_stream_loop_count(X86 *c) {
    if (Stream *s = stream_for(arg(c, 0)))
        s->remaining = s->loops = arg(c, 1);
    set_eax(c, 0);
}

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
    AIL(mem_free_lock, 4, ail_mem_free_lock),
    AIL(file_read, 8, ail_file_read),
    AIL(allocate_sample_handle, 4, ail_allocate_sample_handle),
    AIL(release_sample_handle, 4, ail_release_sample_handle),
    AIL(init_sample, 4, ail_init_sample),
    AIL(set_sample_file, 12, ail_set_sample_file),
    AIL(start_sample, 4, ail_start_sample),
    AIL(end_sample, 4, ail_end_sample),
    AIL(sample_status, 4, ail_sample_status),
    AIL(set_sample_volume, 8, ail_set_sample_volume),
    AIL(set_sample_pan, 8, ail_set_sample_pan),
    AIL(set_sample_loop_count, 8, ail_set_sample_loop_count),
    AIL(sample_loop_count, 4, ail_sample_loop_count),
    AIL(set_sample_reverb, 16, ret0),
    AIL(open_stream, 12, ail_open_stream),
    AIL(start_stream, 4, ail_start_stream),
    AIL(close_stream, 4, ail_close_stream),
    AIL(stream_status, 4, ail_stream_status),
    AIL(set_stream_volume, 8, ail_set_stream_volume),
    AIL(stream_volume, 4, ail_stream_volume),
    AIL(set_stream_loop_count, 8, ail_set_stream_loop_count),
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

void mss32_frame_pump(X86 *c) {
    if (!c)
        return;
    for (auto &s : g_samples)
        if (s.alive)
            sample_update(s);
    for (auto &s : g_streams)
        if (s.alive)
            stream_update(s);
}

void mss32_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_mss32_shims, std::size(g_mss32_shims));
}
