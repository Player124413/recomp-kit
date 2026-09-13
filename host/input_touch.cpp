// input_touch.cpp - see input_touch.h for the gesture table.
#include "input_touch.h"

#include <SDL3/SDL_scancode.h>
#include <math.h>

namespace {
void motion(std::vector<TouchAction> *out, double x, double y) {
    TouchAction a;
    a.kind = TouchAction::Motion;
    a.x = x;
    a.y = y;
    out->push_back(a);
}
void button(std::vector<TouchAction> *out, int b, bool down, double x, double y) {
    TouchAction a;
    a.kind = TouchAction::Button;
    a.button = b;
    a.down = down;
    a.x = x;
    a.y = y;
    out->push_back(a);
}
void key_tap(std::vector<TouchAction> *out, int scancode) {
    TouchAction a;
    a.kind = TouchAction::Key;
    a.scancode = scancode;
    a.down = true;
    out->push_back(a);
    a.down = false;
    out->push_back(a);
}
double dist(double ax, double ay, double bx, double by) {
    return hypot(ax - bx, ay - by);
}
} // namespace

double TouchMapper::centroid_x() const {
    double s = 0;
    for (const Finger &f : fingers_)
        s += f.x;
    return fingers_.empty() ? 0 : s / fingers_.size();
}
double TouchMapper::centroid_y() const {
    double s = 0;
    for (const Finger &f : fingers_)
        s += f.y;
    return fingers_.empty() ? 0 : s / fingers_.size();
}
// Press now; the release follows from tick() once the hold time has passed.
void TouchMapper::click(std::vector<TouchAction> *out, int b, double x, double y, uint64_t now) {
    motion(out, x, y);
    button(out, b, true, x, y);
    release_pending_ = true;
    release_button_ = b;
    release_x_ = x;
    release_y_ = y;
    release_due_ = now + kTouchClickHoldNs;
}

void TouchMapper::reset_gesture() {
    max_fingers_ = 0;
    dragging_ = false;
    long_fired_ = false;
    pan_acc_x_ = pan_acc_y_ = 0;
}

void TouchMapper::finger_down(TouchPoint p, uint64_t now, std::vector<TouchAction> *out) {
    if (release_pending_) {
        // A new finger before the last click released: release it first so
        // the two clicks stay distinct.
        release_pending_ = false;
        button(out, release_button_, false, release_x_, release_y_);
    }
    if (fingers_.empty())
        reset_gesture();
    fingers_.push_back({p.id, p.x, p.y, p.x, p.y, now});
    if ((int)fingers_.size() > max_fingers_)
        max_fingers_ = (int)fingers_.size();
    if (fingers_.size() == 2) {
        // A second finger ends any one-finger gesture: release a drag, forget a tap.
        if (dragging_) {
            button(out, 0, false, fingers_[0].x, fingers_[0].y);
            dragging_ = false;
        }
        pan_cx_ = centroid_x();
        pan_cy_ = centroid_y();
        pan_acc_x_ = pan_acc_y_ = 0;
    }
}

void TouchMapper::finger_motion(TouchPoint p, uint64_t, std::vector<TouchAction> *out) {
    for (Finger &f : fingers_)
        if (f.id == p.id) {
            f.x = p.x;
            f.y = p.y;
        }
    if (fingers_.size() == 1 && max_fingers_ == 1 && !long_fired_) {
        Finger &f = fingers_[0];
        if (!dragging_ && dist(f.x0, f.y0, f.x, f.y) > kTouchTapTravel) {
            dragging_ = true;
            motion(out, f.x0, f.y0);
            button(out, 0, true, f.x0, f.y0);
        }
        if (dragging_)
            motion(out, f.x, f.y);
        return;
    }
    if (fingers_.size() == 2 && max_fingers_ == 2) {
        const double cx = centroid_x(), cy = centroid_y();
        pan_acc_x_ += cx - pan_cx_;
        pan_acc_y_ += cy - pan_cy_;
        pan_cx_ = cx;
        pan_cy_ = cy;
        while (fabs(pan_acc_x_) >= kTouchPanStep || fabs(pan_acc_y_) >= kTouchPanStep) {
            if (fabs(pan_acc_x_) >= fabs(pan_acc_y_)) {
                key_tap(out, pan_acc_x_ > 0 ? SDL_SCANCODE_RIGHT : SDL_SCANCODE_LEFT);
                pan_acc_x_ -= pan_acc_x_ > 0 ? kTouchPanStep : -kTouchPanStep;
            } else {
                key_tap(out, pan_acc_y_ > 0 ? SDL_SCANCODE_DOWN : SDL_SCANCODE_UP);
                pan_acc_y_ -= pan_acc_y_ > 0 ? kTouchPanStep : -kTouchPanStep;
            }
        }
    }
}

void TouchMapper::finger_up(TouchPoint p, uint64_t now, std::vector<TouchAction> *out) {
    Finger lifted{p.id, p.x, p.y, p.x, p.y, now};
    for (size_t i = 0; i < fingers_.size(); ++i)
        if (fingers_[i].id == p.id) {
            lifted = fingers_[i];
            lifted.x = p.x;
            lifted.y = p.y;
            fingers_.erase(fingers_.begin() + (long)i);
            break;
        }
    if (!fingers_.empty())
        return; // the gesture ends when the last finger lifts
    const bool moved = dist(lifted.x0, lifted.y0, lifted.x, lifted.y) > kTouchTapTravel;
    if (max_fingers_ == 1) {
        if (dragging_) {
            motion(out, lifted.x, lifted.y);
            button(out, 0, false, lifted.x, lifted.y);
        } else if (!long_fired_ && !moved && now - lifted.t0 < kTouchLongPressNs) {
            click(out, 0, lifted.x, lifted.y, now);
        }
    } else if (max_fingers_ == 2 && fabs(pan_acc_x_) < kTouchPanStep &&
               fabs(pan_acc_y_) < kTouchPanStep && !moved) {
        key_tap(out, SDL_SCANCODE_ESCAPE);
    } else if (max_fingers_ == 3 && !moved) {
        key_tap(out, SDL_SCANCODE_F10);
    } else if (max_fingers_ == 4 && !moved) {
        text_input_ = !text_input_;
    }
    reset_gesture();
}

void TouchMapper::finger_cancel(int64_t id, std::vector<TouchAction> *out) {
    for (size_t i = 0; i < fingers_.size(); ++i)
        if (fingers_[i].id == id) {
            fingers_.erase(fingers_.begin() + (long)i);
            break;
        }
    if (!fingers_.empty())
        return;
    if (dragging_)
        button(out, 0, false, 0, 0);
    reset_gesture();
}

void TouchMapper::cancel_all(std::vector<TouchAction> *out) {
    if (release_pending_) {
        release_pending_ = false;
        button(out, release_button_, false, release_x_, release_y_);
    }
    if (dragging_)
        button(out, 0, false, 0, 0);
    fingers_.clear();
    reset_gesture();
}

void TouchMapper::tick(uint64_t now, std::vector<TouchAction> *out) {
    if (release_pending_ && now >= release_due_) {
        release_pending_ = false;
        button(out, release_button_, false, release_x_, release_y_);
    }
    if (fingers_.size() != 1 || max_fingers_ != 1 || dragging_ || long_fired_)
        return;
    const Finger &f = fingers_[0];
    if (now - f.t0 >= kTouchLongPressNs && dist(f.x0, f.y0, f.x, f.y) <= kTouchTapTravel) {
        long_fired_ = true;
        click(out, 1, f.x, f.y, now);
    }
}
