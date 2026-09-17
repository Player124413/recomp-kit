// overlay_paint.cpp - see overlay_paint.h.
#include "overlay_paint.h"

#include <algorithm>

namespace controls {

namespace {

Paint flat(int r, int g, int b, int a) {
    Paint p;
    p.kind = Paint::Flat;
    p.c0 = Rgba{uint8_t(r), uint8_t(g), uint8_t(b), uint8_t(a)};
    p.replace = true;
    return p;
}

} // namespace

void paint_overlay(Canvas &c, const ControlsView &view, const Rect &r) {
    for (const Rect &b : view.backdrops)
        c.rect(b.x - r.x, b.y - r.y, b.w, b.h, flat(6, 9, 15, 150));
    for (const DrawControl &d : view.controls) {
        const int x = d.rect.x - r.x, y = d.rect.y - r.y, w = d.rect.w, h = d.rect.h;
        switch (d.kind) {
        case Kind::Key: {
            if (d.lit)
                c.rect(x, y, w, h, flat(120, 160, 255, 220));
            else
                c.rect(x, y, w, h, flat(40, 48, 64, 200));
            const char *label = d.label.c_str();
            const Rgba text_color = d.lit ? Rgba{10, 12, 20, 255} : Rgba{235, 242, 255, 255};
            c.text(x + (w - c.text_width(label)) / 2, y + (h - 16) / 2, label, text_color, 2, true);
            break;
        }
        case Kind::Toggle: {
            c.rect(x, y, w, h, flat(6, 9, 15, 150));
            c.rect(x + 2, y + 2, w - 4, h - 4, flat(40, 48, 64, 200));
            const char *label = d.group_visible ? d.label.c_str() : d.label_off.c_str();
            c.text(x + (w - c.text_width(label)) / 2, y + (h - 16) / 2, label,
                   Rgba{235, 242, 255, 255}, 2, true);
            break;
        }
        default:
            // A placeholder until the pad look lands (Task 11).
            c.disc(x + w / 2.0, y + h / 2.0, std::min(w, h) / 2.0, flat(40, 48, 64, 200));
            break;
        }
    }
}

} // namespace controls
