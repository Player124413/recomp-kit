// d3d11.h - shared software D3D11/DXGI object state. All methods run under
// the guest scheduler baton; only owned snapshots are sent to the host.
#pragma once
#include "com.h"
#include <array>

namespace dx11 {
struct Object {
    ComIface iface = IF_NONE;
    uint32_t device = 0, context = 0, resource = 0;
    uint32_t data = 0, bytes = 0, pitch = 0, shader = 0;
    bool mapped = false, dirty = false, fullscreen = false;
    D3D11_TEXTURE2D_DESC texture{};
    D3D11_BUFFER_DESC buffer{};
    DXGI_SWAP_CHAIN_DESC swap{};
    std::vector<uint8_t> desc;
    // Binding references are owned, unlike the device's weak context id.
    uint32_t rtv = 0, srv = 0, vb = 0, ib = 0, cb = 0, sampler = 0, blend = 0, raster = 0, vs = 0,
             ps = 0, layout = 0;
    uint32_t stride = 0, vertex_offset = 0, index_offset = 0, index_format = 57, topology = 0;
    uint32_t position_offset = 0, texcoord_offset = 12;
    D3D11_VIEWPORT viewport{};
};
Object *get(ComObj *o);
Object *from(uint32_t view, ComIface iface);
ComObj *create(ComKind kind, ComIface iface, uint32_t device = 0);
void retain(uint32_t &slot, uint32_t id);
bool span(uint32_t addr, uint64_t bytes);
uint32_t texture(ComObj *device, const D3D11_TEXTURE2D_DESC &desc);
uint32_t swapchain(ComObj *device, const DXGI_SWAP_CHAIN_DESC &desc);
void put_pixel(Object &o, uint32_t x, uint32_t y, const std::array<float, 4> &colour);
std::array<float, 4> pixel(const Object &o, uint32_t x, uint32_t y);
void present(Object &back, uint32_t owner, uint32_t hwnd, bool fullscreen);
void define(ComIface iface, ComKind kind, const char *dll, const char *name,
            const ComMethod *methods, size_t count, const char *iid);
void get_device(X86 *c);
// Tagged shader data contains the entry point, target and exact source hash.
struct ShaderTag {
    uint32_t magic, version;
    uint64_t hash;
    char entry[32], target[16];
};
uint32_t shader_kind(uint32_t data, uint32_t size, bool vertex);
} // namespace dx11
void d3d11_register();
void d3d11_reset();
void dxgi_register();
void d3dcompiler_register();
void d3dx10_register();
