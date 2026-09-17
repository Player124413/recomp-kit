// binding.h - the mapped binding: turns the shared virtual pad into the keys
// and mouse input a game already reads, per RECOMP_CONTROLS_MAPPED (and a
// player's override file) and a per-tick clock. SDL-free (uses SDL_Scancode
// values spelled out in keypad_layout.h), so it links into the host-free
// unit tests, same as layout.h and vpad.h.
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md.
#pragma once

#include "../input_touch.h"
#include "layout.h"
#include "vpad.h"

#include <cstdint>
#include <string>
#include <vector>

namespace controls {

enum class StickMode { Cursor, Arrows, Wasd, Scroll, Wheel, None };

// What a pad button (or, via a stick/dpad key, a synthesized press) does to
// the game: nothing, a key, a mouse button, a wheel notch, or a host action.
struct Target {
    enum Type { None, Key, Mouse, Wheel, Action } type = None;
    int value = 0;      // Key: scancode; Mouse: 0 left 1 right 2 middle; Wheel: +1 up / -1 down
    std::string action; // Action: "settings" | "system_keyboard" | "edit_layout"
};

// Every RECOMP_CONTROLS_MAPPED key, defaulted the same as tools/game_config.py's
// MAPPED_DEFAULTS so a table built with no input at all matches the game's
// declared defaults.
struct MappedTable {
    StickMode left = StickMode::Arrows, right = StickMode::Cursor;
    StickMode dpad = StickMode::Arrows; // Arrows | Wasd | None
    double cursor_speed = 900;          // points per second at full deflection
    Target buttons[int(PadButton::Count)];
};

// "k=v;k=v" (RECOMP_CONTROLS_MAPPED's syntax, and <profile>/controls/binding.txt's).
// Applies over *table; unknown keys/values fail the whole parse (*error names
// the problem) and leave *table unchanged.
bool parse_mapped(const std::string &text, MappedTable *table, std::string *error);
// Writes every key, sorted, in the same syntax; parse_mapped(write_mapped(t), ...) round-trips.
std::string write_mapped(const MappedTable &table);
// A target's spelling: "key:Space", "mouse_left", "wheel_up", "action:settings", "none".
std::string target_name(const Target &t);

// Turns one tick of the shared virtual pad into TouchActions (the same
// key/mouse/wheel vocabulary host/sdl/main.cpp's touch path emits, in window
// points) and host action names (routed like ControlsSink::action). Holds no
// SDL state; the host drives it from controls_host.cpp.
class Binding {
  public:
    void set_table(const MappedTable &t) {
        table_ = t;
    }
    // The window's size in points, for the Cursor stick mode's clamp.
    void set_bounds(double w, double h) {
        bounds_w_ = w;
        bounds_h_ = h;
    }
    // A real pointer or a touch placed the cursor here (window points): the
    // Cursor stick mode continues from this position instead of its own.
    void set_cursor(double x, double y);
    // One input tick: `dt` is `now_ns` since the previous tick, clamped to 50
    // ms (0 on the first tick). Appends every action this tick produced.
    void tick(const PadState &pad, uint64_t now_ns, std::vector<TouchAction> *out,
              std::vector<std::string> *actions);
    // Releases every key and mouse button currently held by this binding and
    // resets its stick/dpad accumulators and press state.
    void release_all(std::vector<TouchAction> *out);
    double cursor_x() const {
        return cursor_x_;
    }
    double cursor_y() const {
        return cursor_y_;
    }

  private:
    enum class AxisDir { None, Neg, Pos };

    void apply_button_edge(const Target &t, bool down, std::vector<TouchAction> *out,
                           std::vector<std::string> *actions);
    void apply_dpad(uint8_t hat, std::vector<TouchAction> *out);
    void apply_stick(int index, float vx, float vy, double dt, std::vector<TouchAction> *out);
    // Arrows/Wasd: one axis' hysteresis press/release, tracked per stick.
    void update_axis(int index, bool y_axis, float v, bool wasd, std::vector<TouchAction> *out);
    // Cursor mode releases anything an earlier Arrows/Wasd/dpad-style hold left behind.
    void release_axis(int index, std::vector<TouchAction> *out);
    void press_key(int scancode, std::vector<TouchAction> *out);
    void release_key(int scancode, std::vector<TouchAction> *out);
    void tap_key(int scancode, std::vector<TouchAction> *out);
    void emit_wheel(int notches, std::vector<TouchAction> *out);

    MappedTable table_;
    double bounds_w_ = 0, bounds_h_ = 0;
    double cursor_x_ = 0, cursor_y_ = 0;
    double emit_x_ = 0, emit_y_ = 0; // the cursor's position as of the last Motion emitted
    bool has_last_ = false;
    uint64_t last_ns_ = 0;

    uint16_t prev_buttons_ = 0; // the pad's own buttons, as of the previous tick

    int dpad_active_sc_[4] = {0, 0, 0, 0}; // up, right, down, left; the scancode currently held

    AxisDir stick_x_dir_[2] = {AxisDir::None, AxisDir::None};
    AxisDir stick_y_dir_[2] = {AxisDir::None, AxisDir::None};
    int stick_x_sc_[2] = {0, 0};
    int stick_y_sc_[2] = {0, 0};

    double pan_acc_x_[2] = {0, 0}, pan_acc_y_[2] = {0, 0}; // Scroll mode
    double wheel_acc_[2] = {0, 0};                         // Wheel mode

    std::vector<int> held_keys_; // every scancode currently held, from any source
    bool mouse_down_[3] = {false, false, false};
};

} // namespace controls
