// router.cpp - see router.h.
#include "router.h"

#include "../keypad_layout.h"

namespace controls {

namespace {
// The group named `id`, or -1. A Toggle's target may name none (it then
// names a layout, or "next").
int group_named(const Layout &l, const std::string &id) {
    for (size_t i = 0; i < l.groups.size(); ++i)
        if (l.groups[i].id == id)
            return int(i);
    return -1;
}
} // namespace

void Router::set_layout(Layout *layout, ControlsSink &sink) {
    cancel_all(sink); // releases everything held against the old layout
    layout_ = layout;
    states_.clear();
    if (layout_) {
        states_.resize(layout_->groups.size());
        for (size_t g = 0; g < layout_->groups.size(); ++g)
            states_[g].resize(layout_->groups[g].controls.size());
    }
    ++generation_;
}

void Router::set_screen(const Screen &s) {
    screen_ = s;
}

void Router::set_claim_area(const Rect &area) {
    claim_area_ = area;
}

void Router::set_enabled(bool on, ControlsSink &sink) {
    enabled_ = on;
    if (!on)
        cancel_all(sink); // releases everything held, and bumps generation
    else
        ++generation_;
}

bool Router::enabled() const {
    return enabled_;
}

void Router::set_pressed(int group, int control, bool pressed) {
    if (group < 0 || group >= int(states_.size()))
        return;
    if (control < 0 || control >= int(states_[group].size()))
        return;
    states_[group][control].pressed = pressed;
}

void Router::key_down(const Control &c, uint64_t now_ns, ControlsSink &sink) {
    if (keypad_is_modifier(c.scancode)) {
        std::vector<KeypadKeyEvent> events;
        modifiers_.press(c.scancode, now_ns, &events);
        for (const KeypadKeyEvent &e : events)
            sink.key(e.scancode, e.down);
    } else {
        sink.key(c.scancode, true);
    }
}

void Router::key_up(const Control &c, uint64_t now_ns, ControlsSink &sink) {
    if (keypad_is_modifier(c.scancode)) {
        std::vector<KeypadKeyEvent> events;
        modifiers_.release(c.scancode, now_ns, &events);
        for (const KeypadKeyEvent &e : events)
            sink.key(e.scancode, e.down);
    } else {
        sink.key(c.scancode, false);
        std::vector<KeypadKeyEvent> events;
        modifiers_.key_lifted(now_ns, &events);
        for (const KeypadKeyEvent &e : events)
            sink.key(e.scancode, e.down);
    }
}

void Router::key_cancel(const Control &c, ControlsSink &sink) {
    if (keypad_is_modifier(c.scancode)) {
        std::vector<KeypadKeyEvent> events;
        modifiers_.cancel(c.scancode, &events);
        for (const KeypadKeyEvent &e : events)
            sink.key(e.scancode, e.down);
    } else {
        sink.key(c.scancode, false); // no key_lifted: a cancelled key releases nothing else
    }
}

bool Router::finger_down(int64_t id, double px, double py, uint64_t now_ns, ControlsSink &sink) {
    if (!enabled_ || !layout_)
        return false;
    const Hit h = hit_test(*layout_, screen_, px, py);
    if (h.group < 0) {
        if (!claim_area_.contains(px, py))
            return false; // the game's own area
        fingers_[id] = Owned{-1, -1, true};
        return true; // the controls area: claimed, and does nothing
    }

    if (h.gap) {
        fingers_[id] = Owned{h.group, -1, true};
        return true; // claimed; a gap does nothing
    }

    fingers_[id] = Owned{h.group, h.control, false};
    const Control &c = layout_->groups[h.group].controls[h.control];
    set_pressed(h.group, h.control, true);
    ++generation_;

    switch (c.kind) {
    case Kind::Key:
        key_down(c, now_ns, sink);
        break;
    case Kind::Toggle: {
        const int target = group_named(*layout_, c.target);
        if (target >= 0) {
            layout_->groups[target].visible = !layout_->groups[target].visible;
            sink.group_visibility_changed();
        } else {
            sink.switch_layout(c.target);
        }
        break;
    }
    case Kind::Action:
        sink.action(c.action);
        break;
    case Kind::Button:
    case Kind::Dpad:
    case Kind::Stick:
        break; // sticks and the dpad get their motion handling in Task 9
    }
    sink.tap();
    return true;
}

bool Router::finger_motion(int64_t id, double, double, uint64_t, ControlsSink &) {
    // Keys, buttons, toggles and actions ignore motion; sticks and the dpad
    // (Task 9) will need it to track a dragging knob or hat direction.
    return fingers_.count(id) != 0;
}

bool Router::finger_up(int64_t id, uint64_t now_ns, ControlsSink &sink) {
    const auto it = fingers_.find(id);
    if (it == fingers_.end())
        return false;
    const Owned o = it->second;
    fingers_.erase(it);
    if (o.gap)
        return true;

    set_pressed(o.group, o.control, false);
    ++generation_;
    const Control &c = layout_->groups[o.group].controls[o.control];
    if (c.kind == Kind::Key)
        key_up(c, now_ns, sink);
    return true;
}

bool Router::finger_cancel(int64_t id, ControlsSink &sink) {
    const auto it = fingers_.find(id);
    if (it == fingers_.end())
        return false;
    const Owned o = it->second;
    fingers_.erase(it);
    if (o.gap)
        return true;

    set_pressed(o.group, o.control, false);
    ++generation_;
    const Control &c = layout_->groups[o.group].controls[o.control];
    if (c.kind == Kind::Key)
        key_cancel(c, sink);
    return true;
}

void Router::cancel_all(ControlsSink &sink) {
    // Every held non-modifier key releases directly; the modifier slots
    // release themselves below, whether or not a finger still holds them.
    for (const auto &kv : fingers_) {
        const Owned &o = kv.second;
        if (o.gap || o.group < 0 || o.control < 0)
            continue;
        set_pressed(o.group, o.control, false);
        const Control &c = layout_->groups[o.group].controls[o.control];
        if (c.kind == Kind::Key && !keypad_is_modifier(c.scancode))
            sink.key(c.scancode, false);
    }
    fingers_.clear();

    std::vector<KeypadKeyEvent> events;
    modifiers_.cancel_all(&events);
    for (const KeypadKeyEvent &e : events)
        sink.key(e.scancode, e.down);

    ++generation_;
}

bool Router::owns(int64_t id) const {
    return fingers_.count(id) != 0;
}

unsigned Router::lit() const {
    return modifiers_.lit();
}

const ControlState &Router::state(int group, int control) const {
    static const ControlState kZero;
    if (group < 0 || group >= int(states_.size()))
        return kZero;
    if (control < 0 || control >= int(states_[group].size()))
        return kZero;
    return states_[group][control];
}

uint32_t Router::generation() const {
    return generation_;
}

const Layout *Router::layout() const {
    return layout_;
}

} // namespace controls
