// router.h - the SDL-free finger router: decides which on-screen control a
// finger belongs to and drives key, action, layout-switch and haptic-tap
// events from it. Replaces the keypad logic host/sdl/main.cpp used to carry
// directly; the host still owns SDL and calls this with plain numbers.
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md.
#pragma once

#include "layout.h"
#include "vpad.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../keypad_modifiers.h"

namespace controls {

// What the router does with a routed event; the host implements this over
// its real key/gesture/layout-store plumbing (and a test implements it over
// a vector of strings).
class ControlsSink {
  public:
    virtual ~ControlsSink() = default;
    virtual void key(int scancode, bool down) = 0;
    virtual void
    action(const std::string &name) = 0; // "settings", "system_keyboard", "edit_layout"
    virtual void switch_layout(const std::string &target) = 0; // a layout name or "next"
    virtual void group_visibility_changed() = 0;               // persist Layout group visibility
    virtual void tap() = 0;                                    // haptic tick on a press
};

// A control's live, drawable state, indexed by (group, control).
struct ControlState {
    bool pressed = false;
    double knob_x = 0, knob_y = 0; // Stick: output in [-1, 1]
    double base_x = 0,
           base_y = 0; // Stick (floating): the centre, in drawable pixels; 0,0 = default
    uint8_t hat = 0;   // Dpad: 1 up, 2 right, 4 down, 8 left
};

// Owns no SDL state: fingers arrive as (id, point, time) and leave as
// key/action/switch_layout/tap calls on the sink passed to each call. A
// finger the router claims (its `finger_*` returns true) must not also be
// given to any other input path (the touch gesture mapper, in particular).
class Router {
  public:
    // Not owned. Releases everything the router currently holds against the
    // old layout — exactly as cancel_all(sink) would — before swapping in
    // the new one and rebuilding its per-control state.
    void set_layout(Layout *layout, ControlsSink &sink);
    void set_screen(const Screen &s);
    // false: hit_test never claims a new finger, and every finger currently
    // held is released exactly as cancel_all() would release it.
    void set_enabled(bool on, ControlsSink &sink);
    bool enabled() const;
    // True when the finger belongs to the controls (the caller must not give it to TouchMapper).
    bool finger_down(int64_t id, double px, double py, uint64_t now_ns, ControlsSink &sink);
    bool finger_motion(int64_t id, double px, double py, uint64_t now_ns, ControlsSink &sink);
    bool finger_up(int64_t id, uint64_t now_ns, ControlsSink &sink);
    bool finger_cancel(int64_t id, ControlsSink &sink);
    void cancel_all(ControlsSink &sink);
    bool owns(int64_t id) const;
    unsigned lit() const; // keypad_modifier_bit() bits
    const ControlState &state(int group,
                              int control) const; // a static zero state when out of range
    uint32_t generation() const;                  // bumps whenever anything drawn changes
    const Layout *layout() const;
    // The OR of every currently-owned Button/Dpad/Stick control; recomputed
    // whenever a finger claiming one of them lands, moves or lifts.
    const PadState &pad() const;

  private:
    // What a live finger is sitting on: a control (group/control >= 0), or a
    // visible grid group's own gap (control == -1, gap == true).
    struct Owned {
        int group = -1, control = -1;
        bool gap = false;
    };

    void set_pressed(int group, int control, bool pressed);
    // Down on a Key control: modifiers go through modifiers_; anything else
    // is a plain sink.key(scancode, true).
    void key_down(const Control &c, uint64_t now_ns, ControlsSink &sink);
    // Up on a Key control: modifiers release through modifiers_; anything
    // else is sink.key(scancode, false) followed by key_lifted(). Legacy
    // quirk, ported as-is from host/sdl/main.cpp's g_keypad_fingers map: a
    // key's release is not reference-counted across the fingers that land
    // on it, so if two fingers land on the same key, either one lifting
    // releases it, and the other's later lift releases it again.
    void key_up(const Control &c, uint64_t now_ns, ControlsSink &sink);
    // Cancel on a Key control: modifiers cancel through modifiers_ (no
    // latch survives); anything else is sink.key(scancode, false) alone.
    void key_cancel(const Control &c, ControlsSink &sink);
    // Rebuilds pad_ from every currently-owned Button/Dpad/Stick control.
    void recompute_pad();

    Layout *layout_ = nullptr; // not owned
    Screen screen_;
    bool enabled_ = true;
    std::map<int64_t, Owned> fingers_;
    std::vector<std::vector<ControlState>> states_; // sized from layout_
    KeypadModifiers modifiers_;
    uint32_t generation_ = 0;
    PadState pad_;
};

} // namespace controls
