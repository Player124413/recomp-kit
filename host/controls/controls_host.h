// controls_host.h - the on-screen controls' glue for the SDL host: owns the
// layout store, the active layout and the finger router, follows the
// settings rows, and publishes what to draw to the presenter. Called only
// from host/sdl/main.cpp, on its thread.
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md.
#pragma once

#include "layout.h"

#include <cstdint>
#include <vector>

struct TouchAction;

namespace controls {

// What the controls ask of the host.
struct HostHooks {
    void (*key)(int scancode, bool down) = nullptr;
    void (*touch_actions)(const std::vector<TouchAction> &actions) = nullptr; // Task 10
    void (*system_keyboard)() = nullptr;
    void (*open_settings)() = nullptr;
};

// Finds the layouts and registers their names with the settings rows; must
// run before mods_page_init (the first presented frame).
void host_init(const HostHooks &hooks);
// The drawable, its scale and safe area, as of this pump, and where the game
// image is (the presenter's game rectangle) with the safe area's bottom inset,
// in drawable pixels. In portrait the space below the image becomes the
// controls area: the layout anchors in it and every finger there is claimed.
void host_set_screen(const Screen &s, const Rect &game, int safe_bottom);
// keyboard_absent: no hardware keyboard is attached (or RECOMP_KEYPAD forces
// the controls on). controller_present: reserved for Task 12.
void host_set_wanted(bool keyboard_absent, bool controller_present);
// A finger event at drawable pixel (px, py). True when the controls claimed
// the finger; the caller must then not give it to the gesture mapper.
bool host_finger_down(int64_t id, double px, double py, uint64_t now);
bool host_finger_motion(int64_t id, double px, double py, uint64_t now);
bool host_finger_up(int64_t id, uint64_t now);
bool host_finger_cancel(int64_t id);
// Focus loss or backgrounding: every finger is gone, every key and modifier up.
void host_release_all();
// After the events: follow the settings (layout, size, hidden groups), enable
// the router, and publish the view when it changed.
void host_pump(uint64_t now);

} // namespace controls
