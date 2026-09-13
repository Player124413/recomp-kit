// input_touch_tests.cpp - the gesture table, one case per row.
#include "../input_touch.h"
#include "../touch_overlay_layout.h"

#include <SDL3/SDL_scancode.h>
#include <stdio.h>

static int g_failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static const uint64_t MS = 1000000ull;

static void test_tap_is_left_click() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 200}, 0, &out);
    m.finger_up({1, 101, 201}, 80 * MS, &out);
    CHECK(out.size() == 3);
    CHECK(out[0].kind == TouchAction::Motion && out[0].x == 101 && out[0].y == 201);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down);
    CHECK(out[2].kind == TouchAction::Button && out[2].button == 0 && !out[2].down);
}

static void test_long_press_is_right_click() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 50, 60}, 0, &out);
    m.tick(300 * MS, &out);
    CHECK(out.empty());
    m.tick(360 * MS, &out);
    CHECK(out.size() == 3 && out[1].kind == TouchAction::Button && out[1].button == 1 &&
          out[1].down);
    CHECK(out[2].button == 1 && !out[2].down);
    out.clear();
    m.finger_up({1, 50, 60}, 400 * MS, &out);
    CHECK(out.empty()); // the click already fired
}

static void test_drag_is_left_drag() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 10, 10}, 0, &out);
    m.finger_motion({1, 15, 10}, 20 * MS, &out);
    CHECK(out.empty()); // under the travel threshold
    m.finger_motion({1, 30, 10}, 40 * MS, &out);
    CHECK(out.size() == 3); // Motion to start, Button down, Motion to here
    CHECK(out[0].kind == TouchAction::Motion && out[0].x == 10);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down);
    CHECK(out[2].kind == TouchAction::Motion && out[2].x == 30);
    out.clear();
    m.finger_up({1, 40, 10}, 60 * MS, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Motion &&
          out[1].kind == TouchAction::Button && !out[1].down);
}

static void test_two_finger_drag_pans_with_arrows() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_down({2, 140, 100}, 5 * MS, &out);
    m.finger_motion({1, 100, 130}, 30 * MS, &out);
    m.finger_motion({2, 140, 130}, 30 * MS, &out); // centroid moved 30 down: one step
    CHECK(out.size() == 2);
    CHECK(out[0].kind == TouchAction::Key && out[0].scancode == SDL_SCANCODE_DOWN && out[0].down);
    CHECK(out[1].kind == TouchAction::Key && out[1].scancode == SDL_SCANCODE_DOWN && !out[1].down);
    out.clear();
    m.finger_up({1, 100, 130}, 60 * MS, &out);
    m.finger_up({2, 140, 130}, 60 * MS, &out);
    CHECK(out.empty()); // no click from a pan
}

static void test_two_finger_tap_is_escape_three_is_f10_four_toggles_keyboard() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 0, 0}, 0, &out);
    m.finger_down({2, 10, 0}, 0, &out);
    m.finger_up({1, 0, 0}, 50 * MS, &out);
    m.finger_up({2, 10, 0}, 50 * MS, &out);
    CHECK(out.size() == 2 && out[0].scancode == SDL_SCANCODE_ESCAPE && out[0].down && !out[1].down);
    out.clear();
    m.finger_down({1, 0, 0}, 100 * MS, &out);
    m.finger_down({2, 10, 0}, 100 * MS, &out);
    m.finger_down({3, 20, 0}, 100 * MS, &out);
    m.finger_up({1, 0, 0}, 150 * MS, &out);
    m.finger_up({2, 10, 0}, 150 * MS, &out);
    m.finger_up({3, 20, 0}, 150 * MS, &out);
    CHECK(out.size() == 2 && out[0].scancode == SDL_SCANCODE_F10);
    out.clear();
    CHECK(!m.text_input_wanted());
    for (int i = 1; i <= 4; ++i)
        m.finger_down({i, 10.0 * i, 0}, 200 * MS, &out);
    for (int i = 1; i <= 4; ++i)
        m.finger_up({i, 10.0 * i, 0}, 250 * MS, &out);
    CHECK(out.empty() && m.text_input_wanted());
}

static void test_second_finger_cancels_pending_tap() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 0, 0}, 0, &out);
    m.finger_down({2, 10, 0}, 10 * MS, &out);
    m.tick(400 * MS, &out);
    CHECK(out.empty()); // no long press once a second finger joined
}

static void test_overlay_layout_spans_the_bottom_edge() {
    std::vector<TouchKey> keys;
    touch_overlay_layout(2420, 1668, &keys);
    CHECK(keys.size() == 8);
    CHECK(keys.front().x == 0);
    CHECK(keys.back().x + keys.back().w == 2420);
    CHECK(keys.front().y + keys.front().h == 1668);
    CHECK(keys.front().scancode == SDL_SCANCODE_ESCAPE);
    CHECK(keys.back().scancode == SDL_SCANCODE_RETURN);
    CHECK(touch_overlay_height(2420) == keys.front().h);
}

static void test_overlay_hit_matches_layout() {
    std::vector<TouchKey> keys;
    touch_overlay_layout(2420, 1668, &keys);
    for (const TouchKey &k : keys)
        CHECK(touch_overlay_hit(2420, 1668, k.x + k.w / 2.0, k.y + k.h / 2.0) == k.scancode);
    CHECK(touch_overlay_hit(2420, 1668, 1210, 800) == 0);                     // mid screen
    CHECK(touch_overlay_hit(2420, 1668, 10, 1668 - keys.front().h - 1) == 0); // just above the bar
    CHECK(touch_overlay_hit(2420, 1668, -1, 1660) == 0);                      // outside
    CHECK(touch_overlay_hit(0, 0, 0, 0) == 0);
}

int main() {
    test_overlay_layout_spans_the_bottom_edge();
    test_overlay_hit_matches_layout();
    test_tap_is_left_click();
    test_long_press_is_right_click();
    test_drag_is_left_drag();
    test_two_finger_drag_pans_with_arrows();
    test_two_finger_tap_is_escape_three_is_f10_four_toggles_keyboard();
    test_second_finger_cancels_pending_tap();
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("input_touch_tests: ok\n");
    return 0;
}
