// raster.h - a small premultiplied-RGBA canvas the controls overlay draws
// into on the CPU: flat fills, a flat disc and the 6x8 font at 2x. Moved out
// of the old keypad overlay; the DualSense look (Task 8) extends it.
#pragma once

#include <cstdint>
#include <vector>

namespace controls {

// Writes (does not blend) premultiplied pixels into `px`, a w x h RGBA
// buffer the caller owns; everything is clipped to the canvas.
struct Canvas {
    std::vector<uint8_t> &px;
    int w, h;

    void fill(int x, int y, int fw, int fh, int r, int g, int b, int a);
    // A filled circle centred on (cx, cy), in whole pixels.
    void disc(int cx, int cy, int radius, int r, int g, int b, int a);
    // 6x8 glyphs at 2x: 12 pixels per column, 16 per row (text_width).
    void text(int x, int y, const char *str, int r, int g, int b, int a);
};

int text_width(const char *str);

} // namespace controls
