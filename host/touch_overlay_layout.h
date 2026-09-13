// touch_overlay_layout.h - where the on-screen keys are, in drawable pixels.
//
// A strip along the bottom edge: Esc, F10, the four arrows, Space, Enter, and
// a HIDE key at the right end. Hidden, the strip collapses to a small tab in
// the bottom-right corner that brings it back. The same numbers place the
// drawing and answer the hit test, so a finger that lands on a drawn key is
// the key that fires. Pure: no SDL, no GPU.
#pragma once
#include <vector>

struct TouchKey {
    const char *label; // what is drawn
    int scancode;      // SDL_Scancode value sent while the key is held, or kTouchOverlayToggle
    int x, y, w, h;    // drawable pixels
};

// The hit that shows or hides the strip rather than pressing a key.
constexpr int kTouchOverlayToggle = -1;

// The keys' SDL_Scancode values, spelled out so this file needs no SDL header
// (hosts that never link SDL compile it). input_touch_tests.cpp asserts they
// match SDL's enumerators.
enum TouchScancode {
    kTouchScanReturn = 40,
    kTouchScanEscape = 41,
    kTouchScanSpace = 44,
    kTouchScanF10 = 67,
    kTouchScanRight = 79,
    kTouchScanLeft = 80,
    kTouchScanDown = 81,
    kTouchScanUp = 82,
};

// The strip's canvas: keys on a fixed logical grid, scaled to the drawable
// width, so the raster and the hit test share one mapping.
constexpr int kTouchBarKeys = 8;
constexpr int kTouchBarKeyWidth = 96;    // logical pixels per key
constexpr int kTouchBarToggleWidth = 64; // the HIDE key and the collapsed tab
constexpr int kTouchBarHeight = 56;      // logical pixels
constexpr int kTouchBarTabHeight = 24;   // the collapsed tab
constexpr int kTouchBarWidth = kTouchBarKeys * kTouchBarKeyWidth + kTouchBarToggleWidth; // 832

// Scale from the logical canvas to a drawable `w` pixels wide.
double touch_overlay_scale(int w);
// Height in drawable pixels of the strip (or, collapsed, of the tab).
int touch_overlay_height(int w, bool collapsed);
// The keys for a drawable of w x h pixels, bottom-aligned. Collapsed: one
// entry, the tab, whose scancode is kTouchOverlayToggle.
void touch_overlay_layout(int w, int h, bool collapsed, std::vector<TouchKey> *out);
// What is under (px, py) in drawable pixels: a scancode, kTouchOverlayToggle,
// or 0 when nothing is there and the touch belongs to the game.
int touch_overlay_hit(int w, int h, bool collapsed, double px, double py);
