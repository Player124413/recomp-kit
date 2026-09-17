// discovery.cpp - see discovery.h.
#include "discovery.h"

#include "win32.h"

#include "../platform/os.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {

struct Find {
    std::string kind;
    uint32_t from = 0;
    uint32_t hits = 0;
};

std::mutex g_mutex;
std::map<uint32_t, Find> &finds() {
    static auto *m = new std::map<uint32_t, Find>();
    return *m;
}

// The file is opened once, at exit: a run that ends in abort() writes the same
// file as one that returns from its main loop, and a run that finds nothing
// writes an empty file rather than leaving a stale one behind.
const char *path() {
    return recomp_env("DISCOVERY");
}

bool g_registered = false;

// The caller holds g_mutex.
void write_locked();

} // namespace

extern "C" void discovery_note(const char *kind, uint32_t target, uint32_t from) {
    if (!path())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_registered) {
        g_registered = true;
        atexit(discovery_write);
    }
    Find &f = finds()[target];
    bool fresh = f.hits == 0;
    if (fresh) {
        f.kind = kind ? kind : "call";
        f.from = from;
    }
    ++f.hits;
    if (fresh)
        write_locked(); // a run that ends in abort() never reaches atexit
}

extern "C" uint32_t discovery_count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return (uint32_t)finds().size();
}

extern "C" void discovery_print(FILE *out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (finds().empty())
        return;
    fprintf(out, "discovered code the translation does not carry: %zu address%s\n", finds().size(),
            finds().size() == 1 ? "" : "es");
    for (const auto &kv : finds())
        fprintf(out, "    %08x  %s from %08x, %u time%s\n", kv.first, kv.second.kind.c_str(),
                kv.second.from, kv.second.hits, kv.second.hits == 1 ? "" : "s");
}

namespace {
void write_locked() {
    const char *file = path();
    if (!file)
        return;
    FILE *out = fopen(file, "w");
    if (!out) {
        LOGW("discovery: cannot write %s", file);
        return;
    }
    fprintf(out, "# tools/recomp/translate.py --discovered reads this file.\n"
                 "# <address> <call|jump> <the instruction that named it> <times reached>\n");
    for (const auto &kv : finds())
        fprintf(out, "%08x %s %08x %u\n", kv.first, kv.second.kind.c_str(), kv.second.from,
                kv.second.hits);
    fclose(out);
}
} // namespace

extern "C" void discovery_write(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    write_locked();
}
