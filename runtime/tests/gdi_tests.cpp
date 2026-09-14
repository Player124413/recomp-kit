// Guest import integration tests for window and memory canvases; no game image required.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>
static int g_checks = 0, g_failures = 0;
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
static uint32_t g_fake_ret = 0;

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
    uint32_t expected =
        imports_argc(tramp) == ARGC_CDECL ? before - uint32_t(args.size()) * 4 : before;
    if (c->r[R_ESP] != expected) {
        printf("  [FAIL] %s!%s left ESP at %08x, expected %08x (bad argc?)\n", dll, name,
               c->r[R_ESP], expected);
        ++g_failures;
        ++g_checks;
        c->r[R_ESP] = before;
    }
    c->r[R_ESP] = before; // cdecl callers remove their arguments.
    return c->r[R_EAX];
}

static uint32_t make_test_window(X86 *c, uint32_t s, uint32_t w, uint32_t h) {
    memset(g_mem + s, 0, 40);
    wr32(s + 4, imports_resolve("USER32.dll", "DefWindowProcW"));
    gm_put_wstr(s + 0x800, "CanvasTest", 32);
    wr32(s + 36, s + 0x800);
    check(call_import(c, "USER32.dll", "RegisterClassW", {s}) != 0, "RegisterClassW");
    return call_import(c, "USER32.dll", "CreateWindowExW",
                       {0, s + 0x800, 0, 0, 0, 0, w, h, 0, 0, IMAGE_BASE, 0});
}
static void test_window_surface_and_blits() {
    X86 c;
    loader_init_context(&c);
    uint32_t s = 0x00300000;
    uint32_t hwnd =
        make_test_window(&c, s, 64, 48); // RegisterClassW + CreateWindowExW as in test_user32_vcl
    uint32_t dc = call_import(&c, "USER32.dll", "GetDC", {hwnd});
    check(dc != 0, "GetDC");
    uint32_t brush = call_import(&c, "GDI32.dll", "CreateSolidBrush",
                                 {0x00ff0000u}); // COLORREF 0x00bbggrr: blue
    uint32_t rect = s + 0x100;
    wr32(rect, 4);
    wr32(rect + 4, 4);
    wr32(rect + 8, 20);
    wr32(rect + 12, 20);
    check(call_import(&c, "USER32.dll", "FillRect", {dc, rect, brush}) != 0, "FillRect");
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 5, 5}) == 0x00ff0000u,
          "the fill is readable");
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 30, 30}) == 0x00000000u,
          "outside the rectangle is untouched");
    // A memory DC with a DIB section, blitted onto the window with a stretch.
    uint32_t mem = call_import(&c, "GDI32.dll", "CreateCompatibleDC", {dc});
    uint32_t bmi = s + 0x200;
    memset(g_mem + bmi, 0, 40);
    wr32(bmi, 40);
    wr32(bmi + 4, 8);
    wr32(bmi + 8, 8);
    wr16(bmi + 12, 1);
    wr16(bmi + 14, 32);
    uint32_t bits_out = s + 0x300;
    uint32_t dib = call_import(&c, "GDI32.dll", "CreateDIBSection", {mem, bmi, 0, bits_out, 0, 0});
    check(dib != 0, "CreateDIBSection");
    uint32_t bits = rd32(bits_out);
    for (int i = 0; i < 64; ++i)
        wr32(bits + 4 * i, 0x0000ff00u);
    call_import(&c, "GDI32.dll", "SelectObject", {mem, dib});
    check(call_import(&c, "GDI32.dll", "StretchBlt",
                      {dc, 32, 0, 16, 16, mem, 0, 0, 8, 8, 0x00cc0020u}) == 1,
          "StretchBlt");
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 47, 15}) == 0x0000ff00u,
          "the stretched blit reached the corner");
    // Text: a glyph is drawn in the text colour and measured at 8x16.
    call_import(&c, "GDI32.dll", "SetTextColor", {dc, 0x000000ffu});
    gm_put_wstr(s + 0x400, "A", 4);
    check(call_import(&c, "GDI32.dll", "ExtTextOutW", {dc, 0, 32, 0, 0, s + 0x400, 1, 0}) == 1,
          "ExtTextOutW");
    uint32_t sz = s + 0x500;
    check(call_import(&c, "GDI32.dll", "GetTextExtentPoint32W", {dc, s + 0x400, 1, sz}) == 1 &&
              rd32(sz) == 8 && rd32(sz + 4) == 16,
          "GetTextExtentPoint32W = %ux%u", rd32(sz), rd32(sz + 4));
    bool any_red = false;
    for (int y = 32; y < 48 && !any_red; ++y)
        for (int x = 0; x < 8; ++x)
            if (call_import(&c, "GDI32.dll", "GetPixel", {dc, (uint32_t)x, (uint32_t)y}) ==
                0x000000ffu) {
                any_red = true;
                break;
            }
    check(any_red, "the glyph left red pixels");
    call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
}
// Capture the synchronous window presentation seam; the host must copy before returning.
static int presents = 0;
static bool primary_active = false;
static std::vector<uint32_t> presented;
extern "C" bool ddraw_gdi_primary_active() {
    return primary_active;
}
extern "C" void host_display_present_window(const uint32_t *argb, int w, int h) {
    ++presents;
    presented.assign(argb, argb + size_t(w) * h);
}
static void test_model() {
    X86 c;
    loader_init_context(&c);
    uint32_t s = 0x00302000;
    uint32_t hwnd = make_test_window(&c, s, 32, 24);
    uint32_t dc = call_import(&c, "USER32.dll", "GetDC", {hwnd});
    uint32_t brush = call_import(&c, "GDI32.dll", "CreateSolidBrush", {0xff});
    uint32_t old = call_import(&c, "GDI32.dll", "SelectObject", {dc, brush});
    check(old && old != brush, "SelectObject returns the previous brush");
    uint32_t save = call_import(&c, "GDI32.dll", "SaveDC", {dc});
    check(save == 1, "SaveDC first level");
    call_import(&c, "GDI32.dll", "SetWindowOrgEx", {dc, 3, 4, s});
    call_import(&c, "GDI32.dll", "SetViewportOrgEx", {dc, 5, 6, 0});
    call_import(&c, "GDI32.dll", "SetPixel", {dc, 0, 0, 0xff});
    check(call_import(&c, "GDI32.dll", "RestoreDC", {dc, uint32_t(-1)}) == 1,
          "RestoreDC relative level");
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 2, 2}) == 0xff,
          "window and viewport origins transform pixel coordinates");
    check(call_import(&c, "GDI32.dll", "RestoreDC", {dc, save}) == 0,
          "restored levels are discarded");
    check(call_import(&c, "GDI32.dll", "GetDeviceCaps", {dc, 12}) == 32 &&
              call_import(&c, "GDI32.dll", "GetDeviceCaps", {dc, 38}) == 0x2a01 &&
              call_import(&c, "GDI32.dll", "GetDeviceCaps", {dc, 88}) == 96,
          "32-bit canvas raster and DPI capabilities");
    call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
    check(presents == 0, "hidden window does not present");
    call_import(&c, "USER32.dll", "ShowWindow", {hwnd, 5});
    dc = call_import(&c, "USER32.dll", "BeginPaint", {hwnd, s});
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 2, 2}) == 0xff,
          "window pixels survive DC release");
    call_import(&c, "USER32.dll", "EndPaint", {hwnd, s});
    check(presents == 1 && presented.size() == 32 * 24 && presented[2 * 32 + 2] == 0xffff0000,
          "EndPaint presents owned ARGB window pixels");
    dc = call_import(&c, "USER32.dll", "GetDC", {hwnd});
    primary_active = true;
    call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
    check(presents == 1, "DirectDraw primary suppresses window presentation");
    primary_active = false;
    call_import(&c, "USER32.dll", "DestroyWindow", {hwnd});
    check(call_import(&c, "USER32.dll", "GetDC", {hwnd}) == 0, "destroyed window has no DC");
    call_import(&c, "GDI32.dll", "DeleteObject", {brush});
}
int main(int argc, char **argv) {
    mem_init();
    imports_init();
    test_model();
    if (argc < 2 || strcmp(argv[1], "model") != 0)
        test_window_surface_and_blits();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}
