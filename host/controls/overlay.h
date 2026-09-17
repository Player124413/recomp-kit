// overlay.h - draws the on-screen controls onto the drawable. make_view turns
// a layout and the router's live state into a flat list of what to draw (no
// GPU; overlay_view.cpp, unit-tested); Overlay rasterizes that list into one
// premultiplied RGBA texture and blends it with the hud pipeline, on the
// presenter worker's thread only.
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md, 8.1.
#pragma once

#include "layout.h"

#include <cstdint>
#include <string>
#include <vector>

#include "../gpu/gpu.h"

namespace controls {

class Router;

// One control, resolved for drawing.
struct DrawControl {
    Kind kind = Kind::Key;
    Rect rect;
    std::string label;
    bool pressed = false;
    bool lit = false; // a latched or locked modifier key
    PadButton button = PadButton::Cross;
    double knob_x = 0, knob_y = 0;
    double base_x = 0, base_y = 0;
    uint8_t hat = 0;
    bool floating = false;
    bool group_visible = true; // Toggle: its target group is shown (else label_off is drawn)
    std::string label_off;
};

struct ControlsView {
    bool wanted = false;   // draw anything at all
    uint64_t revision = 0; // changes when anything below changes
    int dw = 0, dh = 0;
    double opacity = 1.0;
    std::vector<DrawControl> controls;
    std::vector<Rect> backdrops;      // visible grid groups' boxes, drawn under their keys
    Rect controls_area;               // portrait: fill with the backdrop colour (Task 17)
    bool editing = false;             // Task 20
    std::vector<Rect> guides;         // Task 20
    int selected = -1;                // Task 20
    std::vector<DrawControl> toolbar; // Task 20
};

// Every control a player can see, in layout order: visible groups' controls,
// and every toggle whether or not its own group is shown. `revision` is a
// hash of everything in the returned view (wanted aside), so two calls that
// would draw the same pixels return the same revision.
ControlsView make_view(const Layout &l, const Router &r, const Screen &s, double opacity);

class Overlay {
  public:
    ~Overlay();
    // Re-rasterizes when the view's revision or the drawable size changes,
    // then blends the raster over `target`. Nothing while !view.wanted.
    void draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
              const ControlsView &view);

  private:
    void update(gpu::Device *device, const ControlsView &view, int w, int h);
    void blit(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h);

    gpu::Device *device_ = nullptr;
    gpu::Texture texture_;
    int tex_w_ = 0, tex_h_ = 0;
    Rect rect_; // where the raster is drawn, in drawable pixels
    // What the raster was built for.
    bool built_ = false;
    uint64_t revision_ = 0;
    int dw_ = 0, dh_ = 0;
    std::vector<uint8_t> pixels_;
};

} // namespace controls
