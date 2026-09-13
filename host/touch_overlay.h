// touch_overlay.h - draws the on-screen key bar (touch_overlay_layout.h) onto
// the drawable, the way the performance overlay draws its panel: a premultiplied
// RGBA raster uploaded once per size, blended over the frame with the hud
// pipeline. Owned by the presenter worker; touched only on its thread.
#pragma once
#include "gpu/gpu.h"

#include <cstdint>
#include <vector>

class TouchOverlay {
  public:
    ~TouchOverlay();
    void draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h);

  private:
    void update(gpu::Device *device, int w);
    gpu::Device *device_ = nullptr;
    gpu::Texture texture_;
    int texture_w_ = 0, texture_h_ = 0;
    std::vector<uint8_t> pixels_;
};
