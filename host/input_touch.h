// input_touch.h - fingers become the mouse and keys the game already reads.
//
// A pure gesture state machine: it sees finger down/motion/up in window points
// plus a clock, and emits the mouse motion, mouse buttons and key taps that
// the host's existing input path consumes. No SDL calls, so the tests drive
// it with plain numbers. The gesture table is in the M1 iOS design spec.
#pragma once
#include <stdint.h>

#include <vector>

// Gesture thresholds, in window points and nanoseconds.
constexpr double kTouchTapTravel = 12.0;
constexpr uint64_t kTouchLongPressNs = 350ull * 1000000ull;
constexpr double kTouchPanStep = 24.0;
// A synthesized click stays pressed this long: a game that samples its mouse
// buttons once per frame never sees a press and release inside one frame.
constexpr uint64_t kTouchClickHoldNs = 90ull * 1000000ull;

struct TouchPoint {
    int64_t id;
    double x, y; // window points
};

struct TouchAction {
    enum Kind { Motion, Button, Key } kind;
    // Motion only: also place the game's own cursor here. True for a press
    // and for a left drag; false while the right button is held, when the
    // game is in its camera mode and reads relative movement instead.
    bool place = true;
    double x = 0, y = 0; // Motion, Button
    int button = 0;      // Button: 0 left, 1 right
    bool down = false;   // Button, Key
    int scancode = 0;    // Key: an SDL_Scancode value
};

class TouchMapper {
  public:
    void finger_down(TouchPoint p, uint64_t now_ns, std::vector<TouchAction> *out);
    void finger_motion(TouchPoint p, uint64_t now_ns, std::vector<TouchAction> *out);
    void finger_up(TouchPoint p, uint64_t now_ns, std::vector<TouchAction> *out);
    // Fires time-based gestures (long press). Call once per pump.
    void tick(uint64_t now_ns, std::vector<TouchAction> *out);
    // The system took the finger away (SDL_EVENT_FINGER_CANCELED): forget it
    // without a click; a drag it was holding is released.
    void finger_cancel(int64_t id, std::vector<TouchAction> *out);
    // Focus loss or backgrounding: every finger is gone. Releases anything held.
    void cancel_all(std::vector<TouchAction> *out);
    // Toggled by a four-finger tap; the host shows or hides the keyboard.
    bool text_input_wanted() const {
        return text_input_;
    }

  private:
    struct Finger {
        int64_t id;
        double x0, y0, x, y;
        uint64_t t0;
    };
    std::vector<Finger> fingers_; // currently down, in order of arrival
    int max_fingers_ = 0;         // most fingers down during this gesture
    bool dragging_ = false;       // one-finger drag in progress (left held)
    bool long_fired_ = false;     // long press already emitted for this gesture
    bool right_held_ = false;     // the long press's right button is down until the finger lifts
    uint64_t right_down_ = 0;
    bool text_input_ = false;
    double pan_cx_ = 0, pan_cy_ = 0, pan_acc_x_ = 0, pan_acc_y_ = 0;
    // A click's release, held back until kTouchClickHoldNs after its press.
    bool release_pending_ = false;
    int release_button_ = 0;
    double release_x_ = 0, release_y_ = 0;
    uint64_t release_due_ = 0;
    void click(std::vector<TouchAction> *out, int button, double x, double y, uint64_t now);
    double centroid_x() const;
    double centroid_y() const;
    void reset_gesture();
};
