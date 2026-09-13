// touch_overlay_layout.cpp - see touch_overlay_layout.h.
#include "touch_overlay_layout.h"

#include <SDL3/SDL_scancode.h>

namespace {
struct Slot {
    const char *label;
    int scancode;
};
const Slot kSlots[kTouchBarKeys] = {
    {"ESC", SDL_SCANCODE_ESCAPE},  {"F10", SDL_SCANCODE_F10},      {"<", SDL_SCANCODE_LEFT},
    {"^", SDL_SCANCODE_UP},        {"v", SDL_SCANCODE_DOWN},       {">", SDL_SCANCODE_RIGHT},
    {"SPACE", SDL_SCANCODE_SPACE}, {"ENTER", SDL_SCANCODE_RETURN},
};
} // namespace

double touch_overlay_scale(int w) {
    return w > 0 ? double(w) / kTouchBarWidth : 1.0;
}

int touch_overlay_height(int w) {
    return int(kTouchBarHeight * touch_overlay_scale(w) + 0.5);
}

void touch_overlay_layout(int w, int h, std::vector<TouchKey> *out) {
    out->clear();
    if (w <= 0 || h <= 0)
        return;
    const double scale = touch_overlay_scale(w);
    const int bar_h = touch_overlay_height(w);
    const int y = h - bar_h;
    for (int i = 0; i < kTouchBarKeys; ++i) {
        const int x0 = int(i * kTouchBarKeyWidth * scale + 0.5);
        const int x1 = int((i + 1) * kTouchBarKeyWidth * scale + 0.5);
        out->push_back({kSlots[i].label, kSlots[i].scancode, x0, y, x1 - x0, bar_h});
    }
}

int touch_overlay_hit(int w, int h, double px, double py) {
    if (w <= 0 || h <= 0 || px < 0 || py < 0 || px >= w || py >= h)
        return 0;
    if (py < h - touch_overlay_height(w))
        return 0;
    const int i = int(px / (kTouchBarKeyWidth * touch_overlay_scale(w)));
    return i >= 0 && i < kTouchBarKeys ? kSlots[i].scancode : 0;
}
