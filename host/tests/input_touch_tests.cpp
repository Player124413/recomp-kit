// input_touch_tests.cpp - the gesture table, one case per row.
#include "../input_touch.h"

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
    CHECK(out.size() == 2); // press now; the release is held
    CHECK(out[0].kind == TouchAction::Motion && out[0].x == 101 && out[0].y == 201);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down);
    m.tick(80 * MS + kTouchClickHoldNs / 2, &out);
    CHECK(out.size() == 2); // still held
    m.tick(80 * MS + kTouchClickHoldNs, &out);
    CHECK(out.size() == 3 && out[2].kind == TouchAction::Button && out[2].button == 0 &&
          !out[2].down);
}

static void test_a_new_finger_releases_a_held_click_first() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 0, 0}, 0, &out);
    m.finger_up({1, 0, 0}, 20 * MS, &out);
    out.clear();
    m.finger_down({2, 50, 50}, 30 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && !out[0].down);
}

static void test_long_press_is_right_click() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 50, 60}, 0, &out);
    m.tick(300 * MS, &out);
    CHECK(out.empty());
    m.tick(360 * MS, &out);
    // The rest only places the cursor; the button waits for the lift.
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].place && out[0].x == 50);
    out.clear();
    m.finger_up({1, 50, 60}, 380 * MS, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Motion &&
          out[1].kind == TouchAction::Button && out[1].button == 1 && out[1].down);
    m.tick(380 * MS + kTouchClickHoldNs, &out);
    CHECK(out.size() == 3 && out[2].kind == TouchAction::Button && out[2].button == 1 &&
          !out[2].down);
}

static void test_long_press_then_drag_is_a_wheel_button_drag() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 50, 60}, 0, &out);
    m.tick(360 * MS, &out); // cursor placed
    out.clear();
    m.finger_motion({1, 55, 62}, 400 * MS, &out);
    CHECK(out.empty()); // still resting
    m.finger_motion({1, 120, 90}, 500 * MS, &out);
    CHECK(out.size() == 2);
    CHECK(out[0].kind == TouchAction::Button && out[0].button == 2 && out[0].down &&
          out[0].x == 50 && out[0].y == 60);
    CHECK(out[1].kind == TouchAction::Motion && out[1].x == 120 && !out[1].place);
    out.clear();
    m.finger_motion({1, 125, 92}, 600 * MS, &out);
    CHECK(out.size() == 1 && !out[0].place);
    out.clear();
    m.finger_up({1, 130, 95}, 900 * MS, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Motion && !out[0].place &&
          out[1].kind == TouchAction::Button && out[1].button == 2 && !out[1].down);
    out.clear();
    m.tick(2000 * MS, &out);
    CHECK(out.empty()); // no right click after a drag
}

static void test_edge_hold_scrolls_then_moves_the_cursor_inside() {
    TouchMapper m;
    m.set_bounds(1000, 800);
    std::vector<TouchAction> out;
    m.finger_down({1, 5, 400}, 0, &out);
    m.tick(360 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 0 &&
          out[0].y == 400); // snapped onto the left edge
    out.clear();
    m.tick(1500 * MS, &out);
    CHECK(out.empty()); // an edge hold is not a right click
    m.finger_up({1, 6, 402}, 2000 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].place &&
          out[0].x == kTouchEdgeRelease && out[0].y == 400);
    m.tick(3000 * MS, &out);
    CHECK(out.size() == 1); // and no click
}

static void test_tap_on_an_edge_clicks_there_then_moves_inside() {
    TouchMapper m;
    m.set_bounds(1000, 800);
    std::vector<TouchAction> out;
    m.finger_down({1, 995, 790}, 0, &out);
    m.finger_up({1, 996, 791}, 50 * MS, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Motion && out[0].x == 999 &&
          out[0].y == 799);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down &&
          out[1].x == 999);
    out.clear();
    m.tick(50 * MS + kTouchClickHoldNs, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Button && !out[0].down);
    CHECK(out[1].kind == TouchAction::Motion && out[1].x == 999 - kTouchEdgeRelease &&
          out[1].y == 799 - kTouchEdgeRelease);
}

static void test_no_bounds_means_no_snapping() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 2, 3}, 0, &out);
    m.finger_up({1, 2, 3}, 50 * MS, &out);
    CHECK(out.size() == 2 && out[0].x == 2 && out[0].y == 3);
    m.tick(50 * MS + kTouchClickHoldNs, &out);
    CHECK(out.size() == 3); // release, no nudge
}

static void test_drag_is_left_drag() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 10, 10}, 0, &out);
    m.finger_motion({1, 20, 10}, 20 * MS, &out);
    CHECK(out.empty()); // under the travel threshold
    m.finger_motion({1, 45, 10}, 40 * MS, &out);
    CHECK(out.size() == 3); // Motion to start, Button down, Motion to here
    CHECK(out[0].kind == TouchAction::Motion && out[0].x == 10 && out[0].place);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down);
    CHECK(out[2].kind == TouchAction::Motion && out[2].x == 45);
    out.clear();
    m.finger_up({1, 55, 10}, 60 * MS, &out);
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

int main() {
    test_tap_is_left_click();
    test_a_new_finger_releases_a_held_click_first();
    test_long_press_is_right_click();
    test_long_press_then_drag_is_a_wheel_button_drag();
    test_edge_hold_scrolls_then_moves_the_cursor_inside();
    test_tap_on_an_edge_clicks_there_then_moves_inside();
    test_no_bounds_means_no_snapping();
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
