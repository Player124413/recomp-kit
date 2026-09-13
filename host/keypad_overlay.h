// keypad_overlay.h - draws the split keypad (keypad_layout.h) onto the
// drawable: two premultiplied RGBA rasters, one per half with its tab,
// uploaded when the view or the drawable changes, blended with the hud
// pipeline. Owned by the presenter worker; touched only on its thread.
#pragma once
#include "gpu/gpu.h"
#include "keypad_layout.h"

#include <cstdint>
#include <vector>

class KeypadOverlay {
  public:
    ~KeypadOverlay();
    void draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
              const KeypadView &view);

  private:
    struct Half {
        gpu::Texture texture;
        KeypadRect rect; // where it is drawn, in drawable pixels (the half and its tab)
        int w = 0, h = 0;
        // What the raster was built for.
        bool shown = false;
        int size = -1;
        unsigned lit = 0;
        double scale = 0;
        int dw = 0, dh = 0;
    };
    void update(gpu::Device *device, KeypadSide side, const KeypadView &view, int dw, int dh);
    void blit(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
              const Half &half);
    gpu::Device *device_ = nullptr;
    Half halves_[2];
    std::vector<uint8_t> pixels_;
};
