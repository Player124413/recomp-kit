// touch_overlay_layout.cpp - see touch_overlay_layout.h.
#include "touch_overlay_layout.h"

namespace {
struct Slot {
    const char *label;
    int scancode;
};
const Slot kSlots[kTouchBarKeys] = {
    {"ESC", kTouchScanEscape},  {"F10", kTouchScanF10},      {"<", kTouchScanLeft},
    {"^", kTouchScanUp},        {"v", kTouchScanDown},       {">", kTouchScanRight},
    {"SPACE", kTouchScanSpace}, {"ENTER", kTouchScanReturn},
};
} // namespace

double touch_overlay_scale(int w) {
    return w > 0 ? double(w) / kTouchBarWidth : 1.0;
}

int touch_overlay_height(int w, bool collapsed) {
    return int((collapsed ? kTouchBarTabHeight : kTouchBarHeight) * touch_overlay_scale(w) + 0.5);
}

void touch_overlay_layout(int w, int h, bool collapsed, std::vector<TouchKey> *out) {
    out->clear();
    if (w <= 0 || h <= 0)
        return;
    const double scale = touch_overlay_scale(w);
    const int bar_h = touch_overlay_height(w, collapsed);
    const int y = h - bar_h;
    const int toggle_x = int(kTouchBarKeys * kTouchBarKeyWidth * scale + 0.5);
    if (collapsed) {
        out->push_back({"KEYS", kTouchOverlayToggle, toggle_x, y, w - toggle_x, bar_h});
        return;
    }
    for (int i = 0; i < kTouchBarKeys; ++i) {
        const int x0 = int(i * kTouchBarKeyWidth * scale + 0.5);
        const int x1 = int((i + 1) * kTouchBarKeyWidth * scale + 0.5);
        out->push_back({kSlots[i].label, kSlots[i].scancode, x0, y, x1 - x0, bar_h});
    }
    out->push_back({"HIDE", kTouchOverlayToggle, toggle_x, y, w - toggle_x, bar_h});
}

int touch_overlay_hit(int w, int h, bool collapsed, double px, double py) {
    if (w <= 0 || h <= 0 || px < 0 || py < 0 || px >= w || py >= h)
        return 0;
    if (py < h - touch_overlay_height(w, collapsed))
        return 0;
    const double scale = touch_overlay_scale(w);
    if (px >= kTouchBarKeys * kTouchBarKeyWidth * scale)
        return kTouchOverlayToggle;
    if (collapsed)
        return 0; // the rest of the strip's area belongs to the game
    const int i = int(px / (kTouchBarKeyWidth * scale));
    return i >= 0 && i < kTouchBarKeys ? kSlots[i].scancode : 0;
}
