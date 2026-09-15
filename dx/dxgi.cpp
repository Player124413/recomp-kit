// dxgi.cpp - software swap chains over guest-owned D3D11 back buffers.
// IDXGISwapChain has 18 slots including IUnknown and IDXGIObject/DeviceSubObject.
#include "d3d11.h"
#include <cstring>
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
        dx11::present(*back); // DXGI_PRESENT_TEST does not display
    LOGV("D3DPresent %ux%u", back->texture.Width, back->texture.Height);
    com_ret(c, S_OK);
}
DX_STUB(swap_set_private, E_NOTIMPL)
DX_STUB(swap_set_private_interface, E_NOTIMPL)
DX_STUB(swap_get_private, E_NOTIMPL)
DX_STUB(swap_get_parent, E_NOTIMPL)
DX_STUB(swap_get_device, E_NOTIMPL)
DX_STUB(swap_fullscreen, E_NOTIMPL)
DX_STUB(swap_get_fullscreen, E_NOTIMPL)
DX_STUB(swap_desc, E_NOTIMPL)
DX_STUB(swap_resize, E_NOTIMPL)
DX_STUB(swap_resize_target, E_NOTIMPL)
DX_STUB(swap_output, E_NOTIMPL)
DX_STUB(swap_stats, E_NOTIMPL)
DX_STUB(swap_last_present, E_NOTIMPL)
DX_STUB(factory, E_NOTIMPL)
} // namespace
void dxgi_register() {
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
