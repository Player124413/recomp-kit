// dxgi.cpp - software swap chains over guest-owned D3D11 back buffers.
// IDXGISwapChain has 18 slots including IUnknown and IDXGIObject/DeviceSubObject.
#include "d3d11.h"
#include "host_api.h"
#include "../runtime/display_seam.h"
#include "../runtime/win32.h"
#include <cstring>

namespace {
// The mode a fullscreen swap chain put the display in, for USER32's metrics.
// Zero while every swap chain is windowed, which leaves the desktop as it was.
uint32_t g_fs_w = 0, g_fs_h = 0;

} // namespace
namespace dx11 {
uint32_t swapchain(ComObj *device, const DXGI_SWAP_CHAIN_DESC &d) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = d.BufferDesc.Width;
    td.Height = d.BufferDesc.Height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = d.BufferDesc.Format;
    td.SampleDesc = d.SampleDesc;
    td.BindFlags = 0x20;
    uint32_t back = texture(device, td);
    if (!back)
        return 0;
    auto *obj = create(K_DXGI_SWAP, IF_DXGI_SWAP, device->id);
    auto *o = get(obj);
    o->resource = back;
    o->swap = d;
    o->fullscreen = !d.Windowed;
    if (o->fullscreen) {
        g_fs_w = d.BufferDesc.Width;
        g_fs_h = d.BufferDesc.Height;
        host_set_display_mode(d.BufferDesc.Width, d.BufferDesc.Height, 32);
        host_display_request_window(2);
    }
    return obj->id;
}
} // namespace dx11
namespace {
void get_buffer(X86 *c) {
    auto *s = dx11::from(arg(c, 0), IF_DXGI_SWAP);
    uint32_t out = arg(c, 3);
    if (!dx11::span(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    if (!s || arg(c, 1)) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    if (com_iface_for_iid(arg(c, 2)) != IF_D3D11_TEXTURE) {
        com_ret(c, E_NOINTERFACE);
        return;
    }
    auto *b = com_get(s->resource);
    com_addref(b);
    wr32(out, com_view(b, IF_D3D11_TEXTURE));
    com_ret(c, S_OK);
}
void present(X86 *c) {
    auto *s = dx11::from(arg(c, 0), IF_DXGI_SWAP);
    auto *back = s ? dx11::get(com_get(s->resource)) : nullptr;
    if (!back) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    if (!(arg(c, 2) & 1))
        dx11::present(*back, com_this_arg(c)->id, s->swap.OutputWindow,
                      s->fullscreen); // DXGI_PRESENT_TEST does not display
    LOGV("D3DPresent %ux%u", back->texture.Width, back->texture.Height);
    com_ret(c, S_OK);
}
// Fullscreen requests go through the existing queued host window seam.
// No native window or platform API is accessed from the guest thread.
void apply_mode(dx11::Object &s) {
    if (s.fullscreen) {
        g_fs_w = s.swap.BufferDesc.Width;
        g_fs_h = s.swap.BufferDesc.Height;
        host_set_display_mode(s.swap.BufferDesc.Width, s.swap.BufferDesc.Height, 32);
    } else {
        g_fs_w = g_fs_h = 0;
        uint32_t w = 0, h = 0, bpp = 0;
        win32_display_mode(&w, &h, &bpp);
        host_set_display_mode(w, h, bpp);
    }
    host_display_request_window(s.fullscreen ? 2 : 0);
}
void swap_fullscreen(X86 *c) {
    auto *s = dx11::from(arg(c, 0), IF_DXGI_SWAP);
    if (!s || (arg(c, 2) && !com_this(arg(c, 2), IF_DXGI_OUTPUT))) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    s->fullscreen = arg(c, 1) != 0;
    s->swap.Windowed = !s->fullscreen;
    gdi_forget_surface(com_this_arg(c)->id);
    apply_mode(*s);
    com_ret(c, S_OK);
}
void swap_output(X86 *c) {
    auto *s = dx11::from(arg(c, 0), IF_DXGI_SWAP);
    uint32_t out = arg(c, 1);
    if (!dx11::span(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    if (!s) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    auto *o = dx11::create(K_DXGI_OUTPUT, IF_DXGI_OUTPUT, s->device);
    wr32(out, com_view(o, IF_DXGI_OUTPUT));
    com_ret(c, S_OK);
}
void swap_get_fullscreen(X86 *c) {
    auto *s = dx11::from(arg(c, 0), IF_DXGI_SWAP);
    uint32_t out = arg(c, 1), target = arg(c, 2);
    if (!s || !dx11::span(out, 4) || (target && !dx11::span(target, 4))) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    wr32(out, s->fullscreen);
    if (target) {
        wr32(target, 0);
        if (s->fullscreen) {
            auto *o = dx11::create(K_DXGI_OUTPUT, IF_DXGI_OUTPUT, s->device);
            wr32(target, com_view(o, IF_DXGI_OUTPUT));
        }
    }
    com_ret(c, S_OK);
}
void swap_desc(X86 *c) {
    auto *s = dx11::from(arg(c, 0), IF_DXGI_SWAP);
    if (!s || !dx11::span(arg(c, 1), 60)) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    memcpy(gm_ptr(arg(c, 1)), &s->swap, 60);
    com_ret(c, S_OK);
}
// A view or binding is an outstanding reference too. Reallocation cannot
// invalidate it: the caller must release all back-buffer references first.
void swap_resize(X86 *c) {
    auto *s = dx11::from(arg(c, 0), IF_DXGI_SWAP);
    auto *back = s ? com_get(s->resource) : nullptr;
    if (!back || back->refs != 1) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    auto *old = dx11::get(back);
    D3D11_TEXTURE2D_DESC td = old->texture;
    td.Width = arg(c, 2) ? arg(c, 2) : s->swap.BufferDesc.Width;
    td.Height = arg(c, 3) ? arg(c, 3) : s->swap.BufferDesc.Height;
    if (arg(c, 4))
        td.Format = arg(c, 4);
    uint32_t id = dx11::texture(com_get(s->device), td);
    if (!id) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    s->resource = id;
    com_release(back);
    gdi_forget_surface(com_this_arg(c)->id);
    s->swap.BufferDesc.Width = td.Width;
    s->swap.BufferDesc.Height = td.Height;
    s->swap.BufferDesc.Format = td.Format;
    if (arg(c, 1))
        s->swap.BufferCount = arg(c, 1);
    s->swap.Flags = arg(c, 5);
    apply_mode(*s);
    com_ret(c, S_OK);
}

DX_STUB(swap_set_private, E_NOTIMPL)
DX_STUB(swap_set_private_interface, E_NOTIMPL)
DX_STUB(swap_get_private, E_NOTIMPL)
DX_STUB(swap_get_parent, E_NOTIMPL)
DX_STUB(swap_get_device, E_NOTIMPL)
DX_STUB(swap_resize_target, E_NOTIMPL)
DX_STUB(swap_stats, E_NOTIMPL)
DX_STUB(swap_last_present, E_NOTIMPL)
DX_STUB(factory, E_NOTIMPL)
DX_STUB(output_SetPrivateData, E_NOTIMPL)
DX_STUB(output_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(output_GetPrivateData, E_NOTIMPL)
DX_STUB(output_GetParent, E_NOTIMPL)
DX_STUB(output_GetDesc, E_NOTIMPL)
DX_STUB(output_GetDisplayModeList, E_NOTIMPL)
DX_STUB(output_FindClosestMatchingMode, E_NOTIMPL)
DX_STUB(output_WaitForVBlank, E_NOTIMPL)
DX_STUB(output_TakeOwnership, E_NOTIMPL)
DX_STUB(output_ReleaseOwnership, E_NOTIMPL)
DX_STUB(output_GetGammaControlCapabilities, E_NOTIMPL)
DX_STUB(output_SetGammaControl, E_NOTIMPL)
DX_STUB(output_GetGammaControl, E_NOTIMPL)
DX_STUB(output_SetDisplaySurface, E_NOTIMPL)
DX_STUB(output_GetDisplaySurfaceData, E_NOTIMPL)
DX_STUB(output_GetFrameStatistics, E_NOTIMPL)
static const ComMethod output_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"SetPrivateData", 4, output_SetPrivateData},
    {"SetPrivateDataInterface", 3, output_SetPrivateDataInterface},
    {"GetPrivateData", 4, output_GetPrivateData},
    {"GetParent", 3, output_GetParent},
    {"GetDesc", 2, output_GetDesc},
    {"GetDisplayModeList", 5, output_GetDisplayModeList},
    {"FindClosestMatchingMode", 4, output_FindClosestMatchingMode},
    {"WaitForVBlank", 1, output_WaitForVBlank},
    {"TakeOwnership", 3, output_TakeOwnership},
    {"ReleaseOwnership", 1, output_ReleaseOwnership},
    {"GetGammaControlCapabilities", 2, output_GetGammaControlCapabilities},
    {"SetGammaControl", 2, output_SetGammaControl},
    {"GetGammaControl", 2, output_GetGammaControl},
    {"SetDisplaySurface", 2, output_SetDisplaySurface},
    {"GetDisplaySurfaceData", 2, output_GetDisplaySurfaceData},
    {"GetFrameStatistics", 2, output_GetFrameStatistics},
};
static_assert(std::size(output_methods) == 19);
} // namespace
extern "C" bool dxgi_display_mode(uint32_t *w, uint32_t *h, uint32_t *bpp) {
    if (!g_fs_w || !g_fs_h)
        return false;
    *w = g_fs_w;
    *h = g_fs_h;
    *bpp = 32;
    return true;
}

void dxgi_register() {
    dx11::define(IF_DXGI_OUTPUT, K_DXGI_OUTPUT, "dxgi.dll", "IDXGIOutput", output_methods,
                 std::size(output_methods), "ae02eedb-c735-4690-8d52-5a8dc20213aa");
    static const ComMethod methods[] = {{"QueryInterface", 3, com_QueryInterface},
                                        {"AddRef", 1, com_AddRef},
                                        {"Release", 1, com_Release},
                                        {"SetPrivateData", 4, swap_set_private},
                                        {"SetPrivateDataInterface", 3, swap_set_private_interface},
                                        {"GetPrivateData", 4, swap_get_private},
                                        {"GetParent", 3, swap_get_parent},
                                        {"GetDevice", 3, swap_get_device},
                                        {"Present", 3, present},
                                        {"GetBuffer", 4, get_buffer},
                                        {"SetFullscreenState", 3, swap_fullscreen},
                                        {"GetFullscreenState", 3, swap_get_fullscreen},
                                        {"GetDesc", 2, swap_desc},
                                        {"ResizeBuffers", 6, swap_resize},
                                        {"ResizeTarget", 2, swap_resize_target},
                                        {"GetContainingOutput", 2, swap_output},
                                        {"GetFrameStatistics", 2, swap_stats},
                                        {"GetLastPresentCount", 2, swap_last_present}};
    dx11::define(IF_DXGI_SWAP, K_DXGI_SWAP, "dxgi.dll", "IDXGISwapChain", methods,
                 std::size(methods), "310d36a0-d2e7-4c0a-aa04-6a9d23b8886a");
    static const ImportShim shims[] = {{"dxgi.dll", "CreateDXGIFactory", 2, factory},
                                       {"dxgi.dll", "CreateDXGIFactory1", 2, factory}};
    imports_register(shims, std::size(shims));
}
