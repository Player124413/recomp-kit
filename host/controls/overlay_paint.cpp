// overlay_paint.cpp - see overlay_paint.h.
#include "overlay_paint.h"

#include "pad_art.h"

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

void paint_layer(Canvas &c, const ControlsView &view, int layer, const Rect &r) {
    // Portrait: the area below the game is opaque, whatever the opacity.
    if (layer == 0 && !view.controls_area.empty())
        c.rect(view.controls_area.x - r.x, view.controls_area.y - r.y, view.controls_area.w,
               view.controls_area.h, flat(12, 14, 18, 255));
    for (size_t i = 0; i < view.backdrops.size(); ++i) {
        if (i >= view.backdrop_layers.size() || view.backdrop_layers[i] != layer)
            continue;
        const Rect &b = view.backdrops[i];
        c.rect(b.x - r.x, b.y - r.y, b.w, b.h, flat(6, 9, 15, 150));
    }
    for (const DrawControl &d : view.controls) {
        if (d.layer != layer)
            continue;
        const int x = d.rect.x - r.x, y = d.rect.y - r.y, w = d.rect.w, h = d.rect.h;
        switch (d.kind) {
        case Kind::Key: {
            if (d.lit)
                c.rect(x, y, w, h, flat(120, 160, 255, 220));
            else
                c.rect(x, y, w, h, flat(40, 48, 64, 200));
            const char *label = d.label.c_str();
            const Rgba text_color = d.lit ? Rgba{10, 12, 20, 255} : Rgba{235, 242, 255, 255};
            // A label too wide for a small key (inside a 2px margin each
            // side) drops to the 1x font.
            const int scale = c.text_width(label, 2) > w - 4 ? 1 : 2;
            c.text(x + (w - c.text_width(label, scale)) / 2, y + (h - 8 * scale) / 2, label,
                   text_color, scale, true);
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
            paint_control(c, d, r.x, r.y); // the DualSense look (pad_art.cpp)
            break;
        }
    }
}

void paint_overlay(Canvas &c, const ControlsView &view, const Rect &r) {
    int layers = int(view.layers.size());
    for (const DrawControl &d : view.controls)
        layers = std::max(layers, d.layer + 1);
    for (int layer = 0; layer < layers; ++layer)
        paint_layer(c, view, layer, r);
}

} // namespace controls
