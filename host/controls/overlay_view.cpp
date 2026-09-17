// overlay_view.cpp - make_view (overlay.h): no GPU, so the unit tests link it.
#include "overlay.h"

#include "../keypad_layout.h"
#include "router.h"

#include <cmath>
#include <cstring>

namespace controls {

namespace {

// FNV-1a over the view's fields: the revision the presenter compares.
struct Hash {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void *p, size_t n) {
        const unsigned char *b = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void num(int64_t v) {
        bytes(&v, sizeof v);
    }
    void real(double v) {
        bytes(&v, sizeof v);
    }
    void str(const std::string &s) {
        num(int64_t(s.size()));
        bytes(s.data(), s.size());
    }
    void rect(const Rect &r) {
        num(r.x);
        num(r.y);
        num(r.w);
        num(r.h);
    }
};

int group_named(const Layout &l, const std::string &id) {
    for (size_t i = 0; i < l.groups.size(); ++i)
        if (l.groups[i].id == id)
            return int(i);
    return -1;
}

} // namespace

ControlsView make_view(const Layout &l, const Router &r, const Screen &s, double opacity) {
    ControlsView v;
    v.wanted = true;
    v.dw = s.dw;
    v.dh = s.dh;
    v.opacity = opacity;
    v.controls_area = s.controls_area;
    for (int g = 0; g < int(l.groups.size()); ++g) {
        const Group &grp = l.groups[g];
        if (grp.visible && grp.has_grid)
            v.backdrops.push_back(group_rect(l, g, s));
        for (int c = 0; c < int(grp.controls.size()); ++c) {
            const Control &ctl = grp.controls[c];
            // A toggle's tab is drawn even while its own group is hidden.
            if (!grp.visible && ctl.kind != Kind::Toggle)
                continue;
            const ControlState &st = r.state(g, c);
            DrawControl d;
            d.kind = ctl.kind;
            d.rect = control_rect(l, g, c, s);
            d.label = ctl.label;
            d.pressed = st.pressed;
            d.lit = ctl.kind == Kind::Key && (r.lit() & keypad_modifier_bit(ctl.scancode)) != 0;
            d.button = ctl.button;
            d.knob_x = st.knob_x;
            d.knob_y = st.knob_y;
            d.base_x = st.base_x;
            d.base_y = st.base_y;
            d.hat = st.hat;
            d.floating = ctl.floating;
            // Scaled like the router's own travel (router.cpp), so the knob
            // quad lands where the stick's output says it is.
            if (ctl.kind == Kind::Stick)
                d.radius_px = int(std::lround(ctl.radius * l.scale * s.scale));
            if (ctl.kind == Kind::Toggle) {
                const int target = group_named(l, ctl.target);
                d.group_visible = target < 0 || l.groups[target].visible;
                d.label_off = ctl.label_off;
            }
            v.controls.push_back(d);
        }
    }

    // Only what paint_overlay draws: a key's press must not re-rasterize the
    // whole canvas, and neither may a stick's knob offset, which Overlay
    // draws as its own quad. Extend this with every field a later task
    // starts drawing.
    Hash h;
    h.num(v.dw);
    h.num(v.dh);
    h.real(v.opacity);
    h.rect(v.controls_area);
    h.num(int64_t(v.backdrops.size()));
    for (const Rect &b : v.backdrops)
        h.rect(b);
    h.num(int64_t(v.controls.size()));
    for (const DrawControl &d : v.controls) {
        h.num(int(d.kind));
        h.rect(d.rect);
        if (d.kind == Kind::Key) {
            h.str(d.label);
            h.num(d.lit ? 1 : 0);
        } else if (d.kind == Kind::Toggle) {
            h.str(d.group_visible ? d.label : d.label_off);
        } else {
            // The pad art (pad_art.cpp): labels, the button's glyph, its
            // press, the dpad's lit arrows and a stick's base.
            h.str(d.label);
            h.num(int(d.button));
            h.num(d.pressed ? 1 : 0);
            h.num(d.hat);
            if (d.kind == Kind::Stick) {
                h.num(d.radius_px);
                h.num(d.floating ? 1 : 0);
                if (d.pressed) { // at rest the base sits at the rect's centre
                    h.real(d.base_x);
                    h.real(d.base_y);
                }
            }
        }
    }
    v.revision = h.h;
    return v;
}

} // namespace controls
