// input_touch.cpp - see input_touch.h for the gesture table.
#include "input_touch.h"

#include <SDL3/SDL_scancode.h>
#include <math.h>

namespace {
void motion(std::vector<TouchAction> *out, double x, double y, bool place = true) {
    TouchAction a;
    a.kind = TouchAction::Motion;
    a.place = place;
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

void TouchMapper::set_bounds(double w, double h) {
    bounds_w_ = w;
    bounds_h_ = h;
}

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

void TouchMapper::set_edge_insets(double left, double top, double right, double bottom) {
    inset_l_ = left;
    inset_t_ = top;
    inset_r_ = right;
    inset_b_ = bottom;
}

// A placed motion: the point, snapped onto a window edge when the finger is
// within kTouchEdgeMargin of one, plus whatever strip the system keeps along
// that edge. Remembers whether it landed on an edge.
void TouchMapper::place(std::vector<TouchAction> *out, double x, double y) {
    bool at_edge = false;
    if (bounds_w_ > 0 && bounds_h_ > 0) {
        if (x < kTouchEdgeMargin + inset_l_) {
            x = 0;
            at_edge = true;
        } else if (x > bounds_w_ - 1 - kTouchEdgeMargin - inset_r_) {
            x = bounds_w_ - 1;
            at_edge = true;
        }
        if (y < kTouchEdgeMargin + inset_t_) {
            y = 0;
            at_edge = true;
        } else if (y > bounds_h_ - 1 - kTouchEdgeMargin - inset_b_) {
            y = bounds_h_ - 1;
            at_edge = true;
        }
    }
    snapped_ = at_edge;
    placed_x_ = x;
    placed_y_ = y;
    motion(out, x, y);
}

// Press now; the release follows from tick() once the hold time has passed.
void TouchMapper::click(std::vector<TouchAction> *out, int b, double x, double y, uint64_t now) {
    place(out, x, y);
    // Copied out before the next push: a reference into the vector would not
    // survive the reallocation, and the release would carry whatever was left.
    const double px = out->back().x, py = out->back().y;
    button(out, b, true, px, py);
    release_pending_ = true;
    release_button_ = b;
    release_x_ = px;
    release_y_ = py;
    release_due_ = now + kTouchClickHoldNs;
    release_deadline_ = now + kTouchClickHoldMaxNs;
    presents_at_press_ = presents_;
}

void TouchMapper::frames_presented(uint32_t count) {
    presents_known_ = true;
    presents_ = count;
}

bool TouchMapper::release_ready(uint64_t now) const {
    if (now < release_due_)
        return false;
    if (!presents_known_ || now >= release_deadline_)
        return true;
    return presents_ - presents_at_press_ >= kTouchClickHoldFrames;
}

// The gesture ended on an edge: once nothing is held any more, move the cursor
// back inside so the game stops scrolling.
void TouchMapper::end_edge_hold(std::vector<TouchAction> *out) {
    if (!snapped_)
        return;
    snapped_ = false;
    double x = placed_x_, y = placed_y_;
    if (x <= 0)
        x = kTouchEdgeRelease;
    else if (x >= bounds_w_ - 1)
        x = bounds_w_ - 1 - kTouchEdgeRelease;
    if (y <= 0)
        y = kTouchEdgeRelease;
    else if (y >= bounds_h_ - 1)
        y = bounds_h_ - 1 - kTouchEdgeRelease;
    if (release_pending_) {
        nudge_pending_ = true;
        nudge_x_ = x;
        nudge_y_ = y;
        return;
    }
    nudge_pending_ = false;
    motion(out, x, y);
}

void TouchMapper::release_held(std::vector<TouchAction> *out) {
    release_pending_ = false;
    button(out, release_button_, false, release_x_, release_y_);
    if (nudge_pending_) {
        nudge_pending_ = false;
        motion(out, nudge_x_, nudge_y_);
    }
}

void TouchMapper::reset_gesture() {
    max_fingers_ = 0;
    dragging_ = false;
    long_fired_ = false;
    middle_held_ = false;
    pan_acc_x_ = pan_acc_y_ = 0;
}

void TouchMapper::finger_down(TouchPoint p, uint64_t now, std::vector<TouchAction> *out) {
    if (release_pending_) {
        // A new finger before the last click released: release it first so
        // the two clicks stay distinct.
        release_held(out);
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
        if (middle_held_) {
            button(out, 2, false, fingers_[0].x, fingers_[0].y);
            middle_held_ = false;
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
    if (fingers_.size() == 1 && max_fingers_ == 1) {
        Finger &f = fingers_[0];
        if (middle_held_) {
            motion(out, f.x, f.y, false); // wheel-button drag: deltas only
            return;
        }
        if (long_fired_) {
            // A drag after the long press scrolls the map the way the game's
            // own wheel-button drag does. The press point was placed already.
            if (dist(f.x0, f.y0, f.x, f.y) > kTouchTapTravel) {
                middle_held_ = true;
                snapped_ = false;
                button(out, 2, true, f.x0, f.y0);
                motion(out, f.x, f.y, false);
            }
            return;
        }
        if (!dragging_ && dist(f.x0, f.y0, f.x, f.y) > kTouchTapTravel) {
            dragging_ = true;
            place(out, f.x0, f.y0);
            const double px = out->back().x, py = out->back().y;
            button(out, 0, true, px, py);
        }
        if (dragging_)
            place(out, f.x, f.y);
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
        if (middle_held_) {
            motion(out, lifted.x, lifted.y, false);
            button(out, 2, false, lifted.x, lifted.y);
        } else if (dragging_) {
            place(out, lifted.x, lifted.y);
            const double px = out->back().x, py = out->back().y;
            button(out, 0, false, px, py);
        } else if (long_fired_) {
            // A still long press that lifts is a right click, unless it was an
            // edge hold, which only scrolled.
            if (!moved && !snapped_)
                click(out, 1, lifted.x, lifted.y, now);
        } else if (!moved && now - lifted.t0 < kTouchLongPressNs) {
            click(out, 0, lifted.x, lifted.y, now);
        }
        end_edge_hold(out);
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
    // Let go where the cursor was placed: a release carries a position, and a
    // release at the origin would put the game's cursor there.
    if (dragging_)
        button(out, 0, false, placed_x_, placed_y_);
    if (middle_held_)
        button(out, 2, false, placed_x_, placed_y_);
    end_edge_hold(out);
    reset_gesture();
}

void TouchMapper::cancel_all(std::vector<TouchAction> *out) {
    if (release_pending_)
        release_held(out);
    if (dragging_)
        button(out, 0, false, placed_x_, placed_y_);
    if (middle_held_)
        button(out, 2, false, placed_x_, placed_y_);
    fingers_.clear();
    end_edge_hold(out);
    reset_gesture();
}

void TouchMapper::tick(uint64_t now, std::vector<TouchAction> *out) {
    if (release_pending_ && release_ready(now))
        release_held(out);
    if (fingers_.size() != 1 || max_fingers_ != 1 || dragging_ || long_fired_)
        return;
    const Finger &f = fingers_[0];
    if (now - f.t0 >= kTouchLongPressNs && dist(f.x0, f.y0, f.x, f.y) <= kTouchTapTravel) {
        // The finger has rested: put the cursor under it. What follows decides
        // the button - a lift is a right click, a drag a wheel-button drag, and
        // on a window edge the rest itself scrolls the map.
        long_fired_ = true;
        place(out, f.x0, f.y0);
    }
}
