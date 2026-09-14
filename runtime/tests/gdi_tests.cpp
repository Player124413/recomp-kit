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
static void test_window_surface_and_blits(bool text = true) {
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
    if (!text) {
        call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
        return;
    }
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
    check(call_import(&c, "GDI32.dll", "SelectObject", {0x12345, 0x4f100}) == 0,
          "invalid DC is not implicitly created");
    uint32_t stock = call_import(&c, "GDI32.dll", "GetStockObject", {0});
    call_import(&c, "GDI32.dll", "DeleteObject", {stock});
    check(call_import(&c, "GDI32.dll", "GetObjectW", {stock, 12, s + 900}) == 12,
          "stock object survives DeleteObject");
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
    check(presents == 1 && presented.size() == 1024 * 768 && presented[2 * 1024 + 2] == 0xffff0000,
          "EndPaint presents owned ARGB pixels in the desktop composite");
    dc = call_import(&c, "USER32.dll", "GetDC", {hwnd});
    primary_active = true;
    call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
    check(presents == 1, "unchanged window does not present again with a primary");
    primary_active = false;
    call_import(&c, "USER32.dll", "DestroyWindow", {hwnd});
    check(call_import(&c, "USER32.dll", "GetDC", {hwnd}) == 0, "destroyed window has no DC");
    call_import(&c, "GDI32.dll", "DeleteObject", {brush});
}
static void test_drawing() {
    X86 c;
    loader_init_context(&c);
    uint32_t s = 0x00304000, hwnd = make_test_window(&c, s, 32, 32);
    uint32_t dc = call_import(&c, "USER32.dll", "GetDC", {hwnd});
    uint32_t brush = call_import(&c, "GDI32.dll", "CreateSolidBrush", {0xff00});
    call_import(&c, "GDI32.dll", "SelectObject", {dc, brush});
    check(call_import(&c, "GDI32.dll", "Rectangle", {dc, 1, 1, 10, 10}) == 1 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 4, 4}) == 0xff00,
          "rectangle brush fill");
    call_import(&c, "GDI32.dll", "SaveDC", {dc});
    check(call_import(&c, "GDI32.dll", "IntersectClipRect", {dc, 4, 4, 12, 12}) == 2,
          "rectangular clip");
    check(call_import(&c, "GDI32.dll", "ExcludeClipRect", {dc, 6, 6, 8, 8}) == 3, "clip hole");
    call_import(&c, "GDI32.dll", "PatBlt", {dc, 0, 0, 32, 32, 0x00ff0062});
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 5, 5}) == 0xffffff,
          "clip accepts inner pixels");
    call_import(&c, "GDI32.dll", "RestoreDC", {dc, uint32_t(-1)});
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 6, 6}) == 0xff00 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 0, 0}) == 0,
          "clip hole and outside preserved");
    memset(g_mem + s, 0, 16);
    wr32(s + 4, 1);
    wr32(s + 12, 0xff);
    uint32_t pen = call_import(&c, "GDI32.dll", "CreatePenIndirect", {s});
    call_import(&c, "GDI32.dll", "SelectObject", {dc, pen});
    call_import(&c, "GDI32.dll", "MoveToEx", {dc, 0, 16, 0});
    check(call_import(&c, "GDI32.dll", "LineTo", {dc, 16, 16}) == 1 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 8, 16}) == 0xff,
          "pen draws a line");
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 16, 16}) == 0,
          "LineTo excludes its endpoint");
    call_import(&c, "GDI32.dll", "SetROP2", {dc, 7});
    call_import(&c, "GDI32.dll", "MoveToEx", {dc, 0, 16, 0});
    call_import(&c, "GDI32.dll", "LineTo", {dc, 16, 16});
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 8, 16}) == 0,
          "R2_XORPEN restores line pixels");
    uint32_t rgn = call_import(&c, "GDI32.dll", "CreateRectRgn", {2, 3, 7, 9});
    check(rgn && call_import(&c, "GDI32.dll", "GetRgnBox", {rgn, s}) == 2 && rd32(s) == 2 &&
              rd32(s + 12) == 9,
          "region bounds");
    uint32_t mem = call_import(&c, "GDI32.dll", "CreateCompatibleDC", {dc});
    memset(g_mem + s, 0, 40);
    wr32(s, 40);
    wr32(s + 4, 2);
    wr32(s + 8, 2);
    wr16(s + 12, 1);
    wr16(s + 14, 32);
    uint32_t dib = call_import(&c, "GDI32.dll", "CreateDIBSection", {mem, s, 0, s + 64, 0, 0}),
             bits = rd32(s + 64);
    call_import(&c, "GDI32.dll", "SelectObject", {mem, dib});
    wr32(bits, 0xff0000);
    wr32(bits + 4, 0xff0000);
    wr32(bits + 8, 0xff);
    wr32(bits + 12, 0xff);
    check(call_import(&c, "GDI32.dll", "BitBlt", {dc, 20, 20, 2, 2, mem, 0, 0, 0xcc0020}) == 1 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 20, 20}) == 0xff0000 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 20, 21}) == 0xff,
          "bottom-up DIB orientation and COLORREF conversion");
    call_import(&c, "GDI32.dll", "SetPixel", {mem, 0, 0, 0xff00});
    check(rd32(bits + 8) == 0xff00ff00, "DIB writes stay in guest memory");
    check(call_import(&c, "GDI32.dll", "GetBitmapBits", {dib, 16, s + 128}) == 16 &&
              rd32(s + 136) == 0xff00ff00,
          "GetBitmapBits raw bytes");
    check(call_import(&c, "GDI32.dll", "MaskBlt",
                      {dc, 24, 24, 2, 2, mem, 0, 0, 0, 0, 0, 0xcc0020}) == 1 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 24, 24}) == 0xff00,
          "zero-mask MaskBlt");
    uint32_t copy = call_import(&c, "GDI32.dll", "CreateDIBitmap", {dc, s, 4, bits, s, 0});
    check(copy && call_import(&c, "GDI32.dll", "GetObjectW", {copy, 24, s + 160}) == 24 &&
              rd32(s + 164) == 2,
          "CreateDIBitmap initializes an owned copy");
    check(call_import(&c, "GDI32.dll", "StretchDIBits",
                      {dc, 0, 24, 4, 4, 0, 0, 2, 2, bits, s, 0, 0xcc0020}) == 2 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 0, 24}) == 0xff00,
          "StretchDIBits writes canvas pixels");
    call_import(&c, "GDI32.dll", "DeleteObject", {copy});
    call_import(&c, "GDI32.dll", "DeleteDC", {mem});
    call_import(&c, "GDI32.dll", "DeleteObject", {dib});
    call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
    call_import(&c, "USER32.dll", "DestroyWindow", {hwnd});
}
static void test_dib_rows_and_regions() {
    X86 c;
    loader_init_context(&c);
    uint32_t s = 0x00308000;
    uint32_t hwnd = make_test_window(&c, s, 16, 16),
             dc = call_import(&c, "USER32.dll", "GetDC", {hwnd});
    uint32_t child = call_import(&c, "USER32.dll", "CreateWindowExW",
                                 {0, s + 0x800, 0, 0x40000000, 4, 4, 2, 2, hwnd, 0, IMAGE_BASE, 0});
    uint32_t child_dc = call_import(&c, "USER32.dll", "GetDC", {child});
    call_import(&c, "GDI32.dll", "SetPixel", {child_dc, 0, 0, 0xff});
    call_import(&c, "GDI32.dll", "SetPixel", {child_dc, 2, 0, 0xff});
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 4, 4}) == 0xff &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 6, 4}) == 0,
          "child canvas maps and clips to its client bounds");
    call_import(&c, "USER32.dll", "ReleaseDC", {child, child_dc});
    memset(g_mem + s, 0, 40);
    wr32(s, 40);
    wr32(s + 4, 2);
    wr32(s + 8, 2);
    wr16(s + 12, 1);
    wr16(s + 14, 32);
    wr32(s + 64, 0x00ff0000);
    wr32(s + 68, 0x000000ff);
    check(call_import(&c, "GDI32.dll", "SetDIBitsToDevice",
                      {dc, 0, 0, 2, 2, 0, 0, 0, 1, s + 64, s, 0}) == 1 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 0, 1}) == 0xff &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 1, 1}) == 0xff0000 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 0, 0}) == 0,
          "partial bottom-up DIB upload places its first scan at the bottom");
    uint32_t bitmap = call_import(&c, "GDI32.dll", "CreateDIBSection", {dc, s, 0, s + 128, 0, 0});
    uint32_t bits = rd32(s + 128);
    check(call_import(&c, "GDI32.dll", "SetDIBits", {dc, bitmap, 1, 1, s + 64, s, 0}) == 1 &&
              rd32(bits + 8) == 0xffff0000 && rd32(bits + 12) == 0xff0000ff && rd32(bits) == 0,
          "SetDIBits writes only the requested scan");
    uint32_t mem = call_import(&c, "GDI32.dll", "CreateCompatibleDC", {0});
    call_import(&c, "GDI32.dll", "SelectObject", {mem, bitmap});
    call_import(&c, "GDI32.dll", "SetPixel", {mem, 0, 0, 0xff});
    call_import(&c, "GDI32.dll", "SetPixel", {mem, 1, 0, 0xff00});
    call_import(&c, "GDI32.dll", "BitBlt", {mem, 1, 0, 1, 1, mem, 0, 0, 0xcc0020});
    check(call_import(&c, "GDI32.dll", "GetPixel", {mem, 1, 0}) == 0xff,
          "overlapping self blit snapshots the source");
    uint32_t brush = call_import(&c, "GDI32.dll", "CreateSolidBrush", {0xff00});
    call_import(&c, "GDI32.dll", "SelectObject", {dc, brush});
    check(call_import(&c, "GDI32.dll", "ExtFloodFill", {dc, 15, 15, 0, 1}) == 1 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 15, 0}) == 0xff00 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 4, 4}) == 0xff,
          "flood fill is bounded by different colors");
    uint32_t pen = call_import(&c, "GDI32.dll", "GetStockObject", {8});
    call_import(&c, "GDI32.dll", "SelectObject", {dc, pen});
    call_import(&c, "GDI32.dll", "SetPixel", {dc, 11, 11, 0xff});
    call_import(&c, "GDI32.dll", "SetROP2", {dc, 7});
    call_import(&c, "GDI32.dll", "Rectangle", {dc, 10, 10, 14, 14});
    call_import(&c, "GDI32.dll", "Rectangle", {dc, 10, 10, 14, 14});
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 11, 11}) == 0xff,
          "XOR brush fill is reversible");
    call_import(&c, "GDI32.dll", "DeleteDC", {mem});
    call_import(&c, "GDI32.dll", "DeleteObject", {bitmap});
    call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
    call_import(&c, "USER32.dll", "DestroyWindow", {hwnd});
}
static void test_text() {
    X86 c;
    loader_init_context(&c);
    uint32_t s = 0x00306000;
    uint32_t hwnd = make_test_window(&c, s, 64, 64),
             dc = call_import(&c, "USER32.dll", "GetDC", {hwnd});
    memset(g_mem + s, 0, 92);
    wr32(s, uint32_t(-32));
    wr32(s + 16, 700);
    gm_put_wstr(s + 28, "recomp", 32);
    uint32_t font = call_import(&c, "GDI32.dll", "CreateFontIndirectW", {s});
    check(font != 0, "CreateFontIndirectW");
    call_import(&c, "GDI32.dll", "SelectObject", {dc, font});
    check(call_import(&c, "GDI32.dll", "GetObjectW", {font, 92, s + 128}) == 92 &&
              rd32(s + 128) == uint32_t(-32) && rd32(s + 144) == 700,
          "font height and weight round trip");
    gm_put_wstr(s + 256, "AB", 4);
    check(call_import(&c, "GDI32.dll", "GetTextExtentPointW", {dc, s + 256, 2, s + 300}) == 1 &&
              rd32(s + 300) == 32 && rd32(s + 304) == 32,
          "scaled text extent");
    check(call_import(&c, "GDI32.dll", "GetTextMetricsW", {dc, s + 320}) == 1 &&
              rd32(s + 320) == 32 && rd32(s + 324) == 26 && rd32(s + 328) == 6 &&
              rd32(s + 340) == 16 && rd32(s + 344) == 16 && rd8(s + 375) == 0x30 &&
              rd8(s + 376) == 0,
          "TEXTMETRICW layout and scaled metrics");
    call_import(&c, "GDI32.dll", "SetTextColor", {dc, 0xff});
    call_import(&c, "GDI32.dll", "SetBkColor", {dc, 0xff0000});
    check(call_import(&c, "GDI32.dll", "ExtTextOutW", {dc, 0, 0, 0, 0, s + 256, 1, 0}) == 1 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 15, 31}) == 0xff0000,
          "opaque scaled text background");
    wr32(s + 400, 20);
    wr32(s + 404, 20);
    wr32(s + 408, 24);
    wr32(s + 412, 24);
    call_import(&c, "GDI32.dll", "SetBkMode", {dc, 1});
    check(call_import(&c, "GDI32.dll", "ExtTextOutW", {dc, 0, 0, 2, s + 400, 0, 0, 0}) == 1 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 20, 20}) == 0xff0000,
          "ETO_OPAQUE with an empty string");
    static unsigned callbacks = 0;
    uint32_t cb = imports_alloc_trampoline(
        "TEST", "FontCallback",
        [](X86 *cc) {
            ++callbacks;
            check(gm_wstr(arg(cc, 0) + 28) == "recomp" && rd32(arg(cc, 1)) == 16 &&
                      arg(cc, 3) == 77,
                  "font enumeration guest payload");
            set_eax(cc, 42);
        },
        4);
    check(call_import(&c, "GDI32.dll", "EnumFontsW", {dc, 0, cb, 77}) == 42 && callbacks == 1,
          "EnumFontsW dispatches once through recomp_call");
    check(call_import(&c, "GDI32.dll", "EnumFontFamiliesExW", {dc, s, cb, 77, 0}) == 42 &&
              callbacks == 2,
          "EnumFontFamiliesExW dispatches once");
    check(call_import(&c, "GDI32.dll", "AddFontMemResourceEx", {s, 92, 0, s + 500}) != 0 &&
              rd32(s + 500) == 1,
          "memory font resource uses built-in font");
    check(call_import(&c, "GDI32.dll", "CreateDCW", {0, 0, 0, 0}) == 0 &&
              call_import(&c, "GDI32.dll", "StartDocW", {dc, 0}) == 0 &&
              call_import(&c, "GDI32.dll", "GetEnhMetaFileBits", {0, 0, 0}) == 0,
          "printing and metafiles fail with correct arities");
    call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
    call_import(&c, "USER32.dll", "DestroyWindow", {hwnd});
    call_import(&c, "GDI32.dll", "DeleteObject", {font});
}
// DrawText must paint through the same selected-font canvas as ExtTextOut,
// including its colour and clipping. Successful metrics alone are not drawing.
static void test_draw_text() {
    X86 c;
    loader_init_context(&c);
    uint32_t s = 0x00309000, rect = s + 256, str = s + 320;
    uint32_t hwnd = make_test_window(&c, s, 64, 64);
    uint32_t dc = call_import(&c, "USER32.dll", "GetDC", {hwnd});
    uint32_t brush = call_import(&c, "GDI32.dll", "CreateSolidBrush", {0xff00ff});
    auto bounds = [&](uint32_t l, uint32_t t, uint32_t r, uint32_t b) {
        wr32(rect, l);
        wr32(rect + 4, t);
        wr32(rect + 8, r);
        wr32(rect + 12, b);
    };
    bounds(0, 0, 64, 64);
    call_import(&c, "USER32.dll", "FillRect", {dc, rect, brush});
    call_import(&c, "GDI32.dll", "SetTextColor", {dc, 0});
    call_import(&c, "GDI32.dll", "SetBkMode", {dc, 1});
    gm_put_wstr(str, "AB", 8);
    bounds(4, 4, 36, 28);
    check(call_import(&c, "USER32.dll", "DrawTextW", {dc, str, 2, rect, 0x25}) == 20,
          "DrawTextW centered single line returns bottom offset");
    // Reference at (12,8): centered horizontally and vertically in the rectangle.
    call_import(&c, "GDI32.dll", "ExtTextOutW", {dc, 12, 40, 0, 0, str, 2, 0});
    bool same = true, ink = false;
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x) {
            uint32_t actual = call_import(&c, "GDI32.dll", "GetPixel", {dc, 12 + x, 8 + y});
            same &= actual == call_import(&c, "GDI32.dll", "GetPixel", {dc, 12 + x, 40 + y});
            ink |= actual == 0;
        }
    check(same && ink, "DrawTextW paints the bitmap font in black on magenta");
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 12, 8}) == 0xff00ff,
          "transparent text preserves the background between glyph pixels");
    memset(g_mem + s, 0, 92);
    wr32(s, uint32_t(-32));
    uint32_t font = call_import(&c, "GDI32.dll", "CreateFontIndirectW", {s});
    uint32_t old_font = call_import(&c, "GDI32.dll", "SelectObject", {dc, font});
    bounds(1, 2, 60, 60);
    check(call_import(&c, "USER32.dll", "DrawTextW", {dc, str, 2, rect, 0x420}) == 32 &&
              rd32(rect + 8) == 33 && rd32(rect + 12) == 34,
          "DrawTextW CALCRECT uses the selected font's 16x32 cells");
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 2, 2}) == 0xff00ff,
          "CALCRECT leaves pixels untouched");
    uint32_t screen_dc = call_import(&c, "USER32.dll", "GetDC", {0});
    call_import(&c, "GDI32.dll", "SelectObject", {screen_dc, font});
    bounds(1, 2, 60, 60);
    check(call_import(&c, "USER32.dll", "DrawTextW", {screen_dc, str, 2, rect, 0x420}) == 32 &&
              rd32(rect + 8) == 33 && rd32(rect + 12) == 34,
          "CALCRECT measures a screen DC without a backing surface");
    call_import(&c, "USER32.dll", "ReleaseDC", {0, screen_dc});
    call_import(&c, "GDI32.dll", "SelectObject", {dc, old_font});
    bounds(0, 0, 64, 64);
    call_import(&c, "USER32.dll", "FillRect", {dc, rect, brush});
    bounds(0, 0, 4, 8);
    uint32_t params = s + 400;
    memset(g_mem + params, 0, 20);
    wr32(params, 20);
    check(call_import(&c, "USER32.dll", "DrawTextExW", {dc, str, 2, rect, 0, params}) == 16 &&
              rd32(params + 16) == 2,
          "DrawTextExW paints and reports consumed UTF-16 units");
    check(call_import(&c, "GDI32.dll", "GetPixel", {dc, 3, 0}) == 0 &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 4, 0}) == 0xff00ff &&
              call_import(&c, "GDI32.dll", "GetPixel", {dc, 1, 8}) == 0xff00ff,
          "DrawText clips glyphs to the supplied rectangle");
    check(call_import(&c, "GDI32.dll", "SetPixel", {dc, 20, 20, 0xff}) == 0xff,
          "DrawText restores the caller's clipping state");
    gm_put_wstr(str, "AB CD", 16);
    bounds(0, 0, 24, 64);
    check(call_import(&c, "USER32.dll", "DrawTextW", {dc, str, UINT32_MAX, rect, 0x410}) == 32 &&
              rd32(rect + 8) == 16 && rd32(rect + 12) == 32,
          "DrawText wraps between words when measuring a narrow rectangle");
    gm_put_wstr(str, "A\r\nB", 16);
    bounds(0, 0, 64, 64);
    check(call_import(&c, "USER32.dll", "DrawTextW", {dc, str, UINT32_MAX, rect, 0x400}) == 32 &&
              rd32(rect + 8) == 8,
          "DrawText treats CRLF as one line break");
    gm_put_wstr(str, "&A&&B", 16);
    bounds(0, 0, 64, 64);
    check(call_import(&c, "USER32.dll", "DrawTextW", {dc, str, UINT32_MAX, rect, 0x420}) == 16 &&
              rd32(rect + 8) == 24,
          "DrawText measures mnemonic prefixes and escaped ampersands");
    call_import(&c, "GDI32.dll", "DeleteObject", {font});
    call_import(&c, "GDI32.dll", "DeleteObject", {brush});
    call_import(&c, "USER32.dll", "ReleaseDC", {hwnd, dc});
    call_import(&c, "USER32.dll", "DestroyWindow", {hwnd});
}
// Exercise msimg32 through real stdcall trampolines and top-down guest DIBs.
static void test_msimg32() {
    X86 c;
    loader_init_context(&c);
    const uint32_t s = 0x00310000;
    struct Canvas {
        uint32_t dc, bitmap, bits;
    };
    auto canvas = [&](uint32_t w, uint32_t h) {
        uint32_t dc = call_import(&c, "GDI32.dll", "CreateCompatibleDC", {0});
        memset(g_mem + s, 0, 40);
        wr32(s, 40);
        wr32(s + 4, w);
        wr32(s + 8, -h);
        wr16(s + 12, 1);
        wr16(s + 14, 32);
        uint32_t bitmap =
            call_import(&c, "GDI32.dll", "CreateDIBSection", {dc, s, 0, s + 64, 0, 0});
        call_import(&c, "GDI32.dll", "SelectObject", {dc, bitmap});
        return Canvas{dc, bitmap, rd32(s + 64)};
    };
    auto dst = canvas(8, 8), src = canvas(2, 1);
    uint32_t vertices = s + 128, mesh = s + 256;
    auto vertex = [&](uint32_t i, uint32_t x, uint32_t y, uint32_t rgb) {
        uint32_t p = vertices + 16 * i;
        wr32(p, x);
        wr32(p + 4, y);
        wr16(p + 8, ((rgb >> 16) & 255) * 257);
        wr16(p + 10, ((rgb >> 8) & 255) * 257);
        wr16(p + 12, (rgb & 255) * 257);
        wr16(p + 14, 0);
    };
    // The right/bottom edge is exclusive: pixel 7 is 7/8 along this ramp.
    auto strip = canvas(8, 1);
    vertex(0, 0, 0, 0);
    vertex(1, 8, 1, 0xffffff);
    wr32(mesh, 0);
    wr32(mesh + 4, 1);
    check(call_import(&c, "msimg32.dll", "GradientFill", {strip.dc, vertices, 2, mesh, 1, 0}) ==
                  1 &&
              rd32(strip.bits) == 0xff000000 && rd32(strip.bits + 16) == 0xff808080 &&
              rd32(strip.bits + 28) == 0xffdfdfdf,
          "horizontal gradient endpoints and midpoint in an 8x1 DIB");
    vertex(1, 1, 8, 0xffffff);
    check(call_import(&c, "msimg32.dll", "GradientFill", {dst.dc, vertices, 2, mesh, 1, 1}) == 1 &&
              rd32(dst.bits + 4 * 8 * 4) == 0xff808080,
          "vertical gradient midpoint");
    memset(g_mem + dst.bits, 0, 8 * 8 * 4);
    vertex(0, 0, 0, 0xff0000);
    vertex(1, 8, 0, 0x00ff00);
    vertex(2, 0, 8, 0x0000ff);
    wr32(mesh + 8, 2);
    check(call_import(&c, "msimg32.dll", "GradientFill", {dst.dc, vertices, 3, mesh, 1, 2}) == 1 &&
              rd32(dst.bits + 4 * 9) == 0xff9f3030 && rd32(dst.bits + 4 * 63) == 0,
          "triangle barycentric interior and untouched exterior");
    memset(g_mem + dst.bits, 0, 8 * 8 * 4);
    call_import(&c, "GDI32.dll", "SaveDC", {dst.dc});
    call_import(&c, "GDI32.dll", "SetViewportOrgEx", {dst.dc, 2, 2, 0});
    call_import(&c, "GDI32.dll", "IntersectClipRect", {dst.dc, 1, 0, 3, 1});
    vertex(0, 0, 0, 0xff0000);
    vertex(1, 4, 1, 0xff0000);
    check(call_import(&c, "msimg32.dll", "GradientFill", {dst.dc, vertices, 2, mesh, 1, 0}) == 1 &&
              rd32(dst.bits + 4 * 19) == 0xffff0000 && rd32(dst.bits + 4 * 18) == 0 &&
              rd32(dst.bits + 4 * 21) == 0,
          "gradient honors the DC origin and clip");
    call_import(&c, "GDI32.dll", "RestoreDC", {dst.dc, uint32_t(-1)});
    auto blue = [&] {
        for (unsigned i = 0; i < 64; ++i)
            wr32(dst.bits + 4 * i, 0xff0000ff);
    };
    blue();
    wr32(src.bits, 0x80800000);
    wr32(src.bits + 4, 0);
    check(call_import(&c, "msimg32.dll", "AlphaBlend",
                      {dst.dc, 0, 0, 4, 1, src.dc, 0, 0, 2, 1, 0x01ff0000}) == 1 &&
              rd32(dst.bits) == 0xff80007f && rd32(dst.bits + 4) == 0xff80007f &&
              rd32(dst.bits + 8) == 0xff0000ff,
          "premultiplied half-alpha red over blue with nearest-neighbor scaling");
    blue();
    check(call_import(&c, "msimg32.dll", "AlphaBlend",
                      {dst.dc, 0, 0, 1, 1, src.dc, 0, 0, 1, 1, 0x01800000}) == 1 &&
              rd32(dst.bits) == 0xff4000bf,
          "constant alpha multiplies per-pixel alpha and premultiplied color");
    blue();
    wr32(src.bits, 0x00ff0000);
    check(call_import(&c, "msimg32.dll", "AlphaBlend",
                      {dst.dc, 0, 0, 1, 1, src.dc, 0, 0, 1, 1, 0x00800000}) == 1 &&
              rd32(dst.bits) == 0xff80007f,
          "constant-only alpha ignores the source alpha byte");
    blue();
    wr32(src.bits, 0x12345678);
    wr32(src.bits + 4, 0x4400ff00);
    check(call_import(&c, "msimg32.dll", "TransparentBlt",
                      {dst.dc, 0, 0, 4, 1, src.dc, 0, 0, 2, 1, 0x00785634}) == 1 &&
              rd32(dst.bits) == 0xff0000ff && rd32(dst.bits + 4) == 0xff0000ff &&
              rd32(dst.bits + 8) == 0x4400ff00,
          "scaled color-key blit leaves keyed pixels untouched and copies alpha");
    uint32_t empty = call_import(&c, "GDI32.dll", "CreateCompatibleDC", {0});
    set_last_error(0);
    check(call_import(&c, "msimg32.dll", "GradientFill", {empty, vertices, 2, mesh, 1, 0}) == 0 &&
              get_last_error() == 6,
          "gradient rejects a DC without storage with ERROR_INVALID_HANDLE");
    set_last_error(0);
    check(call_import(&c, "msimg32.dll", "AlphaBlend",
                      {dst.dc, 0, 0, 1, 1, empty, 0, 0, 1, 1, 0x00ff0000}) == 0 &&
              get_last_error() == 6,
          "alpha blend rejects a source without storage");
    set_last_error(0);
    check(call_import(&c, "msimg32.dll", "TransparentBlt",
                      {empty, 0, 0, 1, 1, src.dc, 0, 0, 1, 1, 0}) == 0 &&
              get_last_error() == 6,
          "transparent blit rejects a destination without storage");
    wr32(mesh + 4, 3);
    check(call_import(&c, "msimg32.dll", "GradientFill", {dst.dc, vertices, 2, mesh, 1, 0}) == 0 &&
              get_last_error() == 87,
          "gradient rejects an out-of-range vertex index");
    for (auto item : {dst, src, strip}) {
        call_import(&c, "GDI32.dll", "DeleteDC", {item.dc});
        call_import(&c, "GDI32.dll", "DeleteObject", {item.bitmap});
    }
    call_import(&c, "GDI32.dll", "DeleteDC", {empty});
}

int main(int argc, char **argv) {
    mem_init();
    imports_init();
    test_model();
    if (argc < 2) {
        test_text();
        test_draw_text();
    }
    if (argc < 2 || strcmp(argv[1], "model") != 0) {
        test_drawing();
        test_msimg32();
        test_dib_rows_and_regions();
        test_window_surface_and_blits(argc < 2 || strcmp(argv[1], "draw") != 0);
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}
