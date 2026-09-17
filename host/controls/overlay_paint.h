// overlay_paint.h - the SDL/GPU-free half of Overlay: turns a ControlsView
// into pixels on a Canvas. Split out of overlay.cpp so controls_tests can
// exercise the exact pixels the app draws without linking a GPU backend.
#pragma once

#include "overlay.h"
#include "raster.h"

namespace controls {

// The keypad's look: a translucent backdrop behind each half, flat keys with
// light text that invert when lit, and tabs framed in the backdrop colour.
// Every fill and the text are `replace`d, not composited, so a key at alpha
// 220 overwrites the backdrop drawn under it at alpha 150 rather than
// blending with it. `r` is the raster's place on the drawable, so controls
// are drawn at (x - r.x, y - r.y).
void paint_overlay(Canvas &c, const ControlsView &view, const Rect &r);

} // namespace controls
