// keypad_overlay.cpp - see keypad_overlay.h.
#include "keypad_overlay.h"

#include "../mods/mods_internal.h"

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

KeypadOverlay::~KeypadOverlay() {
    if (device_)
        for (Half &h : halves_)
            if (h.texture)
                device_->destroy(h.texture);
}

void KeypadOverlay::update(gpu::Device *device, KeypadSide side, const KeypadView &view, int dw,
                           int dh) {
    Half &half = halves_[side];
    const bool shown = side == KEYPAD_LEFT ? view.left : view.right;
    if (device_ == device && half.texture && half.shown == shown && half.size == view.size &&
        half.lit == view.lit && half.scale == view.scale && half.dw == dw && half.dh == dh)
        return;
    const KeypadRect pad = keypad_half_rect(side, view.size, view.scale, dw, dh);
    const KeypadRect tab = keypad_tab_rect(side, shown, view.size, view.scale, dw, dh);
    // The raster covers the tab and, when shown, the half below it.
    KeypadRect r = tab;
    if (shown) {
        r.x = std::min(tab.x, pad.x);
        r.y = tab.y;
        r.w = std::max(tab.x + tab.w, pad.x + pad.w) - r.x;
        r.h = pad.y + pad.h - r.y;
    }
    if (r.w <= 0 || r.h <= 0)
        return;
    pixels_.assign(size_t(r.w) * r.h * 4, 0);
    Canvas c{pixels_, r.w, r.h};
    // The tab.
    c.fill(tab.x - r.x, tab.y - r.y, tab.w, tab.h, 6, 9, 15, 150);
    c.fill(tab.x - r.x + 2, tab.y - r.y + 2, tab.w - 4, tab.h - 4, 40, 48, 64, 200);
    const char *tab_label = shown ? "HIDE" : "KEYS";
    c.text(tab.x - r.x + (tab.w - int(strlen(tab_label)) * 12) / 2, tab.y - r.y + (tab.h - 16) / 2,
           tab_label, 235, 242, 255);
    if (shown) {
        c.fill(pad.x - r.x, pad.y - r.y, pad.w, pad.h, 6, 9, 15, 150);
        int n = 0;
        const KeypadKey *keys = keypad_keys(side, &n);
        for (int i = 0; i < n; ++i) {
            const KeypadRect k = keypad_key_rect(side, keys[i], view.size, view.scale, dw, dh);
            const bool lit = (view.lit & keypad_modifier_bit(keys[i].scancode)) != 0;
            if (lit)
                c.fill(k.x - r.x, k.y - r.y, k.w, k.h, 120, 160, 255, 220);
            else
                c.fill(k.x - r.x, k.y - r.y, k.w, k.h, 40, 48, 64, 200);
            const int text_w = int(strlen(keys[i].label)) * 12;
            c.text(k.x - r.x + (k.w - text_w) / 2, k.y - r.y + (k.h - 16) / 2, keys[i].label,
                   lit ? 10 : 235, lit ? 12 : 242, lit ? 20 : 255);
        }
    }
    if (device_ != device || !half.texture || half.w != r.w || half.h != r.h) {
        if (device_ && half.texture)
            device_->destroy(half.texture);
        half.texture = device->create_texture(
            {r.w, r.h, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
        half.w = r.w;
        half.h = r.h;
    }
    device_ = device;
    if (half.texture)
        device->upload(half.texture, {0, 0, r.w, r.h}, pixels_.data(), r.w * 4);
    half.rect = r;
    half.shown = shown;
    half.size = view.size;
    half.lit = view.lit;
    half.scale = view.scale;
    half.dw = dw;
    half.dh = dh;
}

void KeypadOverlay::blit(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w,
                         int h, const Half &half) {
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
    const float x0 = -1.0f + 2.0f * float(half.rect.x) / float(w);
    const float y0 = 1.0f - 2.0f * float(half.rect.y) / float(h);
    float rect[] = {x0, y0, 2.0f * float(half.rect.w) / float(w),
                    -2.0f * float(half.rect.h) / float(h)};
    gpu::RenderPass pass;
    pass.color_count = 1;
    pass.color[0].texture = target;
    pass.color[0].load = gpu::Load::Load;
    pass.color[0].store = gpu::Store::Store;
    device->begin_render_pass(cb, pass);
    device->set_pipeline(cb, pipeline);
    device->set_bytes(cb, gpu::Stage::Vertex, 0, rect, sizeof rect);
    device->set_texture(cb, gpu::Stage::Fragment, 0, half.texture);
    gpu::SamplerState linear;
    linear.mag = linear.min = gpu::Filter::Linear;
    device->set_sampler(cb, gpu::Stage::Fragment, 0, linear);
    device->draw(cb, gpu::Primitive::TriangleStrip, 0, 4);
    device->end_render_pass(cb);
}

void KeypadOverlay::draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w,
                         int h, const KeypadView &view) {
    if (!device || !target || !cb || w <= 0 || h <= 0 || !view.wanted)
        return;
    for (int s = 0; s < 2; ++s) {
        update(device, KeypadSide(s), view, w, h);
        if (halves_[s].texture)
            blit(device, cb, target, w, h, halves_[s]);
    }
}
