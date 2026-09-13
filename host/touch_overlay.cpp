// touch_overlay.cpp - see touch_overlay.h.
#include "touch_overlay.h"

#include "../mods/mods_internal.h"
#include "touch_overlay_layout.h"

#include <algorithm>
#include <cstring>

namespace {
struct Canvas {
    std::vector<uint8_t> &px;
    int w, h;
    void fill(int x, int y, int fw, int fh, int r, int g, int b, int a) {
        // Premultiplied, as the hud pipeline blends One / OneMinusSrcAlpha.
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(w, x + fw), y1 = std::min(h, y + fh);
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx) {
                uint8_t *p = &px[(size_t(yy) * w + xx) * 4];
                p[0] = uint8_t(r * a / 255);
                p[1] = uint8_t(g * a / 255);
                p[2] = uint8_t(b * a / 255);
                p[3] = uint8_t(a);
            }
    }
    // 6x8 glyphs at 2x: 12 pixels per column, 16 per row.
    void text(int x, int y, const char *str, int r, int g, int b) {
        for (; *str; ++str, x += 12) {
            const uint8_t *glyph = mods_font6x8_glyph(*str);
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 6; ++col)
                    if (glyph[row] & (0x20 >> col))
                        fill(x + col * 2, y + row * 2, 2, 2, r, g, b, 255);
        }
    }
};
} // namespace

TouchOverlay::~TouchOverlay() {
    if (device_ && texture_)
        device_->destroy(texture_);
}

// The bar is rasterized on its logical grid (kTouchBarWidth x kTouchBarHeight)
// and stretched to the drawable width when drawn, so the raster never changes
// with the window and the hit test's mapping is the same stretch.
void TouchOverlay::update(gpu::Device *device, bool collapsed) {
    // Collapsed, only the tab's columns are drawn; the raster keeps the strip's
    // logical width so the stretch is the same and the tab lands where the
    // hit test expects it.
    const int w = kTouchBarWidth, h = collapsed ? kTouchBarTabHeight : kTouchBarHeight;
    pixels_.assign(size_t(w) * h * 4, 0);
    Canvas c{pixels_, w, h};
    std::vector<TouchKey> keys;
    touch_overlay_layout(w, h, collapsed, &keys);
    if (!collapsed)
        c.fill(0, 0, w, h, 6, 9, 15, 150);
    for (const TouchKey &k : keys) {
        c.fill(k.x, k.y, k.w, k.h, 6, 9, 15, 150);
        c.fill(k.x + 3, k.y + 3, k.w - 6, k.h - 6, 40, 48, 64, 200);
        const int text_w = int(strlen(k.label)) * 12;
        c.text(k.x + (k.w - text_w) / 2, k.y + (k.h - 16) / 2, k.label, 235, 242, 255);
    }
    collapsed_ = collapsed;
    if (device_ != device || !texture_ || texture_w_ != w || texture_h_ != h) {
        if (device_ && texture_)
            device_->destroy(texture_);
        device_ = device;
        texture_ = device->create_texture(
            {w, h, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
        texture_w_ = w;
        texture_h_ = h;
    }
    if (texture_)
        device->upload(texture_, {0, 0, w, h}, pixels_.data(), w * 4);
}

void TouchOverlay::draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w,
                        int h, bool collapsed) {
    if (!device || !target || !cb || w <= 0 || h <= 0)
        return;
    if (!texture_ || device_ != device || collapsed_ != collapsed)
        update(device, collapsed);
    if (!texture_)
        return;
    gpu::RenderState state;
    state.color_format[0] = device->describe(target).format;
    state.color_count = 1;
    state.blend_enabled = true;
    state.src_rgb = state.src_alpha = gpu::Blend::One;
    state.dst_rgb = state.dst_alpha = gpu::Blend::OneMinusSrcAlpha;
    gpu::Pipeline pipeline = device->render_pipeline("hud", state);
    if (!pipeline)
        return;
    // Full width along the bottom edge; height from the shared layout.
    const double bar_h = touch_overlay_height(w, collapsed);
    float rect[] = {-1.0f, float(-1 + 2 * bar_h / h), 2.0f, float(-2 * bar_h / h)};
    gpu::RenderPass pass;
    pass.color_count = 1;
    pass.color[0].texture = target;
    pass.color[0].load = gpu::Load::Load;
    pass.color[0].store = gpu::Store::Store;
    device->begin_render_pass(cb, pass);
    device->set_pipeline(cb, pipeline);
    device->set_bytes(cb, gpu::Stage::Vertex, 0, rect, sizeof rect);
    device->set_texture(cb, gpu::Stage::Fragment, 0, texture_);
    gpu::SamplerState linear;
    linear.mag = linear.min = gpu::Filter::Linear;
    device->set_sampler(cb, gpu::Stage::Fragment, 0, linear);
    device->draw(cb, gpu::Primitive::TriangleStrip, 0, 4);
    device->end_render_pass(cb);
}
