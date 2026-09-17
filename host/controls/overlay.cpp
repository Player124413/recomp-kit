// overlay.cpp - the GPU half of overlay.h: one raster for the whole view.
#include "overlay.h"

#include "overlay_paint.h"
#include "pad_art.h"
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

// The knob texture's side: big enough for an iPad's largest stick, and
// scaled linearly for any other.
constexpr int kKnobSize = 128;

// Where a control can draw: its rect, and for a stick the whole base circle
// around a finger anywhere in its zone.
Rect drawn_rect(const DrawControl &d) {
    if (d.kind != Kind::Stick || d.radius_px <= 0)
        return d.rect;
    const int r = d.radius_px + 1;
    return Rect{d.rect.x - r, d.rect.y - r, d.rect.w + 2 * r, d.rect.h + 2 * r};
}

} // namespace

Overlay::~Overlay() {
    release();
}

void Overlay::release() {
    if (device_ && texture_)
        device_->destroy(texture_);
    if (device_ && knob_)
        device_->destroy(knob_);
    texture_ = gpu::Texture();
    knob_ = gpu::Texture();
    knob_opacity_ = -1;
}

void Overlay::update(gpu::Device *device, const ControlsView &view, int w, int h) {
    if (device_ == device && built_ && revision_ == view.revision && dw_ == w && dh_ == h)
        return;
    Rect r = view.controls_area;
    for (const Rect &b : view.backdrops)
        r = unite(r, b);
    for (const DrawControl &d : view.controls)
        r = unite(r, drawn_rect(d));
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
    Canvas c(pixels_, r.w, r.h, view.opacity);
    paint_overlay(c, view, r);
    if (device_ != device)
        release();
    if (!texture_ || tex_w_ != r.w || tex_h_ != r.h) {
        if (texture_)
            device->destroy(texture_);
        texture_ = device->create_texture(
            {r.w, r.h, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
        tex_w_ = r.w;
        tex_h_ = r.h;
    }
    device_ = device;
    if (texture_)
        device->upload(texture_, {0, 0, r.w, r.h}, pixels_.data(), r.w * 4);
}

// Paints the knob once, and again only when the opacity changes.
void Overlay::update_knob(gpu::Device *device, double opacity) {
    if (knob_ && knob_opacity_ == opacity)
        return;
    if (!knob_)
        knob_ = device->create_texture(
            {kKnobSize, kKnobSize, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
    if (!knob_)
        return;
    std::vector<uint8_t> px(size_t(kKnobSize) * kKnobSize * 4, 0);
    Canvas c(px, kKnobSize, kKnobSize, opacity);
    paint_knob(c, kKnobSize / 2.0, kKnobSize / 2.0, kKnobSize / 2.0 - 1, true);
    device->upload(knob_, {0, 0, kKnobSize, kKnobSize}, px.data(), kKnobSize * 4);
    knob_opacity_ = opacity;
}

void Overlay::blit(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
                   const std::vector<Quad> &quads) {
    gpu::RenderState state;
    state.color_format[0] = device->describe(target).format;
    state.color_count = 1;
    state.blend_enabled = true;
    state.src_rgb = state.src_alpha = gpu::Blend::One;
    state.dst_rgb = state.dst_alpha = gpu::Blend::OneMinusSrcAlpha;
    gpu::Pipeline pipeline = device->render_pipeline("hud", state);
    if (!pipeline)
        return;
    gpu::RenderPass pass;
    pass.color_count = 1;
    pass.color[0].texture = target;
    pass.color[0].load = gpu::Load::Load;
    pass.color[0].store = gpu::Store::Store;
    device->begin_render_pass(cb, pass);
    device->set_pipeline(cb, pipeline);
    gpu::SamplerState linear;
    linear.mag = linear.min = gpu::Filter::Linear;
    for (const Quad &q : quads) {
        // Clip-space rectangle: x, y of the top-left corner, width, and a
        // negative height (y up), the encoding the hud pipeline's strip used.
        const float x0 = -1.0f + 2.0f * float(q.rect.x) / float(w);
        const float y0 = 1.0f - 2.0f * float(q.rect.y) / float(h);
        float rect[] = {x0, y0, 2.0f * float(q.rect.w) / float(w),
                        -2.0f * float(q.rect.h) / float(h)};
        device->set_bytes(cb, gpu::Stage::Vertex, 0, rect, sizeof rect);
        device->set_texture(cb, gpu::Stage::Fragment, 0, q.texture);
        device->set_sampler(cb, gpu::Stage::Fragment, 0, linear);
        device->draw(cb, gpu::Primitive::TriangleStrip, 0, 4);
    }
    device->end_render_pass(cb);
}

void Overlay::draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
                   const ControlsView &view) {
    if (!device || !target || !cb || w <= 0 || h <= 0 || !view.wanted)
        return;
    update(device, view, w, h);
    std::vector<Quad> quads;
    if (texture_ && device_ == device && !rect_.empty() && tex_w_ == rect_.w && tex_h_ == rect_.h)
        quads.push_back({texture_, rect_});
    // Every held stick's knob, at its base plus the offset.
    for (const DrawControl &d : view.controls) {
        if (d.kind != Kind::Stick || !d.pressed || d.radius_px <= 0)
            continue;
        if (device_ != device)
            break; // update() found nothing to draw on this device yet
        update_knob(device, view.opacity);
        if (!knob_)
            break;
        const int kr = int(std::lround(knob_radius(d.radius_px)));
        const double cx = d.base_x + d.knob_x * d.radius_px;
        const double cy = d.base_y + d.knob_y * d.radius_px;
        quads.push_back(
            {knob_, Rect{int(std::lround(cx)) - kr, int(std::lround(cy)) - kr, 2 * kr, 2 * kr}});
    }
    if (!quads.empty())
        blit(device, cb, target, w, h, quads);
}

} // namespace controls
