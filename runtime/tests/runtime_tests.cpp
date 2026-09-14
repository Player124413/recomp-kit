// runtime_tests.cpp - runtime, loader, allocator and Win32 shim tests.
//
// Run from the repository root:
//   .venv/bin/python tools/test.py --compile-only && build/recomp/runtime_tests
#include "../imports.h"
#include "../mods_seam.h"
#include "../intrinsics.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../../platform/os.h"
#include "game_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <map>
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0, g_failures = 0, g_skips = 0;
static const char *g_section = "";

static void section(const char *s) {
    g_section = s;
    printf("\n== %s\n", s);
}

static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

// ---------------------------------------------------------------------------
// Shim call helper: pushes args and a return address, then dispatches.
// ---------------------------------------------------------------------------
static uint32_t g_fake_ret = RECOMP_ENTRY_POINT;

static uint32_t call_import(X86 *c, const char *dll, const char *name,
                            const std::vector<uint32_t> &args) {
    // imports_resolve allocates a trampoline for a registered shim that no IAT
    // slot referenced, which is the same path GetProcAddress takes.
    uint32_t tramp = imports_resolve(dll, name);
    if (!tramp) {
        printf("  [FAIL] no trampoline for %s!%s\n", dll, name);
        ++g_failures;
        ++g_checks;
        return 0;
    }
    uint32_t esp = c->r[R_ESP];
    uint32_t before = esp;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, g_fake_ret);
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp);
    if (c->r[R_ESP] != before) {
        printf("  [FAIL] %s!%s left ESP at %08x, expected %08x (bad argc?)\n", dll, name,
               c->r[R_ESP], before);
        ++g_failures;
        ++g_checks;
        c->r[R_ESP] = before;
    }
    return c->r[R_EAX];
}

// Writes a NUL-terminated string into a scratch area of the guest stack.
static uint32_t scratch = 0;
static uint32_t put_str(const char *s) {
    uint32_t a = scratch;
    uint32_t n = (uint32_t)strlen(s) + 1;
    memcpy(g_mem + a, s, n);
    scratch += (n + 15) & ~15u;
    return a;
}
// Portable stand-ins for `rm -rf` and `mkdir -p` over the platform layer.
static int remove_tree_entry(const char *name, void *user) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    std::string path = *static_cast<const std::string *>(user) + "/" + name;
    OsStat st{};
    if (os_lstat(path.c_str(), &st) == 0 && st.is_dir && !st.is_symlink) {
        os_listdir(path.c_str(), remove_tree_entry, &path);
        os_rmdir(path.c_str());
    } else {
        os_unlink(path.c_str());
    }
    return 0;
}
static void remove_tree(const std::string &root) {
    OsStat st{};
    if (os_lstat(root.c_str(), &st) != 0)
        return;
    if (st.is_dir && !st.is_symlink) {
        std::string r = root;
        os_listdir(root.c_str(), remove_tree_entry, &r);
        os_rmdir(root.c_str());
    } else {
        os_unlink(root.c_str());
    }
}
static void mkdir_p(const std::string &path) {
    std::string acc;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty())
                os_mkdir(acc.c_str());
        }
        if (i < path.size())
            acc.push_back(path[i]);
    }
}

static uint32_t scratch_block(uint32_t bytes) {
    uint32_t a = scratch;
    memset(g_mem + a, 0, bytes);
    scratch += (bytes + 15) & ~15u;
    return a;
}

// ---------------------------------------------------------------------------
// Sections, cross-checked against pefile.
// ---------------------------------------------------------------------------
struct ExpectedSection {
    std::string name;
    uint32_t va, vsize, raw, offset, flags;
};
struct ExpectedImport {
    uint32_t slot;
    std::string dll, name;
};
struct ExpectedImage {
    uint32_t base = 0, size = 0, entry = 0;
    std::vector<ExpectedSection> sections;
    std::vector<ExpectedImport> imports;
};

static bool pefile_sections(ExpectedImage &out, std::string &err) {
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#define POP_VENV_PYTHON ".venv/Scripts/python.exe"
#else
#define POP_VENV_PYTHON ".venv/bin/python"
#endif
    // The interpreter that configured the build (CTest passes RECOMP_PYTHON), else
    // the checkout's venv relative to the working directory.
    const char *python = recomp_env("PYTHON");
    const std::string cmd_s =
        std::string(python && *python ? python : POP_VENV_PYTHON) +
        " -c \""
        "import pefile;pe=pefile.PE('" RECOMP_DEVELOPER_EXE "');b=pe.OPTIONAL_HEADER.ImageBase;"
        "print('IMAGE %x %x %x' % (b, pe.OPTIONAL_HEADER.SizeOfImage, "
        "b+pe.OPTIONAL_HEADER.AddressOfEntryPoint));"
        "[print('SECTION %s %x %x %x %x %x' % (s.Name.decode().rstrip(chr(0)), b+s.VirtualAddress, "
        "s.Misc_VirtualSize, s.SizeOfRawData, s.PointerToRawData, s.Characteristics)) for s in "
        "pe.sections];"
        "[print('IMPORT %x %s %s' % (i.address, d.dll.decode(), "
        "i.name.decode() if i.name else 'ord%d' % i.ordinal)) "
        "for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []) for i in d.imports]"
        "\" 2>/dev/null";
    const char *cmd = cmd_s.c_str();
    FILE *p = popen(cmd, "r");
    if (!p) {
        err = "popen failed";
        return false;
    }
    char line[1024];
    bool image = false;
    while (fgets(line, sizeof line, p)) {
        char name[512], dll[256];
        unsigned va, vsize, raw, offset, flags;
        if (sscanf(line, "IMAGE %x %x %x", &va, &vsize, &raw) == 3) {
            out.base = va;
            out.size = vsize;
            out.entry = raw;
            image = true;
        } else if (sscanf(line, "SECTION %511s %x %x %x %x %x", name, &va, &vsize, &raw, &offset,
                          &flags) == 6) {
            out.sections.push_back({name, va, vsize, raw, offset, flags});
        } else if (sscanf(line, "IMPORT %x %255s %511s", &va, dll, name) == 3) {
            out.imports.push_back({va, dll, name});
        } else {
            err = "unrecognized pefile record";
        }
    }
    int rc = pclose(p);
    if (rc != 0 || !image || out.sections.empty() || !err.empty()) {
        if (err.empty())
            err = rc == 0 ? "incomplete output from pefile" : "pefile helper failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
// The unit binary dispatches import trampolines and stubs the translated entry.
// Temporarily supply two TLS callbacks so an empty image list still exercises
// argument order, the return sentinel, and both caller and callee stack cleanup.
static uint32_t g_tls_callback_hits = 0;
static void fake_loader_tls_callback(X86 *c) {
    ++g_tls_callback_hits;
    check(rd32(c->r[R_ESP]) == GUEST_RETURN_SENTINEL, "TLS callback has the return sentinel");
    check(arg(c, 0) == loader_image_base() && arg(c, 1) == 1 && arg(c, 2) == 0,
          "TLS callback receives the image base, process attach, and null reserved argument");
    const LoaderTls &tls = loader_tls();
    check(rd32(rd32(c->fs_base + 0x2c) + 4 * tls.index) != 0,
          "TLS callback sees the initialized main thread block");
}

static void test_modules_and_wide() {
    section("modules and wide strings");
    X86 c;
    loader_init_context(&c);
    // This runtime-only binary does not link DirectX. Register a fixture with
    // the same DLL/export spelling to exercise run-time lookup of an extra table.
    static const ImportShim shims[] = {{"DDRAW.dll", "DirectDrawCreate", 3, nullptr}};
    imports_register(shims, sizeof shims / sizeof shims[0]);
    uint32_t name = 0x00300000; // scratch in the arena below the image
    gm_put_str(name, "ddraw.dll", 64);
    uint32_t h = call_import(&c, "KERNEL32.dll", "LoadLibraryA", {name});
    check(h != 0, "LoadLibraryA(ddraw.dll) -> %08x", h);
    gm_put_str(name + 64, "DirectDrawCreate", 64);
    check(call_import(&c, "KERNEL32.dll", "GetProcAddress", {h, name + 64}) != 0,
          "GetProcAddress(ddraw, DirectDrawCreate) resolves");
    gm_put_wstr(name + 128, "soaddraw.dll", 64);
    check(call_import(&c, "KERNEL32.dll", "LoadLibraryW", {name + 128}) == 0,
          "LoadLibraryW(soaddraw.dll): no shims, reported missing");
    check(call_import(&c, "KERNEL32.dll", "GetLastError", {}) == 126,
          "missing wide module reports ERROR_MOD_NOT_FOUND");
    gm_put_wstr(name + 128, "DDRAW.DLL", 64);
    check(call_import(&c, "KERNEL32.dll", "LoadLibraryW", {name + 128}) == h,
          "LoadLibraryW(DDRAW.DLL) returns the same module");
    check(call_import(&c, "KERNEL32.dll", "GetModuleHandleW", {name + 128}) == h,
          "GetModuleHandleW agrees");
    check(gm_wstr(name + 128) == "DDRAW.DLL", "gm_wstr round-trips");
    gm_put_wstr(name + 256, "abc", 3);
    check(gm_wstr(name + 256) == "ab", "gm_put_wstr truncates to the cap");

    check(imports_has_dll("ddraw.dll") && imports_has_dll("DdRaW.DlL"),
          "registered DLL lookup is case-insensitive");
    check(!imports_has_dll("soaddraw.dll") && !imports_has_dll("") && !imports_has_dll(nullptr),
          "unregistered and empty DLL names are absent");
    gm_put_wstr(name + 128, "C:\\WINDOWS\\SYSTEM32\\DDRAW", 64);
    check(call_import(&c, "KERNEL32.dll", "LoadLibraryExW", {name + 128, 0, 8}) == h,
          "LoadLibraryExW normalizes paths and extension and pops three arguments");
    check(call_import(&c, "KERNEL32.dll", "GetModuleHandleW", {0}) == IMAGE_BASE,
          "GetModuleHandleW(NULL) returns the image");
    gm_put_wstr(name + 128, "unregistered.dll", 64);
    check(call_import(&c, "KERNEL32.dll", "GetModuleHandleW", {name + 128}) == 0,
          "GetModuleHandleW reports an unloaded module as missing");

    for (uint32_t module : {0u, IMAGE_BASE, h}) {
        uint32_t narrow = name + 512, wide = name + 1024;
        call_import(&c, "KERNEL32.dll", "GetModuleFileNameA", {module, narrow, 260});
        uint32_t n = call_import(&c, "KERNEL32.dll", "GetModuleFileNameW", {module, wide, 260});
        check(n == gm_str(narrow).size() && gm_wstr(wide) == gm_str(narrow),
              "GetModuleFileNameW(%08x) agrees with the guest path from A", module);
    }
    uint32_t out = name + 2048;
    uint32_t n = call_import(&c, "KERNEL32.dll", "GetModuleFileNameW", {h, out, 3});
    check(n == 2 && gm_wstr(out) == "C:", "GetModuleFileNameW truncates in UTF-16 units");
    wr16(out, 0x1234);
    check(call_import(&c, "KERNEL32.dll", "GetModuleFileNameW", {h, out, 0}) == 0 &&
              rd16(out) == 0x1234,
          "GetModuleFileNameW with zero capacity leaves the buffer alone");

    // Independent code-unit expectations catch mutually wrong encoders/decoders.
    const std::string unicode = "A\xc3\xa9\xe6\xb0\xb4\xf0\x9f\x98\x80";
    check(gm_put_wstr(out, unicode, 6) == 5 && rd16(out) == 'A' && rd16(out + 2) == 0x00e9 &&
              rd16(out + 4) == 0x6c34 && rd16(out + 6) == 0xd83d && rd16(out + 8) == 0xde00 &&
              rd16(out + 10) == 0,
          "gm_put_wstr encodes BMP characters and a surrogate pair");
    check(gm_wstr(out) == unicode, "gm_wstr decodes BMP characters and a surrogate pair");
    check(gm_wstr(out, 3) == "A\xc3\xa9\xe6\xb0\xb4", "gm_wstr respects the code-unit bound");
    check(gm_wstr(out + 6, 1) == "\xef\xbf\xbd",
          "gm_wstr does not consume a surrogate beyond its bound");
    check(gm_put_wstr(out, "\xf0\x9f\x98\x80", 2) == 0 && rd16(out) == 0,
          "gm_put_wstr never writes half a surrogate pair");
    wr16(out + 2, 0x1234);
    check(gm_put_wstr(out, "abc", 1) == 0 && rd16(out) == 0 && rd16(out + 2) == 0x1234,
          "gm_put_wstr capacity one writes only the terminator");
    check(gm_put_wstr(out + 2, "abc", 0) == 0 && rd16(out + 2) == 0x1234,
          "gm_put_wstr capacity zero writes nothing");
    check(gm_wstr(0).empty() && gm_wstr(out, 0).empty() && gm_put_wstr(0, "abc", 8) == 0,
          "wide helpers accept null addresses and zero read bounds");
    check(gm_put_wstr(out, std::string("a\0b", 3), 8) == 1 && gm_wstr(out) == "a",
          "gm_put_wstr stops at an embedded NUL");
    wr16(out, 0xdc00);
    wr16(out + 2, 0);
    check(gm_wstr(out) == "\xef\xbf\xbd", "gm_wstr replaces an unpaired surrogate");
    check(gm_put_wstr(out, std::string("\xe2\x82", 2), 8) == 2 && rd16(out) == 0xfffd &&
              rd16(out + 2) == 0xfffd,
          "gm_put_wstr replaces incomplete UTF-8 without reading past the string");
    check(gm_put_wstr(out, std::string("\xc0\xaf", 2), 8) == 2 &&
              gm_wstr(out) == "\xef\xbf\xbd\xef\xbf\xbd",
          "gm_put_wstr rejects overlong UTF-8");
    wr16(GUEST_SIZE - 2, 'Z');
    check(gm_wstr(GUEST_SIZE - 2) == "Z" && gm_wstr(0xfffffffeu).empty(),
          "gm_wstr stops at the arena boundary without wrapping guest addresses");
    check(gm_put_wstr(GUEST_SIZE - 2, "abc", 8) == 0 && rd16(GUEST_SIZE - 2) == 0,
          "gm_put_wstr reserves a terminator at the arena boundary");
    check(gm_put_wstr(0xfffffffeu, "abc", 8) == 0,
          "gm_put_wstr rejects addresses outside the arena");
}

static void test_loader() {
    section("loader");
    bool ok = loader_load(nullptr);
    if (!check(ok, "loader_load(%s): %s", RECOMP_DEVELOPER_EXE, ok ? "loaded" : loader_error())) {
        printf("cannot continue without the image\n");
        exit(1);
    }
    check(loader_image_base() == RECOMP_IMAGE_BASE, "image base is %08x", loader_image_base());
    check(loader_entry_point() == RECOMP_ENTRY_POINT, "entry point is %08x", loader_entry_point());

    // A Delphi image carries a TLS directory; the loader reserves a slot, writes
    // its index where the image reads it, and gives the main thread a block that
    // starts with the directory's raw bytes.
    const LoaderTls &tls = loader_tls();
    if (tls.raw_end > tls.raw_start) {
        check(tls.index < TLS_SLOTS, "TLS slot %u reserved", tls.index);
        check(rd32(tls.index_addr) == tls.index, "the image's TLS index reads %u",
              rd32(tls.index_addr));
        uint32_t block = rd32(TLS_BASE + 4 * tls.index);
        check(block != 0, "main thread TLS block at %08x", block);
        check(memcmp(g_mem + block, g_mem + tls.raw_start, tls.raw_end - tls.raw_start) == 0,
              "the block starts with the directory's raw data");
    } else {
        check(tls.index == 0xffffffffu, "no TLS directory: no slot reserved");
    }

    if (tls.index < TLS_SLOTS && tls.callbacks) {
        uint32_t saved[3];
        memcpy(saved, g_mem + tls.callbacks, sizeof saved);
        wr32(tls.callbacks,
             imports_alloc_trampoline("test", "tls_callback_cdecl", fake_loader_tls_callback, 0));
        wr32(tls.callbacks + 4,
             imports_alloc_trampoline("test", "tls_callback_stdcall", fake_loader_tls_callback, 3));
        wr32(tls.callbacks + 8, 0);
        X86 c = *loader_context();
        uint32_t esp = c.r[R_ESP];
        g_tls_callback_hits = 0;
        run_entry(&c);
        check(g_tls_callback_hits == 2, "both TLS callbacks ran in the entry path");
        check(c.r[R_ESP] == esp && rd32(esp) == GUEST_RETURN_SENTINEL,
              "TLS callbacks preserve the entry stack and its return sentinel");
        memcpy(g_mem + tls.callbacks, saved, sizeof saved);
    }

    ExpectedImage expect;
    std::string err;
    if (!pefile_sections(expect, err)) {
        check(false, "pefile cross-check could not run: %s", err.c_str());
    } else {
        check(expect.base == loader_image_base() && expect.entry == loader_entry_point(),
              "pefile agrees on base %08x and entry %08x", expect.base, expect.entry);
        check(loader_image_limit() == expect.base + expect.size,
              "the image ends at %08x, derived from SizeOfImage %x", loader_image_limit(),
              expect.size);
        check(loader_iat_patched() == expect.imports.size(), "patched %u IAT slots (expected %zu)",
              loader_iat_patched(), expect.imports.size());
        const auto &got = loader_sections();
        check(got.size() == expect.sections.size(), "section count %zu matches pefile %zu",
              got.size(), expect.sections.size());
        bool all = true;
        for (size_t i = 0; i < std::min(got.size(), expect.sections.size()); ++i) {
            const auto &e = expect.sections[i];
            const auto &section = got[i];
            if (section.name != e.name || section.va != e.va || section.vsize != e.vsize ||
                section.raw_size != e.raw)
                all = false;
        }
        check(all, "every section maps at the address, size and raw size pefile reports");

        // Use the section table for both code samples; entry need not be in the
        // first executable section, and raw offsets need not equal RVAs.
        FILE *f = fopen(RECOMP_DEVELOPER_EXE, "rb");
        if (check(f != nullptr, "opened the image for a byte-level spot check")) {
            auto spot_check = [&](uint32_t va, const ExpectedSection &section, size_t n) {
                uint8_t bytes[64];
                bool read = va >= section.va && uint64_t(va - section.va) + n <= section.raw &&
                            fseek(f, section.offset + (va - section.va), SEEK_SET) == 0 &&
                            fread(bytes, 1, n, f) == n;
                check(read && memcmp(bytes, g_mem + va, n) == 0,
                      "code bytes at %08x in %s match the file", va, section.name.c_str());
            };
            bool code = false, entry = false;
            for (const auto &section : expect.sections) {
                if (!code && (section.flags & 0x20000000u) && section.raw) {
                    spot_check(section.va, section, std::min(section.raw, 64u));
                    code = true;
                }
                uint32_t va = loader_entry_point();
                if (va >= section.va && uint64_t(va - section.va) + 16 <= section.raw) {
                    spot_check(va, section, 16);
                    entry = true;
                }
            }
            check(code && entry, "section table locates executable bytes and the entry point");
            fclose(f);
        }

        // Test the zero-fill tails the image actually has. The loader writes
        // the TLS index after mapping, so those four bytes are no longer BSS.
        bool zeroed = true, has_tail = false;
        for (const auto &section : expect.sections) {
            has_tail |= section.vsize > section.raw;
            for (uint32_t i = section.raw; i < section.vsize; ++i) {
                uint32_t va = section.va + i;
                if (tls.index < TLS_SLOTS && va >= tls.index_addr && va - tls.index_addr < 4)
                    continue;
                zeroed &= rd8(va) == 0;
            }
        }
        if (has_tail)
            check(zeroed, "section tails past SizeOfRawData are zero filled");
        else {
            printf("  [SKIP] the image has no zero-fill section tails\n");
            ++g_skips;
        }

        uint32_t data_slots = 0;
        bool iat_ok = true, imports_weanetr = false;
        for (const auto &import : expect.imports) {
            std::string dll = import.dll;
            std::transform(dll.begin(), dll.end(), dll.begin(),
                           [](unsigned char ch) { return char(std::tolower(ch)); });
            imports_weanetr |= dll == "weanetr.dll";
            uint32_t data = imports_data_address(import.dll.c_str(), import.name.c_str());
            uint32_t trampoline = imports_trampoline_for(import.dll.c_str(), import.name.c_str());
            uint32_t value = rd32(import.slot);
            if (data) {
                ++data_slots;
                iat_ok &= value == data && !imports_is_trampoline(value);
            } else {
                iat_ok &= value == trampoline && imports_is_trampoline(value) &&
                          imports_describe(value) != nullptr;
            }
            if (value != (data ? data : trampoline))
                printf("  IAT mismatch at %08x for %s!%s\n", import.slot, import.dll.c_str(),
                       import.name.c_str());
        }
        check(iat_ok, "every PE import slot holds its symbol's trampoline or data storage");
        check(loader_iat_data_imports() == data_slots,
              "%u IAT slots hold data symbols (expected %u)", loader_iat_data_imports(),
              data_slots);

        // imports_has_dll reports the shim registry, not which DLLs this PE
        // imports. Require both so another image cannot use unallocated data.
        if (imports_weanetr && imports_has_dll("weanetr.dll")) {
            uint32_t guid = imports_data_address("weanetr.dll", "?BFAID_INet@@3U_GUID@@A");
            check(guid != 0 && !imports_is_trampoline(guid),
                  "weanetr!BFAID_INet resolved to guest storage at %08x", guid);
            bool guid_zero = guid != 0;
            for (int i = 0; guid && i < 16; ++i)
                guid_zero &= g_mem[guid + i] == 0;
            check(guid_zero, "the GUID storage is 16 zeroed bytes");
            uint32_t table = imports_data_address("weanetr.dll", "?options_to_parity_table@@3PAHA");
            check(table != 0 && heap_size(table) == 4096,
                  "options_to_parity_table has %u bytes of storage", heap_size(table));
        } else {
            printf("  [SKIP] the image imports no data symbols\n");
            ++g_skips;
        }
    }

    // TEB.
    check(rd32(0x0fe00000) == 0xffffffffu, "FS:[0] SEH head is -1");
    check(rd32(0x0fe00018) == 0x0fe00000u, "FS:[0x18] points at the TEB");
    check(rd32(0x0fe0002c) == 0x0fe01000u, "FS:[0x2c] points at the TLS array");
    check(loader_context()->fs_base == 0x0fe00000u, "fs_base is the TEB");
    const X86 *ic = loader_context();
    check(ic->fpu_cw == 0x037f && ic->fpu_sw == 0 && ic->fpu_tag == 0xffff && ic->fpu_top == 0,
          "the x87 starts at cw=%04x sw=%04x tag=%04x top=%u", ic->fpu_cw, ic->fpu_sw, ic->fpu_tag,
          ic->fpu_top);
    check(x86_get_eflags(ic) == 0x00000202u, "PUSHFD at process start reads %08x",
          x86_get_eflags(ic));
    check(loader_context()->r[R_ESP] > 0x0ef00000 && loader_context()->r[R_ESP] < 0x0f000000,
          "initial ESP %08x is inside the stack", loader_context()->r[R_ESP]);

    // A wrong image must be refused.
    char bad_dir[512];
    snprintf(bad_dir, sizeof bad_dir, "%s/recomp-bad-image-XXXXXX", os_temp_dir());
    if (check(os_mkdtemp(bad_dir) == 0, "created a scratch directory for the wrong image")) {
        std::string bad_path = std::string(bad_dir) + "/wrong.exe";
        FILE *bad = fopen(bad_path.c_str(), "wb");
        if (check(bad != nullptr, "created a deliberately invalid image")) {
            fputs("not the configured executable", bad);
            fclose(bad);
            check(!loader_load(bad_path.c_str()), "a different EXE is refused: %s", loader_error());
        }
        remove_tree(bad_dir);
    }
    check(loader_load(nullptr), "reloaded the correct image");
}

static void test_allocator() {
    section("allocator");
    check(heap_check().empty(), "the heap starts consistent with %u used blocks",
          heap_stats().used_blocks);

    uint32_t a = heap_alloc(100);
    uint32_t b = heap_alloc(100);
    uint32_t c = heap_alloc(100);
    check(a && b && c, "three 100-byte allocations: %08x %08x %08x", a, b, c);
    check((a & 15) == 0 && (b & 15) == 0 && (c & 15) == 0, "all are 16-byte aligned");
    check(b == a + 112 && c == b + 112, "blocks are packed with the size rounded to 16");
    check(heap_size(a) == 100, "heap_size reports the requested size %u", heap_size(a));

    memset(g_mem + a, 0xab, 100);
    uint32_t a2 = heap_realloc(a, 400);
    check(a2 != 0, "realloc 100 -> 400 gives %08x", a2);
    bool kept = true;
    for (int i = 0; i < 100; ++i)
        if (g_mem[a2 + i] != 0xab)
            kept = false;
    check(kept, "realloc preserved the original contents");

    check(heap_free(b), "freed the middle block");
    check(heap_free(c), "freed the third block");
    check(heap_check().empty(), "heap is consistent after coalescing: %s",
          heap_check().empty() ? "no gaps or adjacent free blocks" : heap_check().c_str());

    // a was freed by the realloc, and b and c have just been freed, so the
    // three coalesce into one hole starting at the old `a`.
    uint32_t d = heap_alloc(200);
    check(d == a, "first fit reused the coalesced hole at %08x", d);
    heap_free(d);
    heap_free(a2);
    check(heap_check().empty(), "heap is consistent after freeing everything");

    uint32_t page = heap_alloc(4096, true, 4096);
    check(page && (page & 4095) == 0, "4096-aligned allocation at %08x", page);
    bool zero = true;
    for (int i = 0; i < 4096; ++i)
        if (g_mem[page + i])
            zero = false;
    check(zero, "the zeroing allocation is zero filled");
    heap_free(page);

    HeapStats s = heap_stats();
    check(s.free_bytes > 0xc000000, "free bytes %llu after the churn",
          (unsigned long long)s.free_bytes);
    check(heap_alloc(0x0f000000) == 0, "an allocation larger than the arena fails cleanly");
    check(heap_alloc(0xffffffffu) == 0, "a 0xffffffff request is refused, not rounded to zero");
    uint32_t keep = heap_alloc(64);
    memset(g_mem + keep, 0x5a, 64);
    check(heap_realloc(keep, 0xffffffffu, true) == 0 && g_mem[keep] == 0x5a,
          "a 0xffffffff realloc is refused and leaves the block untouched");
    heap_free(keep);
    check(heap_check().empty(), "heap still consistent: %s",
          heap_check().empty() ? "yes" : heap_check().c_str());
}

static void test_heap_shims(X86 *c) {
    section("HeapAlloc / GlobalAlloc / VirtualAlloc shims");
    uint32_t h = call_import(c, "KERNEL32.dll", "HeapCreate", {0, 0x1000, 0});
    check(h != 0, "HeapCreate -> %08x", h);
    uint32_t p = call_import(c, "KERNEL32.dll", "HeapAlloc", {h, 8, 256});
    check(p != 0, "HeapAlloc(256) -> %08x", p);
    bool zeroed = true;
    for (int i = 0; i < 256; ++i)
        if (g_mem[p + i])
            zeroed = false;
    check(zeroed, "HEAP_ZERO_MEMORY produced a zeroed block");
    check(call_import(c, "KERNEL32.dll", "HeapSize", {h, 0, p}) == 256, "HeapSize reports 256");
    uint32_t p2 = call_import(c, "KERNEL32.dll", "HeapReAlloc", {h, 8, p, 1024});
    check(p2 != 0 && call_import(c, "KERNEL32.dll", "HeapSize", {h, 0, p2}) == 1024,
          "HeapReAlloc to 1024 -> %08x", p2);
    check(call_import(c, "KERNEL32.dll", "HeapFree", {h, 0, p2}) == 1, "HeapFree succeeded");

    uint32_t g = call_import(c, "KERNEL32.dll", "GlobalAlloc", {0x40, 64});
    check(g != 0, "GlobalAlloc(GMEM_ZEROINIT, 64) -> %08x", g);
    check(call_import(c, "KERNEL32.dll", "GlobalLock", {g}) == g, "GlobalLock returns the block");
    check(call_import(c, "KERNEL32.dll", "GlobalFree", {g}) == 0, "GlobalFree returns NULL");

    uint32_t v = call_import(c, "KERNEL32.dll", "VirtualAlloc", {0, 8192, 0x1000, 4});
    check(v != 0 && (v & 4095) == 0, "VirtualAlloc(8192) -> page-aligned %08x", v);
    check(call_import(c, "KERNEL32.dll", "VirtualFree", {v, 0, 0x8000}) == 1,
          "VirtualFree(MEM_RELEASE)");
}

static void test_memory_shims_2(X86 *c) {
    section("VirtualAlloc granularity, waits, modules, TLS, timers");
    uint32_t v1 = call_import(c, "KERNEL32.dll", "VirtualAlloc", {0, 1, 0x1000, 4});
    uint32_t v2 = call_import(c, "KERNEL32.dll", "VirtualAlloc", {0, 1, 0x1000, 4});
    check(v1 && v2 && (v1 & 4095) == 0 && (v2 & 4095) == 0 && (v2 - v1) >= 4096,
          "VirtualAlloc(1 byte) reserves a whole page: %08x then %08x", v1, v2);
    // Committing pages that are already committed must not disturb them.
    memset(g_mem + v1, 0xcd, 4096);
    check(call_import(c, "KERNEL32.dll", "VirtualAlloc", {v1, 4096, 0x1000, 4}) == v1 &&
              g_mem[v1] == 0xcd,
          "MEM_COMMIT over live pages leaves their contents alone");

    // A reservation commits to zeroed pages the first time.
    uint32_t v3 = call_import(c, "KERNEL32.dll", "VirtualAlloc", {0, 8192, 0x2000, 4});
    check(v3 != 0, "VirtualAlloc(MEM_RESERVE) -> %08x", v3);
    memset(g_mem + v3, 0xee, 8192);
    check(call_import(c, "KERNEL32.dll", "VirtualAlloc", {v3, 8192, 0x1000, 4}) == v3 &&
              g_mem[v3] == 0,
          "the first MEM_COMMIT of a reservation zeroes it");
    call_import(c, "KERNEL32.dll", "VirtualFree", {v3, 0, 0x8000});

    // A release of an interior pointer must fail and must leave the region
    // tracked, so a later decommit of the real base still works.
    uint32_t v4 = call_import(c, "KERNEL32.dll", "VirtualAlloc", {0, 8192, 0x1000, 4});
    check(v4 != 0, "VirtualAlloc(8192) -> %08x", v4);
    check(call_import(c, "KERNEL32.dll", "VirtualFree", {v4 + 4096, 0, 0x8000}) == 0,
          "MEM_RELEASE of an interior pointer fails");
    memset(g_mem + v4, 0x77, 8192);
    check(call_import(c, "KERNEL32.dll", "VirtualFree", {v4, 0, 0x4000}) == 1 && g_mem[v4] == 0 &&
              g_mem[v4 + 4096] == 0,
          "the region is still tracked, so decommitting its base succeeds");
    check(call_import(c, "KERNEL32.dll", "VirtualFree", {v4, 0, 0x8000}) == 1,
          "and releasing the base then succeeds");

    check(call_import(c, "KERNEL32.dll", "VirtualFree", {v1, 0, 0x4000}) == 1,
          "VirtualFree(MEM_DECOMMIT)");
    bool cleared = true;
    for (int i = 0; i < 4096; ++i)
        if (g_mem[v1 + i])
            cleared = false;
    check(cleared, "decommitted pages no longer hold their old contents");
    call_import(c, "KERNEL32.dll", "VirtualFree", {v1, 0, 0x8000});
    call_import(c, "KERNEL32.dll", "VirtualFree", {v2, 0, 0x8000});

    // WaitForMultipleObjects(wait all) must not consume anything when it fails.
    uint32_t s1 = call_import(c, "KERNEL32.dll", "CreateSemaphoreA", {0, 1, 4, 0});
    uint32_t s2 = call_import(c, "KERNEL32.dll", "CreateSemaphoreA", {0, 0, 4, 0});
    uint32_t arr = scratch_block(8);
    wr32(arr, s1);
    wr32(arr + 4, s2);
    check(call_import(c, "KERNEL32.dll", "WaitForMultipleObjects", {2, arr, 1, 0}) == 0x102,
          "wait-all on [signalled, unsignalled] reports WAIT_TIMEOUT");
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {s1, 0}) == 0,
          "the first semaphore still holds its count");
    call_import(c, "KERNEL32.dll", "ReleaseSemaphore", {s1, 1, 0});
    call_import(c, "KERNEL32.dll", "ReleaseSemaphore", {s2, 1, 0});
    check(call_import(c, "KERNEL32.dll", "WaitForMultipleObjects", {2, arr, 1, 0}) == 0,
          "wait-all succeeds once both are signalled");
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {s1, 0}) == 0x102 &&
              call_import(c, "KERNEL32.dll", "WaitForSingleObject", {s2, 0}) == 0x102,
          "and it consumed both");

    // LoadLibraryA only succeeds for modules the runtime can serve.
    check(call_import(c, "KERNEL32.dll", "LoadLibraryA", {put_str("unregistered.dll")}) == 0,
          "LoadLibraryA(\"unregistered.dll\") fails: no shims for it");
    uint32_t hmod = call_import(c, "KERNEL32.dll", "LoadLibraryA", {put_str("winmm.dll")});
    check(hmod != 0, "LoadLibraryA(\"winmm.dll\") -> %08x", hmod);
    uint32_t proc =
        call_import(c, "KERNEL32.dll", "GetProcAddress", {hmod, put_str("timeGetTime")});
    check(proc == imports_trampoline_for("WINMM.dll", "timeGetTime"),
          "GetProcAddress returned the timeGetTime trampoline");

    uint32_t slot = call_import(c, "KERNEL32.dll", "TlsAlloc", {});
    call_import(c, "KERNEL32.dll", "TlsSetValue", {slot, 0x99});
    check(call_import(c, "KERNEL32.dll", "TlsFree", {slot}) == 1 &&
              call_import(c, "KERNEL32.dll", "TlsGetValue", {slot}) == 0,
          "TlsFree clears the slot");
    check(call_import(c, "KERNEL32.dll", "TlsAlloc", {}) == slot,
          "the freed index is handed straight back out");
    call_import(c, "KERNEL32.dll", "TlsFree", {slot});
    bool exhausted = false;
    for (int i = 0; i < 200; ++i) {
        uint32_t s = call_import(c, "KERNEL32.dll", "TlsAlloc", {});
        if (s == 0xffffffffu) {
            exhausted = true;
            break;
        }
        call_import(c, "KERNEL32.dll", "TlsFree", {s});
    }
    check(!exhausted, "200 allocate/free cycles do not exhaust the 64 slots");

    uint32_t li = scratch_block(8);
    check(call_import(c, "KERNEL32.dll", "QueryPerformanceFrequency", {li}) == 1 &&
              rd32(li) == 1000000,
          "QueryPerformanceFrequency reports 1 MHz");
    check(call_import(c, "KERNEL32.dll", "QueryPerformanceCounter", {li}) == 1,
          "QueryPerformanceCounter");

    // The wide environment block must be UTF-16, not ANSI bytes.
    uint32_t wenv = call_import(c, "KERNEL32.dll", "GetEnvironmentStringsW", {});
    check(wenv != 0 && rd16(wenv) == 'P' && rd16(wenv + 2) == 'A' && rd16(wenv + 4) == 'T',
          "GetEnvironmentStringsW returns wide characters");
    uint32_t aenv = call_import(c, "KERNEL32.dll", "GetEnvironmentStrings", {});
    check(aenv != wenv && rd8(aenv) == 'P' && rd8(aenv + 1) == 'A',
          "GetEnvironmentStrings returns a separate ANSI block");
}

// The executable's own version resource, as VERSION.dll would serve it: a
// game that shows its version reads it from here. The test finds RT_VERSION
// in the mapped image's resource directory itself, so it knows whether the
// image has one before it asks the shims.
static bool image_has_version_resource() {
    uint32_t base = loader_image_base();
    uint32_t pe = rd32(base + 0x3c);
    uint32_t rsrc = rd32(base + pe + 24 + 96 + 2 * 8);
    if (!rsrc)
        return false;
    uint32_t dir = base + rsrc;
    uint32_t named = rd16(dir + 12), ids = rd16(dir + 14);
    for (uint32_t k = 0; k < named + ids; ++k)
        if (rd32(dir + 16 + 8 * k) == 16) // RT_VERSION
            return true;
    return false;
}

static void test_version_resource(X86 *c) {
    section("VERSION.dll");
    uint32_t name = put_str(RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE);
    uint32_t handle = scratch_block(4);
    uint32_t size = call_import(c, "VERSION.dll", "GetFileVersionInfoSizeA", {name, handle});
    if (!image_has_version_resource()) {
        check(size == 0 && get_last_error() == 1813,
              "an image without a version resource: size 0, ERROR_RESOURCE_TYPE_NOT_FOUND");
        return;
    }
    check(size > 0x34, "GetFileVersionInfoSizeA reports the resource (%u bytes)", size);
    uint32_t block = scratch_block(size + 16);
    check(call_import(c, "VERSION.dll", "GetFileVersionInfoA", {name, 0, size, block}) == 1,
          "GetFileVersionInfoA copies it");
    uint32_t pval = scratch_block(4), plen = scratch_block(4);
    uint32_t root = put_str("\\");
    check(call_import(c, "VERSION.dll", "VerQueryValueA", {block, root, pval, plen}) == 1 &&
              rd32(plen) == 0x34 && rd32(rd32(pval)) == 0xFEEF04BDu,
          "VerQueryValueA(\"\\\\\") finds VS_FIXEDFILEINFO by its signature");
    uint32_t tr = put_str("\\VarFileInfo\\Translation");
    check(call_import(c, "VERSION.dll", "VerQueryValueA", {block, tr, pval, plen}) == 1 &&
              rd32(plen) >= 4,
          "the translation table is there");
    uint32_t lang = rd32(rd32(pval));
    char key[64];
    snprintf(key, sizeof key, "\\StringFileInfo\\%04x%04x\\FileDescription", lang & 0xffff,
             lang >> 16);
    uint32_t sk = put_str(key);
    check(call_import(c, "VERSION.dll", "VerQueryValueA", {block, sk, pval, plen}) == 1 &&
              rd32(plen) > 1 && !gm_str(rd32(pval)).empty(),
          "a string value is found through its translation: \"%s\"", gm_str(rd32(pval)).c_str());
    uint32_t bogus = put_str("\\StringFileInfo\\040904b0\\NoSuchKey");
    check(call_import(c, "VERSION.dll", "VerQueryValueA", {block, bogus, pval, plen}) == 0,
          "an absent key is refused");
    uint32_t other = put_str("C:\\somewhere\\else.exe");
    check(call_import(c, "VERSION.dll", "GetFileVersionInfoSizeA", {other, handle}) == 0,
          "another file has no version resource here");
}

static void test_files(X86 *c) {
    section("file layer");
    // Every game has its configured executable. Flip each ASCII letter's
    // case to exercise the guest resolver on case-sensitive host filesystems.
    std::string flipped = RECOMP_EXECUTABLE;
    for (char &ch : flipped) {
        unsigned char byte = ch;
        ch = char(std::islower(byte) ? std::toupper(byte) : std::tolower(byte));
    }
    uint32_t name = put_str(flipped.c_str());
    uint32_t h =
        call_import(c, "KERNEL32.dll", "CreateFileA", {name, 0x80000000u, 1, 0, 3, 0x80, 0});
    check(h != 0xffffffffu, "CreateFileA(\"%s\") -> handle %08x", flipped.c_str(), h);

    OsStat st{};
    check(os_stat(RECOMP_DEVELOPER_EXE, &st) == 0, "host executable is available for comparison");
    uint32_t size = call_import(c, "KERNEL32.dll", "GetFileSize", {h, 0});
    check(size == (uint32_t)st.size, "GetFileSize reports %u, host file is %lld", size,
          (long long)st.size);

    uint32_t buf = scratch_block(256), read_count = scratch_block(4);
    check(call_import(c, "KERNEL32.dll", "ReadFile", {h, buf, 64, read_count, 0}) == 1,
          "ReadFile of 64 bytes succeeded");
    check(rd32(read_count) == 64, "ReadFile reported 64 bytes");
    FILE *f = fopen(RECOMP_DEVELOPER_EXE, "rb");
    uint8_t host[64];
    size_t got = f ? fread(host, 1, 64, f) : 0;
    if (f)
        fclose(f);
    check(got == 64 && memcmp(host, g_mem + buf, 64) == 0, "the bytes match the host file");

    check(call_import(c, "KERNEL32.dll", "SetFilePointer", {h, 16, 0, 0}) == 16,
          "SetFilePointer to 16");
    check(call_import(c, "KERNEL32.dll", "ReadFile", {h, buf, 8, read_count, 0}) == 1 &&
              memcmp(host + 16, g_mem + buf, 8) == 0,
          "reading after the seek returns offset 16");
    check(call_import(c, "KERNEL32.dll", "CloseHandle", {h}) == 1, "CloseHandle");

    uint32_t missing = put_str("data\\NO_SUCH_FILE.DAT");
    check(call_import(c, "KERNEL32.dll", "CreateFileA", {missing, 0x80000000u, 1, 0, 3, 0x80, 0}) ==
              0xffffffffu,
          "a missing file gives INVALID_HANDLE_VALUE");
    check(call_import(c, "KERNEL32.dll", "GetLastError", {}) == 2,
          "GetLastError is ERROR_FILE_NOT_FOUND");

    uint32_t dirname = put_str(RECOMP_GUEST_ROOT);
    uint32_t attributes = call_import(c, "KERNEL32.dll", "GetFileAttributesA", {dirname});
    check(attributes != 0xffffffffu && (attributes & 0x10),
          "GetFileAttributesA of the executable's directory reports a directory");

    // The executable's stem may also name logs or configuration files. Walk
    // until exhaustion instead of assuming exactly two matches or their order.
    std::string stem = RECOMP_EXECUTABLE;
    size_t extension = stem.find_last_of('.');
    if (extension != std::string::npos)
        stem.erase(extension);
    std::string glob = stem + ".*";
    uint32_t pattern = put_str(glob.c_str());
    uint32_t fd = scratch_block(0x140);
    uint32_t fh = call_import(c, "KERNEL32.dll", "FindFirstFileA", {pattern, fd});
    check(fh != 0xffffffffu, "FindFirstFileA(\"%s\") -> %08x", glob.c_str(), fh);
    if (fh != 0xffffffffu) {
        std::set<std::string> found;
        bool unique = true, matches = true;
        do {
            std::string name = gm_str(fd + 44);
            unique &= found.insert(name).second;
            std::string prefix = stem + ".";
            matches &= name.size() >= prefix.size() &&
                       std::equal(prefix.begin(), prefix.end(), name.begin(),
                                  [](unsigned char a, unsigned char b) {
                                      return std::tolower(a) == std::tolower(b);
                                  });
            if (!unique) // fail instead of hanging on a broken enumerator
                break;
        } while (call_import(c, "KERNEL32.dll", "FindNextFileA", {fh, fd}));
        check(found.count(RECOMP_EXECUTABLE) != 0,
              "file enumeration includes the configured executable %s", RECOMP_EXECUTABLE);
        check(unique && matches, "all %zu enumerated names match the stem and occur once",
              found.size());
        check(call_import(c, "KERNEL32.dll", "GetLastError", {}) == 18,
              "FindNextFileA ends with ERROR_NO_MORE_FILES");
        check(call_import(c, "KERNEL32.dll", "FindNextFileA", {fh, fd}) == 0,
              "exhausted FindNextFileA keeps reporting no more files");
        check(call_import(c, "KERNEL32.dll", "FindClose", {fh}) == 1, "FindClose succeeds");
    }

    // Guest-visible paths.
    uint32_t pathbuf = scratch_block(300);
    uint32_t n = call_import(c, "KERNEL32.dll", "GetModuleFileNameA", {0, pathbuf, 260});
    check(gm_str(pathbuf) == RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE,
          "GetModuleFileNameA -> \"%s\" (%u chars)", gm_str(pathbuf).c_str(), n);
    call_import(c, "KERNEL32.dll", "GetCurrentDirectoryA", {260, pathbuf});
    check(gm_str(pathbuf) == RECOMP_GUEST_ROOT, "GetCurrentDirectoryA -> \"%s\"",
          gm_str(pathbuf).c_str());

    // The path GetModuleFileNameA hands out must open, whatever the guest
    // root's shape: a game installed under C:\GOG Games\<name> spells its
    // own files through two root components, not one.
    uint32_t absolute = put_str(RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE);
    uint32_t ah =
        call_import(c, "KERNEL32.dll", "CreateFileA", {absolute, 0x80000000u, 1, 0, 3, 0x80, 0});
    check(ah != 0xffffffffu, "an absolute path through the whole guest root opens: \"%s\"",
          RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE);
    if (ah != 0xffffffffu)
        call_import(c, "KERNEL32.dll", "CloseHandle", {ah});
}

// The imports a Visual C++ 6 CRT's start-up and a windowed game's first
// frame reach before any DirectDraw: each has a fixed stdcall argument count
// the trampoline must pop, which call_import checks through ESP.
static void test_boot_shims(X86 *c) {
    section("boot-path shims: CRT locale, shell folders, window metrics");
    uint32_t buf = scratch_block(300);
    check(call_import(c, "KERNEL32.dll", "GetEnvironmentVariableA", {put_str("MAJX"), buf, 300}) ==
                  0 &&
              call_import(c, "KERNEL32.dll", "GetLastError", {}) == 203,
          "GetEnvironmentVariableA of an unset name returns 0 with ERROR_ENVVAR_NOT_FOUND");
    check(call_import(c, "KERNEL32.dll", "GetUserDefaultLCID", {}) == 0x0409,
          "GetUserDefaultLCID is en-US");
    check(call_import(c, "KERNEL32.dll", "IsValidCodePage", {1252}) == 1 &&
              call_import(c, "KERNEL32.dll", "IsValidCodePage", {12345}) == 0,
          "IsValidCodePage knows the Windows code pages and refuses an invented one");
    check(call_import(c, "KERNEL32.dll", "IsValidLocale", {0x0409, 1}) == 1,
          "IsValidLocale accepts en-US");
    check(call_import(c, "KERNEL32.dll", "EnumSystemLocalesA", {0, 1}) == 1,
          "EnumSystemLocalesA succeeds without calling back");
    uint32_t path = scratch_block(260);
    check(call_import(c, "SHELL32.dll", "SHGetSpecialFolderPathA", {0, path, 5, 0}) == 1 &&
              gm_str(path).rfind(RECOMP_GUEST_ROOT, 0) == 0 &&
              gm_str(path).size() > strlen(RECOMP_GUEST_ROOT),
          "SHGetSpecialFolderPathA(CSIDL_PERSONAL) names a directory under the guest root: "
          "\"%s\"",
          gm_str(path).c_str());
    check(call_import(c, "USER32.dll", "IsWindowUnicode", {0x10001}) == 0,
          "IsWindowUnicode: every window is ANSI");
    uint32_t cx = call_import(c, "USER32.dll", "GetSystemMetrics", {0});
    uint32_t cy = call_import(c, "USER32.dll", "GetSystemMetrics", {1});
    check(cx >= 640 && cy >= 480 && cx > cy, "GetSystemMetrics reports a screen of %ux%u", cx, cy);
    check(call_import(c, "USER32.dll", "GetSystemMetrics", {4}) > 0,
          "SM_CYCAPTION is a caption height");
    check(call_import(c, "USER32.dll", "LoadCursorA", {0, 0x7f00}) != 0,
          "LoadCursorA(IDC_ARROW) hands out a handle");
    // The command line is the executable's path, plus whatever switches the
    // developer asks for through RECOMP_GUEST_ARGS: a game's own -debugout or
    // -nointro, which are how it is told to write its log or skip its intro.
    os_setenv("RECOMP_GUEST_ARGS", "-debugout -nointro");
    win32_reset_command_line_for_test();
    std::string cmdline = gm_str(call_import(c, "KERNEL32.dll", "GetCommandLineA", {}));
    check(cmdline == std::string(RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE) + " -debugout -nointro",
          "GetCommandLineA appends RECOMP_GUEST_ARGS: \"%s\"", cmdline.c_str());
    os_unsetenv("RECOMP_GUEST_ARGS");
    win32_reset_command_line_for_test();
    uint32_t ms = scratch_block(32);
    wr32(ms, 32);
    call_import(c, "KERNEL32.dll", "GlobalMemoryStatus", {ms});
    check(rd32(ms + 8) >= 256u * 1024 * 1024 && rd32(ms + 12) > 0 &&
              rd32(ms + 12) <= rd32(ms + 8) && rd32(ms + 4) <= 100 &&
              rd32(ms + 24) >= rd32(ms + 28),
          "GlobalMemoryStatus reports %u MB physical, %u MB free, load %u%%", rd32(ms + 8) >> 20,
          rd32(ms + 12) >> 20, rd32(ms + 4));
}

// ---------------------------------------------------------------------------
// MIDI out, with a synth that records rather than sounds.
//
// The game's music is MIDI: 0x575e40 walks the midiOut devices, calls
// midiOutGetDevCapsA on each, and keeps the first whose szPname begins with
// the nine characters "SoundFont". If none does it returns -1 and there is no
// music at all, so the device name is not decoration - it is the whole gate.
// Then it opens with a null callback, sends a twelve-byte sysex through
// PrepareHeader and LongMsg, and plays note by note with ShortMsg.
// ---------------------------------------------------------------------------
static std::vector<uint32_t> g_midi_shorts;
static std::vector<std::vector<uint8_t>> g_midi_sysexes;
static int g_midi_opens = 0, g_midi_closes = 0, g_midi_resets = 0;
static std::string g_midi_sf2;
static bool g_midi_have_synth = true;

extern "C" {
int host_midi_open(const char *path) {
    ++g_midi_opens;
    g_midi_sf2 = path ? path : "";
    return g_midi_have_synth ? 1 : 0;
}
void host_midi_short(uint32_t msg) {
    g_midi_shorts.push_back(msg);
}
void host_midi_sysex(const void *data, uint32_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    g_midi_sysexes.push_back(std::vector<uint8_t>(p, p + bytes));
}
void host_midi_reset(void) {
    ++g_midi_resets;
}
void host_midi_close(void) {
    ++g_midi_closes;
}
}

static void test_midi(X86 *c) {
    section("MIDI out");
    g_midi_shorts.clear();
    g_midi_sysexes.clear();
    g_midi_opens = g_midi_closes = g_midi_resets = 0;

    check(call_import(c, "WINMM.dll", "midiOutGetNumDevs", {}) == 1,
          "midiOutGetNumDevs reports one device, without which the game never asks again");

    // The name gate, exactly as 0x575e40 applies it.
    uint32_t caps = scratch_block(64);
    for (uint32_t i = 0; i < 64; ++i)
        wr8(caps + i, 0xcd);
    check(call_import(c, "WINMM.dll", "midiOutGetDevCapsA", {0, caps, 52}) == 0,
          "midiOutGetDevCapsA succeeds for device 0");
    char name[10] = {0};
    for (int i = 0; i < 9; ++i)
        name[i] = (char)rd8(caps + 8 + (uint32_t)i);
    check(strncmp(name, "SoundFont", 9) == 0,
          "the device is named \"%s...\", which is what the game looks for", name);
    check(rd16(caps + 40) == 7, "wTechnology is MOD_SWSYNTH");
    check(rd16(caps + 46) == 0xffff, "every channel is available");
    check(rd8(caps + 52) == 0xcd, "nothing was written past the 52 bytes it asked for");
    check(call_import(c, "WINMM.dll", "midiOutGetDevCapsA", {1, caps, 52}) == 2,
          "a second device is MMSYSERR_BADDEVICEID");

    // Open, as the game opens it: device 0, no callback.
    uint32_t phmo = scratch_block(4);
    check(call_import(c, "WINMM.dll", "midiOutOpen", {phmo, 0, 0, 0, 0}) == 0,
          "midiOutOpen succeeds");
    uint32_t hmo = rd32(phmo);
    check(hmo != 0, "and hands back a handle");
    check(g_midi_opens == 1, "the host synth was asked to open once");

    // All notes off on channel 0, which is 0x576140's own message.
    check(call_import(c, "WINMM.dll", "midiOutShortMsg", {hmo, 0x00007bb0}) == 0,
          "midiOutShortMsg accepts a short message");
    check(g_midi_shorts.size() == 1 && g_midi_shorts[0] == 0x00007bb0,
          "and forwards it packed, unaltered");
    check(call_import(c, "WINMM.dll", "midiOutShortMsg", {hmo + 1, 0x403c90}) != 0,
          "a message on a handle that was never opened is refused");

    // The sysex path: a twelve-byte buffer, which is the length the game uses.
    uint32_t buf = scratch_block(16);
    const uint8_t sysex[12] = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7,
                               0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    for (uint32_t i = 0; i < 12; ++i)
        wr8(buf + i, sysex[i]);
    uint32_t hdr = scratch_block(64);
    for (uint32_t i = 0; i < 64; ++i)
        wr8(hdr + i, 0);
    wr32(hdr + 0, buf);
    wr32(hdr + 4, 12);

    check(call_import(c, "WINMM.dll", "midiOutLongMsg", {hmo, hdr, 64}) == 64,
          "an unprepared header is MIDIERR_UNPREPARED");
    check(g_midi_sysexes.empty(), "and nothing was sent");
    check(call_import(c, "WINMM.dll", "midiOutPrepareHeader", {hmo, hdr, 64}) == 0,
          "midiOutPrepareHeader succeeds");
    check((rd32(hdr + 16) & 0x2) != 0, "and sets MHDR_PREPARED");
    check(call_import(c, "WINMM.dll", "midiOutLongMsg", {hmo, hdr, 64}) == 0,
          "midiOutLongMsg accepts the prepared header");
    check(g_midi_sysexes.size() == 1 && g_midi_sysexes[0].size() == 12 &&
              g_midi_sysexes[0][0] == 0xf0 && g_midi_sysexes[0][11] == 0x66,
          "and the twelve bytes reach the synth exactly");
    check((rd32(hdr + 16) & 0x1) != 0,
          "MHDR_DONE is set, which is how the guest knows it may reuse the buffer");
    check(call_import(c, "WINMM.dll", "midiOutUnprepareHeader", {hmo, hdr, 64}) == 0 &&
              (rd32(hdr + 16) & 0x2) == 0,
          "unpreparing clears MHDR_PREPARED");

    check(call_import(c, "WINMM.dll", "midiOutReset", {hmo}) == 0 && g_midi_resets == 1,
          "midiOutReset reaches the synth");
    check(call_import(c, "WINMM.dll", "midiOutClose", {hmo}) == 0 && g_midi_closes == 1,
          "midiOutClose closes it");
    check(call_import(c, "WINMM.dll", "midiOutShortMsg", {hmo, 0x403c90}) != 0,
          "and nothing is accepted afterwards");

    // A host with no synth still opens. The game has one MIDI path and no
    // fallback, so failing the open loses the music and gains nothing.
    g_midi_have_synth = false;
    check(call_import(c, "WINMM.dll", "midiOutOpen", {phmo, 0, 0, 0, 0}) == 0,
          "the open succeeds even when the host has no synth");
    check(call_import(c, "WINMM.dll", "midiOutShortMsg", {rd32(phmo), 0x403c90}) == 0,
          "and the messages are accepted and not heard");
    call_import(c, "WINMM.dll", "midiOutClose", {rd32(phmo)});
    g_midi_have_synth = true;
}

// One pinned clock, in the runtime, so the parity fixture and every host pin
// the same counter. Two counters with one name is how a run ends up described
// as pinned while something still reads the wall.
//
// The description is asserted as hard as the counter. A run record that says
// "monotonic" about a pinned run makes two incomparable runs look comparable,
// and installing a time source is not the same as pinning one - the boot hosts
// install a source that reads the real clock.
static void test_pinned_clock(X86 *c) {
    section("pinned clock");
    (void)c;
    host_set_time_source_pinned(100, 50);
    check(host_time_source_is_pinned(), "a pin is installed");
    check(host_millis() == 100u, "it starts where it was told, %u", host_millis());
    check(host_millis() == 100u, "and does not move between reads on its own");

    host_pinned_clock_advance();
    check(host_millis() == 150u, "one advance is one step, %u", host_millis());
    host_pinned_clock_advance();
    host_pinned_clock_advance();
    check(host_millis() == 250u, "and each advance is one more, %u", host_millis());
    check(host_pinned_clock_value() == host_millis(),
          "the counter and the clock the guest reads are the same number");

    check(strcmp(host_clock_description(), "pinned start=100 step=50") == 0,
          "installing it describes it: \"%s\"", host_clock_description());

    // A second pin replaces the first, counter and description together. If
    // the description could lag, a record would name the wrong pin.
    host_set_time_source_pinned(0, 16);
    check(host_millis() == 0u, "a second pin restarts at its own start");
    host_pinned_clock_advance();
    check(host_millis() == 16u, "and steps by its own step, %u", host_millis());
    check(strcmp(host_clock_description(), "pinned start=0 step=16") == 0,
          "and renames itself: \"%s\"", host_clock_description());

    // Leave NOTHING installed. This suite shares a process with tests that
    // wait on time, and a pinned clock nobody advances does not merely change
    // what they see - it stops. Every timed wait becomes eternal and every
    // spin on the clock becomes infinite. The first version of this test put
    // the fixture's 100/50 back "so as not to change what later tests see",
    // which froze the clock for all of them and hung the suite for fifteen
    // minutes while it held the build lock.
    host_clear_time_source();
    check(!host_time_source_is_pinned(), "the pin is gone when this test ends");
    check(strcmp(host_clock_description(), "monotonic") == 0,
          "and the clock is described as what it is again");
    uint32_t a = host_millis();
    uint32_t b = host_millis();
    check(b >= a, "the real clock is back and does not go backwards");
}

// The cadence trace: how often the guest asks the time, and how often anything
// ticks. Intervals rather than absolute times, on the guest's clock rather than
// the wall, so that two runs are comparable at all - see the note in misc.cpp.
static void test_cadence_trace(X86 *c) {
    section("cadence trace");
    const char *path = "build/recomp/cadence-test.log";
    os_unlink(path);
    host_set_cadence_trace(path);

    // Two reads of the clock with a known gap between them, and the gap is
    // MADE rather than waited for. The first version of this spun until the
    // clock had moved 3 ms, which is an infinite loop the moment anything has
    // pinned the clock - and the test above it had. A test that waits for a
    // clock it does not itself advance is a test that can hang, whatever it
    // is testing.
    host_set_time_source_pinned(1000, 50);
    call_import(c, "WINMM.dll", "timeGetTime", {});
    host_pinned_clock_advance();
    call_import(c, "WINMM.dll", "timeGetTime", {});

    // A window timer message, which nothing in this game posts. The seam is
    // real even though the game never uses it, and the test says so.
    //
    // Then the queue is drained, because these are real messages in the real
    // queue that the window tests later in this suite peek at. The first
    // version of this left them there and five checks in test_windows failed -
    // "PeekMessageA on an empty queue returns FALSE" found my WM_TIMER instead.
    // That is the same mistake as leaving a pinned clock installed, one file
    // over: a test that leaves state behind breaks whoever runs next, and the
    // failure surfaces far from its cause.
    host_post_message(0, 0x0113, 0, 0);
    host_post_message(0, 0x0113, 0, 0);
    uint32_t msgbuf = scratch_block(28);
    int drained = 0;
    while (call_import(c, "USER32.dll", "PeekMessageA", {msgbuf, 0, 0, 0, 1}) == 1)
        if (++drained > 8)
            break; // bounded: never spin on a queue
    check(drained == 2, "the two WM_TIMERs were taken back off the queue (%d)", drained);
    check(call_import(c, "USER32.dll", "PeekMessageA", {msgbuf, 0, 0, 0, 0}) == 0,
          "and this test leaves the queue as it found it");

    host_set_cadence_trace(nullptr);

    FILE *f = fopen(path, "r");
    check(f != nullptr, "the trace file was written");
    std::string log;
    if (f) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            log.append(buf, n);
        fclose(f);
    }
    check(log.find("timeGetTime,50") != std::string::npos,
          "it records the interval exactly: one pinned step between the reads");
    check(log.find("WM_TIMER,") != std::string::npos, "it records a WM_TIMER interval");

    // One line per interval, not per event: two events of a kind make one
    // line, because there is no interval before the first.
    size_t lines = 0;
    for (char ch : log)
        if (ch == '\n')
            ++lines;
    check(lines == 2, "two events of each of two kinds make two lines, not four (%zu)", lines);

    // Closing it stops the writing. A trace that kept growing after the run
    // that asked for it would put another run's cadence in the same file.
    size_t was = log.size();
    call_import(c, "WINMM.dll", "timeGetTime", {});
    f = fopen(path, "r");
    size_t now = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        now = (size_t)ftell(f);
        fclose(f);
    }
    check(now == was, "nothing is written after the trace is closed");

    // TRACING MUST NOT CHANGE PACING.
    //
    // A boot host's time source counts a poll on every read and takes a
    // stall-breaking step after 256 of them. If the trace timestamped its
    // entries by reading that source, a traced run would pace differently from
    // an untraced one and the trace would be measuring its own effect. This
    // counts the source's calls directly: noting cadence must not touch it.
    // This check depends on the clock being PINNED: unpinned, host_note_cadence
    // reads host_millis() by design, because boot_clock_poll does nothing when
    // there is no pin and there is no other way to get a real time. So the
    // assertion below is about the pinned path, and it is worthless if the pin
    // is not installed - which is asserted rather than assumed.
    static int source_calls = 0;
    source_calls = 0;
    host_set_time_source_pinned(700, 50);
    check(host_time_source_is_pinned(),
          "the pin is installed, which is what makes the next check meaningful");
    host_set_time_source([]() -> uint32_t {
        ++source_calls;
        return 4242u;
    });
    check(host_time_source_is_pinned(),
          "and installing a counting source over it leaves the pin flag set");
    host_set_cadence_trace(path);
    int before_calls = source_calls;
    for (int i = 0; i < 300; ++i)
        host_note_cadence("timeGetTime");
    check(source_calls == before_calls, "300 traced events read the time source %d times, not %d",
          source_calls - before_calls, 0);
    host_set_cadence_trace(nullptr);
    host_clear_time_source();

    // And a pinned clock does not move because something was traced.
    host_set_time_source_pinned(500, 50);
    host_set_cadence_trace(path);
    for (int i = 0; i < 300; ++i)
        host_note_cadence("GetTickCount");
    check(host_millis() == 500u, "the pinned clock is where it was, %u", host_millis());
    host_set_cadence_trace(nullptr);
    host_clear_time_source();

    // A path that cannot be opened is not fatal: a run that cannot write its
    // trace is still a run, and losing it is better than losing the run. The
    // assertion is that the run CONTINUES and the trace is off, not the
    // check(true) this used to be, which could not fail.
    host_set_cadence_trace("build/recomp/no-such-dir/cadence.log");
    uint32_t before_clock = host_millis();
    call_import(c, "WINMM.dll", "timeGetTime", {});
    host_note_cadence("GetTickCount");
    check(host_millis() >= before_clock,
          "the clock still runs after a trace that could not be opened");
    OsStat no_such{};
    check(os_stat("build/recomp/no-such-dir/cadence.log", &no_such) != 0,
          "and no trace file appeared where one could not be created");
    host_set_cadence_trace(nullptr);
    host_clear_time_source();
    check(!host_time_source_is_pinned(), "and this test leaves no pin behind either");
    os_unlink(path);
}

static void test_misc_shims(X86 *c) {
    section("time, TLS, semaphores, strings");
    uint32_t t0 = call_import(c, "KERNEL32.dll", "GetTickCount", {});
    uint32_t t1 = call_import(c, "WINMM.dll", "timeGetTime", {});
    check(t1 >= t0 && t1 - t0 < 1000, "GetTickCount %u and timeGetTime %u share one clock", t0, t1);

    uint32_t slot = call_import(c, "KERNEL32.dll", "TlsAlloc", {});
    check(slot < 64, "TlsAlloc -> slot %u", slot);
    call_import(c, "KERNEL32.dll", "TlsSetValue", {slot, 0xdeadbeef});
    check(call_import(c, "KERNEL32.dll", "TlsGetValue", {slot}) == 0xdeadbeef, "TLS round trip");

    uint32_t sem = call_import(c, "KERNEL32.dll", "CreateSemaphoreA", {0, 1, 4, 0});
    check(sem != 0, "CreateSemaphoreA(initial 1, max 4) -> %08x", sem);
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {sem, 0}) == 0,
          "the first wait takes the count");
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {sem, 0}) == 0x102,
          "the second wait reports WAIT_TIMEOUT");
    uint32_t prev = scratch_block(4);
    check(call_import(c, "KERNEL32.dll", "ReleaseSemaphore", {sem, 1, prev}) == 1 &&
              rd32(prev) == 0,
          "ReleaseSemaphore restores the count, previous %u", rd32(prev));
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {sem, 0}) == 0,
          "the wait succeeds again after the release");

    uint32_t osvi = scratch_block(160);
    wr32(osvi, 148);
    check(call_import(c, "KERNEL32.dll", "GetVersionExA", {osvi}) == 1 && rd32(osvi + 4) == 4 &&
              rd32(osvi + 8) == 10 && rd32(osvi + 16) == 1,
          "GetVersionExA reports Windows 98 SE (4.10, platform 1)");
    check(call_import(c, "KERNEL32.dll", "GetProcessHeap", {}) != 0, "GetProcessHeap");
    check(call_import(c, "KERNEL32.dll", "IsBadCodePtr", {loader_entry_point()}) == 0 &&
              call_import(c, "KERNEL32.dll", "IsBadCodePtr", {loader_image_limit()}) == 1,
          "IsBadCodePtr uses the image bounds from the PE headers");

    uint32_t s1 = put_str("RuntimeX");
    check(call_import(c, "KERNEL32.dll", "lstrlenA", {s1}) == 8, "lstrlenA");
    uint32_t dst = scratch_block(64);
    call_import(c, "KERNEL32.dll", "lstrcpyA", {dst, s1});
    uint32_t s2 = put_str(" TB");
    call_import(c, "KERNEL32.dll", "lstrcatA", {dst, s2});
    check(gm_str(dst) == "RuntimeX TB", "lstrcpyA + lstrcatA -> \"%s\"", gm_str(dst).c_str());

    uint32_t fmt = put_str("%s has %d units (%04x)");
    uint32_t va = scratch_block(16);
    wr32(va + 0, put_str("blue"));
    wr32(va + 4, 12);
    wr32(va + 8, 0x2a);
    uint32_t out = scratch_block(128);
    call_import(c, "USER32.dll", "wvsprintfA", {out, fmt, va});
    check(gm_str(out) == "blue has 12 units (002a)", "wvsprintfA -> \"%s\"", gm_str(out).c_str());
}

// What a C++ throw looks like from the runtime: the MSVC exception record
// names the type through its throw info, the object usually carries a
// message, and the EBP chain names where it came from. This is what the
// RaiseException shim prints before it gives up, so a throw in a game whose
// exceptions the runtime cannot unwind still says what went wrong.
static void test_cxx_throw_description(X86 *c) {
    section("describing a C++ exception record");
    // TypeDescriptor: vtable, spare, then the mangled name in place.
    uint32_t type = scratch_block(48);
    wr32(type, 0);
    wr32(type + 4, 0);
    memcpy(g_mem + type + 8, ".?AVGameException@@", 20);
    uint32_t catchable = scratch_block(28); // properties, pType, thisDisplacement...
    wr32(catchable, 0);
    wr32(catchable + 4, type);
    uint32_t array = scratch_block(8); // nCatchableTypes, arrayOfCatchableTypes[]
    wr32(array, 1);
    wr32(array + 4, catchable);
    uint32_t throw_info =
        scratch_block(16); // attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray
    wr32(throw_info, 0);
    wr32(throw_info + 4, 0);
    wr32(throw_info + 8, 0);
    wr32(throw_info + 12, array);
    uint32_t what = put_str("Quest file not found: Quests\\random.q");
    uint32_t object = scratch_block(12); // vtable, char *what, int owns
    wr32(object, 0x00680000);
    wr32(object + 4, what);
    wr32(object + 8, 1);
    uint32_t args = scratch_block(12);
    wr32(args, 0x19930520);
    wr32(args + 4, object);
    wr32(args + 8, throw_info);
    std::string text = win32_describe_cxx_throw(0xe06d7363, 3, args);
    check(text.find("GameException") != std::string::npos, "the type is named: %s", text.c_str());
    check(text.find("Quest file not found") != std::string::npos, "the message is quoted");
    // A class of the game's own keeps its text wherever it likes: every dword
    // of the object that points at text is quoted too, with its offset.
    uint32_t line_text = put_str("GplError: unknown function foo, line#12");
    wr32(object, line_text);
    text = win32_describe_cxx_throw(0xe06d7363, 3, args);
    check(text.find("+0 \"GplError: unknown function foo") != std::string::npos,
          "object dwords that point at text are quoted: %s", text.c_str());
    // Without frame pointers the chain stops; the stack still holds return
    // addresses, recognisable because a CALL precedes each one.
    uint32_t code = scratch_block(64);
    g_mem[code + 0] = 0xe8; // CALL rel32 ... the return address is code + 5
    wr32(code + 1, 0x10);
    g_mem[code + 5] = 0x90;
    g_mem[code + 16] = 0xff; // CALL EAX ... return address code + 18
    g_mem[code + 17] = 0xd0;
    g_mem[code + 32] = 0x90; // a NOP: code + 33 follows no CALL
    uint32_t stack = scratch_block(32);
    wr32(stack + 0, 0x12345678);
    wr32(stack + 4, code + 18);
    wr32(stack + 8, code + 33);
    wr32(stack + 12, code + 5);
    std::vector<uint32_t> found = win32_stack_return_candidates(stack, 16, code, code + 64, 8);
    check(found.size() == 2 && found[0] == code + 18 && found[1] == code + 5,
          "the stack scan keeps the %zu dwords that follow a CALL", found.size());
    // A register that holds an object with a vtable is named by its class:
    // MSVC's RTTI hangs the complete-object locator off vtable[-1], and its
    // type descriptor carries the mangled name.  Anything else is described as
    // the bare value.
    uint32_t col = scratch_block(20), vtbl = scratch_block(16), obj = scratch_block(8);
    wr32(col + 12, type); // pCompleteObject locator -> TypeDescriptor
    wr32(vtbl, col);      // vtable[-1]
    wr32(obj, vtbl + 4);  // the object's vtable pointer
    std::string named = win32_describe_pointer(obj);
    check(named.find("GameException") != std::string::npos, "an object is named by its RTTI: %s",
          named.c_str());
    check(win32_describe_pointer(0x12345678).empty() || win32_describe_pointer(0x12345678) == "",
          "a value that points at nothing describes as nothing");
    // A frame chain: [EBP] -> caller's EBP, [EBP+4] -> return address in the image.
    uint32_t f1 = scratch_block(8), f2 = scratch_block(8); // the caller's frame lies above
    wr32(f1, f2);
    wr32(f1 + 4, loader_image_base() + 0x1234);
    wr32(f2, 0);
    wr32(f2 + 4, loader_image_base() + 0x5678);
    std::vector<uint32_t> chain = win32_return_chain(f1, 8);
    check(chain.size() == 2 && chain[0] == loader_image_base() + 0x1234 &&
              chain[1] == loader_image_base() + 0x5678,
          "the return chain walks EBP through %zu frames", chain.size());
}

// The GDI a software-rendered game leans on: a DIB section as its frame
// buffer, a memory DC to hold it, a colour table for 8-bit modes, palettes;
// plus the odd process shims and COM class creation its start-up reaches.
static void test_gdi_and_com(X86 *c) {
    section("GDI DIB sections, palettes, process shims, COM class creation");
    // A top-down 64x32 16-bpp 565 DIB, the shape the game's frame buffer takes.
    uint32_t bmi = scratch_block(0x440);
    memset(g_mem + bmi, 0, 0x440);
    wr32(bmi + 0, 40);
    wr32(bmi + 4, 64);
    wr32(bmi + 8, (uint32_t)-32);
    wr16(bmi + 12, 1);
    wr16(bmi + 14, 16);
    wr32(bmi + 16, 3); // BI_BITFIELDS
    wr32(bmi + 40, 0xf800);
    wr32(bmi + 44, 0x07e0);
    wr32(bmi + 48, 0x001f);
    uint32_t bits = scratch_block(4);
    wr32(bits, 0);
    uint32_t hdc = call_import(c, "USER32.dll", "GetDC", {0});
    uint32_t hbm = call_import(c, "GDI32.dll", "CreateDIBSection", {hdc, bmi, 0, bits, 0, 0});
    check(hbm != 0 && rd32(bits) != 0 && heap_owns(rd32(bits)),
          "CreateDIBSection -> %08x with bits at %08x on the guest heap", hbm, rd32(bits));
    check(heap_size(rd32(bits)) != 0xffffffffu && heap_size(rd32(bits)) >= 64u * 2 * 32,
          "the bits cover 64x32 at 16 bpp (%u bytes)", heap_size(rd32(bits)));
    uint32_t bm = scratch_block(24);
    check(call_import(c, "GDI32.dll", "GetObjectA", {hbm, 24, bm}) == 24 && rd32(bm + 4) == 64 &&
              rd32(bm + 8) == 32 && rd32(bm + 12) == 128 && rd16(bm + 18) == 16 &&
              rd32(bm + 20) == rd32(bits),
          "GetObjectA describes the bitmap: 64x32, stride %u, %u bpp, bits %08x", rd32(bm + 12),
          rd16(bm + 18), rd32(bm + 20));
    uint32_t mdc = call_import(c, "GDI32.dll", "CreateCompatibleDC", {hdc});
    check(mdc != 0 && call_import(c, "GDI32.dll", "SelectObject", {mdc, hbm}) != 0,
          "a memory DC takes the bitmap");
    check(call_import(c, "GDI32.dll", "DeleteDC", {mdc}) == 1, "DeleteDC");
    call_import(c, "USER32.dll", "ReleaseDC", {0, hdc});
    uint32_t old_bits = rd32(bits);
    check(call_import(c, "GDI32.dll", "DeleteObject", {hbm}) == 1 && !heap_owns(old_bits),
          "DeleteObject frees the bits");
    // 8 bpp with a colour table set through the DC it is selected into.
    wr16(bmi + 14, 8);
    wr32(bmi + 16, 0);
    uint32_t hbm8 = call_import(c, "GDI32.dll", "CreateDIBSection", {0, bmi, 0, bits, 0, 0});
    uint32_t mdc8 = call_import(c, "GDI32.dll", "CreateCompatibleDC", {0});
    call_import(c, "GDI32.dll", "SelectObject", {mdc8, hbm8});
    uint32_t pal = scratch_block(16);
    wr32(pal, 0x00ff0000);
    wr32(pal + 4, 0x0000ff00);
    check(hbm8 != 0 && call_import(c, "GDI32.dll", "SetDIBColorTable", {mdc8, 0, 2, pal}) == 2,
          "an 8-bpp DIB takes a colour table");
    call_import(c, "GDI32.dll", "DeleteDC", {mdc8});
    call_import(c, "GDI32.dll", "DeleteObject", {hbm8});
    // A logical palette round-trips its entries.
    uint32_t lp = scratch_block(4 + 4 * 4);
    wr16(lp, 0x300);
    wr16(lp + 2, 4);
    for (uint32_t i = 0; i < 4; ++i)
        wr32(lp + 4 + 4 * i, 0x00102030u + i);
    uint32_t hpal = call_import(c, "GDI32.dll", "CreatePalette", {lp});
    uint32_t got = scratch_block(16);
    check(hpal != 0 && call_import(c, "GDI32.dll", "GetPaletteEntries", {hpal, 1, 2, got}) == 2 &&
              rd32(got) == 0x00102031u && rd32(got + 4) == 0x00102032u,
          "CreatePalette / GetPaletteEntries round-trip");
    check(call_import(c, "GDI32.dll", "SelectPalette", {hdc, hpal, 0}) != 0 &&
              call_import(c, "GDI32.dll", "RealizePalette", {hdc}) == 4,
          "SelectPalette / RealizePalette report the entries");
    call_import(c, "GDI32.dll", "DeleteObject", {hpal});

    check(call_import(c, "KERNEL32.dll", "SetErrorMode", {0x8001}) == 0 &&
              call_import(c, "KERNEL32.dll", "SetErrorMode", {0}) == 0x8001,
          "SetErrorMode returns the previous mode");
    check(call_import(c, "KERNEL32.dll", "GetLogicalDrives", {}) & 0x4, "GetLogicalDrives has C:");
    uint32_t msg = scratch_block(128);
    check(call_import(c, "WINMM.dll", "mciGetErrorStringA", {266, msg, 128}) == 1 &&
              !gm_str(msg).empty(),
          "mciGetErrorStringA(MCIERR_DEVICE_NOT_INSTALLED) -> \"%s\"", gm_str(msg).c_str());

    // The mixer API: no mixer device, said with the right argument counts.
    check(call_import(c, "WINMM.dll", "mixerGetNumDevs", {}) == 0, "mixerGetNumDevs: none");
    uint32_t hmx = scratch_block(4);
    check(call_import(c, "WINMM.dll", "mixerOpen", {hmx, 0, 0, 0, 0, 0}) == 6 &&
              call_import(c, "WINMM.dll", "mixerClose", {0}) == 6 &&
              call_import(c, "WINMM.dll", "mixerGetDevCapsA", {0, msg, 128}) == 6 &&
              call_import(c, "WINMM.dll", "mixerGetLineInfoA", {0, msg, 0}) == 6 &&
              call_import(c, "WINMM.dll", "mixerGetLineControlsA", {0, msg, 0}) == 6 &&
              call_import(c, "WINMM.dll", "mixerGetControlDetailsA", {0, msg, 0}) == 6 &&
              call_import(c, "WINMM.dll", "mixerSetControlDetails", {0, msg, 0}) == 6,
          "every mixer call reports MMSYSERR_NODRIVER");
    // An icon built from bitmaps is a handle the host never draws.
    uint32_t iconinfo = scratch_block(20);
    memset(g_mem + iconinfo, 0, 20);
    wr32(iconinfo, 0); // fIcon = FALSE: a cursor
    uint32_t hicon = call_import(c, "USER32.dll", "CreateIconIndirect", {iconinfo});
    check(hicon != 0 && call_import(c, "USER32.dll", "DestroyIcon", {hicon}) == 1,
          "CreateIconIndirect / DestroyIcon");
    // The version resource of the executable is not served: the game's own
    // version string comes from its data instead.
    uint32_t vh = scratch_block(4);
    check(call_import(c, "VERSION.dll", "GetFileVersionInfoSizeA", {put_str("x.exe"), vh}) == 0 &&
              call_import(c, "VERSION.dll", "GetFileVersionInfoA", {put_str("x.exe"), 0, 0, 0}) ==
                  0 &&
              call_import(c, "VERSION.dll", "VerQueryValueA", {0, put_str("\\"), vh, vh}) == 0,
          "VERSION.dll reports no version information");
}

// A stand-in for generated guest code: a trampoline the stub recomp_call can
// dispatch, so callbacks that go through recomp_call can be tested without the
// generated function table.
static uint32_t g_fake_time = 0;
static uint32_t fake_clock() {
    return g_fake_time;
}
static int g_display_fps = 0;
extern "C" int mods_display_fps() {
    return g_display_fps;
}

static void test_native_draw_waits(X86 *c) {
    section("native cap replaces both original draw waits without changing simulation time");
    // These are synthetic shim calls, so even unused configured sites can
    // exercise the clock hook without executing the image's draw loop.
    if (!gm_valid(RECOMP_HOOK_FRAME_CLOCK_WAIT_DEADLINE, 4) ||
        !gm_valid(RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE, 4)) {
        printf("  [SKIP] frame clock hooks are sentinels for this game\n");
        ++g_skips;
        return;
    }
    const uint32_t canaries = scratch_block(32);
    const uint32_t old_ret = g_fake_ret, old_edi = c->r[R_EDI];
    const uint8_t old_limit = rd8(canaries), old_flags = rd8(canaries + 4);
    const uint32_t addresses[] = {RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE,
                                  RECOMP_HOOK_FRAME_CLOCK_WAIT_DEADLINE, canaries + 8,
                                  canaries + 12, canaries + 16};
    uint32_t saved[5];
    for (int i = 0; i < 5; ++i)
        saved[i] = rd32(addresses[i]);
    host_set_time_source(fake_clock);
    g_fake_time = 1000;
    wr8(canaries, 40);
    wr8(canaries + 4, 8);
    wr32(canaries + 8, 1234);
    wr32(canaries + 12, 83);
    wr32(canaries + 16, 99);
    for (int rate : {40, 60, 120}) {
        g_display_fps = rate;
        for (uint32_t caller : {RECOMP_HOOK_FRAME_CLOCK_WAIT, RECOMP_HOOK_FRAME_CLOCK_WAIT_CLAMP}) {
            g_fake_ret = RECOMP_HOOK_FRAME_CLOCK_BEGIN;
            check(call_import(c, "KERNEL32.dll", "GetTickCount", {}) == 1000 && rd8(canaries) == 40,
                  "%d Hz pacing preserves unrelated storage and the real clock", rate);
            wr32(RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE, 1016);
            wr32(RECOMP_HOOK_FRAME_CLOCK_WAIT_DEADLINE, 1025);
            c->r[R_EDI] = 60;
            g_fake_ret = caller;
            uint32_t now = call_import(c, "KERNEL32.dll", "GetTickCount", {});
            const bool alternate = caller == RECOMP_HOOK_FRAME_CLOCK_WAIT_CLAMP;
            check(now == 1000 && rd32(alternate ? RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE
                                                : RECOMP_HOOK_FRAME_CLOCK_WAIT_DEADLINE) == now,
                  "%08x retires its old wait at %d Hz, without accelerating time", caller, rate);
            check(rd32(alternate
                           ? RECOMP_HOOK_FRAME_CLOCK_WAIT_DEADLINE
                           : RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE) == (alternate ? 1025u : 1016u),
                  "only the active draw deadline changes");
            check(c->r[R_EDI] == (alternate ? uint32_t(rate) : 60u),
                  "alternate wait uses the selected cap for the measured rendering-rate ceiling");
            check(rd32(canaries + 8) == 1234 && rd32(canaries + 12) == 83 &&
                      rd32(canaries + 16) == 99 && rd8(canaries + 4) == 8,
                  "unrelated timing and flag canaries stay intact");
        }
    }
    wr32(RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE, 1016);
    wr32(RECOMP_HOOK_FRAME_CLOCK_WAIT_DEADLINE, 1025);
    c->r[R_EDI] = 60;
    g_fake_ret = GUEST_RETURN_SENTINEL;
    check(call_import(c, "KERNEL32.dll", "GetTickCount", {}) == 1000 &&
              rd32(RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE) == 1016 &&
              rd32(RECOMP_HOOK_FRAME_CLOCK_WAIT_DEADLINE) == 1025 && c->r[R_EDI] == 60,
          "unrelated clock calls cannot alter draw pacing");
    for (const char *pin : {"RECOMP_PIN_CLOCK"}) {
        os_setenv(pin, "1000,8");
        g_fake_ret = RECOMP_HOOK_FRAME_CLOCK_WAIT_CLAMP;
        call_import(c, "KERNEL32.dll", "GetTickCount", {});
        check(rd32(RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE) == 1016 && c->r[R_EDI] == 60,
              "%s retains original fixture behavior", pin);
        os_unsetenv(pin);
    }
    g_display_fps = 0;
    g_fake_ret = RECOMP_HOOK_FRAME_CLOCK_BEGIN;
    call_import(c, "KERNEL32.dll", "GetTickCount", {});
    check(rd8(canaries) == 40, "original mode preserves unrelated storage");
    g_fake_ret = RECOMP_HOOK_FRAME_CLOCK_WAIT_CLAMP;
    call_import(c, "KERNEL32.dll", "GetTickCount", {});
    check(rd32(RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE) == 1016 && c->r[R_EDI] == 60,
          "original mode keeps its alternate wait and rate ceiling");
    host_clear_time_source();
    g_fake_ret = old_ret;
    c->r[R_EDI] = old_edi;
    wr8(canaries, old_limit);
    wr8(canaries + 4, old_flags);
    for (int i = 0; i < 5; ++i)
        wr32(addresses[i], saved[i]);
}

static uint32_t g_callback_hits = 0;
static uint32_t g_callback_args[4] = {0, 0, 0, 0};

static void fake_guest_fn(X86 *c) {
    ++g_callback_hits;
    for (int i = 0; i < 4; ++i)
        g_callback_args[i] = arg(c, i);
    set_eax(c, 0x600d);
}

static void test_windows(X86 *c) {
    section("USER32 windows and messages");
    // The window procedure is a stand-in the test-only recomp_call can reach;
    // it answers WM_NCCREATE with a non-zero value, as a real one must.
    uint32_t wndproc = imports_alloc_trampoline("test", "guest_callback", fake_guest_fn, 4);
    uint32_t clsname = put_str("RuntimeTestWnd");
    uint32_t wc = scratch_block(40);
    wr32(wc + 0, 3);       // style
    wr32(wc + 4, wndproc); // lpfnWndProc
    wr32(wc + 12, 8);      // cbWndExtra
    wr32(wc + 36, clsname);
    check(call_import(c, "USER32.dll", "RegisterClassA", {wc}) != 0, "RegisterClassA");

    uint32_t title = put_str("RuntimeX");
    uint32_t hwnd =
        call_import(c, "USER32.dll", "CreateWindowExA",
                    {0, clsname, title, 0x80000000u, 0, 0, 640, 480, 0, 0, RECOMP_IMAGE_BASE, 0});
    check(hwnd != 0, "CreateWindowExA -> %08x", hwnd);
    check(host_main_window() == hwnd, "host_main_window sees it");
    check(host_window_proc(hwnd) == wndproc, "the class WNDPROC was recorded");

    uint32_t rc = scratch_block(16);
    call_import(c, "USER32.dll", "GetClientRect", {hwnd, rc});
    check(rd32(rc + 8) == 640 && rd32(rc + 12) == 480, "GetClientRect -> %ux%u", rd32(rc + 8),
          rd32(rc + 12));
    // A pointer game converts the cursor it polled with GetCursorPos into
    // client coordinates; a window at the origin maps a point onto itself.
    uint32_t pt = scratch_block(8);
    wr32(pt, 123);
    wr32(pt + 4, 45);
    check(call_import(c, "USER32.dll", "ScreenToClient", {hwnd, pt}) == 1 && rd32(pt) == 123 &&
              rd32(pt + 4) == 45,
          "ScreenToClient through a window at the origin");
    check(call_import(c, "USER32.dll", "GetActiveWindow", {}) == hwnd,
          "GetActiveWindow is the main window");
    check(call_import(c, "USER32.dll", "SetFocus", {hwnd}) == hwnd,
          "SetFocus returns the window that had focus");

    check(call_import(c, "USER32.dll", "SetWindowLongA", {hwnd, 0, 0x1111}) == 0,
          "SetWindowLongA on the first extra dword");
    check(call_import(c, "USER32.dll", "GetWindowLongA", {hwnd, 0}) == 0x1111,
          "GetWindowLongA reads it back");
    check(call_import(c, "USER32.dll", "GetWindowLongA", {hwnd, 0xfffffffcu}) == wndproc,
          "GWL_WNDPROC reads the window procedure");

    uint32_t msg = scratch_block(28);
    check(call_import(c, "USER32.dll", "PeekMessageA", {msg, 0, 0, 0, 0}) == 0,
          "PeekMessageA on an empty queue returns FALSE");
    host_post_message(hwnd, 0x0201 /* WM_LBUTTONDOWN */, 1, 0x00320064);
    check(call_import(c, "USER32.dll", "PeekMessageA", {msg, 0, 0, 0, 1}) == 1,
          "PeekMessageA(PM_REMOVE) returns the posted message");
    check(rd32(msg + 4) == 0x0201 && rd32(msg + 12) == 0x00320064,
          "the message and lParam survived the round trip");
    check(call_import(c, "USER32.dll", "PeekMessageA", {msg, 0, 0, 0, 1}) == 0,
          "the queue is empty again");

    call_import(c, "USER32.dll", "PostMessageA", {hwnd, 0x0100, 0x41, 0});
    check(call_import(c, "USER32.dll", "GetMessageA", {msg, 0, 0, 0}) == 1,
          "GetMessageA returns the posted WM_KEYDOWN");
    host_set_key_state(0x10, false);
    check(call_import(c, "USER32.dll", "TranslateMessage", {msg}) == 1,
          "TranslateMessage synthesised a WM_CHAR");
    check(call_import(c, "USER32.dll", "PeekMessageA", {msg, 0, 0, 0, 1}) == 1 &&
              rd32(msg + 4) == 0x0102 && rd32(msg + 8) == 'a',
          "the WM_CHAR carries 'a'");

    // Message filters, and an empty queue reports the documented error rather
    // than a message the system never sent.
    host_post_message(hwnd, 0x0201, 0, 0);
    check(call_import(c, "USER32.dll", "PeekMessageA", {msg, 0, 0x0100, 0x0109, 1}) == 0,
          "PeekMessageA with a keyboard filter skips the mouse message");
    check(call_import(c, "USER32.dll", "PeekMessageA", {msg, 0, 0x0200, 0x0209, 1}) == 1,
          "the mouse filter matches it");
    check(call_import(c, "USER32.dll", "GetMessageA", {msg, 0, 0, 0}) == 0xffffffffu,
          "GetMessageA on an empty queue with no host returns -1");

    uint32_t clip = scratch_block(16);
    wr32(clip + 0, 10);
    wr32(clip + 4, 20);
    wr32(clip + 8, 110);
    wr32(clip + 12, 220);
    check(call_import(c, "USER32.dll", "ClipCursor", {clip}) == 1, "ClipCursor");
    uint32_t back = scratch_block(16);
    check(call_import(c, "USER32.dll", "GetClipCursor", {back}) == 1 && rd32(back + 8) == 110,
          "GetClipCursor returns the clip rectangle");
    call_import(c, "USER32.dll", "ClipCursor", {0});
    check(call_import(c, "USER32.dll", "ShowCursor", {0}) == 0xffffffffu,
          "ShowCursor(FALSE) drops the display count to -1");
    call_import(c, "USER32.dll", "ShowCursor", {1});

    host_set_key_state(0x41, true);
    check(call_import(c, "USER32.dll", "GetAsyncKeyState", {0x41}) == 0x8000,
          "GetAsyncKeyState sees the host key");
    host_set_key_state(0x41, false);
    // A message box nobody can click is answered with the default button of the
    // set the caller asked for, never with a button that set does not contain.
    check(call_import(c, "USER32.dll", "MessageBoxA",
                      {0, put_str("text"), put_str("caption"), 0}) == 1,
          "MessageBoxA(MB_OK) returns IDOK");
    check(call_import(c, "USER32.dll", "MessageBoxA", {0, put_str("t"), put_str("c"), 4}) == 6,
          "MB_YESNO returns IDYES, not IDOK");
    check(call_import(c, "USER32.dll", "MessageBoxA", {0, put_str("t"), put_str("c"), 4 | 0x100}) ==
              7,
          "MB_YESNO | MB_DEFBUTTON2 returns IDNO");
    check(call_import(c, "USER32.dll", "MessageBoxA", {0, put_str("t"), put_str("c"), 2}) == 3,
          "MB_ABORTRETRYIGNORE returns IDABORT");
    check(call_import(c, "USER32.dll", "MessageBoxA", {0, put_str("t"), put_str("c"), 1 | 0x100}) ==
              2,
          "MB_OKCANCEL | MB_DEFBUTTON2 returns IDCANCEL");
}

// A window procedure that answers WM_PAINT the way a real one does: BeginPaint
// then EndPaint, which is what validates the update region.
static uint32_t g_painted = 0;
static void fake_painting_wndproc(X86 *c) {
    uint32_t hwnd = arg(c, 0), msg = arg(c, 1);
    // Anything but WM_PAINT is answered TRUE, so WM_NCCREATE does not cancel
    // the creation this procedure is being used for.
    if (msg != 0x000f) {
        set_eax(c, 1);
        return;
    }
    ++g_painted;
    uint32_t ps = scratch_block(64);
    uint32_t entry_esp = c->r[R_ESP];
    uint32_t bp = imports_resolve("USER32.dll", "BeginPaint");
    uint32_t ep = imports_resolve("USER32.dll", "EndPaint");
    uint32_t sp = entry_esp;
    sp -= 4;
    wr32(sp, ps);
    sp -= 4;
    wr32(sp, hwnd);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, bp);
    sp = entry_esp;
    sp -= 4;
    wr32(sp, ps);
    sp -= 4;
    wr32(sp, hwnd);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, ep);
    c->r[R_ESP] = entry_esp;
    set_eax(c, 0);
}

// A window procedure that handles nothing itself and passes everything to
// DefWindowProc, which is what an ordinary WNDPROC does with WM_NCCREATE.
static void fake_defproc_wndproc(X86 *c) {
    uint32_t a[4] = {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)};
    uint32_t tramp = imports_resolve("USER32.dll", "DefWindowProcA");
    uint32_t entry_esp = c->r[R_ESP];
    uint32_t sp = entry_esp;
    for (int i = 3; i >= 0; --i) {
        sp -= 4;
        wr32(sp, a[i]);
    }
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, tramp);
    c->r[R_ESP] = entry_esp;
}

// A thread body that clobbers callee-saved registers and returns normally.
static void fake_clobbering_thread(X86 *c) {
    c->r[R_EBX] = 0x11111111;
    c->r[R_ESI] = 0x22222222;
    c->r[R_EDI] = 0x33333333;
    c->r[R_EBP] = 0x44444444;
    set_eax(c, 0x5150);
}

// A thread body that clobbers registers and leaves through ExitThread.
static void fake_exiting_thread(X86 *c) {
    c->r[R_EBX] = 0xdeadbeef;
    c->r[R_EBP] = 0xfeedface;
    uint32_t tramp = imports_resolve("KERNEL32.dll", "ExitThread");
    uint32_t esp = c->r[R_ESP];
    esp -= 4;
    wr32(esp, 0x1234); // exit code
    esp -= 4;
    wr32(esp, 0x00401000); // return address ExitThread never uses
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp); // longjmps out of the thread
}

// Polls a thread the way the game's own creator at 0052d580 does: call
// GetExitCodeThread until it stops reporting STILL_ACTIVE, with a bound so a
// thread that never runs fails the test instead of hanging it. One poll is a
// scheduling point but promises nothing about WHICH thread ran, exactly as on
// Windows, so a caller that wants a particular thread has to keep asking.
static uint32_t poll_exit_code(X86 *c, uint32_t th, uint32_t pcode, int max_polls) {
    for (int i = 0; i < max_polls; ++i) {
        call_import(c, "KERNEL32.dll", "GetExitCodeThread", {th, pcode});
        if (rd32(pcode) != 0x103)
            return rd32(pcode);
    }
    return rd32(pcode);
}

// ---------------------------------------------------------------------------
// Scheduling contracts. These are the behaviours the cooperative scheduler
// exists for, and none of them is visible from a body that just runs and
// returns: a service thread that never finishes, a wait that has to consume
// real time, a suspend count that has to be unwound as many times as it was
// wound, and TLS that has to be per thread.
// ---------------------------------------------------------------------------
static uint32_t g_service_loops = 0;
static uint32_t g_service_event = 0;
static uint32_t g_service_stop = 0;

// A thread shaped like the game's DirectInput workers: it never returns, it
// waits on an event with a short timeout, and it counts its passes. It only
// makes progress if something reschedules it.
static void fake_service_thread(X86 *c) {
    uint32_t waitfn = imports_resolve("KERNEL32.dll", "WaitForSingleObject");
    while (!g_service_stop) {
        ++g_service_loops;
        uint32_t esp = c->r[R_ESP];
        uint32_t sp = esp;
        sp -= 4;
        wr32(sp, 20); // 20 ms timeout
        sp -= 4;
        wr32(sp, g_service_event);
        sp -= 4;
        wr32(sp, 0x00401000);
        c->r[R_ESP] = sp;
        imports_dispatch(c, waitfn);
        c->r[R_ESP] = esp;
    }
    set_eax(c, 0x5e12);
}

// Reads one TLS slot, writes its own marker, reads it back, and reports
// whether the slot was private to it.
static uint32_t g_tls_index = 0;
static uint32_t g_tls_seen_before[4] = {0, 0, 0, 0};
static uint32_t g_tls_read_back[4] = {0, 0, 0, 0};
static uint32_t g_tls_next_slot = 0;
static uint32_t g_image_tls_blocks[4] = {};
static bool g_image_tls_initialized[4] = {};

static void fake_tls_thread(X86 *c) {
    uint32_t slot = g_tls_next_slot++;
    const LoaderTls &tls = loader_tls();
    if (slot < 4 && tls.index < TLS_SLOTS && tls.raw_end > tls.raw_start) {
        uint32_t block = rd32(rd32(c->fs_base + 0x2c) + 4 * tls.index);
        g_image_tls_blocks[slot] = block;
        g_image_tls_initialized[slot] =
            block && memcmp(g_mem + block, g_mem + tls.raw_start, tls.raw_end - tls.raw_start) == 0;
        if (block)
            g_mem[block] ^= (uint8_t)(slot + 1);
    }
    uint32_t get = imports_resolve("KERNEL32.dll", "TlsGetValue");
    uint32_t set = imports_resolve("KERNEL32.dll", "TlsSetValue");
    uint32_t esp = c->r[R_ESP], sp;

    sp = esp;
    sp -= 4;
    wr32(sp, g_tls_index);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, get);
    c->r[R_ESP] = esp;
    if (slot < 4)
        g_tls_seen_before[slot] = c->r[R_EAX];

    sp = esp;
    sp -= 4;
    wr32(sp, 0xd00d0000u + slot);
    sp -= 4;
    wr32(sp, g_tls_index);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, set);
    c->r[R_ESP] = esp;

    sp = esp;
    sp -= 4;
    wr32(sp, g_tls_index);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, get);
    c->r[R_ESP] = esp;
    if (slot < 4)
        g_tls_read_back[slot] = c->r[R_EAX];
    set_eax(c, 0);
}

static double wall_seconds() {
    return (double)os_monotonic_ns() * 1e-9;
}

// Takes a critical section, records the order, holds it across a Sleep so the
// other thread must really block, then leaves it.
static uint32_t g_cs_addr = 0;
static char g_cs_order[16] = {0};
static uint32_t g_cs_order_n = 0;
static void cs_note(char ch) {
    if (g_cs_order_n < sizeof g_cs_order - 1)
        g_cs_order[g_cs_order_n++] = ch;
}
static void fake_critsec_thread(X86 *c) {
    uint32_t enter = imports_resolve("KERNEL32.dll", "EnterCriticalSection");
    uint32_t leave = imports_resolve("KERNEL32.dll", "LeaveCriticalSection");
    uint32_t sleep = imports_resolve("KERNEL32.dll", "Sleep");
    uint32_t esp = c->r[R_ESP], sp;
    sp = esp;
    sp -= 4;
    wr32(sp, g_cs_addr);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, enter);
    c->r[R_ESP] = esp;
    cs_note('B');
    sp = esp;
    sp -= 4;
    wr32(sp, 20);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, sleep);
    c->r[R_ESP] = esp;
    cs_note('b');
    sp = esp;
    sp -= 4;
    wr32(sp, g_cs_addr);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, leave);
    c->r[R_ESP] = esp;
    set_eax(c, 0);
}

// Waits on one event and records which waiter it was.
static uint32_t g_shared_event = 0;
static uint32_t g_wait_results[4] = {0, 0, 0, 0};
static uint32_t g_wait_done = 0;
static void fake_waiter_thread(X86 *c) {
    uint32_t waitfn = imports_resolve("KERNEL32.dll", "WaitForSingleObject");
    uint32_t esp = c->r[R_ESP];
    uint32_t sp = esp;
    sp -= 4;
    wr32(sp, 400);
    sp -= 4;
    wr32(sp, g_shared_event);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, waitfn);
    c->r[R_ESP] = esp;
    if (g_wait_done < 4)
        g_wait_results[g_wait_done++] = c->r[R_EAX];
    set_eax(c, 0);
}

// Takes a mutex and ends without releasing it.
static uint32_t g_abandon_mutex = 0;
static void fake_abandoning_thread(X86 *c) {
    uint32_t waitfn = imports_resolve("KERNEL32.dll", "WaitForSingleObject");
    uint32_t esp = c->r[R_ESP];
    uint32_t sp = esp;
    sp -= 4;
    wr32(sp, 0);
    sp -= 4;
    wr32(sp, g_abandon_mutex);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, waitfn);
    c->r[R_ESP] = esp;
    set_eax(c, 0);
}

// Calls ExitProcess from a worker. The main thread owns the landing pad, so
// this has to reach it however deeply it is blocked.
static uint32_t g_exiting_started = 0;
static void fake_exitprocess_thread(X86 *c) {
    ++g_exiting_started;
    uint32_t fn = imports_resolve("KERNEL32.dll", "ExitProcess");
    uint32_t esp = c->r[R_ESP];
    esp -= 4;
    wr32(esp, 0x2b);
    esp -= 4;
    wr32(esp, 0x00401000);
    c->r[R_ESP] = esp;
    imports_dispatch(c, fn); // does not return
    set_eax(c, 0);
}

// A worker that sleeps a little and then posts the message a filtered
// GetMessageA is waiting for. It only ever runs if GetMessageA yields.
static uint32_t g_post_hwnd = 0;
static uint32_t g_posted_from_worker = 0;
static void fake_posting_thread(X86 *c) {
    uint32_t sleepfn = imports_resolve("KERNEL32.dll", "Sleep");
    uint32_t esp = c->r[R_ESP];
    uint32_t sp = esp;
    sp -= 4;
    wr32(sp, 20);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, sleepfn);
    c->r[R_ESP] = esp;
    host_post_message(g_post_hwnd, 0x0201, 7, 9);
    g_posted_from_worker = 1;
    set_eax(c, 0);
}

// The message waiter a real host installs: service the loop, then report
// whether anything is queued. It also rescues the test after an absurd number
// of calls, so a GetMessageA that busy-loops fails the test instead of hanging
// it.
static uint32_t g_waiter_calls = 0;
static uint32_t g_waiter_rescued = 0;
static bool test_message_waiter() {
    if (++g_waiter_calls > 200000 && !g_waiter_rescued) {
        g_waiter_rescued = 1;
        host_post_message(g_post_hwnd, 0x0201, 7, 9);
    }
    return host_messages_pending();
}

// A thread shaped like the DirectInput service threads: register nothing, just
// wait on an event for a long time and record how it was released. It only
// ever finishes if something signals the event.
static uint32_t g_hostsig_event = 0;
static uint32_t g_hostsig_result = 0xdeadbeef;
static uint32_t g_hostsig_running = 0;
static void fake_host_waiter_thread(X86 *c) {
    g_hostsig_running = 1;
    uint32_t waitfn = imports_resolve("KERNEL32.dll", "WaitForSingleObject");
    uint32_t esp = c->r[R_ESP];
    uint32_t sp = esp;
    sp -= 4;
    wr32(sp, 5000);
    sp -= 4;
    wr32(sp, g_hostsig_event);
    sp -= 4;
    wr32(sp, 0x00401000);
    c->r[R_ESP] = sp;
    imports_dispatch(c, waitfn);
    c->r[R_ESP] = esp;
    g_hostsig_result = c->r[R_EAX];
    set_eax(c, 0x1234);
}

// Signals the event from a genuine host thread, the way an AppKit event
// handler would, while the guest is running.
static void *host_signal_thread(void *arg) {
    os_sleep_us(30 * 1000);
    guest_event_signal_from_host((uint32_t)(uintptr_t)arg);
    return nullptr;
}

// Stands in for a windowed host's idle waiter: it services nothing, sleeps its
// slice and returns, which is the shape the real one has. Counting the calls
// shows the run thread really went through it rather than round it.
static uint32_t g_idle_calls = 0;
static double g_idle_total = 0.0;
static int test_idle_waiter(double seconds) {
    ++g_idle_calls;
    g_idle_total += seconds;
    os_sleep_us((uint64_t)(seconds * 1e6));
    return 0;
}

// Repeated short handoffs model the input workers woken by pointer motion.
// An AppKit wait cannot hear a scheduler condition-variable broadcast, so
// sleeping in that host callback adds a full slice to each completed handoff.
static uint32_t g_pointer_handoffs = 0;
static void fake_pointer_handoff_thread(X86 *c) {
    for (unsigned i = 0; i < 100; ++i) {
        ++g_pointer_handoffs;
        host_guest_yield();
    }
    set_eax(c, 0);
}

static void test_scheduling(X86 *c) {
    section("cooperative scheduling contracts");
    uint32_t pcode = scratch_block(4);

    // --- a service thread makes progress without an explicit yield ---------
    g_service_event = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 1, 0, 0});
    g_service_loops = 0;
    g_service_stop = 0;
    uint32_t svc = imports_alloc_trampoline("test", "service_thread", fake_service_thread, 1);
    uint32_t th = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, svc, 0, 0, 0});
    check(th != 0, "started a service thread that never returns");
    check(g_service_loops == 0, "it has not run yet");

    // Only GetTickCount, which is not a blocking call and never yielded before
    // the bounded checkpoint existed. The service thread must still advance.
    uint32_t before = g_service_loops;
    double t0 = wall_seconds();
    while (wall_seconds() - t0 < 0.25)
        call_import(c, "KERNEL32.dll", "GetTickCount", {});
    check(g_service_loops > before,
          "polling GetTickCount alone let the service thread run (%u passes)", g_service_loops);

    // --- an unsignalled wait consumes real time and reports a timeout ------
    uint32_t ev = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 1, 0, 0});
    t0 = wall_seconds();
    uint32_t r = call_import(c, "KERNEL32.dll", "WaitForSingleObject", {ev, 60});
    double waited = wall_seconds() - t0;
    check(r == 0x102, "an unsignalled wait with a timeout reports WAIT_TIMEOUT");
    check(waited >= 0.05, "and it waited for it (%.0f ms)", waited * 1000.0);

    // --- a zero timeout answers from the current state, without waiting ----
    t0 = wall_seconds();
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {ev, 0}) == 0x102,
          "a zero timeout on an unsignalled object returns WAIT_TIMEOUT at once");
    check(wall_seconds() - t0 < 0.02, "and does not block");

    // --- a signalled object releases the waiter without waiting it out -----
    call_import(c, "KERNEL32.dll", "SetEvent", {ev});
    t0 = wall_seconds();
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {ev, 5000}) == 0,
          "a signalled event satisfies the wait");
    check(wall_seconds() - t0 < 0.05, "immediately, not after the timeout");

    // --- Sleep really sleeps ----------------------------------------------
    t0 = wall_seconds();
    call_import(c, "KERNEL32.dll", "Sleep", {60});
    double slept = wall_seconds() - t0;
    check(slept >= 0.05, "Sleep(60) suspended the caller for the interval (%.0f ms)",
          slept * 1000.0);

    g_service_stop = 1;
    call_import(c, "KERNEL32.dll", "SetEvent", {g_service_event});
    check(poll_exit_code(c, th, pcode, 200) == 0x5e12,
          "the service thread saw the stop request and ended");

    // --- nested suspension -------------------------------------------------
    uint32_t fn = imports_alloc_trampoline("test", "guest_callback", fake_guest_fn, 4);
    g_callback_hits = 0;
    uint32_t th2 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, fn, 0x77, 4, 0});
    check(call_import(c, "KERNEL32.dll", "SuspendThread", {th2}) == 1,
          "SuspendThread on a CREATE_SUSPENDED thread reports count 1");
    check(call_import(c, "KERNEL32.dll", "ResumeThread", {th2}) == 2,
          "the first ResumeThread reports count 2");
    for (int i = 0; i < 8; ++i)
        call_import(c, "KERNEL32.dll", "GetTickCount", {});
    check(g_callback_hits == 0, "still suspended after one resume of two");
    check(call_import(c, "KERNEL32.dll", "ResumeThread", {th2}) == 1,
          "the second ResumeThread reports count 1");
    check(poll_exit_code(c, th2, pcode, 64) == 0x600d && g_callback_hits == 1,
          "and the thread ran once the count reached zero");

    // --- TLS is per thread -------------------------------------------------
    g_tls_index = call_import(c, "KERNEL32.dll", "TlsAlloc", {});
    check(g_tls_index != 0xffffffffu, "TlsAlloc gave index %u", g_tls_index);
    check(g_tls_index != loader_tls().index, "TlsAlloc skips the image's reserved slot");
    call_import(c, "KERNEL32.dll", "TlsSetValue", {g_tls_index, 0x11111111});
    g_tls_next_slot = 0;
    uint32_t tlsfn = imports_alloc_trampoline("test", "tls_thread", fake_tls_thread, 1);
    uint32_t ta = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, tlsfn, 0, 0, 0});
    uint32_t tb = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, tlsfn, 0, 0, 0});
    poll_exit_code(c, ta, pcode, 64);
    poll_exit_code(c, tb, pcode, 64);
    const LoaderTls &tls = loader_tls();
    if (tls.index < TLS_SLOTS && tls.raw_end > tls.raw_start) {
        uint32_t main_block = rd32(TLS_BASE + 4 * tls.index);
        check(g_image_tls_initialized[0] && g_image_tls_initialized[1],
              "both new threads start with the image's TLS raw data");
        check(g_image_tls_blocks[0] != g_image_tls_blocks[1] &&
                  g_image_tls_blocks[0] != main_block && g_image_tls_blocks[1] != main_block,
              "the main thread and both workers have distinct image TLS blocks");
        check(memcmp(g_mem + main_block, g_mem + tls.raw_start, tls.raw_end - tls.raw_start) == 0,
              "worker TLS writes leave the main thread's image TLS data intact");
    }
    check(g_tls_seen_before[0] == 0 && g_tls_seen_before[1] == 0,
          "each thread's slot started at zero, not at the creator's value");
    check(g_tls_read_back[0] == 0xd00d0000u && g_tls_read_back[1] == 0xd00d0001u,
          "and each read back its own value");
    check(call_import(c, "KERNEL32.dll", "TlsGetValue", {g_tls_index}) == 0x11111111,
          "the creator's value is untouched by either");
    call_import(c, "KERNEL32.dll", "TlsFree", {g_tls_index});

    // --- a critical section excludes another thread ------------------------
    uint32_t cs = scratch_block(24);
    call_import(c, "KERNEL32.dll", "InitializeCriticalSection", {cs});
    call_import(c, "KERNEL32.dll", "EnterCriticalSection", {cs});
    call_import(c, "KERNEL32.dll", "EnterCriticalSection", {cs});
    check(rd32(cs + 8) == 2, "entering twice recurses rather than deadlocking");
    check(rd32(cs + 12) != 0, "and records an owner");
    call_import(c, "KERNEL32.dll", "LeaveCriticalSection", {cs});
    check(rd32(cs + 8) == 1, "one leave drops one level");
    call_import(c, "KERNEL32.dll", "LeaveCriticalSection", {cs});
    check(rd32(cs + 12) == 0 && rd32(cs + 4) == 0xffffffffu,
          "the last leave releases it and restores LockCount = -1");
    check(call_import(c, "KERNEL32.dll", "TryEnterCriticalSection", {cs}) == 1,
          "TryEnterCriticalSection takes a free section");
    call_import(c, "KERNEL32.dll", "LeaveCriticalSection", {cs});
    call_import(c, "KERNEL32.dll", "DeleteCriticalSection", {cs});

    // --- a contended critical section really blocks -------------------------
    g_cs_addr = scratch_block(24);
    g_cs_order_n = 0;
    memset(g_cs_order, 0, sizeof g_cs_order);
    call_import(c, "KERNEL32.dll", "InitializeCriticalSection", {g_cs_addr});
    uint32_t csfn = imports_alloc_trampoline("test", "critsec_thread", fake_critsec_thread, 1);
    uint32_t cst = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, csfn, 0, 0, 0});
    call_import(c, "KERNEL32.dll", "GetTickCount", {}); // let it take the lock
    for (int i = 0; i < 4 && g_cs_order_n == 0; ++i) {
        call_import(c, "KERNEL32.dll", "Sleep", {5});
    }
    check(g_cs_order_n >= 1 && g_cs_order[0] == 'B', "the worker took the critical section first");
    cs_note('A');
    call_import(c, "KERNEL32.dll", "EnterCriticalSection", {g_cs_addr});
    cs_note('a');
    check(strcmp(g_cs_order, "BAba") == 0,
          "this thread blocked until the worker left it (order %s)", g_cs_order);
    call_import(c, "KERNEL32.dll", "LeaveCriticalSection", {g_cs_addr});
    poll_exit_code(c, cst, pcode, 200);

    // --- one signal releases exactly one of several waiters -----------------
    g_shared_event = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 0, 0, 0}); // auto-reset
    g_wait_done = 0;
    uint32_t wfn = imports_alloc_trampoline("test", "waiter_thread", fake_waiter_thread, 1);
    uint32_t wa = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, wfn, 0, 0, 0});
    uint32_t wb = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, wfn, 0, 0, 0});
    for (int i = 0; i < 4; ++i)
        call_import(c, "KERNEL32.dll", "GetTickCount", {});
    check(g_wait_done == 0, "both waiters are blocked on the auto-reset event");
    call_import(c, "KERNEL32.dll", "SetEvent", {g_shared_event});
    for (int i = 0; i < 40 && g_wait_done < 1; ++i)
        call_import(c, "KERNEL32.dll", "Sleep", {5});
    check(g_wait_done == 1 && g_wait_results[0] == 0,
          "one SetEvent released exactly one waiter, with WAIT_OBJECT_0");
    call_import(c, "KERNEL32.dll", "SetEvent", {g_shared_event});
    poll_exit_code(c, wa, pcode, 400);
    poll_exit_code(c, wb, pcode, 400);
    check(g_wait_done == 2, "the second signal released the other one");

    // --- a mutex its owner never released is abandoned ----------------------
    g_abandon_mutex = call_import(c, "KERNEL32.dll", "CreateMutexA", {0, 0, 0});
    uint32_t abfn =
        imports_alloc_trampoline("test", "abandoning_thread", fake_abandoning_thread, 1);
    uint32_t abt = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, abfn, 0, 0, 0});
    poll_exit_code(c, abt, pcode, 200);
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {g_abandon_mutex, 0}) == 0x80,
          "a mutex whose owner ended reports WAIT_ABANDONED to the next waiter");
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {g_abandon_mutex, 0}) == 0,
          "and only to the first: it is owned normally after that");
    call_import(c, "KERNEL32.dll", "ReleaseMutex", {g_abandon_mutex});
    call_import(c, "KERNEL32.dll", "ReleaseMutex", {g_abandon_mutex});
    call_import(c, "KERNEL32.dll", "CloseHandle", {g_abandon_mutex});

    // --- a mutex is owned, recursively -------------------------------------
    uint32_t mx = call_import(c, "KERNEL32.dll", "CreateMutexA", {0, 1, 0});
    check(mx != 0, "created a mutex owned by its creator");
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {mx, 0}) == 0,
          "the owner re-enters it without blocking");
    check(call_import(c, "KERNEL32.dll", "ReleaseMutex", {mx}) == 1, "released once");
    check(call_import(c, "KERNEL32.dll", "ReleaseMutex", {mx}) == 1, "released twice");
    check(call_import(c, "KERNEL32.dll", "ReleaseMutex", {mx}) == 0,
          "releasing a mutex this thread no longer owns fails");
    call_import(c, "KERNEL32.dll", "CloseHandle", {mx});

    // --- GetMessageA waits without starving the thread that will post -------
    // A queued message the filter rejects keeps the waiter answering "yes,
    // there is something", so a loop that only yields when the waiter says no
    // spins forever and the worker that would post the matching message never
    // gets the baton.
    {
        g_post_hwnd = host_main_window();
        g_posted_from_worker = 0;
        g_waiter_calls = 0;
        g_waiter_rescued = 0;
        host_set_message_waiter(test_message_waiter);

        // Queued, and outside the filter this GetMessageA will use.
        host_post_message(g_post_hwnd, 0x0400, 0, 0);
        check(host_messages_pending(), "an unmatched message is queued");

        uint32_t pfn = imports_alloc_trampoline("test", "posting_thread", fake_posting_thread, 1);
        call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, pfn, 0, 0, 0});

        uint32_t msg = scratch_block(28);
        uint32_t r = call_import(c, "USER32.dll", "GetMessageA", {msg, 0, 0x0200, 0x0209});
        check(r == 1 && rd32(msg + 4) == 0x0201 && rd32(msg + 8) == 7,
              "GetMessageA waited and returned the matching message");
        check(g_posted_from_worker == 1, "the worker got the baton and posted it");
        check(g_waiter_rescued == 0,
              "GetMessageA yielded rather than spinning on the unmatched message "
              "(%u waiter calls)",
              g_waiter_calls);
        check(host_messages_pending(), "and the unmatched message is still queued, not swallowed");

        // Drain it so later tests see an empty queue.
        call_import(c, "USER32.dll", "PeekMessageA", {msg, 0, 0, 0, 1});
        host_set_message_waiter(nullptr);
    }

    // --- deadlines are exact, not rounded to some coarser tick --------------
    // A hundred Sleep(1) calls must take about a hundred milliseconds. If any
    // wait in the scheduler rounded up to a poll interval, this is where it
    // would show: at a 200 ms poll the same loop would take twenty seconds.
    // The DirectInput workers are alive and waiting 200 ms at a time while
    // this runs, which is exactly the situation that would quantise it.
    {
        g_service_event = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 1, 0, 0});
        g_service_loops = 0;
        g_service_stop = 0;
        uint32_t svc2 = imports_alloc_trampoline("test", "service_thread2", fake_service_thread, 1);
        uint32_t bg = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, svc2, 0, 0, 0});

        double t0 = wall_seconds();
        for (int i = 0; i < 100; ++i)
            call_import(c, "KERNEL32.dll", "Sleep", {1});
        double took = wall_seconds() - t0;
        check(took < 0.300, "100 x Sleep(1) took %.0f ms, so deadlines are not rounded to a poll",
              took * 1000.0);
        check(took >= 0.050, "and it did sleep rather than returning at once (%.0f ms)",
              took * 1000.0);

        g_service_stop = 1;
        call_import(c, "KERNEL32.dll", "SetEvent", {g_service_event});
        poll_exit_code(c, bg, pcode, 400);
    }

    // --- a host thread can release a blocked guest thread --------------------
    // This is the DirectInput path: the game's service threads wait on an
    // event the host signals when input arrives. The host has no baton, so it
    // may not touch the handle table; the signal is queued and applied by the
    // next thread to enter the scheduler.
    {
        g_hostsig_event = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 0, 0, 0});
        g_hostsig_result = 0xdeadbeef;
        g_hostsig_running = 0;
        uint32_t hfn =
            imports_alloc_trampoline("test", "host_waiter_thread", fake_host_waiter_thread, 1);
        uint32_t ht = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, hfn, 0, 0, 0});

        // Let it reach the wait.
        for (int i = 0; i < 20 && !g_hostsig_running; ++i)
            call_import(c, "KERNEL32.dll", "Sleep", {5});
        check(g_hostsig_running == 1, "the waiter thread reached its wait");
        check(g_hostsig_result == 0xdeadbeef, "and is still in it");

        OsThread *sig = os_thread_create(host_signal_thread, (void *)(uintptr_t)g_hostsig_event, 0);

        // Poll on wall-clock time, not on a count: 4000 counted polls run out
        // in under a millisecond, long before the host thread has signalled.
        double t0 = wall_seconds();
        uint32_t code = 0x103;
        while (wall_seconds() - t0 < 2.0) {
            call_import(c, "KERNEL32.dll", "GetExitCodeThread", {ht, pcode});
            code = rd32(pcode);
            if (code != 0x103)
                break;
            call_import(c, "KERNEL32.dll", "Sleep", {5});
        }
        double took = wall_seconds() - t0;
        check(code == 0x1234, "the thread finished");
        os_thread_join(sig);
        check(g_hostsig_result == 0,
              "its wait returned WAIT_OBJECT_0, released by the host signal");
        check(took < 2.0,
              "and it was released when the signal landed, not by its 5 s timeout "
              "(%.0f ms)",
              took * 1000.0);
    }

    // --- short input handoffs must wake the main thread immediately --------
    {
        g_pointer_handoffs = 0;
        uint32_t fn =
            imports_alloc_trampoline("test", "pointer_handoffs", fake_pointer_handoff_thread, 1);
        uint32_t worker = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, fn, 0, 0, 0});
        g_idle_calls = 0;
        g_idle_total = 0;
        host_set_idle_waiter(test_idle_waiter);
        const double started = wall_seconds();
        for (unsigned i = 0; i < 1000 && g_pointer_handoffs < 100; ++i)
            host_guest_yield();
        const double elapsed = wall_seconds() - started;
        host_set_idle_waiter(nullptr);
        check(g_pointer_handoffs == 100, "all 100 pointer worker handoffs ran");
        check(g_idle_calls > 0, "host events were serviced while waiting for the worker");
        check(g_idle_total == 0,
              "baton handoffs requested no uninterruptible host sleep "
              "(requested %.1f ms, elapsed %.1f ms)",
              g_idle_total * 1000, elapsed * 1000);
        poll_exit_code(c, worker, pcode, 400);
    }

    // --- a host signal wakes the RUN thread, on both waiting paths ----------
    // The run thread is the one that services the window system, so it is the
    // one whose wait matters most. It has to come back when the signal lands
    // on either path: the condition variable when no host is attached, and the
    // sliced host waiter when one is.
    for (int with_host = 0; with_host < 2; ++with_host) {
        g_idle_calls = 0;
        g_idle_total = 0.0;
        if (with_host)
            host_set_idle_waiter(test_idle_waiter);

        uint32_t ev = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 0, 0, 0});
        OsThread *sig = os_thread_create(host_signal_thread, (void *)(uintptr_t)ev, 0);

        double t0 = wall_seconds();
        uint32_t r = call_import(c, "KERNEL32.dll", "WaitForSingleObject", {ev, 5000});
        double took = wall_seconds() - t0;
        os_thread_join(sig);

        check(r == 0, "%s: the run thread's wait was satisfied by the host signal",
              with_host ? "with a host waiter" : "with no host waiter");
        check(took < 1.0,
              "%s: and it came back when the signal landed, not on its "
              "5 s timeout (%.0f ms)",
              with_host ? "with a host waiter" : "with no host waiter", took * 1000.0);
        if (with_host)
            check(g_idle_calls > 0, "the run thread went through the host waiter (%u calls)",
                  g_idle_calls);
        host_set_idle_waiter(nullptr);
    }

    // --- a worker's ExitProcess reaches a main thread blocked indefinitely --
    // Only the main thread can perform the exit, because the landing pad is on
    // its stack. An INFINITE wait must not swallow it.
    {
        uint32_t dead = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 1, 0, 0});
        uint32_t xfn =
            imports_alloc_trampoline("test", "exitprocess_thread", fake_exitprocess_thread, 1);
        g_exiting_started = 0;
        call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, xfn, 0, 0, 0});
        bool came_back = false;
        if (setjmp(*process_exit_jmp()) == 0) {
            // Nothing will ever signal this event; only the pending exit can
            // end the wait.
            call_import(c, "KERNEL32.dll", "WaitForSingleObject", {dead, 0xffffffffu});
        } else {
            came_back = true;
        }
        check(came_back, "an INFINITE wait was ended by a worker's ExitProcess");
        check(g_exiting_started == 1, "and the worker is the one that asked for it");
        check(process_exited() && process_exit_code() == 0x2b, "the exit code came through");
    }
}

static void test_callbacks(X86 *c) {
    section("guest callbacks and threads");
    uint32_t fn = imports_alloc_trampoline("test", "guest_callback", fake_guest_fn, 4);
    check(fn != 0, "registered a stand-in guest function at %08x", fn);

    // A WNDPROC reached through DispatchMessageA.
    uint32_t clsname = put_str("CallbackWnd");
    uint32_t wc = scratch_block(40);
    wr32(wc + 4, fn);
    wr32(wc + 36, clsname);
    call_import(c, "USER32.dll", "RegisterClassA", {wc});
    g_callback_hits = 0;
    uint32_t hwnd = call_import(c, "USER32.dll", "CreateWindowExA",
                                {0, clsname, put_str("cb"), 0, 0, 0, 320, 200, 0, 0, 0x400000, 0});
    check(hwnd != 0, "created a window whose WNDPROC is the stand-in");
    check(g_callback_hits == 2 && g_callback_args[1] == 0x0001,
          "CreateWindowExA sent WM_NCCREATE then WM_CREATE to the WNDPROC");

    g_callback_hits = 0;
    uint32_t msg = scratch_block(28);
    wr32(msg + 0, hwnd);
    wr32(msg + 4, 0x0113); // WM_TIMER
    wr32(msg + 8, 7);
    wr32(msg + 12, 0x1234);
    uint32_t esp_before = c->r[R_ESP];
    uint32_t result = call_import(c, "USER32.dll", "DispatchMessageA", {msg});
    check(g_callback_hits == 1 && g_callback_args[1] == 0x0113 && g_callback_args[2] == 7 &&
              g_callback_args[3] == 0x1234,
          "DispatchMessageA passed hwnd, message, wParam, lParam");
    check(result == 0x600d, "DispatchMessageA returned the WNDPROC result");
    check(c->r[R_ESP] == esp_before, "the callback left ESP where it was");
    call_import(c, "USER32.dll", "DestroyWindow", {hwnd});

    // CreateThread starts a cooperative thread: it returns first and the body
    // runs when something yields the baton, which is what Windows does and
    // what the game's own creator at 0052d580 assumes when it polls.
    g_callback_hits = 0;
    uint32_t th = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, fn, 0x2b, 0, 0});
    check(th != 0, "CreateThread returned a thread handle");
    check(g_callback_hits == 0,
          "the body has not run yet: CreateThread returns to its caller first");
    uint32_t pcode = scratch_block(4);
    check(poll_exit_code(c, th, pcode, 32) == 0x600d && g_callback_hits == 1 &&
              g_callback_args[0] == 0x2b,
          "polling GetExitCodeThread let it run with its parameter and reported its return value");
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {th, 0}) == 0,
          "a finished thread object is signalled");

    g_callback_hits = 0;
    uint32_t th2 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, fn, 0x2c, 4, 0});
    check(g_callback_hits == 0, "CREATE_SUSPENDED did not run the body");
    check(call_import(c, "KERNEL32.dll", "GetExitCodeThread", {th2, pcode}) == 1 &&
              rd32(pcode) == 0x103,
          "a thread that has not finished reports STILL_ACTIVE");
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {th2, 0}) == 0x102,
          "and waiting on it times out rather than succeeding");
    check(call_import(c, "KERNEL32.dll", "ResumeThread", {th2}) == 1,
          "ResumeThread reports the previous suspend count");
    check(g_callback_hits == 0, "ResumeThread only makes it runnable; nothing has yielded yet");
    check(poll_exit_code(c, th2, pcode, 32) == 0x600d && g_callback_hits == 1,
          "polling after ResumeThread let it run to completion");
    check(call_import(c, "KERNEL32.dll", "ResumeThread", {th2}) == 0,
          "a second ResumeThread reports no suspend count");

    // Multimedia timer callbacks fire through the same path. A fake clock keeps
    // the test independent of how long the host takes to get here.
    g_callback_hits = 0;
    g_fake_time = 1000;
    host_set_time_source(fake_clock);
    uint32_t id = call_import(c, "WINMM.dll", "timeSetEvent", {10, 0, fn, 0x99, 0});
    check(id != 0, "timeSetEvent(10 ms, one-shot) -> id %u", id);
    host_pump_timers(c);
    check(g_callback_hits == 0, "the timer does not fire before its delay elapses");
    g_fake_time = 1011;
    host_pump_timers(c);
    check(g_callback_hits == 1 && g_callback_args[0] == id && g_callback_args[2] == 0x99,
          "host_pump_timers called the guest timer callback with its id and user data");
    host_pump_timers(c);
    check(g_callback_hits == 1, "the one-shot timer did not fire twice");

    uint32_t pid = call_import(c, "WINMM.dll", "timeSetEvent", {10, 0, fn, 0, 1});
    g_fake_time = 1030;
    host_pump_timers(c);
    g_fake_time = 1045;
    host_pump_timers(c);
    check(g_callback_hits == 3, "a periodic timer fired on each elapsed period");
    call_import(c, "WINMM.dll", "timeKillEvent", {pid});
    g_fake_time = 1100;
    host_pump_timers(c);
    check(g_callback_hits == 3, "timeKillEvent stopped it");

    // TIME_CALLBACK_EVENT_SET: the "callback" is an event handle to signal.
    uint32_t ev = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 1, 0, 0});
    g_callback_hits = 0;
    uint32_t eid = call_import(c, "WINMM.dll", "timeSetEvent", {5, 0, ev, 0, 0x10});
    g_fake_time = 1200;
    host_pump_timers(c);
    check(g_callback_hits == 0, "an event-mode timer does not call the handle as code");
    check(call_import(c, "KERNEL32.dll", "WaitForSingleObject", {ev, 0}) == 0,
          "it signalled the event instead");
    call_import(c, "WINMM.dll", "timeKillEvent", {eid});
    host_set_time_source(nullptr);

    // A thread has its own register file and its own guest stack, so whatever
    // the body does to the callee-saved registers is invisible to the creator.
    uint32_t clob =
        imports_alloc_trampoline("test", "clobbering_thread", fake_clobbering_thread, 1);
    c->r[R_EBX] = 0xb0;
    c->r[R_ESI] = 0x51;
    c->r[R_EDI] = 0xd1;
    c->r[R_EBP] = 0xbb;
    X86 pre = *c;
    uint32_t th4 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, clob, 0, 0, 0});
    check(poll_exit_code(c, th4, pcode, 32) == 0x5150,
          "the body's return value became the thread's exit code");
    check(c->r[R_EBX] == pre.r[R_EBX] && c->r[R_ESI] == pre.r[R_ESI] &&
              c->r[R_EDI] == pre.r[R_EDI] && c->r[R_EBP] == pre.r[R_EBP] &&
              c->r[R_ESP] == pre.r[R_ESP],
          "a thread body that returns normally leaves the creator's registers alone");
    check(c->fs_base == pre.fs_base, "and the creator keeps its own TEB");

    uint32_t th5 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, clob, 0, 4, 0});
    pre = *c;
    call_import(c, "KERNEL32.dll", "ResumeThread", {th5});
    check(poll_exit_code(c, th5, pcode, 32) == 0x5150, "so does a body started by ResumeThread");
    check(c->r[R_EBX] == pre.r[R_EBX] && c->r[R_ESI] == pre.r[R_ESI] &&
              c->r[R_EDI] == pre.r[R_EDI] && c->r[R_EBP] == pre.r[R_EBP],
          "and it too leaves the creator's registers alone");

    // Painting follows the window's update region, not the clock. Showing a
    // hidden window invalidates it; UpdateWindow then paints synchronously,
    // once, and BeginPaint validates it again.
    g_callback_hits = 0;
    uint32_t pwnd =
        call_import(c, "USER32.dll", "CreateWindowExA",
                    {0, clsname, put_str("paint"), 0, 0, 0, 320, 200, 0, 0, 0x400000, 0});
    g_callback_hits = 0;
    check(call_import(c, "USER32.dll", "UpdateWindow", {pwnd}) == 1 && g_callback_hits == 0,
          "UpdateWindow on a window with an empty update region paints nothing");
    call_import(c, "USER32.dll", "ShowWindow", {pwnd, 1});
    g_callback_hits = 0;
    check(call_import(c, "USER32.dll", "UpdateWindow", {pwnd}) == 1 && g_callback_hits == 1 &&
              g_callback_args[1] == 0x000f,
          "showing a window invalidates it and UpdateWindow sends WM_PAINT synchronously");
    // This WNDPROC never calls BeginPaint, so the region is still dirty: only
    // BeginPaint or ValidateRect clears it, and validating before the handler
    // ran would lose the paint for a window whose procedure ignores WM_PAINT.
    g_callback_hits = 0;
    check(call_import(c, "USER32.dll", "UpdateWindow", {pwnd}) == 1 && g_callback_hits == 1,
          "a WNDPROC that ignores WM_PAINT leaves the region dirty and is asked again");
    call_import(c, "USER32.dll", "DestroyWindow", {pwnd});

    // A procedure that does call BeginPaint validates it, so the next
    // UpdateWindow has nothing to do.
    uint32_t paintproc =
        imports_alloc_trampoline("test", "painting_wndproc", fake_painting_wndproc, 4);
    uint32_t pcls = put_str("PaintWnd");
    uint32_t wc3 = scratch_block(40);
    wr32(wc3 + 4, paintproc);
    wr32(wc3 + 36, pcls);
    call_import(c, "USER32.dll", "RegisterClassA", {wc3});
    g_painted = 0;
    // WS_VISIBLE in the style shows the window as part of creation.
    uint32_t vwnd =
        call_import(c, "USER32.dll", "CreateWindowExA",
                    {0, pcls, put_str("v"), 0x10000000, 0, 0, 64, 64, 0, 0, 0x400000, 0});
    check(vwnd != 0 && host_window_visible(vwnd),
          "a window created with WS_VISIBLE is visible without a separate ShowWindow");
    check(call_import(c, "USER32.dll", "UpdateWindow", {vwnd}) == 1 && g_painted == 1,
          "and it is dirty, so UpdateWindow paints it");
    check(call_import(c, "USER32.dll", "UpdateWindow", {vwnd}) == 1 && g_painted == 1,
          "BeginPaint validated the region, so the next UpdateWindow paints nothing");
    call_import(c, "USER32.dll", "InvalidateRect", {vwnd, 0, 0});
    check(call_import(c, "USER32.dll", "UpdateWindow", {vwnd}) == 1 && g_painted == 2,
          "InvalidateRect makes the next UpdateWindow paint again");
    call_import(c, "USER32.dll", "DestroyWindow", {vwnd});
    pwnd = vwnd;

    // The message waiter is asked only when nothing matches, and its answer is
    // taken at face value, so it has to be true only when something arrived.
    check(!host_messages_pending(), "the queue is empty");
    host_post_message(0, 0x0400, 0, 0);
    check(host_messages_pending(), "and not empty once something is posted");
    uint32_t drain = scratch_block(28);
    call_import(c, "USER32.dll", "PeekMessageA", {drain, 0, 0, 0, 1});
    check(!host_messages_pending(), "PeekMessage with PM_REMOVE drained it");

    // A window procedure that delegates to DefWindowProc must not have its
    // window creation cancelled.
    uint32_t defproc = imports_alloc_trampoline("test", "defproc_wndproc", fake_defproc_wndproc, 4);
    uint32_t defcls = put_str("DefProcWnd");
    uint32_t wc2 = scratch_block(40);
    wr32(wc2 + 4, defproc);
    wr32(wc2 + 36, defcls);
    call_import(c, "USER32.dll", "RegisterClassA", {wc2});
    uint32_t dhwnd = call_import(c, "USER32.dll", "CreateWindowExA",
                                 {0, defcls, put_str("d"), 0, 0, 0, 64, 64, 0, 0, 0x400000, 0});
    check(dhwnd != 0, "CreateWindowExA succeeds when WM_NCCREATE goes to DefWindowProc");
    check(call_import(c, "USER32.dll", "DefWindowProcA", {dhwnd, 0x0081, 0, 0}) == 1,
          "DefWindowProcA answers WM_NCCREATE with TRUE");
    call_import(c, "USER32.dll", "DestroyWindow", {dhwnd});

    // WM_QUIT reaches the guest whatever the filter says.
    uint32_t qmsg = scratch_block(28);
    host_post_message(0, 0x0012 /* WM_QUIT */, 0, 0);
    check(call_import(c, "USER32.dll", "PeekMessageA", {qmsg, dhwnd, 0x0200, 0x0209, 1}) == 1 &&
              rd32(qmsg + 4) == 0x0012,
          "a message filter never suppresses WM_QUIT");

    // ExitThread ends its own thread and nothing else.
    uint32_t exiting = imports_alloc_trampoline("test", "exiting_thread", fake_exiting_thread, 1);
    X86 before = *c;
    uint32_t th3 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, exiting, 0, 0, 0});
    check(poll_exit_code(c, th3, pcode, 32) == 0x1234, "ExitThread recorded the exit code");
    check(c->r[R_ESP] == before.r[R_ESP] && c->r[R_EBX] == before.r[R_EBX] &&
              c->r[R_EBP] == before.r[R_EBP],
          "the creator's ESP, EBX and EBP survived ExitThread");
}

// _setjmp / _longjmp, driven the way generated code will drive them.
static void test_intrinsics(X86 *c) {
    section("_setjmp / _longjmp intrinsics");
    uint32_t buf = scratch_block(64);
    uint32_t esp0 = c->r[R_ESP];

    // call _setjmp(buf): the caller pushes the argument, CALL pushes the return
    // address, so ESP points at the return address on entry.
    uint32_t sp = esp0 - 4;
    wr32(sp, buf);
    sp -= 4;
    wr32(sp, g_fake_ret);
    c->r[R_ESP] = sp;

    jmp_buf *env = recomp_setjmp_prepare(c);
    int v = setjmp(*env);
    recomp_setjmp_return(c, v);
    if (v == 0) {
        check(c->r[R_EAX] == 0, "the first _setjmp return is 0");
        check(c->r[R_ESP] == esp0 - 4, "it popped the return address and left the argument");
        // Deeper code calls _longjmp(buf, 7).
        uint32_t lp = c->r[R_ESP] - 0x40;
        lp -= 4;
        wr32(lp, 7);
        lp -= 4;
        wr32(lp, buf);
        lp -= 4;
        wr32(lp, g_fake_ret);
        c->r[R_ESP] = lp;
        c->r[R_EBX] = 0xbadbad; // clobbered, must be restored by the longjmp
        recomp_longjmp(c);
        check(false, "recomp_longjmp returned, which it must never do");
    }
    check(v == 7 && c->r[R_EAX] == 7, "_longjmp(buf, 7) made _setjmp return 7");
    check(c->r[R_ESP] == esp0 - 4, "the guest stack pointer is back at the _setjmp call");
    check(c->r[R_EBX] != 0xbadbad, "the guest registers were restored from the jmp_buf");
    check(rd32(buf) == 0x4d504f50u, "the guest jmp_buf carries the runtime marker");

    // The single-call form must not silently work: it has to abort, because a
    // host setjmp taken there would belong to a frame that has already
    // returned. Checked in a child so this process survives.
    // The child re-runs this binary with --child-setjmp-abort (see main), which
    // builds the same frame and calls recomp_setjmp; an abort reads as 134.
    fflush(stdout);
    char exe[4096];
    check(os_exe_path(exe, sizeof exe) == 0, "the test knows its own path");
    const char *child_argv[] = {exe, "--child-setjmp-abort", nullptr};
    int64_t pid = 0;
    int code = -1;
    check(os_spawn(child_argv, &pid) == 0 && os_wait(pid, &code) == 0 && code == 134,
          "the single-call _setjmp intrinsic aborts instead of pretending to work (exit %d)", code);

    c->r[R_ESP] = esp0;
}

// A call the runtime cannot deliver must still consume the pushed return
// address, or every frame after it is displaced by four bytes.
static void test_undeliverable_calls(X86 *c) {
    section("undeliverable calls behave as a RET");
    uint32_t esp0 = c->r[R_ESP];

    uint32_t sp = esp0 - 4;
    wr32(sp, 0x00401234);
    c->r[R_ESP] = sp;
    recomp_unknown_call(c, 0x00abcdef);
    check(c->r[R_ESP] == esp0 && c->eip == 0x00401234 && c->r[R_EAX] == 0,
          "recomp_unknown_call popped the return address and resumed at %08x", c->eip);

    sp = esp0 - 4;
    wr32(sp, 0x00405678);
    c->r[R_ESP] = sp;
    recomp_unknown_call(c, 0x0fdfff00); // the callback return sentinel
    check(c->r[R_ESP] == esp0 && c->eip == 0x00405678,
          "so does a call to the callback return sentinel");

    sp = esp0 - 4;
    wr32(sp, 0x00409abc);
    c->r[R_ESP] = sp;
    recomp_shim_call(c, 0x0ff00000 + 16 * 4000); // in range, never allocated
    check(c->r[R_ESP] == esp0 && c->eip == 0x00409abc && c->r[R_EAX] == 0,
          "so does a shim call to an unallocated trampoline");

    c->r[R_ESP] = esp0;
}

static void test_registry(X86 *c) {
    section("registry round trip");
    printf("  using %s\n", registry_path().c_str());
    os_unlink(registry_path().c_str());
    registry_load();

    uint32_t sub = put_str("Software\\RecompTests\\Registry");
    uint32_t phk = scratch_block(4), pdisp = scratch_block(4);
    uint32_t rc = call_import(c, "ADVAPI32.dll", "RegCreateKeyExA",
                              {0x80000002u, sub, 0, 0, 0, 0xf003f, 0, phk, pdisp});
    check(rc == 0, "RegCreateKeyExA(HKLM\\Software\\RecompTests\\Registry) -> %u", rc);
    uint32_t hk = rd32(phk);
    check(rd32(pdisp) == 1, "the key was created, not opened");

    uint32_t vname = put_str("InstallPath");
    uint32_t vdata = put_str(RECOMP_GUEST_ROOT);
    check(call_import(c, "ADVAPI32.dll", "RegSetValueExA",
                      {hk, vname, 0, 1, vdata, sizeof(RECOMP_GUEST_ROOT)}) == 0,
          "RegSetValueExA(REG_SZ)");
    uint32_t dname = put_str("Detail");
    uint32_t ddata = scratch_block(4);
    wr32(ddata, 3);
    check(call_import(c, "ADVAPI32.dll", "RegSetValueExA", {hk, dname, 0, 4, ddata, 4}) == 0,
          "RegSetValueExA(REG_DWORD)");
    check(call_import(c, "ADVAPI32.dll", "RegCloseKey", {hk}) == 0, "RegCloseKey");

    OsStat st{};
    check(os_stat(registry_path().c_str(), &st) == 0 && st.size > 0, "%s was written (%lld bytes)",
          registry_path().c_str(), (long long)st.size);

    // Reload from disk and read the values back.
    registry_load();
    uint32_t phk2 = scratch_block(4);
    check(call_import(c, "ADVAPI32.dll", "RegOpenKeyExA", {0x80000002u, sub, 0, 0x20019, phk2}) ==
              0,
          "RegOpenKeyExA after reload");
    uint32_t hk2 = rd32(phk2);
    uint32_t ptype = scratch_block(4), pbuf = scratch_block(sizeof(RECOMP_GUEST_ROOT) + 64),
             pcb = scratch_block(4);
    wr32(pcb, sizeof(RECOMP_GUEST_ROOT) + 64);
    check(call_import(c, "ADVAPI32.dll", "RegQueryValueExA", {hk2, vname, 0, ptype, pbuf, pcb}) ==
              0,
          "RegQueryValueExA(InstallPath)");
    check(rd32(ptype) == 1 && gm_str(pbuf) == RECOMP_GUEST_ROOT,
          "value survived the round trip: type %u \"%s\"", rd32(ptype), gm_str(pbuf).c_str());
    wr32(pcb, sizeof(RECOMP_GUEST_ROOT) + 64);
    check(call_import(c, "ADVAPI32.dll", "RegQueryValueExA", {hk2, dname, 0, ptype, pbuf, pcb}) ==
              0,
          "RegQueryValueExA(Detail)");
    check(rd32(ptype) == 4 && rd32(pbuf) == 3, "the DWORD round tripped as %u", rd32(pbuf));

    uint32_t mixed_key = put_str("SOFTWARE\\recomptests\\REGISTRY");
    uint32_t phk3 = scratch_block(4);
    check(call_import(c, "ADVAPI32.dll", "RegOpenKeyExA",
                      {0x80000002u, mixed_key, 0, 0x20019, phk3}) == 0,
          "the key opens under a different case");
    wr32(pcb, sizeof(RECOMP_GUEST_ROOT) + 64);
    check(call_import(c, "ADVAPI32.dll", "RegQueryValueExA",
                      {rd32(phk3), put_str("installpath"), 0, ptype, pbuf, pcb}) == 0 &&
              gm_str(pbuf) == RECOMP_GUEST_ROOT,
          "so does the value name");
    call_import(c, "ADVAPI32.dll", "RegCloseKey", {rd32(phk3)});

    uint32_t missing = put_str("NoSuchValue");
    wr32(pcb, sizeof(RECOMP_GUEST_ROOT) + 64);
    check(call_import(c, "ADVAPI32.dll", "RegQueryValueExA", {hk2, missing, 0, ptype, pbuf, pcb}) ==
              2,
          "a missing value reports ERROR_FILE_NOT_FOUND");
    call_import(c, "ADVAPI32.dll", "RegCloseKey", {hk2});
}

// Every import in the PE must have a trampoline, and the stack discipline must
// hold for a zero-argument and a multi-argument shim.
static void test_import_coverage(X86 *c) {
    section("import coverage");
    ExpectedImage expect;
    std::string err;
    if (check(pefile_sections(expect, err), "read PE imports for coverage: %s", err.c_str())) {
        std::set<uint32_t> trampolines, data;
        for (const auto &import : expect.imports) {
            uint32_t value = rd32(import.slot);
            if (imports_is_trampoline(value))
                trampolines.insert(value);
            else
                data.insert(value);
        }
        check(imports_count() >= trampolines.size(),
              "%u trampolines allocated; the IAT references %zu distinct trampolines",
              imports_count(), trampolines.size());
        check(imports_data_count() >= data.size(),
              "%u data symbols registered; the IAT references %zu distinct data symbols",
              imports_data_count(), data.size());
    }

    uint32_t esp_before = c->r[R_ESP];
    call_import(c, "KERNEL32.dll", "GetVersion", {});
    check(c->r[R_ESP] == esp_before, "a 0-argument shim leaves ESP unchanged");
    call_import(c, "KERNEL32.dll", "WideCharToMultiByte", {0, 0, 0, 0, 0, 0, 0, 0});
    check(c->r[R_ESP] == esp_before, "an 8-argument shim pops all its arguments");

    // A logging-only import still balances the stack.
    call_import(c, "WINMM.dll", "midiOutShortMsg", {0, 0});
    check(c->r[R_ESP] == esp_before, "a logging-only shim pops its arguments too");

    uint32_t impl = 0, log_only = 0, unknown = 0;
    imports_coverage(&impl, &log_only, &unknown);
    check(impl + log_only == imports_count(), "%u implemented, %u logging-only", impl, log_only);
    check(unknown == 0, "%u imports have an unknown argument count", unknown);
    if (unknown) {
        // Reuse the runtime's classification rather than duplicating its shim
        // registry in this test. Limit the diagnostic to the first 40 names.
        FILE *coverage = tmpfile();
        if (check(coverage != nullptr, "opened import-coverage diagnostic buffer")) {
            imports_dump_coverage(coverage);
            rewind(coverage);
            char line[1024];
            bool names = false;
            unsigned printed = 0;
            printf("  first %u unknown dll!name pairs:\n", std::min(unknown, 40u));
            while (fgets(line, sizeof line, coverage)) {
                if (strncmp(line, "imports with an unknown stdcall argument count", 45) == 0) {
                    names = true;
                } else if (names) {
                    if (line[0] != ' ')
                        break;
                    if (printed++ < 40)
                        fputs(line, stdout);
                }
            }
            check(printed == unknown, "coverage diagnostic names all %u unknown imports", unknown);
            fclose(coverage);
        }
    }
    imports_dump_stats(stdout);
}

// ---------------------------------------------------------------------------
// The mod seams. No mods module is linked here, so the weak defaults must be
// exactly what an unmodded build does, and the file seam must be driven by a
// resolver the test installs itself.
// ---------------------------------------------------------------------------
static std::vector<std::pair<std::string, int>> g_seam_calls;
static std::string g_seam_root;

static int test_resolver(const char *relative, int op, char *out, size_t out_len) {
    g_seam_calls.push_back({relative, op});
    // Reads come from a "mod" directory, everything that writes from a
    // "profile" directory: that is the shape the overlay has, and it is what
    // the shim's operation classification has to produce.
    std::string dir = (op == WIN32_FILE_READ || op == WIN32_FILE_LIST) ? g_seam_root + "/read"
                                                                       : g_seam_root + "/write";
    std::string full = dir + "/" + relative;
    if (op == WIN32_FILE_READ) {
        OsStat st;
        if (os_stat(full.c_str(), &st) != 0)
            return 0;
    }
    if (full.size() + 1 > out_len)
        return 0;
    memcpy(out, full.c_str(), full.size() + 1);
    return 1;
}

static void test_lister(const char *dir, void (*emit)(void *, const char *, const char *),
                        void *ctx) {
    std::string host = g_seam_root + "/read/" + dir;
    emit(ctx, "one.txt", (host + "/one.txt").c_str());
    emit(ctx, "two.txt", (host + "/two.txt").c_str());
}

// Runs `fn` on a real guest thread and waits for it to end. The scheduler only
// lets a guest thread run while this one is at a yield point, so polling
// GetExitCodeThread is both the wait and the thing that lets it run.
static uint32_t g_run_on_guest_slot = 0;
static void (*g_run_on_guest_fn)() = nullptr;
static void run_on_guest_body(X86 *c) {
    if (g_run_on_guest_fn)
        g_run_on_guest_fn();
    set_eax(c, 1);
}

static void run_on_guest_thread(X86 *c, void (*fn)()) {
    g_run_on_guest_fn = fn;
    if (!g_run_on_guest_slot)
        g_run_on_guest_slot =
            imports_alloc_trampoline("test", "run_on_guest", run_on_guest_body, 1);
    uint32_t pcode = scratch + 0xc00;
    uint32_t th =
        call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, g_run_on_guest_slot, 0, 0, 0});
    poll_exit_code(c, th, pcode, 4096);
    call_import(c, "KERNEL32.dll", "CloseHandle", {th});
    g_run_on_guest_fn = nullptr;
}

static void test_mod_seams(X86 *c) {
    section("mod seams");

    // The weak defaults are no-ops that report "nothing installed".
    check(mods_load_all(), "mods_load_all defaults to success with no module");
    check(std::string(mods_active_callback_desc()).empty(),
          "no active mod callback without the module");
    check(!mods_input_key(0x44, 0x79, 1), "input is never consumed without the module");
    uint8_t *px = nullptr;
    uint32_t bytes = 0;
    check(!mods_texture_override(1, 2, 2, 0, &px, &bytes),
          "no texture override without the module");
    mods_hooks_unwind_to_esp(0); // must not crash

    // The file seam classifies each operation, and a write never resolves
    // through the read tier.
    g_seam_root = "build/recomp/seam-test";
    remove_tree(g_seam_root);
    mkdir_p(g_seam_root + "/read/data");
    mkdir_p(g_seam_root + "/write/data");
    FILE *f = fopen((g_seam_root + "/read/data/shared.txt").c_str(), "wb");
    fputs("read tier", f);
    fclose(f);
    win32_set_file_ops(test_resolver, test_lister);

    g_seam_calls.clear();
    std::string r = win32_host_path_op("data\\shared.txt", WIN32_FILE_READ);
    check(r == g_seam_root + "/read/data/shared.txt", "a read resolves through the read tier");
    check(g_seam_calls.size() == 1 && g_seam_calls[0].first == "data/shared.txt",
          "the resolver sees a normalised, separator-free relative path");

    std::string w = win32_host_path_op("data\\shared.txt", WIN32_FILE_WRITE);
    check(w == g_seam_root + "/write/data/shared.txt",
          "a write resolves through the write tier, not the read tier");

    // CreateFileA with GENERIC_WRITE and OPEN_EXISTING opens through the READ
    // tier and defers the write classification to the first WriteFile. A game
    // opens its archives read/write and never writes them; classifying the
    // open as a write copied every archive into the profile. The write tier is
    // resolved when a byte is actually written, and the handle continues at
    // the offset it had reached; the read tier is never written.
    g_seam_calls.clear();
    uint32_t name = put_str("data\\shared.txt");
    uint32_t hw = call_import(c, "KERNEL32.dll", "CreateFileA",
                              {name, 0xC0000000u, 0, 0, 3 /*OPEN_EXISTING*/, 0, 0});
    check(hw != 0xffffffffu && !g_seam_calls.empty() &&
              g_seam_calls.back().second == WIN32_FILE_READ,
          "read/write OPEN_EXISTING opens through the read tier");
    uint32_t rbuf = scratch + 0x900, got = scratch + 0x9f0;
    check(call_import(c, "KERNEL32.dll", "ReadFile", {hw, rbuf, 4, got, 0}) == 1 &&
              rd32(got) == 4 && memcmp(g_mem + rbuf, "read", 4) == 0,
          "and reads the read tier's bytes");
    g_seam_calls.clear();
    uint32_t wdata = put_str("WXYZ");
    check(call_import(c, "KERNEL32.dll", "WriteFile", {hw, wdata, 4, got, 0}) == 1 &&
              rd32(got) == 4,
          "the first WriteFile on it succeeds");
    check(!g_seam_calls.empty() && g_seam_calls.back().second == WIN32_FILE_WRITE,
          "and is what resolves the write tier");
    call_import(c, "KERNEL32.dll", "CloseHandle", {hw});
    {
        FILE *wf = fopen((g_seam_root + "/write/data/shared.txt").c_str(), "rb");
        check(wf != nullptr, "the write tier now holds the file");
        if (wf) {
            char b[8] = {0};
            fseek(wf, 4, SEEK_SET);
            size_t n = fread(b, 1, 4, wf);
            fclose(wf);
            check(n == 4 && memcmp(b, "WXYZ", 4) == 0,
                  "the bytes landed at the offset the handle had reached");
        }
        FILE *rf = fopen((g_seam_root + "/read/data/shared.txt").c_str(), "rb");
        char rb[16] = {0};
        size_t rn = rf ? fread(rb, 1, 9, rf) : 0;
        if (rf)
            fclose(rf);
        check(rn == 9 && memcmp(rb, "read tier", 9) == 0, "the read tier is untouched");
    }
    // A write-only OPEN_EXISTING that never writes touches no tier either.
    g_seam_calls.clear();
    uint32_t hwo = call_import(c, "KERNEL32.dll", "CreateFileA",
                               {name, 0x40000000u, 0, 0, 3 /*OPEN_EXISTING*/, 0, 0});
    check(hwo != 0xffffffffu && g_seam_calls.back().second == WIN32_FILE_READ,
          "write-only OPEN_EXISTING is classified at the first write, not the open");
    call_import(c, "KERNEL32.dll", "CloseHandle", {hwo});

    g_seam_calls.clear();
    call_import(c, "KERNEL32.dll", "DeleteFileA", {name});
    check(!g_seam_calls.empty() && g_seam_calls.back().second == WIN32_FILE_DELETE,
          "DeleteFileA is classified as a delete");

    // FindFirstFile takes the host path from the lister, so a match living in
    // another tier still reports the right metadata.
    uint32_t pattern = put_str("data\\*.txt");
    uint32_t data = scratch + 0x800;
    uint32_t h = call_import(c, "KERNEL32.dll", "FindFirstFileA", {pattern, data});
    check(h != 0xffffffffu, "FindFirstFileA found a match through the lister");
    check(gm_str(data + 44) == std::string("one.txt"), "the first match is the lister's");
    check(call_import(c, "KERNEL32.dll", "FindNextFileA", {h, data}) == 1 &&
              gm_str(data + 44) == std::string("two.txt"),
          "the second match follows");
    call_import(c, "KERNEL32.dll", "FindClose", {h});

    win32_set_file_ops(nullptr, nullptr);
    check(win32_host_path("data\\shared.txt").empty() ||
              win32_host_path("data\\shared.txt").find(win32_game_dir()) == 0,
          "with no resolver the shim resolves in the game directory as before");

    // With no resolver installed, a rename or a copy to a filename that does
    // not exist yet has to keep working: that is the ordinary case, and the
    // seam is not allowed to change it. Both go through the game directory.
    {
        std::string dir = win32_game_dir();
        std::string src = dir + "/seam-src.tmp";
        FILE *sf = fopen(src.c_str(), "wb");
        check(sf != nullptr, "created a source file in the game directory");
        if (sf) {
            fputs("payload", sf);
            fclose(sf);
        }
        win32_invalidate_dir_cache();
        os_unlink((dir + "/seam-copy.tmp").c_str());
        os_unlink((dir + "/seam-moved.tmp").c_str());

        uint32_t from = put_str("seam-src.tmp");
        uint32_t cto = put_str("seam-copy.tmp");
        check(call_import(c, "KERNEL32.dll", "CopyFileA", {from, cto, 0}) == 1,
              "unmodded CopyFileA creates a destination that did not exist");
        OsStat st{};
        check(os_stat((dir + "/seam-copy.tmp").c_str(), &st) == 0, "and the copy is really there");

        win32_invalidate_dir_cache();
        uint32_t mto = put_str("seam-moved.tmp");
        check(call_import(c, "KERNEL32.dll", "MoveFileA", {from, mto}) == 1,
              "unmodded MoveFileA renames to a destination that did not exist");
        check(os_stat((dir + "/seam-moved.tmp").c_str(), &st) == 0,
              "and the renamed file is really there");

        os_unlink((dir + "/seam-copy.tmp").c_str());
        os_unlink((dir + "/seam-moved.tmp").c_str());
        os_unlink((dir + "/seam-src.tmp").c_str());
        win32_invalidate_dir_cache();
    }

    // Finding 4: the guest root is the empty string to BOTH callbacks. A "."
    // component is exactly what the contract says the resolver never sees.
    {
        static std::vector<std::string> seen_dirs;
        seen_dirs.clear();
        g_seam_calls.clear();
        win32_set_file_ops(
            [](const char *rel, int op, char *out, size_t n) -> int {
                g_seam_calls.push_back({rel, op});
                std::string full = g_seam_root + "/read/" + rel;
                if (full.size() + 1 > n)
                    return 0;
                memcpy(out, full.c_str(), full.size() + 1);
                return 1;
            },
            [](const char *dir, void (*emit)(void *, const char *, const char *), void *ctx) {
                seen_dirs.push_back(dir);
                emit(ctx, "root.txt", (g_seam_root + "/read/root.txt").c_str());
            });
        win32_host_path_op(RECOMP_GUEST_ROOT, WIN32_FILE_READ);
        check(!g_seam_calls.empty() && g_seam_calls.back().first.empty(),
              "the guest root reaches the resolver as the empty string");
        uint32_t rootpat = put_str("*.txt");
        uint32_t rh = call_import(c, "KERNEL32.dll", "FindFirstFileA", {rootpat, data});
        check(seen_dirs.size() == 1 && seen_dirs[0].empty(),
              "and the lister is given the same empty root, not \".\"");
        if (rh != 0xffffffffu)
            call_import(c, "KERNEL32.dll", "FindClose", {rh});
        win32_set_file_ops(nullptr, nullptr);
    }

    // Scheduler coordination. This test's own thread never entered the
    // scheduler, so it must say it is not a guest thread - which is the whole
    // point, because t_self would have called it thread 0.
    check(!sched_is_guest_thread(), "a plain host thread is not a guest thread");
    static bool inside = false, baton_inside = false;
    run_on_guest_thread(c, [] {
        inside = sched_is_guest_thread();
        baton_inside = sched_holds_baton();
    });
    check(inside, "a registered guest thread says so");
    check(baton_inside, "and it holds the baton while it runs");
    check(sched_guest_threads_stopped(), "and it has finished by the time we look");

    sched_registry_lock();
    sched_registry_unlock();
    static int pumped = 0;
    sched_set_checkpoint([] { ++pumped; });
    guest_sleep_ms(0); // a yield point
    check(pumped > 0, "the scheduler ran the registry checkpoint at a yield");
    sched_set_checkpoint(nullptr);

    // A finished guest thread announces its exit exactly once, which is what
    // unwinds whatever it left on the mod layer's invocation stack.
    static int exits = 0;
    static bool stopped_during_exit = true;
    static bool guest_during_exit = false, baton_during_exit = false;
    exits = 0;
    sched_set_thread_exit_observer([] {
        ++exits;
        // The whole ordering requirement, asserted from inside the exit: this
        // thread has NOT published completion yet, so a plugin unload cannot
        // have started underneath code that is still running, and it still
        // holds the baton, so this unwind is still mutually exclusive with
        // every other guest thread.
        stopped_during_exit = sched_guest_threads_stopped();
        guest_during_exit = sched_is_guest_thread();
        baton_during_exit = sched_holds_baton();
    });
    run_on_guest_thread(c, [] {});
    check(exits == 1, "a finished guest thread announced its exit once");
    check(!stopped_during_exit, "the exit ran before the thread was published as finished");
    check(guest_during_exit && baton_during_exit,
          "and while it still held the baton as a guest thread");
    sched_set_thread_exit_observer(nullptr);

    // The locked form answers the same question from under the registry lock,
    // which is the one place the ordinary form cannot be asked: both take the
    // scheduler's single non-recursive mutex, so asking there would hang. This
    // test would not return at all if that were still the only form.
    sched_set_guest_thread(true);
    check(sched_holds_baton(), "the run thread holds the baton");
    sched_registry_lock();
    bool locked_answer = sched_holds_baton_locked();
    sched_registry_unlock();
    check(locked_answer, "sched_holds_baton_locked answers under the registry lock, and returns");
    sched_set_guest_thread(false);
    // Under the lock again: this is the only way the function may ever be
    // called, and asking it unlocked would be the very misuse the locked form
    // exists to avoid - masked here by the non-guest short-circuit, which
    // returns before it would have touched anything.
    sched_registry_lock();
    bool not_guest = sched_holds_baton_locked();
    sched_registry_unlock();
    check(!not_guest, "and it agrees with the plain form for a thread that is not a guest thread");

    check(std::string(loader_exe_sha256()) == std::string(LOADER_EXPECTED_SHA256),
          "the loader reports the digest of the image it actually mapped");
}

// ---------------------------------------------------------------------------
// Queued input is runnable work, not something to sleep through.
//
// A host cannot apply input decoded during a scheduler idle slice: another
// guest thread may hold the baton. So it queues, and the queue is drained by
// the host's tick - which the guest reaches through its own clock read. A
// guest BLOCKED on an event never reads its clock, so if the thing that would
// signal that event is sitting in the queue, the wait and the drain are each
// waiting for the other and the thread waits out its whole timeout.
//
// The scheduler therefore drains the queue itself, from the one point where
// nothing else is runnable and so nothing else is in guest code.
// ---------------------------------------------------------------------------
static uint32_t g_wake_event = 0;
static int g_drain_calls = 0;
static bool g_input_queued = false;
static bool fake_input_pending() {
    return g_input_queued;
}
static void fake_input_drain() {
    ++g_drain_calls;
    g_input_queued = false;
    win32_signal_event(g_wake_event, false);
}

static void test_queued_input_wakes_a_blocked_wait(X86 *c) {
    section("queued input is runnable work");
    g_wake_event = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 0, 0, 0});
    check(g_wake_event != 0, "an auto-reset event to wait on");

    g_input_queued = true;
    g_drain_calls = 0;
    sched_set_input_queue(fake_input_pending, fake_input_drain);

    // Nothing else will signal this. Only the queued "input" will, and only if
    // the scheduler decides to drain it rather than sleep.
    double t0 = wall_seconds();
    uint32_t r = call_import(c, "KERNEL32.dll", "WaitForSingleObject", {g_wake_event, 5000});
    double waited = wall_seconds() - t0;

    check(r == 0, "the wait was satisfied rather than timing out (%u)", r);
    check(g_drain_calls >= 1, "the scheduler drained the queue itself");
    check(waited < 1.0, "and it woke in %.0f ms, not by waiting out 5000", waited * 1000.0);

    sched_set_input_queue(nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// The run thread's ending hands the baton on.
//
// A worker runs only when the thread holding the baton gives it up. The run
// thread is never spawned by the scheduler, so nothing announced its ending:
// clearing the thread-local guest flag says nothing about the baton, and a
// worker waiting for it waited for a thread that had already stopped running
// guest code. A host polling for quiescence then polled until its bound
// expired and reported a hang that was really a handover that never happened.
//
// This runs LAST. It retires the main thread, and nothing that drives guest
// code should follow it.
// ---------------------------------------------------------------------------
// A worker that blocks on a finite wait. Nothing but an expiring deadline can
// wake it, which is exactly what a plain polling loop cannot provide once the
// run thread has been retired.
static uint32_t g_drive_slot = 0;
static volatile int g_drive_ran = 0;

static void drive_sleeper(X86 *c) {
    guest_sleep_ms(20);
    ++g_drive_ran;
    set_eax(c, 0x5150);
}

// A worker that sleeps for longer than the first drive's timeout, so the
// second drive begins with it already parked inside guest_block.
static uint32_t g_parked_slot = 0;
static volatile int g_parked_ran = 0;

static void parked_sleeper(X86 *c) {
    guest_sleep_ms(400);
    ++g_parked_ran;
    set_eax(c, 0x5151);
}

// A worker whose deadline expires while another worker is running. It must not
// take the baton back for itself when it wakes: it waits for the handoff.
static uint32_t g_short_slot = 0;
static volatile int g_short_ran = 0;

static void short_sleeper(X86 *c) {
    guest_sleep_ms(2);
    ++g_short_ran;
    set_eax(c, 0x5152);
}

// A worker that stays running for a while, checking throughout that it is
// still the only holder and that nothing expired deadlines underneath it.
static volatile int g_solo_violations = 0;
static volatile int g_solo_running = 0;
static volatile int g_solo_slices = 0;

static void solo_worker(X86 *c) {
    g_solo_running = 1;
    for (int i = 0; i < 200; ++i) {
        // Under the scheduler's own lock, so the answer cannot be torn.
        sched_registry_lock();
        bool mine = sched_holds_baton_locked();
        sched_registry_unlock();
        if (!mine)
            ++g_solo_violations;
        ++g_solo_slices;
        os_sleep_us(200);
    }
    g_solo_running = 0;
    set_eax(c, 0x5010);
}

static void test_run_thread_finished(X86 *c) {
    section("the run thread's ending");
    sched_set_guest_thread(true);

    // A thread that never ran guest code must not be able to retire the run
    // thread on its behalf: t_self defaults to 0 everywhere, so without the
    // pthread check this call from another thread would retire thread 0.
    std::thread([] { sched_run_thread_finished(); }).join();
    check(sched_holds_baton(),
          "a foreign thread cannot retire the run thread: this one still holds "
          "the baton");

    // The unconditional unwind touches only the calling thread's own frames,
    // so it is safe anywhere and is what a teardown path uses when its thread
    // never registered with the scheduler.
    std::thread([] { sched_run_thread_unwind_frames(); }).join();
    check(true, "the unconditional unwind runs on an unregistered thread");

    uint32_t fn = imports_alloc_trampoline("test", "guest_callback", fake_guest_fn, 4);
    g_callback_hits = 0;
    uint32_t th = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, fn, 0x5a, 0, 0});
    check(th != 0, "created a worker (%08x)", th);
    check(!sched_guest_threads_stopped(), "which has not finished yet");

    // Nothing below drives the scheduler: no shim call, no clock read, no
    // wait. The worker cannot run while this thread holds the baton, and this
    // thread is not going to ask for anything that would give it up.
    sched_run_thread_finished();
    bool stopped = false;
    for (int i = 0; i < 1000 && !stopped; ++i) {
        os_sleep_us(1000);
        stopped = sched_guest_threads_stopped();
    }
    check(stopped, "the worker finished once the run thread handed the baton on");

    // With the run thread retired, a worker that BLOCKS cannot be helped by
    // polling: the baton would go to a thread that has finished and nothing
    // would expire its deadline, so a twenty-millisecond sleep would never
    // end. Driving is the primitive that does both.
    g_drive_ran = 0;
    if (!g_drive_slot)
        g_drive_slot = imports_alloc_trampoline("test", "drive_sleeper", drive_sleeper, 1);
    uint32_t th2 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, g_drive_slot, 0, 0, 0});
    check(th2 != 0, "started a worker that blocks on a finite sleep");
    double t0 = wall_seconds();
    bool drove = sched_drive_until_stopped(2.0);
    double took = wall_seconds() - t0;
    check(drove, "driving stopped it even though the run thread had retired");
    // It comes back still registered and still holding the baton, because the
    // caller's next act is to run the mods' exit handlers and those are
    // ordinary mod code: an exit that removes its own hook needs the baton,
    // and without it the removal would be queued on the one thread left to
    // apply the queue.
    check(sched_holds_baton(), "and comes back holding the baton, for the exits");
    sched_drive_release();
    check(!sched_holds_baton(), "which it gives up when the drive is released");
    check(g_drive_ran == 1, "and it really ran its start routine (%d)", g_drive_ran);
    check(took < 1.5, "without waiting out the timeout (%.0f ms)", took * 1000.0);

    // A worker that runs for a while with the driver active alongside it. The
    // driver may not hand its baton to anyone else while it runs, and may not
    // expire deadlines underneath it: expiry satisfies waits and releases
    // mutexes the running thread may be inside, and the scheduler's mutex does
    // not protect those.
    g_solo_violations = 0;
    g_solo_slices = 0;
    static uint32_t solo_slot = 0;
    if (!solo_slot)
        solo_slot = imports_alloc_trampoline("test", "solo_worker", solo_worker, 1);
    uint32_t th3 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, solo_slot, 0, 0, 0});
    check(th3 != 0, "started a worker that runs for a while");
    check(sched_drive_until_stopped(5.0), "the driver ran it to completion");
    check(g_solo_slices > 0, "and it really ran (%d slices)", g_solo_slices);
    check(g_solo_violations == 0, "it held the baton alone throughout (%d moments it did not)",
          g_solo_violations);
    sched_drive_release();
    call_import(c, "KERNEL32.dll", "CloseHandle", {th3});

    // A worker parked in guest_block when the drive starts. The first drive
    // gives it time to get there and then times out; the second begins with it
    // already parked, which is the state the old driver misread - it treated
    // `blocked` as parked and handed the baton away while a worker was still
    // between releasing the mutex and parking.
    g_parked_ran = 0;
    if (!g_parked_slot)
        g_parked_slot = imports_alloc_trampoline("test", "parked_sleeper", parked_sleeper, 1);
    uint32_t th4 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, g_parked_slot, 0, 0, 0});
    check(th4 != 0, "started a worker that sleeps longer than a short drive");
    check(!sched_drive_until_stopped(0.05),
          "a drive that ends before the worker does returns false");
    check(g_parked_ran == 0, "and the worker is still parked, not finished");
    // Releasing after a timeout must not park the baton on this thread. It is
    // retired and will never yield again, so a worker blocked behind it would
    // be stranded for good; the baton goes to a worker that can use it or to
    // nobody, and a later drive picks it up.
    sched_drive_release();
    // The precondition it names is the caller's, not the scheduler's: the
    // worker is fine, the timeout was simply shorter than the sleep.
    check(sched_drive_until_stopped(3.0), "a second drive finds it parked and finishes it");
    check(g_parked_ran == 1, "and it ran to the end (%d)", g_parked_ran);
    sched_drive_release();
    call_import(c, "KERNEL32.dll", "CloseHandle", {th4});

    // A worker whose deadline expires while another one is running. The
    // sleeper wakes in two milliseconds, long before the runner is done, and
    // must wait for the ordinary handoff: taking the baton on waking put it
    // beside the runner and both executed guest code at once.
    g_solo_violations = 0;
    g_solo_slices = 0;
    g_short_ran = 0;
    if (!g_short_slot)
        g_short_slot = imports_alloc_trampoline("test", "short_sleeper", short_sleeper, 1);
    uint32_t th5 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, solo_slot, 0, 0, 0});
    uint32_t th6 = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, g_short_slot, 0, 0, 0});
    check(th5 != 0 && th6 != 0, "started a long runner and a short sleeper");
    check(sched_drive_until_stopped(5.0), "the driver ran both to completion");
    check(g_short_ran == 1, "the sleeper woke and finished (%d)", g_short_ran);
    check(g_solo_slices > 0, "the runner really ran (%d slices)", g_solo_slices);
    check(g_solo_violations == 0,
          "and it held the baton alone throughout, expiry or no expiry (%d "
          "moments it did not)",
          g_solo_violations);
    sched_drive_release();
    call_import(c, "KERNEL32.dll", "CloseHandle", {th5});
    call_import(c, "KERNEL32.dll", "CloseHandle", {th6});

    // Everything is stopped, so asking again is true and immediate.
    t0 = wall_seconds();
    check(sched_drive_until_stopped(2.0), "driving an already-stopped scheduler is true");
    check(wall_seconds() - t0 < 0.5, "and immediate");
    sched_drive_release();
    call_import(c, "KERNEL32.dll", "CloseHandle", {th2});
    check(g_callback_hits == 1, "and it really ran its start routine");

    // Saying it twice is not two endings.
    sched_run_thread_finished();
    check(sched_guest_threads_stopped(), "saying it again changes nothing");
}

// ---------------------------------------------------------------------------
// Input arriving while a guest thread is parked wakes it.
//
// A thread with nothing runnable to hand the baton to parks on the scheduler's
// condition with a deadline of up to a second, draining queued input first.
// Input queued AFTER it parked left it asleep with the input already in hand,
// so a keypress could take a second to be seen for no reason but the slice it
// slept through. sched_input_arrived is what ends that sleep.
// ---------------------------------------------------------------------------
static volatile int g_late_input_queued = 0;
static volatile int g_late_input_drained = 0;
static uint32_t g_late_event = 0;

static bool late_input_pending() {
    return g_late_input_queued > g_late_input_drained;
}

static void late_input_drain() {
    g_late_input_drained = g_late_input_queued;
    // What the host's drain really does: turn input into something the guest
    // is waiting for. Here that is the event, so the wait below ends exactly
    // when the drain runs and the elapsed time is the measurement.
    win32_signal_event(g_late_event, false);
}

static void test_input_wakes_a_parked_thread(X86 *c) {
    section("input arriving wakes a parked guest thread");
    sched_set_guest_thread(true);
    sched_set_input_queue(late_input_pending, late_input_drain);

    g_late_input_queued = 0;
    g_late_input_drained = 0;
    g_late_event = call_import(c, "KERNEL32.dll", "CreateEventA", {0, 1, 0, 0});
    check(g_late_event != 0, "created an event only the drain will signal");

    // A host thread that queues input a little after this thread has parked.
    double queued_at = 0.0;
    std::thread announcer([&] {
        os_sleep_us(80000); // long enough to be parked
        queued_at = wall_seconds();
        g_late_input_queued = 1;
        sched_input_arrived();
    });

    // Nothing else is runnable, so this parks, and only the drain can end it.
    double t0 = wall_seconds();
    uint32_t r = call_import(c, "KERNEL32.dll", "WaitForSingleObject", {g_late_event, 5000});
    double ended = wall_seconds();
    announcer.join();

    check(r == 0, "the wait was satisfied rather than timing out (%u)", r);
    check(g_late_input_drained == 1, "the queued input was drained");
    check(ended - t0 < 2.0, "well inside the 5000 ms deadline (%.0f ms)", (ended - t0) * 1000.0);
    // The point of the announcement: the gap between input arriving and the
    // parked thread acting on it is a slice, not the second it would sleep.
    if (queued_at > 0.0)
        check(ended - queued_at < 0.5, "and within a slice of the input arriving (%.0f ms)",
              (ended - queued_at) * 1000.0);

    sched_set_input_queue(nullptr, nullptr);
    call_import(c, "KERNEL32.dll", "CloseHandle", {g_late_event});
    sched_set_guest_thread(false);
}

static void child_setjmp_abort(X86 *c) {
    // The single-call _setjmp form: a jmp_buf pushed, a fake return address,
    // no enclosing call. It has to abort.
    uint32_t buf2 = scratch_block(64);
    uint32_t sp = c->r[R_ESP] - 4;
    wr32(sp, buf2);
    sp -= 4;
    wr32(sp, g_fake_ret);
    c->r[R_ESP] = sp;
    recomp_setjmp(c);
    os_exit_immediately(0); // reached only if it failed to abort
}

int main(int argc, char **argv) {
    const bool child = argc > 1 && strcmp(argv[1], "--child-setjmp-abort") == 0;
    if (child) {
        freopen(os_null_device(), "w", stdout);
        freopen(os_null_device(), "w", stderr);
    }
    os_setenv("RECOMP_REGISTRY", "build/recomp/registry-test.json");
    if (!recomp_env("LOG"))
        os_setenv("RECOMP_LOG", "1");
    // The suite loads the developer's game image; without one (the kit's
    // stub game, a checkout with no original/) there is nothing to test.
    if (FILE *image = fopen(RECOMP_DEVELOPER_EXE, "rb"))
        fclose(image);
    else {
        printf("runtime_tests: no game image at %s; the game-backed checks were skipped\n",
               RECOMP_DEVELOPER_EXE);
        return 0;
    }

    test_loader();
    test_modules_and_wide();
    X86 *c = loader_context();
    if (child)
        child_setjmp_abort(c);
    scratch = 0x0ee00000; // scratch area below the stack, inside the arena

    test_allocator();
    test_heap_shims(c);
    test_memory_shims_2(c);
    test_files(c);
    test_version_resource(c);
    test_pinned_clock(c);
    test_cadence_trace(c);
    test_misc_shims(c);
    test_boot_shims(c);
    test_gdi_and_com(c);
    test_cxx_throw_description(c);
    test_native_draw_waits(c);
    test_midi(c);
    test_windows(c);
    test_callbacks(c);
    test_scheduling(c);
    test_mod_seams(c);
    test_input_wakes_a_parked_thread(c);
    test_intrinsics(c);
    test_undeliverable_calls(c);
    test_registry(c);
    test_import_coverage(c);
    test_queued_input_wakes_a_blocked_wait(c);
    // Last: it retires the main thread.
    test_run_thread_finished(c);

    printf("\n%d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skips);
    return g_failures ? 1 : 0;
}
