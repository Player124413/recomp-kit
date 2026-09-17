// haptics.cpp - see haptics.h.
#include "haptics.h"

namespace controls {

RumbleSink rumble_sink(bool controller_connected, bool touch_device) {
    if (controller_connected)
        return RumbleSink::Controller;
    if (touch_device)
        return RumbleSink::Device;
    return RumbleSink::None;
}

} // namespace controls
