// keypad_tests.cpp - the split keypad's tables, geometry and modifiers.
#include "../keypad_layout.h"

#include <SDL3/SDL_scancode.h>
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

// The layout spells SDL's scancodes out to stay SDL-free; keep them honest.
static_assert(
    int(kScanA) == int(SDL_SCANCODE_A) && int(kScanZ) == int(SDL_SCANCODE_Z) &&
        int(kScan1) == int(SDL_SCANCODE_1) && int(kScan0) == int(SDL_SCANCODE_0) &&
        int(kScanReturn) == int(SDL_SCANCODE_RETURN) &&
        int(kScanEscape) == int(SDL_SCANCODE_ESCAPE) &&
        int(kScanBackspace) == int(SDL_SCANCODE_BACKSPACE) &&
        int(kScanTab) == int(SDL_SCANCODE_TAB) && int(kScanSpace) == int(SDL_SCANCODE_SPACE) &&
        int(kScanMinus) == int(SDL_SCANCODE_MINUS) &&
        int(kScanEquals) == int(SDL_SCANCODE_EQUALS) &&
        int(kScanLeftBracket) == int(SDL_SCANCODE_LEFTBRACKET) &&
        int(kScanRightBracket) == int(SDL_SCANCODE_RIGHTBRACKET) &&
        int(kScanBackslash) == int(SDL_SCANCODE_BACKSLASH) &&
        int(kScanSemicolon) == int(SDL_SCANCODE_SEMICOLON) &&
        int(kScanApostrophe) == int(SDL_SCANCODE_APOSTROPHE) &&
        int(kScanGrave) == int(SDL_SCANCODE_GRAVE) && int(kScanComma) == int(SDL_SCANCODE_COMMA) &&
        int(kScanPeriod) == int(SDL_SCANCODE_PERIOD) &&
        int(kScanSlash) == int(SDL_SCANCODE_SLASH) && int(kScanF1) == int(SDL_SCANCODE_F1) &&
        int(kScanF12) == int(SDL_SCANCODE_F12) && int(kScanInsert) == int(SDL_SCANCODE_INSERT) &&
        int(kScanHome) == int(SDL_SCANCODE_HOME) && int(kScanPageUp) == int(SDL_SCANCODE_PAGEUP) &&
        int(kScanDelete) == int(SDL_SCANCODE_DELETE) && int(kScanEnd) == int(SDL_SCANCODE_END) &&
        int(kScanPageDown) == int(SDL_SCANCODE_PAGEDOWN) &&
        int(kScanRight) == int(SDL_SCANCODE_RIGHT) && int(kScanLeft) == int(SDL_SCANCODE_LEFT) &&
        int(kScanDown) == int(SDL_SCANCODE_DOWN) && int(kScanUp) == int(SDL_SCANCODE_UP) &&
        int(kScanLCtrl) == int(SDL_SCANCODE_LCTRL) &&
        int(kScanLShift) == int(SDL_SCANCODE_LSHIFT) && int(kScanLAlt) == int(SDL_SCANCODE_LALT),
    "keypad_layout.h scancodes drifted from SDL");

static KeypadView view_at(double scale, int size = 1) {
    KeypadView v;
    v.wanted = true;
    v.scale = scale;
    v.size = size;
    return v;
}

static void test_tables_fill_the_grid_once() {
    for (int side = 0; side < 2; ++side) {
        int n = 0;
        const KeypadKey *keys = keypad_keys(KeypadSide(side), &n);
        int cells = 0;
        bool used[kKeypadRows][kKeypadCols] = {};
        for (int i = 0; i < n; ++i) {
            const KeypadKey &k = keys[i];
            CHECK(k.label && strlen(k.label) >= 1 && strlen(k.label) <= 5);
            CHECK(k.scancode > 0);
            CHECK(k.row >= 0 && k.row < kKeypadRows && k.col >= 0 && k.span >= 1 &&
                  k.col + k.span <= kKeypadCols);
            for (int c = k.col; c < k.col + k.span; ++c) {
                CHECK(!used[k.row][c]); // no overlap
                used[k.row][c] = true;
                ++cells;
            }
        }
        CHECK(cells == kKeypadRows * kKeypadCols); // no gap
    }
    int n = 0;
    const KeypadKey *left = keypad_keys(KEYPAD_LEFT, &n);
    bool has_shift = false, has_ctrl = false, has_alt = false, has_esc = false;
    for (int i = 0; i < n; ++i) {
        has_shift |= left[i].scancode == kScanLShift;
        has_ctrl |= left[i].scancode == kScanLCtrl;
        has_alt |= left[i].scancode == kScanLAlt;
        has_esc |= left[i].scancode == kScanEscape;
    }
    CHECK(has_shift && has_ctrl && has_alt && has_esc);
    const KeypadKey *right = keypad_keys(KEYPAD_RIGHT, &n);
    bool has_space = false, has_enter = false;
    int arrows = 0;
    for (int i = 0; i < n; ++i) {
        has_space |= right[i].scancode == kScanSpace && right[i].span == 3;
        has_enter |= right[i].scancode == kScanReturn;
        arrows += right[i].scancode == kScanUp || right[i].scancode == kScanDown ||
                  right[i].scancode == kScanLeft || right[i].scancode == kScanRight;
    }
    CHECK(has_space && has_enter && arrows == 4);
}

static void test_geometry_at_default_size() {
    // 1180x820 points at scale 2: 2360x1640 pixels. Pitch 40 pt = 80 px.
    CHECK(keypad_pitch_px(1, 2.0) == 80);
    CHECK(keypad_gap_px(2.0) == 8);
    KeypadRect l = keypad_half_rect(KEYPAD_LEFT, 1, 2.0, 2360, 1640);
    KeypadRect r = keypad_half_rect(KEYPAD_RIGHT, 1, 2.0, 2360, 1640);
    CHECK(l.x == 0 && l.w == 640 && l.h == 400 && l.y + l.h == 1640);
    CHECK(r.x + r.w == 2360 && r.w == 640 && r.h == 400 && r.y == l.y);
    CHECK(l.x + l.w < r.x); // the game shows between them
    KeypadRect lt = keypad_tab_rect(KEYPAD_LEFT, true, 1, 2.0, 2360, 1640);
    CHECK(lt.x == 0 && lt.w == 128 && lt.h == 40 && lt.y + lt.h == l.y); // above the half
    KeypadRect rt = keypad_tab_rect(KEYPAD_RIGHT, false, 1, 2.0, 2360, 1640);
    CHECK(rt.x + rt.w == 2360 && rt.y + rt.h == 1640); // hidden: in the corner
    // Sizes step the pitch.
    CHECK(keypad_pitch_px(0, 2.0) == 72 && keypad_pitch_px(2, 2.0) == 88);
    CHECK(keypad_pitch_px(1, 1.0) == 40);
}

static void test_every_key_hits_itself() {
    const struct {
        int dw, dh;
        double scale;
    } screens[] = {{2360, 1640, 2.0}, {1024, 768, 1.0}};
    for (const auto &s : screens)
        for (int size = 0; size < 3; ++size) {
            const KeypadView view = view_at(s.scale, size);
            for (int side = 0; side < 2; ++side) {
                int n = 0;
                const KeypadKey *keys = keypad_keys(KeypadSide(side), &n);
                for (int i = 0; i < n; ++i) {
                    KeypadRect k =
                        keypad_key_rect(KeypadSide(side), keys[i], size, s.scale, s.dw, s.dh);
                    KeypadHit h = keypad_hit(view, s.dw, s.dh, k.x + k.w / 2.0, k.y + k.h / 2.0);
                    CHECK(h.kind == KeypadHit::Key && h.scancode == keys[i].scancode &&
                          h.side == KeypadSide(side));
                    KeypadRect half = keypad_half_rect(KeypadSide(side), size, s.scale, s.dw, s.dh);
                    CHECK(half.contains(k.x, k.y));
                }
            }
        }
}

static void test_halves_do_not_overlap_and_middle_is_the_game() {
    for (int size = 0; size < 3; ++size) {
        KeypadRect l = keypad_half_rect(KEYPAD_LEFT, size, 2.0, 2360, 1640);
        KeypadRect r = keypad_half_rect(KEYPAD_RIGHT, size, 2.0, 2360, 1640);
        CHECK(l.x + l.w <= r.x);
        const KeypadView view = view_at(2.0, size);
        CHECK(keypad_hit(view, 2360, 1640, 1180, 1500).kind == KeypadHit::None); // between
        CHECK(keypad_hit(view, 2360, 1640, 100, 100).kind == KeypadHit::None);   // above
    }
}

static void test_tabs_toggle_and_a_hidden_half_is_only_its_tab() {
    KeypadView view = view_at(2.0);
    view.right = false;
    KeypadRect lt = keypad_tab_rect(KEYPAD_LEFT, true, 1, 2.0, 2360, 1640);
    KeypadHit h = keypad_hit(view, 2360, 1640, lt.x + 5, lt.y + 5);
    CHECK(h.kind == KeypadHit::Toggle && h.side == KEYPAD_LEFT);
    KeypadRect rt = keypad_tab_rect(KEYPAD_RIGHT, false, 1, 2.0, 2360, 1640);
    h = keypad_hit(view, 2360, 1640, rt.x + 5, rt.y + 5);
    CHECK(h.kind == KeypadHit::Toggle && h.side == KEYPAD_RIGHT);
    // Where the right half would be is the game while it is hidden.
    KeypadRect r = keypad_half_rect(KEYPAD_RIGHT, 1, 2.0, 2360, 1640);
    CHECK(keypad_hit(view, 2360, 1640, r.x + 10, r.y + 10).kind == KeypadHit::None);
    CHECK(keypad_hit(view, 0, 0, 0, 0).kind == KeypadHit::None);
    CHECK(keypad_hit(view, 2360, 1640, -1, 1600).kind == KeypadHit::None);
    // Not wanted (a hardware keyboard is attached): nothing is hit at all.
    view.wanted = false;
    CHECK(keypad_hit(view, 2360, 1640, lt.x + 5, lt.y + 5).kind == KeypadHit::None);
}

static void test_a_gap_between_keys_is_the_keypads() {
    const KeypadView view = view_at(2.0);
    KeypadRect half = keypad_half_rect(KEYPAD_LEFT, 1, 2.0, 2360, 1640);
    // The first pixel of the half is inside the gap before the first key.
    KeypadHit h = keypad_hit(view, 2360, 1640, half.x + 1, half.y + 1);
    CHECK(h.kind == KeypadHit::Key && h.scancode == 0);
}

static void test_modifier_predicate() {
    CHECK(keypad_is_modifier(kScanLShift) && keypad_is_modifier(kScanLCtrl) &&
          keypad_is_modifier(kScanLAlt));
    CHECK(!keypad_is_modifier(kScanA) && !keypad_is_modifier(kScanReturn));
    CHECK(keypad_modifier_bit(kScanLShift) == 1u && keypad_modifier_bit(kScanLCtrl) == 2u &&
          keypad_modifier_bit(kScanLAlt) == 4u && keypad_modifier_bit(kScanA) == 0u);
}

int main() {
    test_tables_fill_the_grid_once();
    test_geometry_at_default_size();
    test_every_key_hits_itself();
    test_halves_do_not_overlap_and_middle_is_the_game();
    test_tabs_toggle_and_a_hidden_half_is_only_its_tab();
    test_a_gap_between_keys_is_the_keypads();
    test_modifier_predicate();
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("keypad_tests: ok\n");
    return 0;
}
