// raster.cpp - see raster.h.
#include "raster.h"

#include "../../mods/mods_internal.h"

#include <algorithm>
#include <cstring>

namespace controls {

void Canvas::fill(int x, int y, int fw, int fh, int r, int g, int b, int a) {
    // Premultiplied, as the hud pipeline blends One / OneMinusSrcAlpha.
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(w, x + fw), y1 = std::min(h, y + fh);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            uint8_t *p = &px[(size_t(yy) * w + xx) * 4];
            p[0] = uint8_t(r * a / 255);
            p[1] = uint8_t(g * a / 255);
            p[2] = uint8_t(b * a / 255);
            p[3] = uint8_t(a);
        }
}

void Canvas::disc(int cx, int cy, int radius, int r, int g, int b, int a) {
    for (int dy = -radius; dy < radius; ++dy) {
        // Half-width of the row through the pixel centres.
        const double yc = dy + 0.5;
        int half = 0;
        while ((half + 0.5) * (half + 0.5) + yc * yc <= double(radius) * radius)
            ++half;
        fill(cx - half, cy + dy, 2 * half, 1, r, g, b, a);
    }
}

void Canvas::text(int x, int y, const char *str, int r, int g, int b, int a) {
    for (; *str; ++str, x += 12) {
        const uint8_t *glyph = mods_font6x8_glyph(*str);
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 6; ++col)
                if (glyph[row] & (0x20 >> col))
                    fill(x + col * 2, y + row * 2, 2, 2, r, g, b, a);
    }
}

int text_width(const char *str) {
    return int(strlen(str)) * 12;
}

} // namespace controls
