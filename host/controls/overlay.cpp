// overlay.cpp - the GPU half of overlay.h: one raster for the whole view.
#include "overlay.h"

#include "raster.h"

#include <algorithm>
#include <cmath>

namespace controls {

namespace {

Rect unite(const Rect &a, const Rect &b) {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

// The keypad's look, unchanged: a translucent backdrop behind each half, flat
// keys with light text that invert when lit, and tabs framed in the backdrop
// colour. Every alpha is scaled by the view's opacity; `r` is the raster's
// place on the drawable, so rects are drawn at (x - r.x, y - r.y).
void paint(Canvas &c, const ControlsView &view, const Rect &r) {
    const auto alpha = [&](int a) { return int(lround(a * std::clamp(view.opacity, 0.0, 1.0))); };
    for (const Rect &b : view.backdrops)
        c.fill(b.x - r.x, b.y - r.y, b.w, b.h, 6, 9, 15, alpha(150));
    for (const DrawControl &d : view.controls) {
        const int x = d.rect.x - r.x, y = d.rect.y - r.y, w = d.rect.w, h = d.rect.h;
        switch (d.kind) {
        case Kind::Key: {
            if (d.lit)
                c.fill(x, y, w, h, 120, 160, 255, alpha(220));
            else
                c.fill(x, y, w, h, 40, 48, 64, alpha(200));
            const char *label = d.label.c_str();
            c.text(x + (w - text_width(label)) / 2, y + (h - 16) / 2, label, d.lit ? 10 : 235,
                   d.lit ? 12 : 242, d.lit ? 20 : 255, alpha(255));
            break;
        }
        case Kind::Toggle: {
            c.fill(x, y, w, h, 6, 9, 15, alpha(150));
            c.fill(x + 2, y + 2, w - 4, h - 4, 40, 48, 64, alpha(200));
            const char *label = d.group_visible ? d.label.c_str() : d.label_off.c_str();
            c.text(x + (w - text_width(label)) / 2, y + (h - 16) / 2, label, 235, 242, 255,
                   alpha(255));
            break;
        }
        default:
            // A placeholder until the pad look lands (Task 11).
            c.disc(x + w / 2, y + h / 2, std::min(w, h) / 2, 40, 48, 64, alpha(200));
            break;
        }
    }
}

} // namespace

Overlay::~Overlay() {
    if (device_ && texture_)
        device_->destroy(texture_);
}

void Overlay::update(gpu::Device *device, const ControlsView &view, int w, int h) {
    if (device_ == device && built_ && revision_ == view.revision && dw_ == w && dh_ == h)
        return;
    Rect r;
    for (const Rect &b : view.backdrops)
        r = unite(r, b);
    for (const DrawControl &d : view.controls)
        r = unite(r, d.rect);
    // Clip to the drawable.
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(w, r.x + r.w), y1 = std::min(h, r.y + r.h);
    r = Rect{x0, y0, x1 - x0, y1 - y0};
    built_ = true;
    revision_ = view.revision;
    dw_ = w;
    dh_ = h;
    rect_ = r;
    if (r.empty())
        return;
    pixels_.assign(size_t(r.w) * r.h * 4, 0);
    Canvas c{pixels_, r.w, r.h};
    paint(c, view, r);
    if (device_ != device || !texture_ || tex_w_ != r.w || tex_h_ != r.h) {
        if (device_ && texture_)
            device_->destroy(texture_);
        texture_ = device->create_texture(
            {r.w, r.h, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
        tex_w_ = r.w;
        tex_h_ = r.h;
    }
    device_ = device;
    if (texture_)
        device->upload(texture_, {0, 0, r.w, r.h}, pixels_.data(), r.w * 4);
}

void Overlay::blit(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h) {
    gpu::RenderState state;
    state.color_format[0] = device->describe(target).format;
    state.color_count = 1;
    state.blend_enabled = true;
    state.src_rgb = state.src_alpha = gpu::Blend::One;
    state.dst_rgb = state.dst_alpha = gpu::Blend::OneMinusSrcAlpha;
    gpu::Pipeline pipeline = device->render_pipeline("hud", state);
    if (!pipeline)
        return;
    // Clip-space rectangle: x, y of the top-left corner, width, and a negative
    // height (y up), the encoding the hud pipeline's strip used.
    const float x0 = -1.0f + 2.0f * float(rect_.x) / float(w);
    const float y0 = 1.0f - 2.0f * float(rect_.y) / float(h);
    float rect[] = {x0, y0, 2.0f * float(rect_.w) / float(w), -2.0f * float(rect_.h) / float(h)};
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

void Overlay::draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
                   const ControlsView &view) {
    if (!device || !target || !cb || w <= 0 || h <= 0 || !view.wanted)
        return;
    update(device, view, w, h);
    if (texture_ && device_ == device && !rect_.empty() && tex_w_ == rect_.w && tex_h_ == rect_.h)
        blit(device, cb, target, w, h);
}

} // namespace controls
