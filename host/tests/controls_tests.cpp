// controls_tests.cpp - the on-screen controls: json, layouts, router, pad, binding, editor.
#include "../../platform/os.h"
#include "../controls/builtin_layouts.h"
#include "../controls/json.h"
#include "../controls/layout.h"
#include "../controls/layout_fallback.h"
#include "../controls/layout_store.h"
#include "../controls/overlay.h"
#include "../controls/overlay_paint.h"
#include "../controls/raster.h"
#include "../controls/router.h"
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
    CHECK(l.groups[1].controls.size() == 3); // the unknown kind was skipped
    Layout again;
    CHECK(parse_layout(write_layout(l), &again, &err));
    CHECK(write_layout(again) == write_layout(l));
    CHECK(!parse_layout("{\"groups\": 5}", &l, &err));
    CHECK(scancode_from_name("F5") == kScanF5 && std::string(scancode_name(kScanUp)) == "Up");
    CHECK(scancode_from_name("NotAKey") == 0);
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

int main() {
    test_json_round_trip();
    test_json_errors_name_the_line();
    test_layout_parse_and_write();
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
    test_make_view_keys();
    test_make_view_matches_the_old_keypad();
    test_make_view_revision_ignores_undrawn_press();
    test_tablet_fallback();
    test_raster_shape_bounds_are_clamped();
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("controls_tests: all passed\n");
    return 0;
}
