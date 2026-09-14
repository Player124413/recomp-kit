// Guest GDI objects, DC state and shared DIB/window pixel access.
#include "imports.h"
#include "gdi_image.h"
#include "gdi32_internal.h"
#include "user32_internal.h"
#include "display_seam.h"
#include <climits>
#include <algorithm>
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

using namespace gdi;
namespace {

struct Dib {
    int32_t width = 0, height = 0; // height as created: negative is top-down
    uint16_t bpp = 0;
    uint32_t stride = 0;
    uint32_t bits = 0; // guest address of the pixel rows
    uint32_t size = 0;
    uint32_t compression = 0;
    bool owned = true;
    uint32_t masks[3] = {0, 0, 0};
    std::vector<uint32_t> colors; // RGBQUADs for bpp <= 8
};

struct Palette {
    std::vector<uint32_t> entries; // PALETTEENTRYs
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
    int64_t rows64 = height < 0 ? -int64_t(height) : height;
    uint64_t stride64 = ((uint64_t(uint32_t(width)) * bpp + 31) / 32) * 4;
    if (width <= 0 || rows64 <= 0 || stride64 * rows64 > GUEST_SIZE / 4)
        return 0;
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

namespace {
// Convert the existing DIB representation to the common controls' 32-bpp
// snapshot. Handle row orientation and palette/bitfield formats here once.
bool snapshot_dib(const Dib &d, GdiImage *image, bool preserve_alpha = false) {
    int64_t height = d.height < 0 ? -int64_t(d.height) : d.height;
    if (d.width <= 0 || height <= 0 || uint64_t(d.width) * height > GUEST_SIZE / 4 ||
        !gm_valid(d.bits, d.size))
        return false;
    GdiImage result;
    result.width = d.width;
    result.height = int32_t(height);
    result.pixels.resize(size_t(d.width) * size_t(height));
    bool alpha = false;
    auto component = [](uint32_t pixel, uint32_t mask) -> uint32_t {
        if (!mask)
            return 0;
        while (!(mask & 1)) {
            mask >>= 1;
            pixel >>= 1;
        }
        return uint32_t(uint64_t(pixel & mask) * 255 / mask);
    };
    for (int32_t y = 0; y < height; ++y) {
        uint32_t row = d.bits + uint32_t(d.height > 0 ? height - 1 - y : y) * d.stride;
        for (int32_t x = 0; x < d.width; ++x) {
            uint32_t pixel = 0;
            if (d.bpp <= 8) {
                uint32_t index = d.bpp == 1   ? (rd8(row + x / 8) >> (7 - x % 8)) & 1
                                 : d.bpp == 4 ? (rd8(row + x / 2) >> (x % 2 ? 0 : 4)) & 15
                                              : rd8(row + x);
                pixel = index < d.colors.size() ? d.colors[index] & 0xffffff : index ? 0xffffff : 0;
            } else if (d.bpp == 16) {
                uint32_t v = rd16(row + x * 2);
                pixel = component(v, d.masks[0] ? d.masks[0] : 0x7c00) << 16 |
                        component(v, d.masks[1] ? d.masks[1] : 0x03e0) << 8 |
                        component(v, d.masks[2] ? d.masks[2] : 0x001f);
            } else if (d.bpp == 24) {
                uint32_t at = row + x * 3;
                pixel = rd8(at) | rd8(at + 1) << 8 | rd8(at + 2) << 16;
            } else if (d.bpp == 32) {
                pixel = rd32(row + x * 4);
                alpha |= (pixel >> 24) != 0;
            } else
                return false;
            result.pixels[size_t(y) * d.width + x] = pixel;
        }
    }
    // Legacy RGB bitmaps leave the reserved byte zero. Only treat alpha as
    // meaningful when at least one pixel supplies it.
    if (!alpha && !preserve_alpha)
        for (auto &p : result.pixels)
            p |= 0xff000000;
    *image = std::move(result);
    return true;
}
std::map<uint32_t, GdiImage> &icons() {
    static std::map<uint32_t, GdiImage> m;
    return m;
}
uint32_t next_image_icon = 0x00090000;
} // namespace
bool gdi_read_bitmap(uint32_t bitmap, GdiImage *image, bool preserve_alpha) {
    Dib *d = dib_of(bitmap);
    return d && snapshot_dib(*d, image, preserve_alpha);
}
uint32_t gdi_image_bitmap(const GdiImage &image) {
    if (image.width <= 0 || image.height <= 0 ||
        uint64_t(image.width) * image.height != image.pixels.size())
        return 0;
    uint32_t h = make_dib(image.width, -image.height, 32, 0, 0);
    if (h)
        memcpy(g_mem + dib_of(h)->bits, image.pixels.data(), image.pixels.size() * 4);
    return h;
}
uint32_t gdi_image_mask(const GdiImage &image) {
    if (image.width <= 0 || image.height <= 0 ||
        uint64_t(image.width) * image.height != image.pixels.size())
        return 0;
    uint32_t h = make_dib(image.width, -image.height, 1, 0, 0);
    if (!h)
        return 0;
    Dib &d = *dib_of(h);
    d.colors = {0, 0x00ffffff};
    for (int32_t y = 0; y < image.height; ++y)
        for (int32_t x = 0; x < image.width; ++x)
            if (!(image.pixels[size_t(y) * image.width + x] >> 24)) {
                uint32_t at = d.bits + uint32_t(y) * d.stride + uint32_t(x) / 8;
                wr8(at, rd8(at) | uint8_t(0x80 >> (x % 8)));
            }
    return h;
}
void gdi_delete_bitmap(uint32_t bitmap) {
    auto it = dibs().find(bitmap);
    if (it == dibs().end())
        return;
    if (it->second.owned)
        heap_free(it->second.bits);
    dibs().erase(it);
    for (auto &dc : dcs())
        if (dc.second.bitmap == bitmap)
            dc.second.bitmap = 0;
}
// Shared drawing entry used by icons, image lists and USER32 rectangles.
bool gdi_draw_image(uint32_t dc, const GdiImage &image, int32_t x, int32_t y, int32_t w,
                    int32_t h) {
    int dw, dh;
    if (!dc_size(dc, &dw, &dh))
        return false;
    Rect clip = clip_box(dc);
    for (int64_t yy = std::max<int64_t>(0, int64_t(clip.t) - y);
         yy < std::min<int64_t>({h, image.height, int64_t(clip.b) - y}); ++yy)
        for (int64_t xx = std::max<int64_t>(0, int64_t(clip.l) - x);
             xx < std::min<int64_t>({w, image.width, int64_t(clip.r) - x}); ++xx)
            write_pixel(dc, int64_t(x) + xx, int64_t(y) + yy,
                        image.pixels[size_t(yy) * image.width + xx], true);
    return true;
}
bool gdi_focus_rect(uint32_t dc, int32_t l, int32_t t, int32_t r, int32_t b) {
    int w, h;
    if (!dc_size(dc, &w, &h))
        return false;
    Rect clip = clip_box(dc);
    for (int64_t y = std::max(t, clip.t); y < std::min(b, clip.b); ++y)
        for (int64_t x = std::max(l, clip.l); x < std::min(r, clip.r); ++x) {
            uint32_t p;
            if ((x == l || x == int64_t(r) - 1 || y == t || y == int64_t(b) - 1) &&
                !((x + y) & 1) && read_pixel(dc, x, y, &p))
                write_pixel(dc, x, y, p ^ 0xffffff);
        }
    return true;
}

// Validate the packed DIB before reading colors or rows. Resources and BMP
// files share this format; compressed RLE/JPEG/PNG data is not a DIB here.
bool gdi_decode_image(uint32_t at, uint32_t bytes, GdiImage *image, uint32_t pixel_offset) {
    if (!at || bytes < 40 || !gm_valid(at, bytes))
        return false;
    uint32_t header = rd32(at);
    if (header < 40 || header > bytes || rd16(at + 12) != 1)
        return false;
    Dib d;
    d.width = int32_t(rd32(at + 4));
    d.height = int32_t(rd32(at + 8));
    d.bpp = rd16(at + 14);
    d.compression = rd32(at + 16);
    if (d.width <= 0 || !d.height ||
        !(d.bpp == 1 || d.bpp == 4 || d.bpp == 8 || d.bpp == 16 || d.bpp == 24 || d.bpp == 32) ||
        !(d.compression == 0 || (d.compression == 3 && (d.bpp == 16 || d.bpp == 32))))
        return false;
    uint64_t rows = d.height < 0 ? -int64_t(d.height) : d.height;
    uint64_t stride = ((uint64_t(d.width) * d.bpp + 31) / 32) * 4;
    uint64_t offset = header;
    if (d.compression == 3) {
        uint32_t masks = header >= 52 ? at + 40 : at + header;
        if (masks - at + 12 > bytes)
            return false;
        for (int i = 0; i < 3; ++i)
            d.masks[i] = rd32(masks + i * 4);
        if (header < 52)
            offset += 12;
    }
    if (d.bpp <= 8) {
        uint32_t count = rd32(at + 32);
        if (!count)
            count = 1u << d.bpp;
        if (count > (1u << d.bpp) || offset + count * 4 > bytes)
            return false;
        for (uint32_t i = 0; i < count; ++i)
            d.colors.push_back(rd32(at + uint32_t(offset) + i * 4));
        offset += count * 4;
    }
    if (pixel_offset) {
        if (pixel_offset < offset)
            return false;
        offset = pixel_offset;
    }
    if (offset + stride * rows > bytes)
        return false;
    d.bits = at + uint32_t(offset);
    d.stride = uint32_t(stride);
    d.size = uint32_t(stride * rows);
    return snapshot_dib(d, image);
}
uint32_t gdi_create_icon(const GdiImage &image) {
    if (image.pixels.empty())
        return 0;
    uint32_t handle = next_image_icon++;
    icons()[handle] = image;
    return handle;
}
bool gdi_read_icon(uint32_t icon, GdiImage *image) {
    auto it = icons().find(icon);
    if (it == icons().end())
        return false;
    *image = it->second;
    return true;
}
bool gdi_delete_icon(uint32_t icon) {
    return icons().erase(icon) != 0;
}

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
    auto object = objects().find(handle);
    if (object != objects().end()) {
        const Object &o = object->second;
        uint32_t size = o.kind == Object::Font ? 92 : o.kind == Object::Pen ? 16 : 12;
        if (!out) {
            set_eax(c, size);
            return;
        }
        if (bytes < size || !gm_valid(out, size)) {
            set_eax(c, 0);
            return;
        }
        memset(g_mem + out, 0, size);
        if (o.kind == Object::Font)
            memcpy(g_mem + out, o.logfont.data(), size);
        else {
            wr32(out, o.style);
            wr32(out + 4, o.kind == Object::Pen ? o.width : o.color);
            if (o.kind == Object::Pen)
                wr32(out + 12, o.color);
        }
        set_eax(c, size);
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
        if (di->second.owned && di->second.bits)
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
    objects().erase(handle);
    set_eax(c, handle ? 1 : 0);
}

void g_CreateCompatibleDC(X86 *c) {
    uint32_t hdc = g_next_dc++;
    dcs()[hdc] = DeviceContext();
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
    DeviceContext &dc = dcs()[hdc];
    if (dib_of(obj) || obj == DEFAULT_BITMAP) {
        uint32_t prev = dc.bitmap ? dc.bitmap : DEFAULT_BITMAP;
        dc.bitmap = obj == DEFAULT_BITMAP ? 0 : obj;
        set_eax(c, prev);
        return;
    }
    auto it = objects().find(obj);
    if (it == objects().end()) {
        set_eax(c, 0);
        return;
    }
    uint32_t *slot = it->second.kind == Object::Brush  ? &dc.brush
                     : it->second.kind == Object::Pen  ? &dc.pen
                     : it->second.kind == Object::Font ? &dc.font
                                                       : &dc.region;
    uint32_t prev = *slot;
    *slot = obj;
    if (it->second.kind == Object::Region) {
        dc.clipped = true;
        dc.clip = {it->second.rect};
        set_eax(c, 2);
    } else
        set_eax(c, prev);
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
    DeviceContext &dc = dcs()[hdc];
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
    uint32_t w = 1024, h = 768, bpp = 32;
    ddraw_display_mode(&w, &h, &bpp);
    uint32_t value = 0;
    switch (arg(c, 1)) {
    case 2:
        value = 1;
        break;
    case 88:
    case 90:
        value = 96;
        break;
    case 8: // HORZRES
        value = w;
        break;
    case 10: // VERTRES
        value = h;
        break;
    case 12: // BITSPIXEL
        value = 32;
        break;
    case 14: // PLANES
        value = 1;
        break;
    case 38: // RASTERCAPS: RC_PALETTE
        value = 0x2a01u;
        break;
    case 104: // SIZEPALETTE
        value = bpp == 8 ? 256u : 0u;
        break;
    case 24: // NUMCOLORS
        value = 0xffffffffu;
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
    DeviceContext &dc = dcs()[arg(c, 0)];
    uint32_t prev = dc.text_color;
    dc.text_color = arg(c, 1);
    set_eax(c, prev);
}
void g_SetBkColor(X86 *c) {
    DeviceContext &dc = dcs()[arg(c, 0)];
    uint32_t prev = dc.bk_color;
    dc.bk_color = arg(c, 1);
    set_eax(c, prev);
}
void g_SetBkMode(X86 *c) {
    DeviceContext &dc = dcs()[arg(c, 0)];
    uint32_t prev = dc.bk_mode;
    dc.bk_mode = arg(c, 1);
    set_eax(c, prev);
}

const ImportShim g_gdi32_shims[] = {
    {"GDI32.dll", "CreateDIBSection", 6, g_CreateDIBSection},
    {"GDI32.dll", "CreateCompatibleBitmap", 3, g_CreateCompatibleBitmap},
    {"GDI32.dll", "GetObjectA", 3, g_GetObjectA},
    {"GDI32.dll", "GetObjectW", 3, g_GetObjectA},
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

namespace gdi {
std::map<uint32_t, DeviceContext> &dcs() {
    static std::map<uint32_t, DeviceContext> value;
    return value;
}
DeviceContext *dc_of(uint32_t dc) {
    auto it = dcs().find(dc);
    return it == dcs().end() ? nullptr : &it->second;
}
std::map<uint32_t, Object> &objects() {
    static std::map<uint32_t, Object> value = [] {
        std::map<uint32_t, Object> m;
        for (uint32_t i = 0; i <= 19; ++i) {
            Object o;
            o.kind = i <= 5 || i == 18   ? Object::Brush
                     : i <= 8 || i == 19 ? Object::Pen
                                         : Object::Font;
            o.style = i == 5 ? 1 : i == 8 ? 5 : 0;
            o.color = i == 0 || i == 6 ? 0xffffff
                      : i == 1         ? 0xc0c0c0
                      : i == 2         ? 0x808080
                      : i == 3         ? 0x404040
                                       : 0;
            m.emplace(0x4f100 + i, o);
        }
        return m;
    }();
    return value;
}
uint32_t make_object(const Object &o) {
    static uint32_t next = 0x00078000;
    objects()[next] = o;
    return next++;
}
uint32_t colorref(uint32_t p) {
    return (p & 255) << 16 | (p & 0xff00) | ((p >> 16) & 255);
}
uint32_t argb(uint32_t c) {
    return 0xff000000 | colorref(c);
}
bool brush_color(uint32_t h, uint32_t *p) {
    auto it = objects().find(h);
    if (it == objects().end() || it->second.kind != Object::Brush || it->second.style == 1)
        return false;
    *p = argb(it->second.color);
    return true;
}
namespace {
// Resizing preserves the intersection. DCs name the window, never a vector's
// storage, so a resize cannot leave a stale host pointer in another DC.
Surface *surface_of(DeviceContext &dc) {
    auto *window = user32::find_window(dc.surface);
    if (!window)
        return nullptr;
    int w = std::max(0, window->w), h = std::max(0, window->h);
    if (uint64_t(w) * h > GUEST_SIZE / 4)
        return nullptr;
    auto &s = window->surface;
    if (s.w != w || s.h != h) {
        std::vector<uint32_t> pixels(size_t(w) * h, 0xff000000);
        for (int y = 0; y < std::min(h, s.h); ++y)
            std::copy_n(s.argb.begin() + size_t(y) * s.w, std::min(w, s.w),
                        pixels.begin() + size_t(y) * w);
        s.argb = std::move(pixels);
        s.w = w;
        s.h = h;
    }
    return &s;
}
void offset(DeviceContext &dc, int64_t *x, int64_t *y) {
    *x += int64_t(dc.viewport_x) - dc.org_x;
    *y += int64_t(dc.viewport_y) - dc.org_y;
    if (dc.window && dc.window != dc.surface) {
        int32_t wx = 0, wy = 0, sx = 0, sy = 0;
        user32::client_origin(dc.window, &wx, &wy);
        user32::client_origin(dc.surface, &sx, &sy);
        *x += int64_t(wx) - sx;
        *y += int64_t(wy) - sy;
    }
}
bool contains(Rect r, int64_t x, int64_t y) {
    return x >= r.l && y >= r.t && x < r.r && y < r.b;
}
uint32_t unpack(uint32_t p, uint32_t mask) {
    if (!mask)
        return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        p >>= 1;
    }
    return uint32_t(uint64_t(p & mask) * 255 / mask);
}
uint32_t pack(uint32_t p, uint32_t mask) {
    if (!mask)
        return 0;
    unsigned shift = 0;
    while (!(mask & 1)) {
        mask >>= 1;
        ++shift;
    }
    return uint32_t((uint64_t(p) * mask + 127) / 255) << shift;
}
uint32_t raw_dib(const Dib &d, uint32_t at, int x) {
    return d.bpp == 1    ? (rd8(at) >> (7 - x % 8)) & 1
           : d.bpp == 4  ? (rd8(at) >> (x % 2 ? 0 : 4)) & 15
           : d.bpp == 8  ? rd8(at)
           : d.bpp == 16 ? rd16(at)
           : d.bpp == 24 ? rd8(at) | rd8(at + 1) << 8 | rd8(at + 2) << 16
                         : rd32(at);
}
// One path handles both borrowed DirectDraw pixels and owned guest DIBs.
bool pixel(uint32_t hdc, int64_t x, int64_t y, uint32_t *p, bool write, bool blend) {
    auto *dc = dc_of(hdc);
    if (!dc)
        return false;
    offset(*dc, &x, &y);
    if (dc->clipped &&
        std::none_of(dc->clip.begin(), dc->clip.end(), [&](Rect r) { return contains(r, x, y); }))
        return false;
    Dib *d = dib_in_dc(hdc);
    Surface *s = d ? nullptr : surface_of(*dc);
    int64_t w = d ? d->width : s ? s->w : 0;
    int64_t h = d ? std::abs(int64_t(d->height)) : s ? s->h : 0;
    if (x < 0 || y < 0 || x >= w || y >= h)
        return false;
    uint32_t at = 0, value = 0;
    if (d) {
        at = d->bits + uint32_t(d->height > 0 ? h - 1 - y : y) * d->stride +
             uint32_t(x * d->bpp / 8);
        if (!gm_valid(at, (d->bpp + 7) / 8))
            return false;
        uint32_t raw = raw_dib(*d, at, int(x));
        if (d->bpp <= 8)
            value = raw < d->colors.size() ? d->colors[raw] : raw ? 0xffffff : 0;
        else if (d->bpp == 16 || d->compression == 3)
            value = unpack(raw, d->masks[0] ? d->masks[0] : 0x7c00) << 16 |
                    unpack(raw, d->masks[1] ? d->masks[1] : 0x03e0) << 8 |
                    unpack(raw, d->masks[2] ? d->masks[2] : 0x001f);
        else
            value = raw;
    } else
        value = s->argb[size_t(y) * s->w + x];
    if (!write) {
        *p = value | 0xff000000;
        return true;
    }
    uint32_t result = *p;
    if (blend) {
        uint32_t alpha = result >> 24;
        if (!alpha)
            return true;
        result = 0xff000000;
        for (int c = 0; c < 24; c += 8)
            result |=
                ((((*p >> c) & 255) * alpha + ((value >> c) & 255) * (255 - alpha) + 127) / 255)
                << c;
    }
    if (!d) {
        s->argb[size_t(y) * s->w + x] = result | 0xff000000;
        return true;
    }
    if (d->bpp <= 8) {
        uint32_t best = 0;
        uint64_t distance = UINT64_MAX;
        uint32_t count = 1u << d->bpp;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t color = i < d->colors.size() ? d->colors[i] : i ? 0xffffff : 0;
            uint64_t delta = 0;
            for (int c = 0; c < 24; c += 8) {
                int v = int((result >> c) & 255) - int((color >> c) & 255);
                delta += v * v;
            }
            if (delta < distance) {
                best = i;
                distance = delta;
            }
        }
        if (d->bpp == 8)
            wr8(at, best);
        else {
            int shift = d->bpp == 1 ? 7 - int(x) % 8 : int(x) % 2 ? 0 : 4;
            uint32_t mask = (count - 1) << shift;
            wr8(at, (rd8(at) & ~mask) | (best << shift));
        }
    } else if (d->bpp == 16 || d->compression == 3) {
        uint32_t packed = pack((result >> 16) & 255, d->masks[0] ? d->masks[0] : 0x7c00) |
                          pack((result >> 8) & 255, d->masks[1] ? d->masks[1] : 0x03e0) |
                          pack(result & 255, d->masks[2] ? d->masks[2] : 0x001f);
        if (d->bpp == 16)
            wr16(at, packed);
        else
            wr32(at, packed);
    } else if (d->bpp == 32)
        wr32(at, result);
    else {
        wr8(at, result);
        wr8(at + 1, result >> 8);
        wr8(at + 2, result >> 16);
    }
    return true;
}
} // namespace
bool read_pixel(uint32_t dc, int64_t x, int64_t y, uint32_t *p) {
    return pixel(dc, x, y, p, false, false);
}
bool write_pixel(uint32_t dc, int64_t x, int64_t y, uint32_t p, bool blend) {
    return pixel(dc, x, y, &p, true, blend);
}
bool drawable(uint32_t dc, int64_t x, int64_t y) {
    uint32_t p;
    return read_pixel(dc, x, y, &p);
}
bool dc_size(uint32_t hdc, int *w, int *h) {
    auto *dc = dc_of(hdc);
    if (!dc)
        return false;
    if (auto *d = dib_in_dc(hdc)) {
        *w = d->width;
        *h = int(std::abs(int64_t(d->height)));
        return true;
    }
    if (auto *s = surface_of(*dc)) {
        *w = s->w;
        *h = s->h;
        return true;
    }
    return false;
}
Rect clip_box(uint32_t hdc) {
    int w = 0, h = 0;
    if (!dc_size(hdc, &w, &h))
        return {};
    auto &dc = *dc_of(hdc);
    Rect r{0, 0, w, h};
    if (dc.clipped) {
        Rect bound{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
        for (Rect p : dc.clip) {
            bound.l = std::min(bound.l, p.l);
            bound.t = std::min(bound.t, p.t);
            bound.r = std::max(bound.r, p.r);
            bound.b = std::max(bound.b, p.b);
        }
        r = {std::max(r.l, bound.l), std::max(r.t, bound.t), std::min(r.r, bound.r),
             std::min(r.b, bound.b)};
        if (r.r <= r.l || r.b <= r.t)
            return {};
    }
    int64_t x = 0, y = 0;
    offset(dc, &x, &y);
    auto clamp = [](int64_t v) { return int32_t(std::clamp<int64_t>(v, INT_MIN, INT_MAX)); };
    return {clamp(r.l - x), clamp(r.t - y), clamp(r.r - x), clamp(r.b - y)};
}
void fill(uint32_t dc, Rect r, uint32_t p) {
    Rect clip = clip_box(dc);
    for (int64_t y = std::max(r.t, clip.t); y < std::min(r.b, clip.b); ++y)
        for (int64_t x = std::max(r.l, clip.l); x < std::min(r.r, clip.r); ++x)
            write_pixel(dc, x, y, p);
}
} // namespace gdi

uint32_t gdi_window_dc(uint32_t hwnd) {
    DeviceContext dc;
    // GetDC(NULL) has state/capabilities even when there is no desktop bitmap.
    if (hwnd) {
        auto *w = user32::find_window(hwnd);
        if (!w)
            return 0;
        dc.window = hwnd;
        while (w->parent && (w->style & 0x40000000u)) {
            auto *parent = user32::find_window(w->parent);
            if (!parent)
                break;
            w = parent;
        }
        dc.surface = w->hwnd;
    }
    uint32_t handle = g_next_dc++;
    dcs()[handle] = dc;
    return handle;
}
bool gdi_release_window_dc(uint32_t hwnd, uint32_t hdc) {
    auto *dc = dc_of(hdc);
    if (!dc || dc->window != hwnd)
        return false;
    auto *w = user32::find_window(dc->surface);
    int width = 0, height = 0;
    if (w && w->visible && !ddraw_gdi_primary_active() && dc_size(hdc, &width, &height))
        host_display_present_window(w->surface.argb.data(), width, height);
    dcs().erase(hdc);
    return true;
}
void gdi_destroy_window(uint32_t window) {
    for (auto it = dcs().begin(); it != dcs().end();)
        if (it->second.window == window || it->second.surface == window)
            it = dcs().erase(it);
        else
            ++it;
}

namespace {
void solid_brush(X86 *c) {
    Object o;
    o.color = arg(c, 0);
    set_eax(c, make_object(o));
}
void indirect_object(X86 *c, bool pen) {
    uint32_t p = arg(c, 0), size = pen ? 16 : 12;
    if (!p || !gm_valid(p, size)) {
        set_eax(c, 0);
        return;
    }
    Object o;
    o.kind = pen ? Object::Pen : Object::Brush;
    o.style = rd32(p);
    o.width = int32_t(rd32(p + 4));
    o.color = rd32(p + (pen ? 12 : 4));
    set_eax(c, make_object(o));
}
void brush_indirect(X86 *c) {
    indirect_object(c, false);
}
void pen_indirect(X86 *c) {
    indirect_object(c, true);
}
void stock_object(X86 *c) {
    uint32_t i = arg(c, 0);
    set_eax(c, i <= 19 ? 0x4f100 + i : 0);
}
void get_pixel(X86 *c) {
    uint32_t p;
    set_eax(c, read_pixel(arg(c, 0), int32_t(arg(c, 1)), int32_t(arg(c, 2)), &p) ? colorref(p)
                                                                                 : 0xffffffff);
}
void set_pixel(X86 *c) {
    set_eax(c, write_pixel(arg(c, 0), int32_t(arg(c, 1)), int32_t(arg(c, 2)), argb(arg(c, 3)))
                   ? arg(c, 3) & 0xffffff
                   : 0xffffffff);
}
void save_dc(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    if (!dc) {
        set_eax(c, 0);
        return;
    }
    dc->saved.push_back(*dc);
    set_eax(c, uint32_t(dc->saved.size()));
}
void restore_dc(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    int64_t level = int32_t(arg(c, 1));
    if (!dc || !level) {
        set_eax(c, 0);
        return;
    }
    int64_t index = level > 0 ? level - 1 : int64_t(dc->saved.size()) + level;
    if (index < 0 || index >= int64_t(dc->saved.size())) {
        set_eax(c, 0);
        return;
    }
    static_cast<DcState &>(*dc) = dc->saved[size_t(index)];
    dc->saved.resize(size_t(index));
    set_eax(c, 1);
}
void origins(X86 *c, int which, bool set) {
    auto *dc = dc_of(arg(c, 0));
    uint32_t out = arg(c, set ? 3 : 1);
    if (!dc || (out && !gm_valid(out, 8))) {
        set_eax(c, 0);
        return;
    }
    int32_t *x = which == 0   ? &dc->org_x
                 : which == 1 ? &dc->viewport_x
                 : which == 2 ? &dc->brush_x
                              : &dc->pos_x;
    int32_t *y = which == 0   ? &dc->org_y
                 : which == 1 ? &dc->viewport_y
                 : which == 2 ? &dc->brush_y
                              : &dc->pos_y;
    if (out) {
        wr32(out, *x);
        wr32(out + 4, *y);
    }
    if (set) {
        *x = int32_t(arg(c, 1));
        *y = int32_t(arg(c, 2));
    }
    set_eax(c, 1);
}
#define ORIGIN(name, which, set)                                                                   \
    void name(X86 *c) {                                                                            \
        origins(c, which, set);                                                                    \
    }
ORIGIN(window_org, 0, true)
ORIGIN(get_window_org, 0, false)
ORIGIN(viewport_org, 1, true) ORIGIN(brush_org, 2, true) ORIGIN(get_brush_org, 2, false)
    ORIGIN(move_to, 3, true) ORIGIN(get_position, 3, false)
#undef ORIGIN
        void stretch_mode(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    int v = int(arg(c, 1));
    if (!dc || v < 1 || v > 4) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, dc->stretch_mode);
    dc->stretch_mode = v;
}
void get_stretch_mode(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    set_eax(c, dc ? dc->stretch_mode : 0);
}
void rop2(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    int v = int(arg(c, 1));
    if (!dc || v < 1 || v > 16) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, dc->rop2);
    dc->rop2 = v;
}
void flush(X86 *c) {
    set_eax(c, 1);
}
void unrealize(X86 *c) {
    set_eax(c, arg(c, 0) != 0);
}
void resize_palette(X86 *c) {
    auto it = palettes().find(arg(c, 0));
    uint32_t n = arg(c, 1);
    if (it == palettes().end() || n > 65535) {
        set_eax(c, 0);
        return;
    }
    it->second.entries.resize(n);
    set_eax(c, 1);
}
void halftone(X86 *c) {
    Palette p;
    for (uint32_t i = 0; i < 256; ++i)
        p.entries.push_back(i * 0x010101);
    uint32_t h = g_next_palette++;
    palettes()[h] = p;
    set_eax(c, h);
}
void nearest_palette(X86 *c) {
    auto it = palettes().find(arg(c, 0));
    uint32_t best = 0, distance = UINT_MAX;
    if (it == palettes().end()) {
        set_eax(c, 0xffffffff);
        return;
    }
    for (size_t i = 0; i < it->second.entries.size(); ++i) {
        uint32_t delta = 0;
        for (int k = 0; k < 24; k += 8) {
            int v = int((arg(c, 1) >> k) & 255) - int((it->second.entries[i] >> k) & 255);
            delta += v * v;
        }
        if (delta < distance) {
            distance = delta;
            best = uint32_t(i);
        }
    }
    set_eax(c, best);
}
} // namespace
void gdi_model_register() {
#define G(name, n, fn)                                                                             \
    {                                                                                              \
        "GDI32.dll", name, n, fn                                                                   \
    }
    static const ImportShim shims[] = {G("CreateSolidBrush", 1, solid_brush),
                                       G("CreateBrushIndirect", 1, brush_indirect),
                                       G("CreatePenIndirect", 1, pen_indirect),
                                       G("GetStockObject", 1, stock_object),
                                       G("GetPixel", 3, get_pixel),
                                       G("SetPixel", 4, set_pixel),
                                       G("SaveDC", 1, save_dc),
                                       G("RestoreDC", 2, restore_dc),
                                       G("SetWindowOrgEx", 4, window_org),
                                       G("GetWindowOrgEx", 2, get_window_org),
                                       G("SetViewportOrgEx", 4, viewport_org),
                                       G("SetBrushOrgEx", 4, brush_org),
                                       G("GetBrushOrgEx", 2, get_brush_org),
                                       G("MoveToEx", 4, move_to),
                                       G("GetCurrentPositionEx", 2, get_position),
                                       G("SetStretchBltMode", 2, stretch_mode),
                                       G("GetStretchBltMode", 1, get_stretch_mode),
                                       G("SetROP2", 2, rop2),
                                       G("GdiFlush", 0, flush),
                                       G("UnrealizeObject", 1, unrealize),
                                       G("ResizePalette", 2, resize_palette),
                                       G("CreateHalftonePalette", 1, halftone),
                                       G("GetNearestPaletteIndex", 2, nearest_palette)};
#undef G
    imports_register(shims, sizeof(shims) / sizeof(shims[0]));
}
