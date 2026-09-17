// controls_tests.cpp - the on-screen controls: json, layouts, router, pad, binding, editor.
#include "../../platform/os.h"
#include "../controls/binding.h"
#include "../controls/builtin_layouts.h"
#include "../controls/editor.h"
#include "../controls/haptics.h"
#include "../controls/json.h"
#include "../controls/layout.h"
#include "../controls/layout_fallback.h"
#include "../controls/layout_store.h"
#include "../controls/overlay.h"
#include "../controls/overlay_paint.h"
#include "../controls/raster.h"
#include "../controls/router.h"
#include "../controls/vpad.h"
#include "../keypad_layout.h"
#include "keypad_legacy_oracle.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace controls;

static void test_json_round_trip() {
    Json v;
    std::string err;
    CHECK(json_parse(R"({"a": 1, "b": [true, false, null], "c": "x\"y\\né", "d": -2.5e1})", &v,
                     &err));
    CHECK(v.type == Json::Object);
    CHECK(v.num("a", 0) == 1);
    CHECK(v.get("b") && v.get("b")->a.size() == 3 && v.get("b")->a[0].b);
    CHECK(v.str("c", "") == "x\"y\\n\xc3\xa9");
    CHECK(v.num("d", 0) == -25);
    Json again;
    CHECK(json_parse(json_write(v), &again, &err));
    CHECK(json_write(again) == json_write(v));
    CHECK(json_write(Json::number(3)) == "3");
}

static void test_json_errors_name_the_line() {
    Json v;
    std::string err;
    CHECK(!json_parse("{\n\"a\": 1,\n\"b\" 2}", &v, &err));
    CHECK(err.rfind("line 3:", 0) == 0);
    CHECK(!json_parse("[1, 2", &v, &err));
    CHECK(!json_parse("{} trailing", &v, &err));
    CHECK(v.num("missing", 7) == 7); // a failed parse leaves a usable value
}

static const char *kTinyLayout = R"({
  "version": 1, "name": "tiny", "opacity": 0.5, "safe_inset": false, "future_field": 3,
  "groups": [
    {"id": "g", "grid": {"cols": 2, "rows": 1, "key": 36, "gap": 4}, "anchor": "bottom-right",
     "controls": [
       {"kind": "key", "scancode": "LShift", "col": 0, "row": 0},
       {"kind": "key", "scancode": "Space", "col": 1, "row": 0, "label": "SP"}]},
    {"id": "loose", "controls": [
       {"kind": "button", "button": "cross", "anchor": "top-left", "x": 10, "y": 20, "size": 50},
       {"kind": "stick", "stick": "right", "mode": "fixed", "anchor": "center", "radius": 30},
       {"kind": "hologram"},
       {"kind": "toggle", "target": "g", "label": "HIDE", "label_off": "KEYS",
        "anchor": "bottom-right", "w": 64, "h": 20, "stack_on": "g"}]}
  ]})";

static void test_layout_parse_and_write() {
    Layout l;
    std::string err;
    std::vector<std::string> warnings;
    CHECK(parse_layout(kTinyLayout, &l, &err, &warnings));
    CHECK(warnings.size() == 1 && warnings[0].find("hologram") != std::string::npos);
    CHECK(l.name == "tiny" && l.opacity == 0.5 && !l.safe_inset);
    CHECK(l.groups.size() == 2 && l.groups[0].has_grid && l.groups[0].grid.cols == 2);
    CHECK(l.groups[0].controls[0].scancode == kScanLShift);
    CHECK(l.groups[0].controls[0].label == "LShift");
    CHECK(l.groups[0].controls[1].label == "SP");
    const Control &stick = l.groups[1].controls[1];
    CHECK(stick.kind == Kind::Stick && stick.stick == 1 && !stick.floating && stick.w == 60);
    CHECK(stick.radius == 30);               // no "zone": the zone defaults to 2 * radius
    CHECK(l.groups[1].controls.size() == 3); // the unknown kind was skipped
    Layout again;
    CHECK(parse_layout(write_layout(l), &again, &err));
    CHECK(write_layout(again) == write_layout(l));
    CHECK(!parse_layout("{\"groups\": 5}", &l, &err));
    CHECK(scancode_from_name("F5") == kScanF5 && std::string(scancode_name(kScanUp)) == "Up");
    CHECK(scancode_from_name("NotAKey") == 0);
}

// A stick's "radius" (knob travel) and "zone" (w/h, the hit-test/base rect)
// are independent: without "zone" the zone defaults to 2 * radius (as
// "radius" alone used to size the whole control); with one, it is exactly
// what is given. Both round-trip through write_layout.
static const char *kStickZoneLayout = R"({
  "version": 1, "name": "sticks",
  "groups": [
    {"id": "g", "controls": [
       {"kind": "stick", "stick": "left", "mode": "floating", "anchor": "top-left",
        "radius": 40},
       {"kind": "stick", "stick": "right", "mode": "fixed", "anchor": "top-left",
        "x": 300, "radius": 40, "zone": [300, 150]}
    ]}
  ]})";

static void test_stick_radius_and_zone_round_trip() {
    Layout l;
    std::string err;
    CHECK(parse_layout(kStickZoneLayout, &l, &err));
    const Control &no_zone = l.groups[0].controls[0];
    CHECK(no_zone.radius == 40 && no_zone.w == 80 && no_zone.h == 80);
    const Control &explicit_zone = l.groups[0].controls[1];
    CHECK(explicit_zone.radius == 40 && explicit_zone.w == 300 && explicit_zone.h == 150);

    Layout again;
    CHECK(parse_layout(write_layout(l), &again, &err));
    CHECK(write_layout(again) == write_layout(l));
    const Control &no_zone2 = again.groups[0].controls[0];
    CHECK(no_zone2.radius == 40 && no_zone2.w == 80 && no_zone2.h == 80);
    const Control &explicit_zone2 = again.groups[0].controls[1];
    CHECK(explicit_zone2.radius == 40 && explicit_zone2.w == 300 && explicit_zone2.h == 150);
}

static Screen screen(int dw, int dh, double scale) {
    Screen s;
    s.dw = dw;
    s.dh = dh;
    s.scale = scale;
    s.safe = {0, 0, dw, dh};
    return s;
}

static void test_layout_geometry_and_hits() {
    Layout l;
    std::string err;
    CHECK(parse_layout(kTinyLayout, &l, &err));
    const Screen s = screen(2000, 1000, 2.0);
    // Grid: pitch = lround((36 + 4) * 2) = 80, box 160x80 in the bottom-right corner.
    const Rect box = group_rect(l, 0, s);
    CHECK(box.x == 1840 && box.y == 920 && box.w == 160 && box.h == 80);
    const Rect k1 = control_rect(l, 0, 1, s);
    CHECK(k1.x == 1840 + 80 + 4 && k1.y == 924 && k1.w == 72 && k1.h == 72);
    const Rect cross = control_rect(l, 1, 0, s);
    CHECK(cross.x == 20 && cross.y == 40 && cross.w == 100 && cross.h == 100);
    const Rect stick = control_rect(l, 1, 1, s);
    CHECK(stick.x == 940 && stick.y == 440 && stick.w == 120);
    // The toggle sits on the visible group, and in the corner once it is hidden.
    CHECK(control_rect(l, 1, 2, s).y == 920 - 40);
    l.groups[0].visible = false;
    CHECK(control_rect(l, 1, 2, s).y == 1000 - 40);
    Hit h = hit_test(l, s, 1990, 990);
    CHECK(h.group == 1 && h.control == 2);        // the tab, even with its group hidden
    CHECK(hit_test(l, s, 1850, 950).group == -1); // hidden group: the game's
    l.groups[0].visible = true;
    h = hit_test(l, s, 1930, 960);
    CHECK(h.group == 0 && h.control == 1);
    h = hit_test(l, s, 1841, 921); // the half-gap at the box's corner
    CHECK(h.group == 0 && h.gap);
    CHECK(hit_test(l, s, 500, 900).group == -1); // non-grid groups never claim gaps
    l.scale = 40.0 / 36.0;
    CHECK(control_rect(l, 0, 0, s).w == lround((40 + 4) * 2.0) - 8);
    Screen none;
    CHECK(hit_test(l, none, 0, 0).group == -1);
}

// The built-in "keys" tablet layout must reproduce host/keypad_layout.cpp's
// geometry exactly, at every size step and screen scale: same key rects,
// same scancodes and labels, same tab rects shown and hidden.
static void test_builtin_keys_matches_the_old_keypad() {
    Layout l;
    std::string err;
    CHECK(parse_layout(builtin_layout("keys", Form::Tablet), &l, &err));
    const double scales[] = {1.0, 2.0, 3.0};
    const int sizes_pt[] = {32, 36, 40};
    for (double sc : scales)
        for (int size = 0; size < 3; ++size) {
            l.scale = sizes_pt[size] / 36.0;
            const Screen s = screen(int(1180 * sc), int(820 * sc), sc);
            for (int side = 0; side < 2; ++side) {
                int n = 0;
                const KeypadKey *keys = legacy_keypad_keys(KeypadSide(side), &n);
                const Group &g = l.groups[side];
                CHECK(int(g.controls.size()) == n);
                for (int i = 0; i < n; ++i) {
                    const KeypadRect old =
                        legacy_keypad_key_rect(KeypadSide(side), keys[i], size, sc, s.dw, s.dh);
                    const Rect now = control_rect(l, side, i, s);
                    CHECK(now.x == old.x && now.y == old.y && now.w == old.w && now.h == old.h);
                    CHECK(g.controls[i].scancode == keys[i].scancode);
                    CHECK(g.controls[i].label == keys[i].label);
                }
                if (size == 1) { // tabs do not scale with key size in the old keypad
                    for (int shown = 0; shown < 2; ++shown) {
                        l.groups[side].visible = shown != 0;
                        const KeypadRect old = legacy_keypad_tab_rect(KeypadSide(side), shown != 0,
                                                                      size, sc, s.dw, s.dh);
                        const Rect now = control_rect(l, 2, side, s);
                        CHECK(now.x == old.x && now.y == old.y && now.w == old.w && now.h == old.h);
                    }
                    l.groups[side].visible = true;
                }
            }
        }
}

static void write_file(const std::filesystem::path &path, const std::string &text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    FILE *f = fopen(path.string().c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

static void test_form_for() {
    CHECK(form_for(2360, 1640, 2.0) == Form::Tablet);
    CHECK(form_for(2532, 1170, 3.0) == Form::PhoneLandscape);
    CHECK(form_for(1170, 2532, 3.0) == Form::PhonePortrait);
}

// Exercises the profile-then-game-then-built-in search, names() and the
// save/delete round trip against a scratch directory tree.
static void test_layout_store() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/controls-store-test-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    const std::filesystem::path root = dir;
    const std::filesystem::path profile_dir = root / "profile";
    const std::filesystem::path game_dir = root / "game";

    // The tiny layout, renamed "keys" so it round-trips through keys.json.
    std::string keys_json = kTinyLayout;
    size_t pos = keys_json.find("\"tiny\"");
    CHECK(pos != std::string::npos);
    if (pos != std::string::npos)
        keys_json.replace(pos, 6, "\"keys\"");

    write_file(game_dir / "keys.json", keys_json);
    write_file(game_dir / "pad.json", "{");
    write_file(game_dir / "extra.phone-portrait.json", keys_json);

    LayoutStore store;
    store.set_dirs(profile_dir.string(), game_dir.string());

    const std::vector<std::string> expected_names = {"pad", "keys", "pad+keys", "extra"};
    CHECK(store.names() == expected_names);

    Layout game_keys;
    std::string problem;
    CHECK(store.load("keys", Form::Tablet, &game_keys, &problem));
    CHECK(game_keys.name == "keys" && game_keys.opacity == 0.5);
    CHECK(problem.empty());

    problem.clear();
    Layout unused;
    CHECK(!store.load("pad", Form::Tablet, &unused, &problem));
    CHECK(problem.find("pad.json") != std::string::npos);
    CHECK(problem.find("line 1") != std::string::npos);

    // No file and no built-in for this name/form: a clean miss.
    problem.clear();
    CHECK(!store.load("nope", Form::Tablet, &unused, &problem));
    CHECK(problem.empty());

    // save_user_copy -> load returns the player's copy; has_user_copy sees it.
    Layout mine;
    std::string parse_err;
    CHECK(parse_layout(keys_json, &mine, &parse_err));
    mine.opacity = 0.9;
    std::string save_err;
    CHECK(!store.has_user_copy("keys", Form::Tablet));
    CHECK(store.save_user_copy(mine, Form::Tablet, &save_err));
    CHECK(store.has_user_copy("keys", Form::Tablet));
    Layout loaded;
    problem.clear();
    CHECK(store.load("keys", Form::Tablet, &loaded, &problem));
    CHECK(loaded.opacity == 0.9);

    // delete_user_copy removes it; load falls back to the game copy.
    CHECK(store.delete_user_copy("keys", Form::Tablet));
    CHECK(!store.has_user_copy("keys", Form::Tablet));
    Layout fallback;
    problem.clear();
    CHECK(store.load("keys", Form::Tablet, &fallback, &problem));
    CHECK(fallback.opacity == 0.5);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// --- Raster: anti-aliased premultiplied primitives -------------------------

static Paint flat_paint(Rgba c) {
    Paint p;
    p.kind = Paint::Flat;
    p.c0 = c;
    return p;
}

static void test_raster_disc() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64);
    c.disc(32, 32, 16, flat_paint(Rgba{255, 255, 255, 255}));

    const Rgba centre = c.at(32, 32);
    CHECK(centre.r == 255 && centre.g == 255 && centre.b == 255 && centre.a == 255);
    CHECK(c.at(32, 10).a == 0);
    const int edge_a = c.at(48, 32).a;
    CHECK(edge_a > 0 && edge_a < 255);

    long sum = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            sum += c.at(x, y).a;
    const double expected = 3.14159265358979323846 * 16.0 * 16.0 * 255.0;
    CHECK(std::abs(sum - expected) < expected * 0.02);
}

static void test_raster_ring() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64);
    c.ring(32, 32, 14, 10, flat_paint(Rgba{255, 255, 255, 255}));
    CHECK(c.at(32, 32).a == 0);   // the hole
    CHECK(c.at(32, 20).a == 255); // 12 from centre: between the radii
}

static void test_raster_radial_gradient() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64);
    Paint p;
    p.kind = Paint::Radial;
    p.c0 = Rgba{255, 0, 0, 255};
    p.c1 = Rgba{0, 0, 255, 255};
    p.x0 = 32;
    p.y0 = 32;
    p.x1 = 16; // radius
    c.disc(32, 32, 16, p);
    const Rgba centre = c.at(32, 32);
    CHECK(centre.r > 200 && centre.b < 20);
    const Rgba near_edge = c.at(32, 21); // 11 px from centre, close to the 16 px radius
    CHECK(near_edge.b > near_edge.r);
}

static void test_raster_opacity_premultiplies() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64, 0.5);
    c.disc(32, 32, 16, flat_paint(Rgba{255, 255, 255, 255}));
    const Rgba centre = c.at(32, 32);
    CHECK(centre.a == 127 || centre.a == 128);
    CHECK(centre.r == centre.a); // premultiplied: white * alpha == alpha
}

static void test_raster_polygon() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64);
    const std::vector<std::pair<double, double>> tri = {{0, 0}, {63, 0}, {0, 63}};
    c.polygon(tri, flat_paint(Rgba{255, 255, 255, 255}));
    CHECK(c.at(5, 5).a > 0);
    CHECK(c.at(60, 60).a == 0);
}

static void test_raster_stroke() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64);
    const std::vector<std::pair<double, double>> line = {{0, 32}, {63, 32}};
    c.stroke(line, 4, false, flat_paint(Rgba{255, 255, 255, 255}));
    CHECK(c.at(32, 32).a > 0);
    CHECK(c.at(32, 36).a == 0);
}

static void test_raster_source_over() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64);
    c.disc(32, 32, 20, flat_paint(Rgba{255, 255, 255, 255}));
    Paint black_half;
    black_half.c0 = Rgba{0, 0, 0, 128};
    c.rect(10, 10, 44, 44, black_half); // fully inside the disc
    const Rgba centre = c.at(32, 32);
    CHECK(std::abs(int(centre.r) - 128) <= 2);
    CHECK(std::abs(int(centre.g) - 128) <= 2);
    CHECK(std::abs(int(centre.b) - 128) <= 2);
}

static void test_raster_text() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64);
    c.text(0, 0, "A", Rgba{255, 255, 255, 255});
    bool any_set = false;
    for (int y = 0; y < 16 && !any_set; ++y)
        for (int x = 0; x < 12 && !any_set; ++x)
            if (c.at(x, y).a > 0)
                any_set = true;
    CHECK(any_set);
    CHECK(c.text_width("AB") == 24);
}

// --- Old raster.cpp (pre-Task-8), frozen here as a regression oracle: it
// replaced (never blended) pixels, and Task 7's keypad look depends on that
// replace semantics exactly. Never update this to track raster.h/.cpp.
extern "C" const uint8_t *mods_font6x8_glyph(char c); // mods/font6x8.cpp

namespace legacy_raster {

struct Canvas {
    std::vector<uint8_t> &px;
    int w, h;

    void fill(int x, int y, int fw, int fh, int r, int g, int b, int a) {
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

    void disc(int cx, int cy, int radius, int r, int g, int b, int a) {
        for (int dy = -radius; dy < radius; ++dy) {
            const double yc = dy + 0.5;
            int half = 0;
            while ((half + 0.5) * (half + 0.5) + yc * yc <= double(radius) * radius)
                ++half;
            fill(cx - half, cy + dy, 2 * half, 1, r, g, b, a);
        }
    }

    void text(int x, int y, const char *str, int r, int g, int b, int a) {
        for (; *str; ++str, x += 12) {
            const uint8_t *glyph = mods_font6x8_glyph(*str);
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 6; ++col)
                    if (glyph[row] & (0x20 >> col))
                        fill(x + col * 2, y + row * 2, 2, 2, r, g, b, a);
        }
    }
};

int text_width(const char *str) {
    return int(strlen(str)) * 12;
}

// A byte-for-byte copy of overlay.cpp's pre-Task-8 `paint()`.
void paint(Canvas &c, const ControlsView &view, const Rect &r) {
    const auto alpha = [&](int a) { return int(lround(a * std::clamp(view.opacity, 0.0, 1.0))); };
    for (const Rect &b : view.backdrops)
        c.fill(b.x - r.x, b.y - r.y, b.w, b.h, 6, 9, 15, alpha(150));
    for (const DrawControl &d : view.controls) {
        const int x = d.rect.x - r.x, y = d.rect.y - r.y, w = d.rect.w, h = d.rect.h;
        switch (d.kind) {
        case Kind::Key: {
            if (d.lit)
                c.fill(x, y, w, h, 120, 160, 255, alpha(220));
            else
                c.fill(x, y, w, h, 40, 48, 64, alpha(200));
            const char *label = d.label.c_str();
            c.text(x + (w - text_width(label)) / 2, y + (h - 16) / 2, label, d.lit ? 10 : 235,
                   d.lit ? 12 : 242, d.lit ? 20 : 255, alpha(255));
            break;
        }
        case Kind::Toggle: {
            c.fill(x, y, w, h, 6, 9, 15, alpha(150));
            c.fill(x + 2, y + 2, w - 4, h - 4, 40, 48, 64, alpha(200));
            const char *label = d.group_visible ? d.label.c_str() : d.label_off.c_str();
            c.text(x + (w - text_width(label)) / 2, y + (h - 16) / 2, label, 235, 242, 255,
                   alpha(255));
            break;
        }
        default:
            c.disc(x + w / 2, y + h / 2, std::min(w, h) / 2, 40, 48, 64, alpha(200));
            break;
        }
    }
}

} // namespace legacy_raster

// --- Router: fingers, keys, modifiers, toggles ----------------------------

// Records every call as a short string: "k44+", "k44-", "a:settings",
// "sw:next", "vis", "tap".
struct Rec : ControlsSink {
    std::vector<std::string> calls;
    void key(int scancode, bool down) override {
        calls.push_back("k" + std::to_string(scancode) + (down ? "+" : "-"));
    }
    void action(const std::string &name) override {
        calls.push_back("a:" + name);
    }
    void switch_layout(const std::string &target) override {
        calls.push_back("sw:" + target);
    }
    void group_visibility_changed() override {
        calls.push_back("vis");
    }
    void tap() override {
        calls.push_back("tap");
    }
};

static Layout keys_layout() {
    Layout l;
    std::string err;
    CHECK(parse_layout(builtin_layout("keys", Form::Tablet), &l, &err));
    return l;
}

// The tablet "keys" layout, rendered through the frozen legacy_raster::paint
// and through overlay_paint.cpp's new Canvas path, must produce identical
// buffers: the keypad's look must not move a single pixel under Task 8.
static void test_raster_matches_legacy_keypad_pixels() {
    Layout l = keys_layout();
    const int dw = 2360, dh = 1640;
    const Screen s = screen(dw, dh, 2.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    const ControlsView v = make_view(l, r, s, 1.0);
    const Rect full{0, 0, dw, dh};

    std::vector<uint8_t> old_px(size_t(dw) * dh * 4, 0);
    legacy_raster::Canvas old_c{old_px, dw, dh};
    legacy_raster::paint(old_c, v, full);

    std::vector<uint8_t> new_px(size_t(dw) * dh * 4, 0);
    Canvas new_c(new_px, dw, dh, v.opacity);
    paint_overlay(new_c, v, full);

    CHECK(old_px == new_px);
}

static int find_key(const Layout &l, int group, int scancode) {
    const Group &g = l.groups[group];
    for (size_t i = 0; i < g.controls.size(); ++i)
        if (g.controls[i].kind == Kind::Key && g.controls[i].scancode == scancode)
            return int(i);
    return -1;
}

// The middle of a control's current rect: recompute this right before a
// finger_down when the control (a toggle tab, in particular) may have moved
// since an earlier press changed the layout's visibility.
static void center(const Layout &l, int group, int control, const Screen &s, double *x, double *y) {
    const Rect r = control_rect(l, group, control, s);
    CHECK(!r.empty());
    *x = r.x + r.w / 2.0;
    *y = r.y + r.h / 2.0;
}

static void test_router_space_key() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    const uint32_t gen0 = r.generation();

    double x, y;
    center(l, 1, find_key(l, 1, kScanSpace), s, &x, &y); // Space lives on the right half
    CHECK(r.finger_down(1, x, y, 0, rec));
    CHECK((rec.calls == std::vector<std::string>{"k44+", "tap"}));
    CHECK(r.generation() != gen0);
    const uint32_t gen1 = r.generation();
    rec.calls.clear();

    CHECK(r.finger_up(1, 10, rec));
    CHECK((rec.calls == std::vector<std::string>{"k44-"}));
    CHECK(r.generation() != gen1);
}

static void test_router_latched_shift() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double sx, sy, ax, ay;
    center(l, 0, find_key(l, 0, kScanLShift), s, &sx, &sy);
    center(l, 0, find_key(l, 0, kScanA), s, &ax, &ay);

    uint32_t gen = r.generation();
    CHECK(r.finger_down(1, sx, sy, 0, rec)); // Shift down
    CHECK((rec.calls == std::vector<std::string>{"k225+", "tap"}));
    CHECK(r.lit() == 0); // Held, not latched yet
    CHECK(r.generation() != gen);
    gen = r.generation();
    rec.calls.clear();

    CHECK(r.finger_up(1, 100ull * 1000000ull, rec)); // a 100 ms tap latches
    CHECK(rec.calls.empty());                        // Held->Latched crosses no Off boundary
    CHECK(r.lit() == 1);
    CHECK(r.generation() != gen);
    gen = r.generation();
    rec.calls.clear();

    CHECK(r.finger_down(2, ax, ay, 200ull * 1000000ull, rec));
    CHECK((rec.calls == std::vector<std::string>{"k4+", "tap"}));
    CHECK(r.generation() != gen);
    gen = r.generation();
    rec.calls.clear();

    CHECK(r.finger_up(2, 250ull * 1000000ull, rec)); // A lifts: the latch releases too
    CHECK((rec.calls == std::vector<std::string>{"k4-", "k225-"}));
    CHECK(r.lit() == 0);
    CHECK(r.generation() != gen);
}

static void test_router_left_tab_toggle() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    uint32_t gen = r.generation();

    double x, y;
    center(l, 2, 0, s, &x, &y); // the tabs group's first toggle targets "left"
    CHECK(r.finger_down(1, x, y, 0, rec));
    CHECK((rec.calls == std::vector<std::string>{"vis", "tap"}));
    CHECK(!l.groups[0].visible);
    CHECK(r.generation() != gen);
    gen = r.generation();
    rec.calls.clear();

    CHECK(r.finger_up(1, 0, rec)); // the toggle is claimed and does nothing more
    CHECK(rec.calls.empty());

    center(l, 2, 0, s, &x, &y); // the tab moved: its group is hidden now
    CHECK(r.finger_down(2, x, y, 0, rec));
    CHECK((rec.calls == std::vector<std::string>{"vis", "tap"}));
    CHECK(l.groups[0].visible);
    CHECK(r.generation() != gen);
}

static void test_router_gap_is_claimed_silently() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    const Rect box = group_rect(l, 0, s);
    CHECK(r.finger_down(1, box.x + 1, box.y + 1, 0, rec)); // the half-gap at the box's corner
    CHECK(rec.calls.empty());
    CHECK(r.owns(1));
}

static void test_router_game_area_not_claimed() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    CHECK(!r.finger_down(1, s.dw / 2.0, s.dh / 2.0, 0, rec));
    CHECK(rec.calls.empty());
    CHECK(!r.owns(1));
}

static void test_router_cancel_does_not_release_latch() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double sx, sy, ax, ay;
    center(l, 0, find_key(l, 0, kScanLShift), s, &sx, &sy);
    CHECK(r.finger_down(1, sx, sy, 0, rec));
    CHECK(r.finger_up(1, 50ull * 1000000ull, rec)); // latches Shift
    CHECK(r.lit() == 1);
    rec.calls.clear();

    center(l, 0, find_key(l, 0, kScanA), s, &ax, &ay);
    CHECK(r.finger_down(2, ax, ay, 100ull * 1000000ull, rec));
    rec.calls.clear();
    const uint32_t gen = r.generation();

    CHECK(r.finger_cancel(2, rec));
    CHECK((rec.calls == std::vector<std::string>{"k4-"}));
    CHECK(r.lit() == 1); // key_lifted never runs on a cancel: the latch survives
    CHECK(!r.owns(2));
    CHECK(r.generation() != gen);
}

static void test_router_two_fingers_shift_held_and_a() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double sx, sy, ax, ay;
    center(l, 0, find_key(l, 0, kScanLShift), s, &sx, &sy);
    center(l, 0, find_key(l, 0, kScanA), s, &ax, &ay);

    CHECK(r.finger_down(1, sx, sy, 0, rec)); // Shift: finger stays down (chording)
    CHECK((rec.calls == std::vector<std::string>{"k225+", "tap"}));
    rec.calls.clear();

    CHECK(r.finger_down(2, ax, ay, 10ull * 1000000ull, rec));
    CHECK((rec.calls == std::vector<std::string>{"k4+", "tap"}));
    rec.calls.clear();

    CHECK(r.finger_up(2, 20ull * 1000000ull, rec)); // A lifts; Shift is Held, not Latched
    CHECK((rec.calls == std::vector<std::string>{"k4-"}));
    rec.calls.clear();

    CHECK(r.finger_up(1, 300ull * 1000000ull, rec)); // held well past the tap window
    CHECK((rec.calls == std::vector<std::string>{"k225-"}));
}

static void test_router_set_enabled_false_releases_and_blocks() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double ax, ay;
    center(l, 0, find_key(l, 0, kScanA), s, &ax, &ay);
    CHECK(r.finger_down(1, ax, ay, 0, rec));
    rec.calls.clear();
    const uint32_t gen = r.generation();

    r.set_enabled(false, rec);
    CHECK((rec.calls == std::vector<std::string>{"k4-"}));
    CHECK(!r.enabled());
    CHECK(!r.owns(1));
    CHECK(r.generation() != gen);

    Rec rec2;
    CHECK(!r.finger_down(2, ax, ay, 0, rec2)); // disabled: hits nothing
    CHECK(rec2.calls.empty());
}

static void test_router_cancel_all_releases_everything() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double sx, sy, ax, ay;
    center(l, 0, find_key(l, 0, kScanLShift), s, &sx, &sy);
    center(l, 0, find_key(l, 0, kScanA), s, &ax, &ay);
    CHECK(r.finger_down(1, sx, sy, 0, rec));
    CHECK(r.finger_down(2, ax, ay, 0, rec));
    rec.calls.clear();
    const uint32_t gen = r.generation();

    r.cancel_all(rec);
    CHECK((rec.calls == std::vector<std::string>{"k4-", "k225-"}));
    CHECK(!r.owns(1) && !r.owns(2));
    CHECK(r.lit() == 0);
    CHECK(r.generation() != gen);
}

// set_layout swaps the layout under a live game: everything the old layout's
// fingers held must let go first, exactly as cancel_all() releases it, or
// the game is left holding a key with no finger (and no way) to release it.
static void test_router_set_layout_releases_held_key() {
    Layout l = keys_layout();
    Layout other = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double ax, ay;
    center(l, 0, find_key(l, 0, kScanA), s, &ax, &ay);
    CHECK(r.finger_down(1, ax, ay, 0, rec));
    rec.calls.clear();
    const uint32_t gen = r.generation();

    r.set_layout(&other, rec);
    CHECK((rec.calls == std::vector<std::string>{"k4-"}));
    CHECK(r.layout() == &other);
    CHECK(!r.owns(1));
    CHECK(r.generation() != gen);
}

// A latched modifier has no finger holding it at all (the finger that
// latched it already lifted), so only KeypadModifiers::cancel_all — run
// unconditionally by cancel_all(), not just the per-finger release loop —
// can catch it when the layout swaps out from under it.
static void test_router_set_layout_releases_latched_modifier() {
    Layout l = keys_layout();
    Layout other = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double sx, sy;
    center(l, 0, find_key(l, 0, kScanLShift), s, &sx, &sy);
    CHECK(r.finger_down(1, sx, sy, 0, rec));
    CHECK(r.finger_up(1, 50ull * 1000000ull, rec)); // a tap latches Shift; the finger is gone
    CHECK(r.lit() == 1);
    rec.calls.clear();

    r.set_layout(&other, rec);
    CHECK((rec.calls == std::vector<std::string>{"k225-"}));
    CHECK(r.lit() == 0);
}

static void test_router_finger_cancel_on_held_modifier() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double sx, sy;
    center(l, 0, find_key(l, 0, kScanLShift), s, &sx, &sy);
    CHECK(r.finger_down(1, sx, sy, 0, rec)); // held, not yet a tap: no latch
    rec.calls.clear();
    const uint32_t gen = r.generation();

    CHECK(r.finger_cancel(1, rec));
    CHECK((rec.calls == std::vector<std::string>{"k225-"}));
    CHECK(r.lit() == 0); // it was never latched, so there is no latch to leave behind
    CHECK(!r.owns(1));
    CHECK(r.generation() != gen);
}

// Legacy quirk, ported as-is from host/sdl/main.cpp's g_keypad_fingers map:
// a key's down/up events are not reference-counted across the fingers that
// land on it. Two fingers on the same key each drive their own down event,
// and either one lifting — not just the last one — releases the key; the
// other finger's later lift releases it again.
static void test_router_two_fingers_same_key_first_lift_releases() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    double ax, ay;
    center(l, 0, find_key(l, 0, kScanA), s, &ax, &ay);
    CHECK(r.finger_down(1, ax, ay, 0, rec));
    CHECK(r.finger_down(2, ax, ay, 0, rec));
    CHECK((rec.calls == std::vector<std::string>{"k4+", "tap", "k4+", "tap"}));
    rec.calls.clear();

    CHECK(r.finger_up(1, 10, rec)); // the first lift already releases the key
    CHECK((rec.calls == std::vector<std::string>{"k4-"}));
    CHECK(r.owns(2)); // the second finger is still tracked, sitting on an already-released key
    rec.calls.clear();

    CHECK(r.finger_up(2, 20, rec)); // its own lift releases the key again
    CHECK((rec.calls == std::vector<std::string>{"k4-"}));
}

static void test_router_set_layout_null_disables_hit_testing() {
    Router r;
    Rec rec;
    r.set_screen(screen(1180, 820, 1.0));
    r.set_layout(nullptr, rec);
    CHECK(r.layout() == nullptr);
    CHECK(!r.finger_down(1, 10, 10, 0, rec));
    CHECK(rec.calls.empty());
}

static void test_router_state_out_of_range_is_zero() {
    Router r;
    const ControlState &cs = r.state(5, 5);
    CHECK(!cs.pressed && cs.knob_x == 0 && cs.knob_y == 0 && cs.hat == 0);
}

// --- Vpad: stick/dpad math, source merge, edge queue -----------------------

static void test_stick_output() {
    float x, y;
    stick_output(0, 0, 100, 0.15, &x, &y);
    CHECK(x == 0 && y == 0);

    stick_output(10, 0, 100, 0.15, &x, &y); // 0.1 * radius, inside the deadzone
    CHECK(x == 0 && y == 0);

    stick_output(100, 0, 100, 0.15, &x, &y);
    CHECK(fabs(x - 1.0) < 0.001 && fabs(y) < 0.001);

    stick_output(200, 0, 100, 0.15, &x, &y); // beyond the radius, clamped
    CHECK(fabs(x - 1.0) < 0.001 && fabs(y) < 0.001);

    stick_output(57.5, 0, 100, 0.15, &x, &y); // 0.575 * radius
    CHECK(fabs(x - 0.5) < 0.01 && fabs(y) < 0.01);

    stick_output(100, 100, 100, 0.15, &x, &y); // diagonal: magnitude 1
    CHECK(fabs(std::hypot(double(x), double(y)) - 1.0) < 0.001);
}

static void test_dpad_hat() {
    CHECK(dpad_hat(0, -60, 60) == kHatUp);
    CHECK(dpad_hat(60, 60, 60) == (kHatRight | kHatDown));
    CHECK(dpad_hat(6, 0, 60) == 0); // 0.1 * half, inside the neutral radius
}

static void test_pad_state_merge() {
    PadState a, b;
    a.buttons = kPadCross;
    b.buttons = kPadCircle;
    const PadState m1 = merge(a, b);
    CHECK(m1.buttons == (kPadCross | kPadCircle));

    a = PadState();
    b = PadState();
    a.lx = 0.2f;
    b.lx = -0.7f;
    CHECK(merge(a, b).lx == -0.7f); // larger magnitude wins
}

static void test_vpad_edges() {
    Vpad v;
    PadState cross;
    cross.buttons = kPadCross;
    v.set_source(kPadSourceTouch, cross);
    CHECK(v.packet() != 0);
    PadEdge e{};
    CHECK(v.next_edge(0, &e));
    CHECK(e.kind == 0 && e.index == 0 && e.value == 1);
    const uint32_t after_first = e.sequence;

    // Setting the same state again adds no edge.
    v.set_source(kPadSourceTouch, cross);
    PadEdge e2{};
    CHECK(!v.next_edge(after_first, &e2));

    // A controller source holding cross while the touch source releases it:
    // the merged state still has cross, and no edge is added.
    v.set_source(kPadSourceController, cross);
    v.set_source(kPadSourceTouch, PadState());
    PadEdge e3{};
    CHECK(!v.next_edge(after_first, &e3));
    CHECK(v.state().buttons == kPadCross);

    // 300 changes overflow the 256-entry ring; the oldest surviving edge is
    // number 45 (300 - 256 + 1).
    Vpad v2;
    for (int i = 0; i < 300; ++i) {
        PadState toggled;
        toggled.buttons = (i % 2 == 0) ? kPadCross : 0;
        v2.set_source(kPadSourceTouch, toggled);
    }
    PadEdge e4{};
    CHECK(v2.next_edge(0, &e4));
    CHECK(e4.sequence > 44);
}

// to_host's rounding: lx -1 hits the int16 minimum exactly (-1 * 32767);
// ly 0.5 rounds 16383.5 up to 16384; l2 (a byte-scaled trigger) hits 255.
static void test_to_host_scales_axes() {
    PadState s;
    s.lx = -1.0f;
    s.ly = 0.5f;
    s.l2 = 1.0f;
    const HostPadState h = to_host(s);
    CHECK(h.lx == -32767);
    CHECK(h.ly == 16384);
    CHECK(h.l2 == 255);
}

// A layout with a cross button, an l2 button, a dpad, a floating left stick
// and a fixed right stick, all in one non-grid group, at scale 1 so rects
// match points exactly: cross {0,0,60,60}, l2 {100,0,60,60}, dpad
// {200,0,120,120} (centre 260,60, half 60), floating stick {400,0,200,200}
// (centre 500,100), fixed stick {650,0,200,200} (centre 750,100). Both
// sticks have radius 100 (points; the zone is independent of it).
static Layout pad_layout() {
    Layout l;
    l.groups.resize(1);
    Group &g = l.groups[0];
    g.id = "pad";

    Control cross;
    cross.kind = Kind::Button;
    cross.button = PadButton::Cross;
    cross.anchor = Anchor::TopLeft;
    cross.x = 0;
    cross.y = 0;
    cross.w = cross.h = 60;
    g.controls.push_back(cross);

    Control l2;
    l2.kind = Kind::Button;
    l2.button = PadButton::L2;
    l2.anchor = Anchor::TopLeft;
    l2.x = 100;
    l2.y = 0;
    l2.w = l2.h = 60;
    g.controls.push_back(l2);

    Control dpad;
    dpad.kind = Kind::Dpad;
    dpad.anchor = Anchor::TopLeft;
    dpad.x = 200;
    dpad.y = 0;
    dpad.w = dpad.h = 120;
    g.controls.push_back(dpad);

    Control stick;
    stick.kind = Kind::Stick;
    stick.stick = 0;
    stick.floating = true;
    stick.deadzone = 0.15;
    stick.radius = 100;
    stick.anchor = Anchor::TopLeft;
    stick.x = 400;
    stick.y = 0;
    stick.w = stick.h = 200;
    g.controls.push_back(stick);

    Control fixed_stick;
    fixed_stick.kind = Kind::Stick;
    fixed_stick.stick = 1;
    fixed_stick.floating = false;
    fixed_stick.deadzone = 0.15;
    fixed_stick.radius = 100;
    fixed_stick.anchor = Anchor::TopLeft;
    fixed_stick.x = 650;
    fixed_stick.y = 0;
    fixed_stick.w = fixed_stick.h = 200;
    g.controls.push_back(fixed_stick);

    return l;
}

static void test_router_pad_buttons() {
    Layout l = pad_layout();
    const Screen s = screen(2000, 1000, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    CHECK(r.finger_down(1, 30, 30, 0, rec)); // cross
    CHECK(r.pad().buttons == kPadCross);

    CHECK(r.finger_down(2, 130, 30, 0, rec)); // l2
    CHECK(r.pad().l2 == 1.0f);

    // Two fingers on cross: pad() and the drawn `pressed` stay set while
    // either one still holds it.
    CHECK(r.finger_down(5, 40, 40, 0, rec));
    CHECK((r.pad().buttons & kPadCross) != 0);
    CHECK(r.finger_up(1, 10, rec));
    CHECK((r.pad().buttons & kPadCross) != 0); // finger 5 still holds it
    CHECK(r.state(0, 0).pressed);
    CHECK(r.finger_up(5, 10, rec));
    CHECK((r.pad().buttons & kPadCross) == 0);
    CHECK(!r.state(0, 0).pressed);

    CHECK(r.finger_up(2, 10, rec));
    CHECK((r.pad() == PadState()));
}

static void test_router_pad_stick_dpad_and_cancel() {
    Layout l = pad_layout();
    const Screen s = screen(2000, 1000, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    // A floating stick's base is the off-centre finger position exactly (no
    // clamp to the centre), and the knob starts at 0; a further motion by
    // +radius saturates lx.
    CHECK(r.finger_down(1, 590, 100, 0, rec));
    CHECK(r.state(0, 3).base_x == 590 && r.state(0, 3).base_y == 100);
    CHECK(r.state(0, 3).knob_x == 0 && r.state(0, 3).knob_y == 0);
    CHECK(r.finger_motion(1, 690, 100, 0, rec));
    CHECK(r.pad().lx == 1.0f);
    CHECK(r.state(0, 3).knob_x == 1.0);

    // A fixed stick's base is always its rect centre (750, 100): an
    // off-centre down deflects it immediately.
    CHECK(r.finger_down(6, 830, 100, 0, rec)); // 80pt right of centre
    CHECK(r.state(0, 4).base_x == 750 && r.state(0, 4).base_y == 100);
    CHECK(r.pad().rx > 0.0f);
    CHECK(r.finger_up(6, 10, rec));

    // Dpad motion from up to right changes the hat without a lift.
    CHECK(r.finger_down(2, 260, 0, 0, rec));
    CHECK(r.pad().hat == kHatUp);
    CHECK(r.finger_motion(2, 320, 60, 0, rec));
    CHECK(r.pad().hat == kHatRight);

    // Lifting clears everything; a stick's base returns to its rect centre
    // (not (0, 0)).
    CHECK(r.finger_up(1, 10, rec));
    CHECK(r.finger_up(2, 10, rec));
    CHECK((r.pad() == PadState()));
    CHECK(r.state(0, 3).knob_x == 0.0 && r.state(0, 3).base_x == 500 &&
          r.state(0, 3).base_y == 100);

    // cancel_all clears everything, including a mid-drag stick.
    CHECK(r.finger_down(3, 30, 30, 0, rec));   // cross
    CHECK(r.finger_down(4, 590, 100, 0, rec)); // stick
    CHECK(r.finger_motion(4, 690, 100, 0, rec));
    CHECK(r.pad().buttons == kPadCross);
    r.cancel_all(rec);
    CHECK((r.pad() == PadState()));
    CHECK(r.state(0, 3).knob_x == 0.0 && r.state(0, 3).base_x == 500 &&
          r.state(0, 3).base_y == 100);
}

// A stick or dpad has a single owner: a second finger claimed on top of it
// changes nothing and taps nothing, and its own lift does nothing either.
static void test_router_second_finger_on_stick_or_dpad_is_inert() {
    Layout l = pad_layout();
    const Screen s = screen(2000, 1000, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    CHECK(r.finger_down(1, 590, 100, 0, rec)); // primary, floating stick
    const size_t calls_after_primary = rec.calls.size();
    CHECK(r.finger_down(2, 410, 20, 0, rec));       // second finger, same stick's zone
    CHECK(rec.calls.size() == calls_after_primary); // no tap for the second finger
    CHECK(r.state(0, 3).base_x == 590);             // untouched by the second finger
    CHECK(r.finger_up(2, 10, rec));                 // its lift does nothing
    CHECK(r.state(0, 3).base_x == 590);
    CHECK(r.finger_motion(1, 690, 100, 0, rec)); // the owner still drives it
    CHECK(r.pad().lx == 1.0f);

    CHECK(r.finger_down(3, 260, 0, 0, rec)); // primary, dpad: up
    CHECK(r.pad().hat == kHatUp);
    CHECK(r.finger_down(4, 300, 30, 0, rec)); // second finger, same dpad's zone
    CHECK(r.pad().hat == kHatUp);             // unchanged by the second finger
    CHECK(r.finger_up(4, 10, rec));
    CHECK(r.pad().hat == kHatUp);
    CHECK(r.finger_motion(3, 320, 60, 0, rec)); // the owner still drives it
    CHECK(r.pad().hat == kHatRight);
}

// generation() bumps on a stick/dpad motion only when the hat or knob
// actually changed, not on every finger_motion call.
static void test_router_generation_bumps_only_on_hat_or_knob_change() {
    Layout l = pad_layout();
    const Screen s = screen(2000, 1000, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);

    CHECK(r.finger_down(1, 590, 100, 0, rec)); // floating stick, knob at 0
    const uint32_t gen0 = r.generation();
    CHECK(r.finger_motion(1, 590, 100, 0, rec)); // no motion at all
    CHECK(r.generation() == gen0);
    CHECK(r.finger_motion(1, 690, 100, 0, rec)); // now the knob moves
    CHECK(r.generation() != gen0);

    CHECK(r.finger_down(2, 260, 0, 0, rec)); // dpad: up
    const uint32_t gen1 = r.generation();
    CHECK(r.finger_motion(2, 260, -10, 0, rec)); // still "up": same hat
    CHECK(r.generation() == gen1);
    CHECK(r.finger_motion(2, 320, 60, 0, rec)); // now "right": hat changes
    CHECK(r.generation() != gen1);
}

// --- Overlay view: what the presenter draws -------------------------------

static int count_kind(const ControlsView &v, Kind k) {
    int n = 0;
    for (const DrawControl &d : v.controls)
        n += d.kind == k;
    return n;
}

// The keys built-in on an iPad-sized drawable: every key and both tabs, the
// latched Shift lit, and a backdrop behind each visible half.
static void test_make_view_keys() {
    Layout l = keys_layout();
    const Screen s = screen(2360, 1640, 2.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    double x, y;
    center(l, 0, find_key(l, 0, kScanLShift), s, &x, &y);
    CHECK(r.finger_down(1, x, y, 0, rec));
    CHECK(r.finger_up(1, 100ull * 1000000ull, rec)); // a short tap latches Shift
    CHECK(r.lit() == 1);

    ControlsView v = make_view(l, r, s, 1.0);
    CHECK(v.dw == 2360 && v.dh == 1640);
    const int left_keys = int(l.groups[0].controls.size());
    CHECK(left_keys + int(l.groups[1].controls.size()) == 77);
    CHECK(count_kind(v, Kind::Key) == 77);
    CHECK(count_kind(v, Kind::Toggle) == 2);
    int lit = 0;
    for (const DrawControl &d : v.controls)
        if (d.lit) {
            ++lit;
            CHECK(d.label == "Shift");
        }
    CHECK(lit == 1);
    CHECK(v.backdrops.size() == 2);
    if (v.backdrops.size() == 2)
        for (int g = 0; g < 2; ++g) {
            const Rect a = v.backdrops[g], b = group_rect(l, g, s);
            CHECK(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
        }
    for (const DrawControl &d : v.controls)
        if (d.kind == Kind::Toggle)
            CHECK(d.group_visible && d.label == "HIDE" && d.label_off == "KEYS");

    // The revision follows what is drawn.
    const uint64_t rev = v.revision;
    CHECK(make_view(l, r, s, 1.0).revision == rev);
    CHECK(make_view(l, r, s, 0.5).revision != rev);

    // Hide the left half: its keys go, its tab stays and reads KEYS.
    l.groups[0].visible = false;
    v = make_view(l, r, s, 1.0);
    CHECK(v.revision != rev);
    CHECK(count_kind(v, Kind::Key) == 77 - left_keys);
    CHECK(count_kind(v, Kind::Toggle) == 2);
    CHECK(v.backdrops.size() == 1);
    int hidden_tabs = 0;
    for (const DrawControl &d : v.controls)
        if (d.kind == Kind::Toggle && !d.group_visible)
            ++hidden_tabs;
    CHECK(hidden_tabs == 1);
    CHECK(v.controls.size() == size_t(77 - left_keys + 2));
}

// At the default size the drawn rects are exactly the old keypad's.
static void test_make_view_matches_the_old_keypad() {
    Layout l = keys_layout();
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    for (double sc : {1.0, 2.0}) {
        const Screen s = screen(int(1180 * sc), int(820 * sc), sc);
        r.set_screen(s);
        for (int shown = 1; shown >= 0; --shown) {
            l.groups[0].visible = l.groups[1].visible = shown != 0;
            const ControlsView v = make_view(l, r, s, 1.0);
            size_t at = 0;
            for (int side = 0; side < 2; ++side) {
                const KeypadRect half =
                    legacy_keypad_half_rect(KeypadSide(side), 1, sc, s.dw, s.dh);
                if (shown) {
                    int n = 0;
                    const KeypadKey *keys = legacy_keypad_keys(KeypadSide(side), &n);
                    for (int i = 0; i < n && at < v.controls.size(); ++i, ++at) {
                        const KeypadRect old =
                            legacy_keypad_key_rect(KeypadSide(side), keys[i], 1, sc, s.dw, s.dh);
                        const Rect now = v.controls[at].rect;
                        CHECK(now.x == old.x && now.y == old.y && now.w == old.w && now.h == old.h);
                        CHECK(v.controls[at].label == keys[i].label);
                    }
                    CHECK(v.backdrops.size() == 2 && v.backdrops[side].x == half.x &&
                          v.backdrops[side].y == half.y && v.backdrops[side].w == half.w &&
                          v.backdrops[side].h == half.h);
                } else {
                    CHECK(v.backdrops.empty());
                }
            }
            for (int side = 0; side < 2 && at < v.controls.size(); ++side, ++at) {
                const DrawControl &tab = v.controls[at];
                const KeypadRect old =
                    legacy_keypad_tab_rect(KeypadSide(side), shown != 0, 1, sc, s.dw, s.dh);
                CHECK(tab.kind == Kind::Toggle);
                CHECK(tab.rect.x == old.x && tab.rect.y == old.y && tab.rect.w == old.w &&
                      tab.rect.h == old.h);
                CHECK(tab.group_visible == (shown != 0));
                CHECK(tab.label == "HIDE" && tab.label_off == "KEYS");
            }
            CHECK(at == v.controls.size());
        }
    }
}

// A plain key's press draws nothing new, so the revision (and the raster)
// stays; a latched modifier lights, so it changes.
static void test_make_view_revision_ignores_undrawn_press() {
    Layout l = keys_layout();
    const Screen s = screen(2360, 1640, 2.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    const uint64_t rev = make_view(l, r, s, 1.0).revision;
    double x, y;
    center(l, 1, find_key(l, 1, kScanSpace), s, &x, &y);
    CHECK(r.finger_down(1, x, y, 0, rec));
    CHECK(make_view(l, r, s, 1.0).revision == rev);
    CHECK(r.finger_up(1, 10, rec));
    CHECK(make_view(l, r, s, 1.0).revision == rev);
    center(l, 0, find_key(l, 0, kScanLShift), s, &x, &y);
    CHECK(r.finger_down(2, x, y, 20, rec));
    CHECK(make_view(l, r, s, 1.0).revision == rev); // held, not lit yet
    CHECK(r.finger_up(2, 100ull * 1000000ull, rec));
    CHECK(r.lit() == 1);
    CHECK(make_view(l, r, s, 1.0).revision != rev);
}

// A phone form with no layout of that name anywhere uses the tablet one; a
// form-agnostic or phone file still wins.
static void test_tablet_fallback() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/controls-fallback-test-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    const std::filesystem::path root = dir;
    std::string tiny_json = kTinyLayout;
    write_file(root / "profile" / "mine.tablet.json", tiny_json);
    write_file(root / "profile" / "any.json", tiny_json);
    LayoutStore store;
    store.set_dirs((root / "profile").string(), "");

    Layout l;
    std::string problem;
    bool fell_back = true;
    CHECK(load_with_tablet_fallback(store, "keys", Form::Tablet, &l, &problem, &fell_back));
    CHECK(!fell_back);
    CHECK(load_with_tablet_fallback(store, "keys", Form::PhoneLandscape, &l, &problem, &fell_back));
    CHECK(fell_back && l.name == "keys");
    CHECK(load_with_tablet_fallback(store, "mine", Form::PhonePortrait, &l, &problem, &fell_back));
    CHECK(fell_back);
    CHECK(load_with_tablet_fallback(store, "any", Form::PhonePortrait, &l, &problem, &fell_back));
    CHECK(!fell_back);
    CHECK(!load_with_tablet_fallback(store, "nope", Form::PhonePortrait, &l, &problem, &fell_back));
    CHECK(!fell_back && problem.empty());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// A player-edited layout can hand a shape a huge, negative or NaN
// coordinate; fill_shape must clamp before converting to int rather than
// invoke undefined behaviour, and must simply draw nothing.
static void test_raster_shape_bounds_are_clamped() {
    std::vector<uint8_t> px(64 * 64 * 4, 0);
    Canvas c(px, 64, 64);
    c.disc(1e12, -1e12, 1e12, flat_paint(Rgba{255, 255, 255, 255}));
    for (uint8_t b : px)
        CHECK(b == 0);

    c.disc(32, 32, std::nan(""), flat_paint(Rgba{255, 255, 255, 255}));
    for (uint8_t b : px)
        CHECK(b == 0);
}

// parse_mapped changes only the entries it names, and leaves *table alone on
// any error; write_mapped round-trips through parse_mapped.
static void test_binding_parse_mapped() {
    MappedTable t;
    std::string err;
    CHECK(parse_mapped("cross=key:Space;left_stick=cursor", &t, &err));
    CHECK(t.buttons[int(PadButton::Cross)].type == Target::Key);
    CHECK(t.buttons[int(PadButton::Cross)].value == kScanSpace);
    CHECK(t.left == StickMode::Cursor);
    CHECK(t.dpad == StickMode::Arrows); // untouched
    // untouched: MappedTable's own default (tools/game_config.py's MAPPED_DEFAULTS)
    CHECK(t.buttons[int(PadButton::Circle)].type == Target::Mouse &&
          t.buttons[int(PadButton::Circle)].value == 1);

    MappedTable before = t;
    CHECK(!parse_mapped("cross=key:Nope", &t, &err));
    CHECK(write_mapped(t) == write_mapped(before)); // unchanged on error
    CHECK(!parse_mapped("bogus=none", &t, &err));
    CHECK(write_mapped(t) == write_mapped(before));

    MappedTable w;
    w.buttons[int(PadButton::Cross)] = Target{Target::Mouse, 0, ""};
    w.buttons[int(PadButton::Ps)] = Target{Target::Action, 0, "settings"};
    w.buttons[int(PadButton::L1)] = Target{Target::Wheel, 1, ""};
    w.cursor_speed = 450;
    const std::string text = write_mapped(w);
    MappedTable w2;
    CHECK(parse_mapped(text, &w2, &err));
    CHECK(write_mapped(w2) == text);
}

// Cross mapped to mouse_left (the built-in default): a press places the
// cursor and clicks; a release only lifts the button.
static void test_binding_button_mouse() {
    MappedTable t;
    t.buttons[int(PadButton::Cross)] = Target{Target::Mouse, 0, ""};
    Binding b;
    b.set_table(t);
    b.set_bounds(1000, 800);
    b.set_cursor(100, 100);
    PadState p;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;
    p.buttons = kPadCross;
    b.tick(p, 0, &out, &actions);
    CHECK(out.size() == 2);
    CHECK(out[0].kind == TouchAction::Motion && out[0].place && out[0].x == 100 && out[0].y == 100);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down);
    out.clear();
    p.buttons = 0;
    b.tick(p, 10ull * 1000000ull, &out, &actions);
    CHECK(out.size() == 1);
    CHECK(out[0].kind == TouchAction::Button && out[0].button == 0 && !out[0].down);
}

// Right stick on Cursor (the default), speed 900, bounds 1000x800: the
// cursor moves by cursor_speed * |v|^2 * dt each tick and clamps at the edge.
static void test_binding_cursor_stick() {
    MappedTable t; // right_stick=cursor, cursor_speed=900 by default
    Binding b;
    b.set_table(t);
    b.set_bounds(1000, 800);
    b.set_cursor(0, 0);
    PadState p;
    p.rx = 1;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;
    uint64_t now = 0;
    for (int i = 0; i < 3; ++i) {
        out.clear();
        b.tick(p, now, &out, &actions);
        now += 10ull * 1000000ull;
    }
    CHECK(std::fabs(b.cursor_x() - 18.0) <= 0.5);

    Binding b2;
    b2.set_table(t);
    b2.set_bounds(1000, 800);
    b2.set_cursor(0, 0);
    p.rx = 0.5f;
    now = 0;
    for (int i = 0; i < 3; ++i) {
        out.clear();
        b2.tick(p, now, &out, &actions);
        now += 10ull * 1000000ull;
    }
    CHECK(std::fabs(b2.cursor_x() - 4.5) <= 0.3);

    Binding b3;
    b3.set_table(t);
    b3.set_bounds(1000, 800);
    b3.set_cursor(0, 0);
    p.rx = 1;
    now = 0;
    for (int i = 0; i < 200; ++i) {
        out.clear();
        b3.tick(p, now, &out, &actions);
        now += 10ull * 1000000ull;
    }
    CHECK(b3.cursor_x() == 999.0); // clamped to w - 1: no point outside the window
}

// Left stick on Arrows (the default): per-axis hysteresis, 0.5 to press and
// 0.35 to release.
static void test_binding_arrows_stick() {
    MappedTable t; // left_stick=arrows by default
    Binding b;
    b.set_table(t);
    PadState p;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;
    p.lx = 0.6f;
    b.tick(p, 0, &out, &actions);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Key && out[0].scancode == kScanRight &&
          out[0].down);
    out.clear();
    p.lx = 0.4f;
    b.tick(p, 1, &out, &actions);
    CHECK(out.empty());
    out.clear();
    p.lx = 0.3f;
    b.tick(p, 2, &out, &actions);
    CHECK(out.size() == 1 && out[0].scancode == kScanRight && !out[0].down);
    out.clear();
    p.lx = 0;
    p.ly = -0.9f;
    b.tick(p, 3, &out, &actions);
    CHECK(out.size() == 1 && out[0].scancode == kScanUp && out[0].down);
}

// l1 = wheel_up: one Wheel(+1) per press, nothing on release.
static void test_binding_wheel_button() {
    MappedTable t;
    std::string err;
    CHECK(parse_mapped("l1=wheel_up", &t, &err));
    Binding b;
    b.set_table(t);
    PadState p;
    p.buttons = kPadL1;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;
    b.tick(p, 0, &out, &actions);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Wheel && out[0].wheel == 1);
    out.clear();
    p.buttons = 0;
    b.tick(p, 1, &out, &actions);
    CHECK(out.empty());
}

// ps = action:settings: the action name once per press, nothing on release.
static void test_binding_action_button() {
    MappedTable t;
    std::string err;
    CHECK(parse_mapped("ps=action:settings", &t, &err));
    Binding b;
    b.set_table(t);
    PadState p;
    p.buttons = kPadPs;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;
    b.tick(p, 0, &out, &actions);
    CHECK(actions.size() == 1 && actions[0] == "settings");
    actions.clear();
    b.tick(p, 1, &out, &actions); // held: no repeat
    CHECK(actions.empty());
    p.buttons = 0;
    b.tick(p, 2, &out, &actions); // release: nothing
    CHECK(actions.empty());
}

// Scroll mode taps the arrow key once per whole kTouchPanStep accumulated.
static void test_binding_scroll_stick() {
    MappedTable t;
    std::string err;
    CHECK(parse_mapped("left_stick=scroll", &t, &err));
    Binding b;
    b.set_table(t);
    PadState p;
    p.lx = 1;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;
    uint64_t now = 0;
    int right_taps = 0;
    for (int i = 0; i < 100; ++i) {
        out.clear();
        b.tick(p, now, &out, &actions);
        for (const TouchAction &a : out)
            if (a.kind == TouchAction::Key && a.scancode == kScanRight && a.down)
                ++right_taps;
        now += 10ull * 1000000ull;
    }
    CHECK(right_taps >= 18 && right_taps <= 22);
}

// release_all lets go of every key and mouse button the binding is holding.
static void test_binding_release_all() {
    MappedTable t; // left_stick=arrows by default
    t.buttons[int(PadButton::Cross)] = Target{Target::Mouse, 0, ""};
    Binding b;
    b.set_table(t);
    b.set_cursor(0, 0);
    PadState p;
    p.buttons = kPadCross;
    p.lx = 1;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;
    b.tick(p, 0, &out, &actions);
    out.clear();
    b.release_all(&out);
    bool key_right_up = false, mouse_left_up = false;
    for (const TouchAction &a : out) {
        if (a.kind == TouchAction::Key && a.scancode == kScanRight && !a.down)
            key_right_up = true;
        if (a.kind == TouchAction::Button && a.button == 0 && !a.down)
            mouse_left_up = true;
    }
    CHECK(key_right_up && mouse_left_up);
}

// The cursor starts unknown; the first set_bounds (no set_cursor yet, as
// happens the first time the host reports the window's size) centres it, so
// a button press with no prior pointer event still lands somewhere sane
// rather than at (0, 0). Bounds 1000x800 centre at (499.5, 399.5), the
// midpoint of [0, 999] x [0, 799].
static void test_binding_cursor_starts_centred() {
    MappedTable t; // cross=mouse_left by default
    Binding b;
    b.set_table(t);
    b.set_bounds(1000, 800); // no set_cursor call at all
    PadState p;
    p.buttons = kPadCross;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;
    b.tick(p, 0, &out, &actions);
    CHECK(out.size() == 2);
    CHECK(out[0].kind == TouchAction::Motion && out[0].x == 499.5 && out[0].y == 399.5);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down);
}

// left_stick=arrows and dpad=arrows both alias onto the Right key (the
// defaults do exactly this): held_keys_ is ref-counted, so the key stays
// down until every source holding it lets go, not just the first to release.
static void test_binding_aliased_key_is_ref_counted() {
    MappedTable t; // left_stick=arrows, dpad=arrows by default
    Binding b;
    b.set_table(t);
    PadState p;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;

    p.lx = 1; // stick right: one Down
    b.tick(p, 0, &out, &actions);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Key && out[0].scancode == kScanRight &&
          out[0].down);

    out.clear();
    p.hat = kHatRight; // dpad-right too, same key: already held, no second Down
    b.tick(p, 1, &out, &actions);
    CHECK(out.empty());

    out.clear();
    p.hat = 0; // dpad-right released: the stick still holds it, no Up
    b.tick(p, 2, &out, &actions);
    CHECK(out.empty());

    out.clear();
    p.lx = 0; // the stick returns to neutral: now it releases
    b.tick(p, 3, &out, &actions);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Key && out[0].scancode == kScanRight &&
          !out[0].down);
}

// Two pad buttons mapped to mouse_left: the mouse button stays down until
// both are released, not just the first.
static void test_binding_aliased_mouse_button_is_ref_counted() {
    MappedTable t;
    t.buttons[int(PadButton::Cross)] = Target{Target::Mouse, 0, ""};
    t.buttons[int(PadButton::Circle)] = Target{Target::Mouse, 0, ""};
    Binding b;
    b.set_table(t);
    b.set_bounds(1000, 800);
    b.set_cursor(50, 50);
    PadState p;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;

    p.buttons = kPadCross;
    b.tick(p, 0, &out, &actions);
    CHECK(out.size() == 2); // Motion + Button down

    out.clear();
    p.buttons |= kPadCircle; // already held: no second Motion/Button
    b.tick(p, 1, &out, &actions);
    CHECK(out.empty());

    out.clear();
    p.buttons &= ~kPadCross; // circle still holds it: no Up
    b.tick(p, 2, &out, &actions);
    CHECK(out.empty());

    out.clear();
    p.buttons &= ~kPadCircle; // both released: now it lifts
    b.tick(p, 3, &out, &actions);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && out[0].button == 0 &&
          !out[0].down);
}

// After release_all, a button release_all forced up but the pad still holds
// does not re-fire until the pad itself reports it released: a one-shot
// Action target must not repeat every tick just because release_all reset
// the internal edge state.
static void test_binding_release_all_latches_still_held_buttons() {
    MappedTable t; // ps=action:settings by default
    Binding b;
    b.set_table(t);
    PadState p;
    p.buttons = kPadPs;
    std::vector<TouchAction> out;
    std::vector<std::string> actions;

    b.tick(p, 0, &out, &actions);
    CHECK(actions.size() == 1 && actions[0] == "settings");

    out.clear();
    b.release_all(&out);

    actions.clear();
    b.tick(p, 1, &out, &actions); // ps is still held: must not re-fire
    CHECK(actions.empty());
    b.tick(p, 2, &out, &actions); // still held on a later tick: still nothing
    CHECK(actions.empty());

    p.buttons = 0;
    b.tick(p, 3, &out, &actions); // now the pad reports it released
    CHECK(actions.empty());

    actions.clear();
    p.buttons = kPadPs; // a fresh press fires normally again
    b.tick(p, 4, &out, &actions);
    CHECK(actions.size() == 1 && actions[0] == "settings");
}

// The rumble decision's whole truth table: a connected controller always
// wins (it has its own motors), the device motor only stands in when there
// is no controller, and neither leaves nothing to rumble.
static void test_rumble_sink_truth_table() {
    CHECK(rumble_sink(true, true) == RumbleSink::Controller);
    CHECK(rumble_sink(true, false) == RumbleSink::Controller);
    CHECK(rumble_sink(false, true) == RumbleSink::Device);
    CHECK(rumble_sink(false, false) == RumbleSink::None);
}

// Portrait on a phone: the space below the game image is the controls area.
// Landscape, or a game image that fills the drawable, has none.
static void test_controls_area_below_the_game() {
    const Rect game{0, 141, 1170, 878};
    const Rect a = controls_area_below(1170, 2532, game, 102);
    CHECK(a.x == 0 && a.y == 141 + 878 && a.w == 1170 && a.h == 2532 - (141 + 878) - 102);
    CHECK(controls_area_below(2532, 1170, Rect{0, 0, 2532, 1170}, 0).empty());
    CHECK(controls_area_below(1170, 2532, Rect{0, 0, 1170, 2532}, 102).empty());
    CHECK(controls_area_below(1170, 2532, Rect{}, 102).empty());
}

// Every touch in the controls area belongs to the controls, even between
// them; a touch outside it that hits no control still goes to the game.
static void test_router_claims_the_controls_area() {
    Layout l = keys_layout();
    const Screen s = screen(1180, 820, 1.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    // A point no control covers, inside the claim area, and another outside it.
    CHECK(hit_test(l, s, 590, 20).group < 0);
    CHECK(hit_test(l, s, 590, 300).group < 0);
    CHECK(!r.finger_down(1, 590, 20, 0, rec));
    r.set_claim_area(Rect{0, 0, 1180, 100});
    CHECK(r.finger_down(2, 590, 20, 0, rec));
    CHECK(r.owns(2));
    CHECK(rec.calls.empty()); // claimed, and does nothing
    CHECK(r.finger_motion(2, 600, 30, 5, rec));
    CHECK(r.finger_up(2, 10, rec));
    CHECK(!r.owns(2));
    CHECK(rec.calls.empty());
    CHECK(!r.finger_down(3, 590, 300, 20, rec)); // outside the area: the game's
    CHECK(r.finger_down(4, 590, 20, 30, rec));
    CHECK(r.finger_cancel(4, rec));
    CHECK(r.finger_down(5, 590, 20, 40, rec));
    r.cancel_all(rec);
    CHECK(!r.owns(5));
    CHECK(rec.calls.empty());
    // A control inside the area still works as a control.
    double x, y;
    center(l, 1, find_key(l, 1, kScanSpace), s, &x, &y);
    r.set_claim_area(Rect{0, 0, 1180, 820});
    CHECK(r.finger_down(6, x, y, 50, rec));
    CHECK((rec.calls == std::vector<std::string>{"k44+", "tap"}));
    r.set_claim_area(Rect{});
    CHECK(!r.finger_down(7, 590, 20, 60, rec));
}

// make_view copies the controls area, and a new area is a new raster.
static void test_make_view_carries_the_controls_area() {
    Layout l = keys_layout();
    Screen s = screen(1170, 2532, 3.0);
    Router r;
    Rec rec;
    r.set_layout(&l, rec);
    r.set_screen(s);
    const uint64_t rev = make_view(l, r, s, 1.0).revision;
    s.controls_area = Rect{0, 1019, 1170, 1411};
    const ControlsView v = make_view(l, r, s, 1.0);
    CHECK(v.controls_area.x == 0 && v.controls_area.y == 1019 && v.controls_area.w == 1170 &&
          v.controls_area.h == 1411);
    CHECK(v.revision != rev);
}

// --- editor ------------------------------------------------------------------

// Stands in for the `pad` built-in (another track adds it): two floating
// sticks, a dpad, four face buttons in a diamond and two shoulders.
static const char *kEditorPadLayout = R"({
  "version": 1, "name": "pad",
  "groups": [
    {"id": "sticks", "controls": [
       {"kind": "stick", "stick": "left", "anchor": "bottom-left", "x": 40, "y": 40, "radius": 60},
       {"kind": "stick", "stick": "right", "anchor": "bottom-right", "x": 300, "y": 40, "radius": 60}]},
    {"id": "dpad", "controls": [
       {"kind": "dpad", "anchor": "bottom-left", "x": 60, "y": 200, "size": 140}]},
    {"id": "face", "controls": [
       {"kind": "button", "button": "cross", "anchor": "bottom-right", "x": 110, "y": 200, "size": 60},
       {"kind": "button", "button": "circle", "anchor": "bottom-right", "x": 40, "y": 270, "size": 60},
       {"kind": "button", "button": "square", "anchor": "bottom-right", "x": 180, "y": 270, "size": 60},
       {"kind": "button", "button": "triangle", "anchor": "bottom-right", "x": 110, "y": 340, "size": 60}]},
    {"id": "shoulders", "controls": [
       {"kind": "button", "button": "l1", "anchor": "top-left", "x": 40, "y": 60, "w": 100, "h": 50},
       {"kind": "button", "button": "r1", "anchor": "top-right", "x": 40, "y": 60, "w": 100, "h": 50}]}
  ]})";

static Screen editor_screen() {
    Screen s;
    s.dw = 2360;
    s.dh = 1640;
    s.scale = 2;
    s.safe = Rect{0, 0, 2360, 1640};
    return s;
}

static Layout editor_pad() {
    Layout l;
    std::string err;
    CHECK(parse_layout(kEditorPadLayout, &l, &err));
    return l;
}

static void tap(Editor &e, double x, double y, int64_t id = 1) {
    e.finger_down(id, x, y);
    e.finger_up(id);
}

static void tap_rect(Editor &e, const Rect &r) {
    tap(e, r.x + r.w / 2.0, r.y + r.h / 2.0);
}

static void tap_tool(Editor &e, Tool t) {
    for (const auto &item : e.toolbar())
        if (item.tool == t) {
            tap_rect(e, item.rect);
            return;
        }
    CHECK(!"tool not on the toolbar");
}

// Taps the picker row whose value is `value`, dragging the list up a row at
// a time until it is in view.
static bool pick(Editor &e, const std::string &value) {
    for (int step = 0; step < 400; ++step) {
        const auto &items = e.picker();
        if (items.empty())
            return false;
        for (const auto &it : items)
            if (it.value == value && it.rect.h >= 32 * 2) {
                tap_rect(e, it.rect);
                return true;
            }
        Rect box = e.picker_rect();
        double x = box.x + box.w / 2.0, y = box.y + box.h / 2.0;
        e.finger_down(9, x, y);
        e.finger_motion(9, x, y - 64);
        e.finger_up(9);
    }
    return false;
}

static Rect cross_rect(const Editor &e, const Screen &s) {
    return control_rect(e.layout(), 2, 0, s);
}

static void test_editor_open_and_toolbar() {
    Editor e;
    Screen s = editor_screen();
    CHECK(!e.is_open());
    e.set_screen(s);
    e.open(editor_pad(), Form::Tablet, MappedTable{}, false, true);
    CHECK(e.is_open());
    CHECK(e.snap());
    CHECK(e.layout().name == "pad");
    // Ruling: eight visible tools (the brief's nine counted Tool::None).
    const Tool order[] = {Tool::Add,  Tool::Delete, Tool::Bind,  Tool::Stick,
                          Tool::Snap, Tool::Layout, Tool::Reset, Tool::Done};
    CHECK(e.toolbar().size() == 8);
    for (size_t i = 0; i < e.toolbar().size() && i < 8; ++i) {
        const Rect &r = e.toolbar()[i].rect;
        CHECK(e.toolbar()[i].tool == order[i]);
        CHECK(e.toolbar()[i].label && *e.toolbar()[i].label);
        CHECK(r.w == 144 && r.h == 72);
        CHECK(r.y >= 0 && r.y + r.h <= 100);
        if (i > 0)
            CHECK(r.x == e.toolbar()[i - 1].rect.x + 144 + 12);
    }
    // Centred in the safe area.
    const Rect &first = e.toolbar().front().rect;
    const Rect &last = e.toolbar().back().rect;
    CHECK(std::abs((first.x - 0) - (2360 - (last.x + last.w))) <= 1);
    CHECK(e.picker().empty());
    CHECK(e.selected_group() == -1 && e.selected_control() == -1);

    // Portrait: the toolbar sits at the top of the controls area.
    Screen p;
    p.dw = 1170;
    p.dh = 2532;
    p.scale = 3;
    p.safe = Rect{0, 141, 1170, 2289};
    p.controls_area = Rect{0, 1500, 1170, 930};
    e.set_screen(p);
    CHECK(e.toolbar().front().rect.y >= 1500 && e.toolbar().front().rect.y < 1500 + 30);
    CHECK(e.toolbar().front().rect.x >= 0 &&
          e.toolbar().back().rect.x + e.toolbar().back().rect.w <= 1170);
}

static void test_editor_select_drag_and_reanchor() {
    Editor e;
    Screen s = editor_screen();
    e.set_screen(s);
    e.open(editor_pad(), Form::Tablet, MappedTable{}, false, true);
    Rect before = cross_rect(e, s);
    CHECK(before.x == 2020 && before.y == 1120 && before.w == 120);
    CHECK(e.layout().groups[2].controls[0].anchor == Anchor::BottomRight);

    uint32_t gen = e.generation();
    tap_rect(e, before);
    CHECK(e.selected_group() == 2 && e.selected_control() == 0);
    CHECK(e.generation() != gen);

    // Tapping empty space clears the selection.
    tap(e, 1180, 700);
    CHECK(e.selected_group() == -1 && e.selected_control() == -1);

    double cx = before.x + 60, cy = before.y + 60;
    e.finger_down(1, cx, cy);
    CHECK(e.selected_group() == 2 && e.selected_control() == 0);
    e.finger_motion(1, cx - 403, cy - 597);
    Rect moved = cross_rect(e, s);
    CHECK(std::abs(moved.x - (before.x - 400)) <= 12 && std::abs(moved.y - (before.y - 600)) <= 12);
    CHECK(moved.x % 20 == 0 && moved.y % 20 == 0);
    CHECK(!e.guides().empty());
    e.finger_up(1);
    // The centre (1680, 580) is in the right third and the middle third.
    CHECK(e.layout().groups[2].controls[0].anchor == Anchor::Right);
    Rect after = cross_rect(e, s);
    CHECK(after.x == moved.x && after.y == moved.y && after.w == moved.w && after.h == moved.h);
    CHECK(e.guides().empty());
    // Still in place on a different screen size's anchor maths: the offset is
    // measured from the right edge, centred vertically.
    const Control &c = e.layout().groups[2].controls[0];
    CHECK(std::lround(c.x * 2) == 2360 - 120 - after.x);
}

static void test_editor_snap_off_and_snap_to_control() {
    Editor e;
    Screen s = editor_screen();
    e.set_screen(s);
    e.open(editor_pad(), Form::Tablet, MappedTable{}, false, false);
    CHECK(!e.snap());
    Rect before = cross_rect(e, s);
    double cx = before.x + 60, cy = before.y + 60;
    e.finger_down(1, cx, cy);
    e.finger_motion(1, cx - 403, cy - 597);
    e.finger_up(1);
    Rect after = cross_rect(e, s);
    CHECK(after.x == before.x - 403 && after.y == before.y - 597);
    CHECK(e.guides().empty());

    // Snap back on: a drop within 6 pt of the circle's left edge lines up with it.
    tap_tool(e, Tool::Snap);
    CHECK(e.snap());
    Rect circle = control_rect(e.layout(), 2, 1, s);
    Rect cr = cross_rect(e, s);
    e.finger_down(1, cr.x + 60, cr.y + 60);
    // Put the cross's left edge 7 px right of the circle's left edge, well
    // below it so nothing else is near; 7 px is off the 20 px grid.
    double tx = circle.x + 7 + 60, ty = 1400 + 3 + 60;
    e.finger_motion(1, tx, ty);
    Rect snapped = cross_rect(e, s);
    CHECK(snapped.x == circle.x);
    bool vertical_guide = false;
    for (const Rect &g : e.guides())
        if (g.w == 1 && g.x == circle.x)
            vertical_guide = true;
    CHECK(vertical_guide);
    e.finger_up(1);
}

static void test_editor_pinch() {
    Editor e;
    Screen s = editor_screen();
    e.set_screen(s);
    e.open(editor_pad(), Form::Tablet, MappedTable{}, false, true);
    Rect r = cross_rect(e, s);
    double cx = r.x + 60, cy = r.y + 60;
    e.finger_down(1, cx, cy);
    e.finger_down(2, cx - 100, cy);
    e.finger_motion(2, cx - 200, cy);
    const Control &c = e.layout().groups[2].controls[0];
    CHECK(c.w == 120 && c.h == 120);
    // The centre stays put.
    Rect grown = cross_rect(e, s);
    CHECK(std::abs(grown.x + grown.w / 2 - cx) <= 2 && std::abs(grown.y + grown.h / 2 - cy) <= 2);
    e.finger_motion(2, cx - 1000, cy);
    CHECK(e.layout().groups[2].controls[0].w == 240);
    e.finger_motion(2, cx - 10, cy);
    CHECK(e.layout().groups[2].controls[0].w == 24);
    e.finger_up(2);
    e.finger_up(1);
    // The wheel resizes the selection too, in 2 pt steps.
    e.wheel(3);
    CHECK(e.layout().groups[2].controls[0].w > 24);
    CHECK(int(e.layout().groups[2].controls[0].w) % 2 == 0);

    // A stick's pinch scales its travel and zone together.
    Rect st = control_rect(e.layout(), 0, 0, s);
    double sx = st.x + st.w / 2.0, sy = st.y + st.h / 2.0;
    e.finger_down(1, sx, sy);
    e.finger_down(2, sx + 50, sy);
    e.finger_motion(2, sx + 75, sy);
    const Control &stick = e.layout().groups[0].controls[0];
    CHECK(stick.w == 180 && stick.h == 180 && stick.radius == 90);
    e.finger_up(2);
    e.finger_up(1);
}

static void test_editor_add_delete_bind() {
    Editor e;
    Screen s = editor_screen();
    e.set_screen(s);
    e.open(editor_pad(), Form::Tablet, MappedTable{}, false, true);
    size_t groups = e.layout().groups.size();
    tap_tool(e, Tool::Add);
    CHECK(!e.picker().empty());
    CHECK(e.picker().size() == 7);
    Rect box = e.picker_rect();
    CHECK(box.w == 400);
    CHECK(box.y >= e.toolbar().front().rect.y + e.toolbar().front().rect.h);
    CHECK(pick(e, "button"));
    CHECK(pick(e, "button:circle"));
    CHECK(e.picker().empty());
    CHECK(e.layout().groups.size() == groups + 1);
    const Group &custom = e.layout().groups.back();
    CHECK(custom.id == "custom" && custom.controls.size() == 1);
    CHECK(custom.controls[0].kind == Kind::Button &&
          custom.controls[0].button == PadButton::Circle);
    CHECK(e.selected_group() == int(groups) && e.selected_control() == 0);
    // Placed at the centre of the area.
    Rect nr = control_rect(e.layout(), int(groups), 0, s);
    CHECK(std::abs(nr.x + nr.w / 2 - 1180) <= 1 && std::abs(nr.y + nr.h / 2 - 820) <= 1);

    // A second add joins the same group.
    tap_tool(e, Tool::Add);
    CHECK(pick(e, "key"));
    CHECK(e.layout().groups.back().controls.size() == 2);
    CHECK(e.layout().groups.back().controls[1].kind == Kind::Key);
    // Bind the key.
    tap_tool(e, Tool::Bind);
    CHECK(pick(e, "Q"));
    CHECK(e.layout().groups.back().controls[1].scancode == kScanQ);
    tap_tool(e, Tool::Delete);
    CHECK(e.selected_group() == -1);
    CHECK(e.layout().groups.back().controls.size() == 1);
    tap_rect(e, control_rect(e.layout(), int(groups), 0, s));
    tap_tool(e, Tool::Delete);
    CHECK(e.layout().groups.size() == groups);
    for (const auto &g : e.layout().groups)
        CHECK(g.id != "custom");

    // Bind in mapped mode: cross -> key:Space.
    tap_rect(e, cross_rect(e, s));
    tap_tool(e, Tool::Bind);
    CHECK(pick(e, "key:Space"));
    const Target &t = e.mapped().buttons[int(PadButton::Cross)];
    CHECK(t.type == Target::Key && t.value == kScanSpace);
    // A stick binds to a stick mode.
    tap_rect(e, control_rect(e.layout(), 0, 1, s));
    tap_tool(e, Tool::Bind);
    CHECK(pick(e, "wasd"));
    CHECK(e.mapped().right == StickMode::Wasd);
    // Stick cycles floating/fixed and the dead zone.
    double dz = e.layout().groups[0].controls[1].deadzone;
    tap_tool(e, Tool::Stick);
    CHECK(e.layout().groups[0].controls[1].deadzone != dz ||
          !e.layout().groups[0].controls[1].floating);
    for (int i = 0; i < 5; ++i)
        tap_tool(e, Tool::Stick);
    CHECK(e.layout().groups[0].controls[1].floating);
    CHECK(e.layout().groups[0].controls[1].deadzone == dz);

    // Tapping outside an open picker just closes it.
    tap_tool(e, Tool::Add);
    CHECK(!e.picker().empty());
    tap(e, 1180, 1500);
    CHECK(e.picker().empty());
    CHECK(e.layout().groups.size() == groups);
}

static void test_editor_bind_native_button() {
    Editor e;
    Screen s = editor_screen();
    e.set_screen(s);
    e.open(editor_pad(), Form::Tablet, MappedTable{}, true, true);
    tap_rect(e, cross_rect(e, s));
    tap_tool(e, Tool::Bind);
    for (const auto &it : e.picker())
        CHECK(it.value.rfind("key:", 0) != 0);
    CHECK(pick(e, "start"));
    CHECK(e.layout().groups[2].controls[0].button == PadButton::Start);
    CHECK(e.mapped().buttons[int(PadButton::Cross)].type == Target::Mouse);
}

static void test_editor_grid_key_moves_its_group() {
    Editor e;
    Screen s = editor_screen();
    e.set_screen(s);
    Layout l;
    std::string err;
    CHECK(parse_layout(kTinyLayout, &l, &err));
    l.safe_inset = true;
    e.open(l, Form::Tablet, MappedTable{}, false, false);
    Rect box = group_rect(e.layout(), 0, s);
    Rect key = control_rect(e.layout(), 0, 0, s);
    e.finger_down(1, key.x + 5, key.y + 5);
    CHECK(e.selected_group() == 0 && e.selected_control() == 0);
    e.finger_motion(1, key.x + 5 - 1400, key.y + 5 - 700);
    e.finger_up(1);
    Rect moved = group_rect(e.layout(), 0, s);
    CHECK(moved.x == box.x - 1400 && moved.y == box.y - 700);
    CHECK(e.layout().groups[0].anchor == Anchor::Center);
    // Pinch scales the grid's key size, clamped to 24..60.
    Rect k = control_rect(e.layout(), 0, 0, s);
    e.finger_down(1, k.x + 5, k.y + 5);
    e.finger_down(2, k.x + 105, k.y + 5);
    e.finger_motion(2, k.x + 505, k.y + 5);
    CHECK(e.layout().groups[0].grid.key == 60);
    e.finger_up(2);
    e.finger_up(1);
    // A grid group survives losing its last control.
    tap_rect(e, control_rect(e.layout(), 0, 1, s));
    tap_tool(e, Tool::Delete);
    tap_rect(e, control_rect(e.layout(), 0, 0, s));
    tap_tool(e, Tool::Delete);
    CHECK(e.layout().groups.size() == 2 && e.layout().groups[0].controls.empty());
}

static void test_editor_done_reset_and_layout_names() {
    Editor e;
    Screen s = editor_screen();
    e.set_screen(s);
    e.set_names({"pad", "keys", "pad+keys", "pad copy"});
    e.open(editor_pad(), Form::Tablet, MappedTable{}, false, true);
    CHECK(!e.take_save());
    tap_tool(e, Tool::Done);
    CHECK(e.take_save());
    CHECK(!e.take_save());
    tap_tool(e, Tool::Reset);
    CHECK(e.take_reset());
    CHECK(!e.take_reset());
    CHECK(!e.take_cancel());

    // A built-in refuses a rename.
    std::string current;
    tap_tool(e, Tool::Layout);
    CHECK(pick(e, "rename"));
    CHECK(!e.take_rename(&current));
    CHECK(e.layout().name == "pad");

    // Duplicate picks the first free "<name> copy N".
    tap_tool(e, Tool::Layout);
    CHECK(pick(e, "duplicate"));
    CHECK(e.layout().name == "pad copy 2");

    tap_tool(e, Tool::Layout);
    CHECK(pick(e, "rename"));
    CHECK(e.take_rename(&current));
    CHECK(current == "pad copy 2");
    CHECK(!e.take_rename(&current));
    e.text("mi");
    e.text("ne");
    e.text_done();
    CHECK(e.layout().name == "mine");
    // Text outside a rename is ignored; a built-in name is refused.
    e.text("x");
    e.text_done();
    CHECK(e.layout().name == "mine");
    tap_tool(e, Tool::Layout);
    CHECK(pick(e, "rename"));
    CHECK(e.take_rename(&current));
    e.text("keys");
    e.text_done();
    CHECK(e.layout().name == "mine");

    // Toggle binding lists the layout names plus next.
    tap_tool(e, Tool::Add);
    CHECK(pick(e, "toggle"));
    tap_tool(e, Tool::Bind);
    CHECK(pick(e, "keys"));
    CHECK(e.layout().groups.back().controls.back().target == "keys");

    e.close();
    CHECK(!e.is_open());
}

int main() {
    test_json_round_trip();
    test_json_errors_name_the_line();
    test_layout_parse_and_write();
    test_stick_radius_and_zone_round_trip();
    test_layout_geometry_and_hits();
    test_builtin_keys_matches_the_old_keypad();
    test_form_for();
    test_layout_store();
    test_raster_disc();
    test_raster_ring();
    test_raster_radial_gradient();
    test_raster_opacity_premultiplies();
    test_raster_polygon();
    test_raster_stroke();
    test_raster_source_over();
    test_raster_text();
    test_raster_matches_legacy_keypad_pixels();
    test_router_space_key();
    test_router_latched_shift();
    test_router_left_tab_toggle();
    test_router_gap_is_claimed_silently();
    test_router_game_area_not_claimed();
    test_router_cancel_does_not_release_latch();
    test_router_two_fingers_shift_held_and_a();
    test_router_set_enabled_false_releases_and_blocks();
    test_router_cancel_all_releases_everything();
    test_router_set_layout_releases_held_key();
    test_router_set_layout_releases_latched_modifier();
    test_router_finger_cancel_on_held_modifier();
    test_router_two_fingers_same_key_first_lift_releases();
    test_router_set_layout_null_disables_hit_testing();
    test_router_state_out_of_range_is_zero();
    test_stick_output();
    test_dpad_hat();
    test_pad_state_merge();
    test_vpad_edges();
    test_to_host_scales_axes();
    test_router_pad_buttons();
    test_router_pad_stick_dpad_and_cancel();
    test_router_second_finger_on_stick_or_dpad_is_inert();
    test_router_generation_bumps_only_on_hat_or_knob_change();
    test_make_view_keys();
    test_make_view_matches_the_old_keypad();
    test_make_view_revision_ignores_undrawn_press();
    test_tablet_fallback();
    test_raster_shape_bounds_are_clamped();
    test_binding_parse_mapped();
    test_binding_button_mouse();
    test_binding_cursor_stick();
    test_binding_arrows_stick();
    test_binding_wheel_button();
    test_binding_action_button();
    test_binding_scroll_stick();
    test_binding_release_all();
    test_binding_cursor_starts_centred();
    test_binding_aliased_key_is_ref_counted();
    test_binding_aliased_mouse_button_is_ref_counted();
    test_binding_release_all_latches_still_held_buttons();
    test_rumble_sink_truth_table();
    test_controls_area_below_the_game();
    test_router_claims_the_controls_area();
    test_make_view_carries_the_controls_area();
    test_editor_open_and_toolbar();
    test_editor_select_drag_and_reanchor();
    test_editor_snap_off_and_snap_to_control();
    test_editor_pinch();
    test_editor_add_delete_bind();
    test_editor_bind_native_button();
    test_editor_grid_key_moves_its_group();
    test_editor_done_reset_and_layout_names();
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("controls_tests: all passed\n");
    return 0;
}
