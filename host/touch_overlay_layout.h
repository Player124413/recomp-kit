// touch_overlay_layout.h - where the on-screen keys are, in drawable pixels.
//
// A strip along the bottom edge: Esc, F10, the four arrows, Space, Enter. The
// same numbers place the drawing and answer the hit test, so a finger that
// lands on a drawn key is the key that fires. Pure: no SDL, no GPU.
#pragma once
#include <vector>

struct TouchKey {
    const char *label; // what is drawn
    int scancode;      // SDL_Scancode value sent while the key is held
    int x, y, w, h;    // drawable pixels
};

// The bar's canvas: keys are laid out on a fixed logical grid and scaled to
// the drawable width, so the raster and the hit test share one mapping.
constexpr int kTouchBarKeys = 8;
constexpr int kTouchBarKeyWidth = 96;                             // logical pixels per key
constexpr int kTouchBarHeight = 56;                               // logical pixels
constexpr int kTouchBarWidth = kTouchBarKeys * kTouchBarKeyWidth; // 768

// Scale from the logical canvas to a drawable `w` pixels wide.
double touch_overlay_scale(int w);
// Height of the bar in drawable pixels for a drawable `w` wide.
int touch_overlay_height(int w);
// The keys for a drawable of w x h pixels, bottom-aligned.
void touch_overlay_layout(int w, int h, std::vector<TouchKey> *out);
// The scancode under (px, py) in drawable pixels, or 0 when no key is there.
int touch_overlay_hit(int w, int h, double px, double py);
