// haptics.h - which motor a rumble command actually drives: a connected
// controller's own motors when there is one, else the touch device's motor,
// else nowhere. The routing itself (reading vpad().rumble(), calling
// gamepad_rumble or platform_ui_device_rumble) is host/controls/controls_host.cpp's.
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md.
#pragma once

namespace controls {

enum class RumbleSink { None, Controller, Device };

// controller_connected: a gamepad is open and can rumble itself.
// touch_device: this platform has a device motor to fall back to (iOS/Android).
RumbleSink rumble_sink(bool controller_connected, bool touch_device);

} // namespace controls
