// gdi32.cpp - the GDI a software-rendered game leans on.
//
// The game draws into a DIB section (a bitmap whose pixels live in memory it
// can address) and moves the result onto its DirectDraw surface itself, so
// what GDI has to provide is the bitmap, a memory DC to hold it, the colour
// table an 8-bit mode needs and the palette calls around it. Every handle is
// a pseudo handle from its own range; nothing here touches a host drawing
// API. Text output is accepted and not drawn, which is worth one line.

#include "imports.h"
#include "memory.h"
#include "win32.h"

#include <map>
#include <string.h>
#include <vector>

// Like the runtime's other weak host hooks, this keeps runtime-only binaries
// independent of DX. A linked DirectDraw shim supplies the accepted mode.
extern "C" __attribute__((weak)) bool ddraw_display_mode(uint32_t *, uint32_t *, uint32_t *) {
    return false;
}

namespace {

struct Dib {
    int32_t width = 0, height = 0; // height as created: negative is top-down
    uint16_t bpp = 0;
    uint32_t stride = 0;
    uint32_t bits = 0; // guest address of the pixel rows
    uint32_t size = 0;
    uint32_t compression = 0;
    uint32_t masks[3] = {0, 0, 0};
    std::vector<uint32_t> colors; // RGBQUADs for bpp <= 8
};

struct Palette {
    std::vector<uint32_t> entries; // PALETTEENTRYs
};

struct Dc {
    uint32_t bitmap = 0;
    uint32_t palette = 0;
    uint32_t text_color = 0;
    uint32_t bk_color = 0x00ffffff; // white, the default opaque text background
    uint32_t bk_mode = 2;           // OPAQUE
};

// Pseudo handles: bitmaps, palettes and memory DCs each in their own run,
// outside every mapped guest region and apart from user32's DCs.
constexpr uint32_t BITMAP_HANDLE_BASE = 0x00050000u;
constexpr uint32_t PALETTE_HANDLE_BASE = 0x00058000u;
constexpr uint32_t DC_HANDLE_BASE = 0x00060000u;
constexpr uint32_t DEFAULT_BITMAP = 0x0004f001u;  // what SelectObject reports as "previous"
constexpr uint32_t DEFAULT_PALETTE = 0x0004f002u; // the system palette handle
constexpr uint32_t TEXT_HEIGHT = 16;
constexpr uint32_t TEXT_AVERAGE_CHAR_WIDTH = 7;

uint32_t g_next_bitmap = BITMAP_HANDLE_BASE;
uint32_t g_next_palette = PALETTE_HANDLE_BASE;
uint32_t g_next_dc = DC_HANDLE_BASE;

std::map<uint32_t, Dib> &dibs() {
    static std::map<uint32_t, Dib> m;
    return m;
}
std::map<uint32_t, Palette> &palettes() {
    static std::map<uint32_t, Palette> m;
    return m;
}
// Any DC handle may be drawn into: a memory DC from here, or a window or
// surface DC from user32 or DirectDraw. State is kept for whichever is named.
std::map<uint32_t, Dc> &dcs() {
    static std::map<uint32_t, Dc> m;
    return m;
}

Dib *dib_of(uint32_t handle) {
    auto it = dibs().find(handle);
    return it == dibs().end() ? nullptr : &it->second;
}
Dib *dib_in_dc(uint32_t hdc) {
    auto it = dcs().find(hdc);
    return it == dcs().end() ? nullptr : dib_of(it->second.bitmap);
}

uint32_t dib_stride(int32_t width, uint32_t bpp) {
    return ((uint32_t)width * bpp + 31u) / 32u * 4u;
}

uint32_t make_dib(int32_t width, int32_t height, uint16_t bpp, uint32_t compression,
                  uint32_t bits_out) {
    Dib d;
    d.width = width;
    d.height = height;
    d.bpp = bpp;
    d.compression = compression;
    d.stride = dib_stride(width, bpp);
    uint32_t rows = (uint32_t)(height < 0 ? -height : height);
    d.size = d.stride * rows;
    // A section is page granular on Windows and the game may assume the
    // alignment; zeroed, as fresh section pages are.
    d.bits = d.size ? heap_alloc(d.size, true, 4096) : 0;
    if (d.size && !d.bits)
        return 0;
    uint32_t handle = g_next_bitmap++;
    if (bits_out)
        wr32(bits_out, d.bits);
    dibs()[handle] = d;
    return handle;
}

} // namespace

// CreateDIBSection(hdc, pbmi, usage, ppvBits, hSection, offset)
void g_CreateDIBSection(X86 *c) {
    uint32_t bmi = arg(c, 1), bits_out = arg(c, 3);
    if (!bmi || !gm_valid(bmi, 40)) {
        set_eax(c, 0);
        return;
    }
    uint32_t hdr_size = rd32(bmi);
    int32_t width = (int32_t)rd32(bmi + 4), height = (int32_t)rd32(bmi + 8);
    uint16_t bpp = rd16(bmi + 14);
    uint32_t compression = rd32(bmi + 16), clr_used = rd32(bmi + 32);
    if (width <= 0 || height == 0 || !(bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) {
        LOGW("CreateDIBSection: unsupported %dx%d at %u bpp", width, height, bpp);
        set_eax(c, 0);
        return;
    }
    uint32_t handle = make_dib(width, height, bpp, compression, bits_out);
    if (!handle) {
        set_eax(c, 0);
        return;
    }
    Dib &d = dibs()[handle];
    uint32_t after = bmi + hdr_size;
    if (compression == 3) { // BI_BITFIELDS: three masks follow the header
        for (int i = 0; i < 3; ++i)
            d.masks[i] = rd32(after + 4u * (uint32_t)i);
    } else if (bpp <= 8) {
        uint32_t n = clr_used ? clr_used : (1u << bpp);
        if (n > 256)
            n = 256;
        d.colors.assign(n, 0);
        for (uint32_t i = 0; i < n; ++i)
            d.colors[i] = rd32(after + 4u * i);
    }
    LOGV("gdi: CreateDIBSection %dx%d %u bpp -> %08x, bits %08x", width, height, bpp, handle,
         d.bits);
    set_eax(c, handle);
}

// CreateCompatibleBitmap(hdc, width, height): a bitmap at the DC's depth,
// else 32 bpp; its pixels are addressable like a section's.
void g_CreateCompatibleBitmap(X86 *c) {
    int32_t width = (int32_t)arg(c, 1), height = (int32_t)arg(c, 2);
    Dib *in = dib_in_dc(arg(c, 0));
    uint16_t bpp = in ? in->bpp : 32;
    if (width <= 0 || height <= 0) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, make_dib(width, height, bpp, 0, 0));
}

// GetObjectA(handle, bytes, out): BITMAP (24 bytes) or DIBSECTION (84) for a
// bitmap, the entry count (a WORD) for a palette.
void g_GetObjectA(X86 *c) {
    uint32_t handle = arg(c, 0), bytes = arg(c, 1), out = arg(c, 2);
    if (Dib *d = dib_of(handle)) {
        uint32_t have = bytes >= 84 ? 84u : 24u;
        if (!out) {
            set_eax(c, 84);
            return;
        }
        if (bytes < 24) {
            set_eax(c, 0);
            return;
        }
        uint32_t rows = (uint32_t)(d->height < 0 ? -d->height : d->height);
        wr32(out + 0, 0);
        wr32(out + 4, (uint32_t)d->width);
        wr32(out + 8, rows);
        wr32(out + 12, d->stride);
        wr16(out + 16, 1);
        wr16(out + 18, d->bpp);
        wr32(out + 20, d->bits);
        if (have == 84) {
            uint32_t bmih = out + 24;
            wr32(bmih + 0, 40);
            wr32(bmih + 4, (uint32_t)d->width);
            wr32(bmih + 8, (uint32_t)d->height);
            wr16(bmih + 12, 1);
            wr16(bmih + 14, d->bpp);
            wr32(bmih + 16, d->compression);
            wr32(bmih + 20, d->size);
            wr32(bmih + 24, 0);
            wr32(bmih + 28, 0);
            wr32(bmih + 32, (uint32_t)d->colors.size());
            wr32(bmih + 36, 0);
            for (int i = 0; i < 3; ++i)
                wr32(out + 64 + 4u * (uint32_t)i, d->masks[i]);
            wr32(out + 76, 0); // dshSection
            wr32(out + 80, 0); // dsOffset
        }
        set_eax(c, have);
        return;
    }
    auto pi = palettes().find(handle);
    if (pi != palettes().end()) {
        if (out && bytes >= 2)
            wr16(out, (uint16_t)pi->second.entries.size());
        set_eax(c, out ? 2 : 2);
        return;
    }
    set_eax(c, 0);
}

// DeleteObject(handle): a bitmap gives its pixels back; a palette goes away;
// a stock object is fine to delete, as on Windows.
void g_DeleteObject(X86 *c) {
    uint32_t handle = arg(c, 0);
    auto di = dibs().find(handle);
    if (di != dibs().end()) {
        if (di->second.bits)
            heap_free(di->second.bits);
        dibs().erase(di);
        for (auto &kv : dcs())
            if (kv.second.bitmap == handle)
                kv.second.bitmap = 0;
        set_eax(c, 1);
        return;
    }
    if (palettes().erase(handle)) {
        for (auto &kv : dcs())
            if (kv.second.palette == handle)
                kv.second.palette = 0;
        set_eax(c, 1);
        return;
    }
    set_eax(c, handle ? 1 : 0);
}

void g_CreateCompatibleDC(X86 *c) {
    uint32_t hdc = g_next_dc++;
    dcs()[hdc] = Dc();
    set_eax(c, hdc);
}

void g_DeleteDC(X86 *c) {
    set_eax(c, dcs().erase(arg(c, 0)) ? 1 : 0);
}

// SelectObject(hdc, object): a bitmap goes into the DC and the previous one
// comes back; a pen, brush or font is accepted as selected.
void g_SelectObject(X86 *c) {
    uint32_t hdc = arg(c, 0), obj = arg(c, 1);
    if (!hdc) {
        set_eax(c, 0);
        return;
    }
    Dc &dc = dcs()[hdc];
    if (dib_of(obj)) {
        uint32_t prev = dc.bitmap ? dc.bitmap : DEFAULT_BITMAP;
        dc.bitmap = obj;
        set_eax(c, prev);
        return;
    }
    set_eax(c, obj ? obj : 0);
}

// SetDIBColorTable(hdc, start, count, RGBQUAD*): the colour table of the
// 8-bit DIB the DC holds.
void g_SetDIBColorTable(X86 *c) {
    uint32_t start = arg(c, 1), count = arg(c, 2), src = arg(c, 3);
    Dib *d = dib_in_dc(arg(c, 0));
    if (!d || d->bpp > 8 || !src) {
        set_eax(c, 0);
        return;
    }
    uint32_t limit = 1u << d->bpp;
    if (d->colors.size() < limit)
        d->colors.resize(limit, 0);
    uint32_t n = 0;
    for (; n < count && start + n < limit; ++n)
        d->colors[start + n] = rd32(src + 4u * n);
    set_eax(c, n);
}

// CreatePalette(LOGPALETTE*): version word, count word, then the entries.
void g_CreatePalette(X86 *c) {
    uint32_t lp = arg(c, 0);
    if (!lp || !gm_valid(lp, 4)) {
        set_eax(c, 0);
        return;
    }
    uint32_t count = rd16(lp + 2);
    Palette p;
    p.entries.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        p.entries[i] = rd32(lp + 4 + 4u * i);
    uint32_t handle = g_next_palette++;
    palettes()[handle] = p;
    set_eax(c, handle);
}

void g_SelectPalette(X86 *c) {
    uint32_t hdc = arg(c, 0), hpal = arg(c, 1);
    if (!hdc || (hpal && !palettes().count(hpal))) {
        set_eax(c, 0);
        return;
    }
    Dc &dc = dcs()[hdc];
    uint32_t prev = dc.palette ? dc.palette : DEFAULT_PALETTE;
    dc.palette = hpal;
    set_eax(c, prev);
}

// RealizePalette(hdc): every entry of the selected palette is "mapped".
void g_RealizePalette(X86 *c) {
    auto it = dcs().find(arg(c, 0));
    uint32_t n = 0;
    if (it != dcs().end()) {
        auto pi = palettes().find(it->second.palette);
        if (pi != palettes().end())
            n = (uint32_t)pi->second.entries.size();
    }
    set_eax(c, n);
}

void g_GetPaletteEntries(X86 *c) {
    uint32_t start = arg(c, 1), count = arg(c, 2), out = arg(c, 3);
    auto pi = palettes().find(arg(c, 0));
    if (pi == palettes().end()) {
        set_eax(c, 0);
        return;
    }
    const std::vector<uint32_t> &e = pi->second.entries;
    if (!out) {
        set_eax(c, (uint32_t)e.size());
        return;
    }
    uint32_t n = 0;
    for (; n < count && start + n < e.size(); ++n)
        wr32(out + 4u * n, e[start + n]);
    set_eax(c, n);
}

void g_GetSystemPaletteUse(X86 *c) {
    set_eax(c, 1); // SYSPAL_STATIC
}
void g_SetSystemPaletteUse(X86 *c) {
    set_eax(c, 1); // the previous use
}

// BitBlt(dst, x, y, w, h, src, x1, y1, rop): between two DIBs of one depth
// it copies; anything else has no pixels here and is reported once.
void g_BitBlt(X86 *c) {
    Dib *dst = dib_in_dc(arg(c, 0));
    Dib *src = dib_in_dc(arg(c, 5));
    int32_t x = (int32_t)arg(c, 1), y = (int32_t)arg(c, 2);
    int32_t w = (int32_t)arg(c, 3), h = (int32_t)arg(c, 4);
    int32_t sx = (int32_t)arg(c, 6), sy = (int32_t)arg(c, 7);
    if (!dst || !src || dst->bpp != src->bpp) {
        log_once("gdi.bitblt", "gdi: BitBlt between %s and %s: nothing is drawn in this runtime",
                 dst ? "a DIB" : "a non-DIB DC", src ? "a DIB" : "a non-DIB DC");
        set_eax(c, 1);
        return;
    }
    int32_t dh = dst->height < 0 ? -dst->height : dst->height;
    int32_t sh = src->height < 0 ? -src->height : src->height;
    uint32_t bytes_px = dst->bpp / 8u;
    for (int32_t row = 0; row < h; ++row) {
        int32_t dy = y + row, syy = sy + row;
        if (dy < 0 || dy >= dh || syy < 0 || syy >= sh)
            continue;
        int32_t x0 = x < 0 ? 0 : x, sx0 = sx + (x0 - x);
        int32_t n = w - (x0 - x);
        if (x0 + n > dst->width)
            n = dst->width - x0;
        if (sx0 + n > src->width)
            n = src->width - sx0;
        if (n <= 0 || sx0 < 0)
            continue;
        memmove(g_mem + dst->bits + (uint32_t)dy * dst->stride + (uint32_t)x0 * bytes_px,
                g_mem + src->bits + (uint32_t)syy * src->stride + (uint32_t)sx0 * bytes_px,
                (size_t)n * bytes_px);
    }
    set_eax(c, 1);
}

// PatBlt(hdc, x, y, w, h, rop): BLACKNESS and WHITENESS fill; other raster
// operations need a brush this runtime does not model and leave the pixels.
void g_PatBlt(X86 *c) {
    Dib *d = dib_in_dc(arg(c, 0));
    int32_t x = (int32_t)arg(c, 1), y = (int32_t)arg(c, 2);
    int32_t w = (int32_t)arg(c, 3), h = (int32_t)arg(c, 4);
    uint32_t rop = arg(c, 5);
    if (!d || !(rop == 0x00000042u || rop == 0x00FF0062u)) {
        set_eax(c, 1);
        return;
    }
    uint8_t fill = rop == 0x00FF0062u ? 0xff : 0x00;
    int32_t rows = d->height < 0 ? -d->height : d->height;
    uint32_t bytes_px = d->bpp / 8u;
    for (int32_t row = 0; row < h; ++row) {
        int32_t dy = y + row;
        if (dy < 0 || dy >= rows)
            continue;
        int32_t x0 = x < 0 ? 0 : x, x1 = x + w > d->width ? d->width : x + w;
        if (x1 <= x0)
            continue;
        memset(g_mem + d->bits + (uint32_t)dy * d->stride + (uint32_t)x0 * bytes_px, fill,
               (size_t)(x1 - x0) * bytes_px);
    }
    set_eax(c, 1);
}

// GetDIBits(hdc, hbm, start, lines, bits, bmi, usage): the rows as stored.
void g_GetDIBits(X86 *c) {
    Dib *d = dib_of(arg(c, 1));
    uint32_t start = arg(c, 2), lines = arg(c, 3), out = arg(c, 4), bmi = arg(c, 5);
    if (!d) {
        set_eax(c, 0);
        return;
    }
    uint32_t rows = (uint32_t)(d->height < 0 ? -d->height : d->height);
    if (!out) {
        if (bmi && gm_valid(bmi, 40)) {
            wr32(bmi + 4, (uint32_t)d->width);
            wr32(bmi + 8, (uint32_t)d->height);
            wr16(bmi + 12, 1);
            wr16(bmi + 14, d->bpp);
            wr32(bmi + 16, d->compression);
            wr32(bmi + 20, d->size);
        }
        set_eax(c, rows);
        return;
    }
    uint32_t n = 0;
    for (; n < lines && start + n < rows; ++n)
        memcpy(g_mem + out + n * d->stride, g_mem + d->bits + (start + n) * d->stride, d->stride);
    set_eax(c, n);
}

// Report the accepted DirectDraw mode, or the palettized desktop fallback
// before a mode is set (also used by builds without the DirectDraw module).
void g_GetDeviceCaps(X86 *c) {
    uint32_t w = 640, h = 480, bpp = 8;
    ddraw_display_mode(&w, &h, &bpp);
    uint32_t value = 0;
    switch (arg(c, 1)) {
    case 8: // HORZRES
        value = w;
        break;
    case 10: // VERTRES
        value = h;
        break;
    case 12: // BITSPIXEL
        value = bpp;
        break;
    case 14: // PLANES
        value = 1;
        break;
    case 38: // RASTERCAPS: RC_PALETTE
        value = bpp == 8 ? 0x100u : 0u;
        break;
    case 104: // SIZEPALETTE
        value = bpp == 8 ? 256u : 0u;
        break;
    case 24: // NUMCOLORS
        value = bpp == 8 ? 256u : 0xffffffffu;
        break;
    }
    set_eax(c, value);
}

// Text: accepted and measured with fixed metrics, not drawn.
void g_TextOutA(X86 *c) {
    log_once("gdi.textout", "gdi: TextOutA is accepted and not drawn in this runtime");
    set_eax(c, 1);
}
void g_GetTextMetricsA(X86 *c) {
    uint32_t tm = arg(c, 1);
    if (!tm || !gm_valid(tm, 56)) {
        set_eax(c, 0);
        return;
    }
    memset(g_mem + tm, 0, 56);
    wr32(tm + 0, TEXT_HEIGHT);              // tmHeight
    wr32(tm + 4, 13);                       // tmAscent
    wr32(tm + 8, 3);                        // tmDescent
    wr32(tm + 12, 3);                       // tmInternalLeading
    wr32(tm + 16, 1);                       // tmExternalLeading
    wr32(tm + 20, TEXT_AVERAGE_CHAR_WIDTH); // tmAveCharWidth
    wr32(tm + 24, 14);                      // tmMaxCharWidth
    wr32(tm + 28, 400);                     // tmWeight
    g_mem[tm + 44] = 0x20;                  // tmFirstChar
    g_mem[tm + 45] = 0xff;                  // tmLastChar
    g_mem[tm + 46] = 0x3f;                  // tmDefaultChar
    g_mem[tm + 47] = 0x20;                  // tmBreakChar
    g_mem[tm + 52] = 0x02;                  // tmPitchAndFamily: variable pitch
    set_eax(c, 1);
}
void g_GetTextExtentPointA(X86 *c) {
    uint32_t size = arg(c, 3);
    if (!size || !gm_valid(size, 8)) {
        set_eax(c, 0);
        return;
    }
    wr32(size, arg(c, 2) * TEXT_AVERAGE_CHAR_WIDTH);
    wr32(size + 4, TEXT_HEIGHT);
    set_eax(c, 1);
}
void g_SetTextColor(X86 *c) {
    Dc &dc = dcs()[arg(c, 0)];
    uint32_t prev = dc.text_color;
    dc.text_color = arg(c, 1);
    set_eax(c, prev);
}
void g_SetBkColor(X86 *c) {
    Dc &dc = dcs()[arg(c, 0)];
    uint32_t prev = dc.bk_color;
    dc.bk_color = arg(c, 1);
    set_eax(c, prev);
}
void g_SetBkMode(X86 *c) {
    Dc &dc = dcs()[arg(c, 0)];
    uint32_t prev = dc.bk_mode;
    dc.bk_mode = arg(c, 1);
    set_eax(c, prev);
}

const ImportShim g_gdi32_shims[] = {
    {"GDI32.dll", "CreateDIBSection", 6, g_CreateDIBSection},
    {"GDI32.dll", "CreateCompatibleBitmap", 3, g_CreateCompatibleBitmap},
    {"GDI32.dll", "GetObjectA", 3, g_GetObjectA},
    {"GDI32.dll", "DeleteObject", 1, g_DeleteObject},
    {"GDI32.dll", "CreateCompatibleDC", 1, g_CreateCompatibleDC},
    {"GDI32.dll", "DeleteDC", 1, g_DeleteDC},
    {"GDI32.dll", "SelectObject", 2, g_SelectObject},
    {"GDI32.dll", "SetDIBColorTable", 4, g_SetDIBColorTable},
    {"GDI32.dll", "CreatePalette", 1, g_CreatePalette},
    {"GDI32.dll", "SelectPalette", 3, g_SelectPalette},
    {"GDI32.dll", "RealizePalette", 1, g_RealizePalette},
    {"GDI32.dll", "GetPaletteEntries", 4, g_GetPaletteEntries},
    {"GDI32.dll", "GetSystemPaletteUse", 1, g_GetSystemPaletteUse},
    {"GDI32.dll", "SetSystemPaletteUse", 2, g_SetSystemPaletteUse},
    {"GDI32.dll", "BitBlt", 9, g_BitBlt},
    {"GDI32.dll", "PatBlt", 6, g_PatBlt},
    {"GDI32.dll", "GetDIBits", 7, g_GetDIBits},
    {"GDI32.dll", "GetDeviceCaps", 2, g_GetDeviceCaps},
    {"GDI32.dll", "TextOutA", 5, g_TextOutA},
    {"GDI32.dll", "GetTextMetricsA", 2, g_GetTextMetricsA},
    {"GDI32.dll", "GetTextExtentPointA", 4, g_GetTextExtentPointA},
    {"GDI32.dll", "SetTextColor", 2, g_SetTextColor},
    {"GDI32.dll", "SetBkColor", 2, g_SetBkColor},
    {"GDI32.dll", "SetBkMode", 2, g_SetBkMode},
};
const size_t g_gdi32_shim_count = sizeof(g_gdi32_shims) / sizeof(g_gdi32_shims[0]);
