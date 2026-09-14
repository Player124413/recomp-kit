// Fixed bitmap-font text for guest canvases. No host font services or guest
// pointers escape this file; callbacks receive temporary guest-heap records.
#include "gdi32_internal.h"
#include "gdi32_font8x16.h"
#include "memory.h"
#include "win32.h"
#include <algorithm>
#include <cstring>
#include <climits>
using namespace gdi;
namespace {
uint32_t font_word(const Object &font, int offset) {
    uint32_t v;
    memcpy(&v, font.logfont.data() + offset, 4);
    return v;
}
int32_t font_scale(uint32_t dc) {
    auto *d = dc_of(dc);
    auto it = objects().find(d ? d->font : 0);
    int64_t height = it == objects().end() ? 16 : int32_t(font_word(it->second, 0));
    return int32_t(std::max<int64_t>(1, std::abs(height) / 16));
}
void create_font(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p || !gm_valid(p, 92)) {
        set_eax(c, 0);
        return;
    }
    Object font;
    font.kind = Object::Font;
    memcpy(font.logfont.data(), g_mem + p, 92);
    set_eax(c, make_object(font));
}
// TEXTMETRICW is 60 bytes on x86: eleven DWORDs, four UTF-16
// characters, five BYTE fields and three bytes of trailing padding.
void metrics(uint32_t out, int32_t scale, uint32_t weight) {
    memset(g_mem + out, 0, 60);
    wr32(out, 16u * scale);
    wr32(out + 4, 13u * scale);
    wr32(out + 8, 3u * scale);
    wr32(out + 20, 8u * scale);
    wr32(out + 24, 8u * scale);
    wr32(out + 28, weight);
    wr32(out + 36, 96);
    wr32(out + 40, 96);
    wr16(out + 44, 32);
    wr16(out + 46, 127);
    wr16(out + 48, '?');
    wr16(out + 50, ' ');
    wr8(out + 55, 0x30);
    wr8(out + 56, 0);
}
void get_metrics(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    uint32_t out = arg(c, 1);
    if (!dc || !out || !gm_valid(out, 60)) {
        set_eax(c, 0);
        return;
    }
    auto font = objects().find(dc->font);
    uint32_t weight = font == objects().end() ? 400 : font_word(font->second, 16);
    if (!weight)
        weight = 400;
    metrics(out, font_scale(arg(c, 0)), weight);
    set_eax(c, 1);
}
void extent(X86 *c) {
    uint32_t dc = arg(c, 0), n = arg(c, 2), out = arg(c, 3);
    uint64_t scale = font_scale(dc), width = 8 * scale * n;
    if (!dc_of(dc) || !out || !gm_valid(out, 8) || width > INT_MAX || n > GUEST_SIZE / 2 ||
        (n && (!arg(c, 1) || !gm_valid(arg(c, 1), n * 2)))) {
        set_eax(c, 0);
        return;
    }
    wr32(out, uint32_t(width));
    wr32(out + 4, uint32_t(16 * scale));
    set_eax(c, 1);
}
// All wide text entry points share these bitmap glyphs and DC colour/clip rules.
void glyph(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline = false) {
    auto *dc = dc_of(hdc);
    if (ch < 32 || ch > 127)
        ch = '?';
    int64_t scale = font_scale(hdc), width = 8 * scale, height = 16 * scale;
    Rect clip = clip_box(hdc);
    uint32_t fg = argb(dc->text_color), bg = argb(dc->bk_color);
    for (int64_t yy = std::max<int64_t>(y, clip.t); yy < std::min<int64_t>(y + height, clip.b);
         ++yy)
        for (int64_t xx = std::max<int64_t>(x, clip.l); xx < std::min<int64_t>(x + width, clip.r);
             ++xx) {
            uint8_t row = recomp_font::font8x16[ch - 32][size_t((yy - y) / scale)];
            if ((row & (0x80 >> ((xx - x) / scale))) || (underline && yy - y >= 14 * scale))
                write_pixel(hdc, xx, yy, fg);
            else if (dc->bk_mode == 2)
                write_pixel(hdc, xx, yy, bg);
        }
}
void text_out(X86 *c) {
    uint32_t hdc = arg(c, 0), flags = arg(c, 3), rp = arg(c, 4), text = arg(c, 5), n = arg(c, 6),
             dx = arg(c, 7);
    auto *dc = dc_of(hdc);
    int w, h;
    if (!dc || !dc_size(hdc, &w, &h) || n > GUEST_SIZE / 4 ||
        (n && (!text || !gm_valid(text, n * 2))) || (dx && !gm_valid(dx, n * 4)) ||
        ((flags & 6) && (!rp || !gm_valid(rp, 16)))) {
        set_eax(c, 0);
        return;
    }
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    Rect rect{};
    if (flags & 6)
        rect = {int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
                int32_t(rd32(rp + 12))};
    if (flags & 4) {
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    uint32_t bg = argb(dc->bk_color);
    if (flags & 2)
        fill(hdc, rect, bg);
    int64_t width = 8 * font_scale(hdc), x = int32_t(arg(c, 1)), y = int32_t(arg(c, 2));
    for (uint32_t i = 0; i < n; ++i) {
        glyph(hdc, x, y, rd16(text + 2 * i));
        x += dx ? int32_t(rd32(dx + 4 * i)) : width;
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    set_eax(c, 1);
}
void memory_font(X86 *c) {
    uint32_t count = arg(c, 3);
    if (count && !gm_valid(count, 4)) {
        set_eax(c, 0);
        return;
    }
    Object font;
    font.kind = Object::Font;
    uint32_t handle = make_object(font);
    if (count)
        wr32(count, 1);
    set_eax(c, handle);
}
void enumerate(X86 *c) {
    uint32_t cb = arg(c, 2), param = arg(c, 3);
    if (!dc_of(arg(c, 0)) || !cb) {
        set_eax(c, 0);
        return;
    }
    uint32_t data = heap_alloc(92 + 60, true);
    if (!data) {
        set_eax(c, 0);
        return;
    }
    wr32(data, 16);
    wr32(data + 4, 8);
    wr32(data + 16, 400);
    wr8(data + 27, 0x30);
    gm_put_wstr(data + 28, "recomp", 32);
    metrics(data + 92, 1, 400);
    uint32_t result = guest_call(c, cb, data, data + 92, 1, param);
    heap_free(data);
    set_eax(c, result);
}
void unsupported(X86 *c) {
    set_eax(c, 0);
}
} // namespace
namespace gdi {
// DrawText's basic VCL layout uses the same fixed font as ExtTextOutW.
// Measurement must use that font too; CALCRECT never mutates canvas pixels.
uint32_t draw_text(uint32_t hdc, uint32_t text, uint32_t count, uint32_t rp, uint32_t flags) {
    auto *dc = dc_of(hdc);
    if (!dc || !rp || !gm_valid(rp, 16) || !text)
        return 0;
    if (count == UINT32_MAX) {
        count = 0;
        while (uint64_t(text) + 2ull * count + 2 <= GUEST_SIZE && gm_valid(text + count * 2, 2) &&
               rd16(text + count * 2))
            ++count;
    }
    if (!count || count > GUEST_SIZE / 2 || !gm_valid(text, count * 2))
        return 0;
    Rect rect{int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
              int32_t(rd32(rp + 12))};
    int64_t cw = 8 * int64_t(font_scale(hdc)), ch = 16 * int64_t(font_scale(hdc));
    struct Cell {
        uint16_t ch;
        bool underline;
    };
    std::vector<std::vector<Cell>> lines(1);
    bool prefix = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t unit = rd16(text + 2 * i);
        if (!(flags & 0x800) && unit == '&') { // DT_NOPREFIX
            if (i + 1 < count && rd16(text + 2 * (i + 1)) == '&')
                ++i;
            else {
                prefix = true;
                continue;
            }
        }
        if (unit == '\r' || unit == '\n') {
            if (unit == '\r' && i + 1 < count && rd16(text + 2 * (i + 1)) == '\n')
                ++i;
            if (!(flags & 0x20)) {
                lines.emplace_back();
                prefix = false;
                continue;
            }
            unit = ' ';
        }
        if (unit == '\t' && (flags & 0x40)) {
            size_t spaces = 8 - lines.back().size() % 8;
            lines.back().insert(lines.back().end(), spaces, Cell{' ', false});
        } else
            lines.back().push_back({unit, prefix && !(flags & 0x100000)});
        prefix = false;
    }
    if ((flags & 0x10) && !(flags & 0x20) && rect.r > rect.l) { // DT_WORDBREAK
        size_t cols = size_t((int64_t(rect.r) - rect.l) / cw);
        for (size_t i = 0; i < lines.size(); ++i) {
            auto &line = lines[i];
            if (!cols || line.size() <= cols)
                continue;
            size_t split = std::min(cols, line.size() - 1);
            while (split && line[split].ch != ' ')
                --split;
            if (!split) { // A long word is never split in the middle.
                split = cols;
                while (split < line.size() && line[split].ch != ' ')
                    ++split;
            }
            if (split < line.size()) {
                std::vector<Cell> tail(line.begin() + split + 1, line.end());
                line.resize(split);
                lines.insert(lines.begin() + i + 1, std::move(tail));
            }
        }
    }
    int64_t height = ch * lines.size(), maxwidth = 0;
    for (const auto &line : lines)
        maxwidth = std::max(maxwidth, int64_t(line.size()) * cw);
    if (height > INT_MAX || maxwidth > INT_MAX)
        return 0;
    if (flags & 0x400) {
        wr32(rp + 8, uint32_t(int64_t(rect.l) + maxwidth));
        wr32(rp + 12, uint32_t(int64_t(rect.t) + height));
        return uint32_t(height);
    }
    int w, h;
    if (!dc_size(hdc, &w, &h))
        return 0;
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    if (!(flags & 0x100)) { // DT_NOCLIP
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    int64_t y = rect.t;
    if (flags & 0x20) {
        if (flags & 4)
            y += (int64_t(rect.b) - rect.t - height) / 2;
        else if (flags & 8)
            y = int64_t(rect.b) - height;
    }
    for (const auto &line : lines) {
        int64_t x = rect.l, width = int64_t(line.size()) * cw;
        if (flags & 1)
            x += (int64_t(rect.r) - rect.l - width) / 2;
        else if (flags & 2)
            x = int64_t(rect.r) - width;
        for (Cell cell : line) {
            glyph(hdc, x, y, cell.ch, cell.underline);
            x += cw;
        }
        y += ch;
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    return uint32_t(y - rect.t);
}
void register_text() {
#define G(n, a, f)                                                                                 \
    {                                                                                              \
        "GDI32.dll", n, a, f                                                                       \
    }
    static const ImportShim shims[] = {G("CreateFontIndirectW", 1, create_font),
                                       G("ExtTextOutW", 8, text_out),
                                       G("GetTextExtentPoint32W", 4, extent),
                                       G("GetTextExtentPointW", 4, extent),
                                       G("GetTextMetricsW", 2, get_metrics),
                                       G("AddFontMemResourceEx", 4, memory_font),
                                       G("EnumFontsW", 4, enumerate),
                                       G("EnumFontFamiliesExW", 5, enumerate),
                                       G("CreateEnhMetaFileW", 4, unsupported),
                                       G("CreateEnhMetaFileA", 4, unsupported),
                                       G("CloseEnhMetaFile", 1, unsupported),
                                       G("DeleteEnhMetaFile", 1, unsupported),
                                       G("GetEnhMetaFileW", 1, unsupported),
                                       G("GetEnhMetaFileA", 1, unsupported),
                                       G("GetEnhMetaFileBits", 3, unsupported),
                                       G("GetEnhMetaFileHeader", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionW", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionA", 3, unsupported),
                                       G("GetEnhMetaFilePaletteEntries", 3, unsupported),
                                       G("GetEnhMetaFilePixelFormat", 3, unsupported),
                                       G("SetEnhMetaFileBits", 2, unsupported),
                                       G("SetWinMetaFileBits", 4, unsupported),
                                       G("GetWinMetaFileBits", 5, unsupported),
                                       G("PlayEnhMetaFile", 3, unsupported),
                                       G("CopyEnhMetaFileW", 2, unsupported),
                                       G("CreateDCW", 4, unsupported),
                                       G("CreateICW", 4, unsupported),
                                       G("StartDocW", 2, unsupported),
                                       G("EndDoc", 1, unsupported),
                                       G("StartPage", 1, unsupported),
                                       G("EndPage", 1, unsupported),
                                       G("AbortDoc", 1, unsupported),
                                       G("SetAbortProc", 2, unsupported)};
#undef G
    imports_register(shims, sizeof(shims) / sizeof(shims[0]));
}
} // namespace gdi
