// controls_tests.cpp - the on-screen controls: json, layouts, router, pad, binding, editor.
#include "../controls/builtin_layouts.h"
#include "../controls/json.h"
#include "../controls/layout.h"
#include "../keypad_layout.h"
#include "keypad_legacy_oracle.h"

#include <cmath>
#include <stdio.h>
#include <string.h>

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

int main() {
    test_json_round_trip();
    test_json_errors_name_the_line();
    test_layout_parse_and_write();
    test_layout_geometry_and_hits();
    test_builtin_keys_matches_the_old_keypad();
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("controls_tests: all passed\n");
    return 0;
}
