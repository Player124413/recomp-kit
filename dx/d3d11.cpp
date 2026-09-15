// Shader contract, quoted verbatim from D3DRenderer.pas at 417cbd0 (2021).
// The quoted HLSL is LGPL-2.1; attribution and full license are in NOTICE.
// Each displayed newline represents CRLF; tabs and trailing spaces are
// significant to the exact source digest in D3DCompile.
// D3DShader.pas input layout / D3DMesh.pas TDXVertex:
// POSITION: float3, R32G32B32_FLOAT, slot 0, byte offset 0.
// TEXCOORD: float2, R32G32_FLOAT, slot 0, APPEND_ALIGNED_ELEMENT (=12).
// Both PER_VERTEX_DATA, semantic index 0, step rate 0; stride 20 bytes.
// Indices (R16_UINT): 0,2,1,0,1,3; TRIANGLELIST.
// Constant buffer: OutputPosition then InputPosition (128 bytes).
// D3DXMatrixMultiplyTranspose uploads transposed row-vector products.
// HLSL's default column-major layout interprets those bytes as the original
// row-vector matrix; transform[k] = sum(input[j] * uploaded[k*4+j]).
// Exact ANSI source bytes below use C string escapes (CRLF is \r\n).
// vertex_shader:
// "cbuffer MatrixBuffer\r\n"
// "{\r\n"
// "    matrix OutputPosition;\r\n"
// "    matrix InputPosition;\r\n"
// "}\r\n"
// "struct VSInput\r\n"
// "{\r\n"
// "    float4 position : POSITION;\r\n"
// "    float2 texcoords : TEXCOORD0;\r\n"
// "};\r\n"
// "\r\n"
// "struct PSInput\r\n"
// "{\r\n"
// "    float4 position : SV_POSITION;\r\n"
// "    float2 texcoords : TEXCOORD0;\r\n"
// "};\r\n"
// "\r\n"
// "PSInput VSEntry(VSInput input)\r\n"
// "{\r\n"
// "    PSInput output;\r\n"
// "    input.position.w = 1.0f;\r\n"
// "    output.position = mul(input.position, OutputPosition);\r\n"
// "    float4 tex = float4(input.texcoords.x, input.texcoords.y, 0.0f, 1.0f);\r\n"
// "    tex = mul(tex, InputPosition);\r\n"
// "    output.texcoords = float2(tex.x, 1.0f - tex.y);\r\n"
// "    return output;\r\n"
// "}\r\n"
// fragment_shader:
// "Texture2D DiffuseMap;\r\n"
// "SamplerState SampleType;\r\n"
// "\r\n"
// "struct PSInput\r\n"
// "{\r\n"
// "    float4 position : SV_POSITION;\r\n"
// "    float2 texcoords : TEXCOORD0;\r\n"
// "};\r\n"
// "\r\n"
// "float4 PSEntry(PSInput vs_out) : SV_TARGET\r\n"
// "{\r\n"
// "\tfloat4 color = DiffuseMap.Sample(SampleType, vs_out.texcoords); \r\n"
// "\treturn color.rgba; \r\n"
// "}\r\n"
// fragment_shader_R16_int:
// "Texture2D DiffuseMap;\r\n"
// "SamplerState SampleType;\r\n"
// "\r\n"
// "struct PSInput\r\n"
// "{\r\n"
// "    float4 position : SV_POSITION;\r\n"
// "    float2 texcoords : TEXCOORD0;\r\n"
// "};\r\n"
// "\r\n"
// "float4 PSEntry(PSInput vs_out) : SV_TARGET\r\n"
// "{\r\n"
// "    float4 c = DiffuseMap.Sample(SampleType, vs_out.texcoords);\r\n"
// "    int c565 = c.r * 65535;\r\n"
// "    int t = c565 / 32;\r\n"
// "    t = t * 32;\r\n"
// "    int b = c565 - t;\r\n"
// "    c565 = (c565 - b) / 32;\r\n"
// "    t = c565 / 64;\r\n"
// "    t = t * 64;\r\n"
// "    int g = c565 - t;\r\n"
// "    c565 = (c565 - g) / 64;\r\n"
// "    t = c565 / 32;\r\n"
// "    t = t * 32;\r\n"
// "    int r = c565 - t;\r\n"
// "\treturn float4(r / 32.0f, g / 64.0f, b / 32.0f, 1.0f);\r\n"
// "}\r\n"
// d3d11.cpp - a software, single-sample 2D Direct3D 11 adapter.
// Shader arithmetic and the input layout are documented below. No executable
// address, window title or renderer-selection policy belongs in this module.
#include "d3d11.h"
#include "host_api.h"
#include "../runtime/memory.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <cstdio>

namespace dx11 {
namespace {
std::map<uint32_t, Object> &objects() {
    static auto *v = new std::map<uint32_t, Object>();
    return *v;
}
// Drop owned resource and binding references. mem_init invalidates the arena;
// reset therefore discards metadata instead of freeing a previous generation.
void destroy(ComObj *obj) {
    auto it = objects().find(obj->id);
    if (it == objects().end())
        return;
    Object o = std::move(it->second);
    objects().erase(it);
    if (o.data)
        heap_free(o.data);
    for (uint32_t id : {o.resource, o.rtv, o.srv, o.vb, o.ib, o.cb, o.sampler, o.blend, o.raster,
                        o.vs, o.ps, o.layout, o.device})
        if (id)
            com_release(com_get(id));
}
} // namespace
bool span(uint32_t p, uint64_t n) {
    return p && gm_fits(p, n);
}
Object *get(ComObj *o) {
    if (!o)
        return nullptr;
    auto it = objects().find(o->id);
    return it == objects().end() ? nullptr : &it->second;
}
Object *from(uint32_t view, ComIface iface) {
    return get(com_this(view, iface));
}
ComObj *create(ComKind kind, ComIface iface, uint32_t device) {
    auto *obj = com_new(kind);
    auto &o = objects()[obj->id];
    o.iface = iface;
    o.device = device;
    com_addref(com_get(device));
    return obj;
}
void retain(uint32_t &slot, uint32_t id) {
    if (slot == id)
        return;
    com_addref(com_get(id));
    uint32_t old = slot;
    slot = id;
    com_release(com_get(old));
}
// Bind distinct kinds, preserving IUnknown identity and refusing unsupported
// interfaces (including optional IDXGIDevice queries) through the COM core.
void define(ComIface iface, ComKind kind, const char *dll, const char *name, const ComMethod *m,
            size_t n, const char *iid) {
    com_define(iface, dll, name, m, n);
    com_bind(iface, kind);
    com_set_destructor(kind, destroy);
    unsigned a, b, c, d[8];
    if (sscanf(iid, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3],
               &d[4], &d[5], &d[6], &d[7]) == 11) {
        uint8_t guid[16] = {uint8_t(a), uint8_t(a >> 8), uint8_t(a >> 16), uint8_t(a >> 24),
                            uint8_t(b), uint8_t(b >> 8), uint8_t(c),       uint8_t(c >> 8)};
        for (int i = 0; i < 8; ++i)
            guid[8 + i] = uint8_t(d[i]);
        com_register_iid(iface, guid);
    }
}
// Allocate only base-level storage: views of other mips/arrays are refused.
// MipLevels=0 is accepted for clients that expose and update only mip zero.
uint32_t texture(ComObj *dev, const D3D11_TEXTURE2D_DESC &d) {
    uint32_t bpp = (d.Format == 85 || d.Format == 56)   ? 2
                   : (d.Format == 28 || d.Format == 87) ? 4
                                                        : 0;
    uint64_t size = uint64_t(d.Width) * d.Height * bpp;
    if (!bpp || !d.Width || !d.Height || d.Width > 16384 || d.Height > 16384 || d.ArraySize != 1 ||
        d.SampleDesc.Count != 1 || d.SampleDesc.Quality || size > 256 * 1024 * 1024)
        return 0;
    auto *obj = create(K_D3D11_TEXTURE, IF_D3D11_TEXTURE, dev->id);
    auto *o = get(obj);
    o->texture = d;
    o->pitch = d.Width * bpp;
    o->bytes = uint32_t(size);
    o->data = heap_alloc(o->bytes, true, 16);
    if (!o->data) {
        com_release(obj);
        return 0;
    }
    return obj->id;
}
std::array<float, 4> pixel(const Object &o, uint32_t x, uint32_t y) {
    uint32_t p =
        o.data + y * o.pitch + x * ((o.texture.Format == 85 || o.texture.Format == 56) ? 2 : 4);
    if (o.texture.Format == 56)
        return {rd16(p) / 65535.f, 0, 0, 1};
    if (o.texture.Format == 85) {
        uint32_t v = rd16(p);
        return {((v >> 11) & 31) / 31.f, ((v >> 5) & 63) / 63.f, (v & 31) / 31.f, 1};
    }
    bool bgra = o.texture.Format == 87;
    return {rd8(p + (bgra ? 2 : 0)) / 255.f, rd8(p + 1) / 255.f, rd8(p + (bgra ? 0 : 2)) / 255.f,
            rd8(p + 3) / 255.f};
}
void put_pixel(Object &o, uint32_t x, uint32_t y, const std::array<float, 4> &c) {
    auto q = [&](int i, int max) {
        return uint32_t(std::lround(std::clamp(c[i], 0.f, 1.f) * max));
    };
    uint32_t p =
        o.data + y * o.pitch + x * ((o.texture.Format == 85 || o.texture.Format == 56) ? 2 : 4);
    if (o.texture.Format == 56)
        wr16(p, uint16_t(q(0, 65535)));
    else if (o.texture.Format == 85)
        wr16(p, uint16_t(q(0, 31) << 11 | q(1, 63) << 5 | q(2, 31)));
    else {
        bool b = o.texture.Format == 87;
        wr8(p + (b ? 2 : 0), q(0, 255));
        wr8(p + 1, q(1, 255));
        wr8(p + (b ? 0 : 2), q(2, 255));
        wr8(p + 3, q(3, 255));
    }
    o.dirty = true;
}
// host_present copies the ARGB snapshot synchronously; resource memory is
// never lent to a presenter thread. Frame sealing is integrated separately.
void present(Object &o) {
    std::vector<uint32_t> argb(size_t(o.texture.Width) * o.texture.Height);
    for (uint32_t y = 0; y < o.texture.Height; ++y)
        for (uint32_t x = 0; x < o.texture.Width; ++x) {
            auto c = pixel(o, x, y);
            argb[size_t(y) * o.texture.Width + x] =
                0xff000000u | uint32_t(std::lround(c[0] * 255)) << 16 |
                uint32_t(std::lround(c[1] * 255)) << 8 | uint32_t(std::lround(c[2] * 255));
        }
    host_present(argb.data(), o.texture.Width, o.texture.Height, 32, nullptr, o.texture.Width * 4);
}
void get_device(X86 *c) {
    auto *o = get(com_this_arg(c));
    uint32_t out = arg(c, 1);
    if (!span(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    auto *d = o ? com_get(o->device) : nullptr;
    if (!d) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    com_addref(d);
    wr32(out, com_view(d, IF_D3D11_DEVICE));
    com_ret(c, S_OK);
}
} // namespace dx11
void d3d11_reset() {
    dx11::objects().clear();
}

namespace {
void create_rtv(X86 *c) {
    auto *d = com_this_arg(c, IF_D3D11_DEVICE);
    auto *r = com_this(arg(c, 1), IF_D3D11_TEXTURE);
    uint32_t out = arg(c, 3);
    if (!dx11::span(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    if (!d || !r || dx11::get(r)->device != d->id || arg(c, 2)) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    auto *v = dx11::create(K_D3D11_RTV, IF_D3D11_RTV, d->id);
    dx11::retain(dx11::get(v)->resource, r->id);
    wr32(out, com_view(v, IF_D3D11_RTV));
    com_ret(c, S_OK);
}
void clear_rtv(X86 *c) {
    auto *v = dx11::from(arg(c, 1), IF_D3D11_RTV);
    auto *r = v ? dx11::get(com_get(v->resource)) : nullptr;
    if (!r || !dx11::span(arg(c, 2), 16)) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    std::array<float, 4> colour;
    memcpy(colour.data(), gm_ptr(arg(c, 2)), 16);
    for (float f : colour)
        if (!std::isfinite(f)) {
            com_ret(c, E_INVALIDARG);
            return;
        }
    for (uint32_t y = 0; y < r->texture.Height; ++y)
        for (uint32_t x = 0; x < r->texture.Width; ++x)
            dx11::put_pixel(*r, x, y, colour);
    com_ret(c, S_OK);
}
void create_device(X86 *c) {
    uint32_t desc = arg(c, 7);
    for (int i : {8, 9, 11})
        if (!dx11::span(arg(c, i), 4)) {
            com_ret(c, E_POINTER);
            return;
        }
    for (int i : {8, 9, 11})
        wr32(arg(c, i), 0);
    if (!dx11::span(desc, sizeof(DXGI_SWAP_CHAIN_DESC)) || arg(c, 6) != 7) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    DXGI_SWAP_CHAIN_DESC sd;
    memcpy(&sd, gm_ptr(desc), sizeof(sd));
    auto *dev = dx11::create(K_D3D11_DEVICE, IF_D3D11_DEVICE);
    auto *ctx = dx11::create(K_D3D11_CONTEXT, IF_D3D11_CONTEXT, dev->id);
    dx11::get(dev)->context = ctx->id;
    uint32_t swap = dx11::swapchain(dev, sd);
    if (!swap) {
        com_release(ctx);
        com_release(dev);
        com_ret(c, E_INVALIDARG);
        return;
    }
    wr32(arg(c, 8), com_view(com_get(swap), IF_DXGI_SWAP));
    wr32(arg(c, 9), com_view(dev, IF_D3D11_DEVICE));
    wr32(arg(c, 11), com_view(ctx, IF_D3D11_CONTEXT));
    if (dx11::span(arg(c, 10), 4))
        wr32(arg(c, 10), 0xa000);
    LOGV("D3D11CreateDeviceAndSwapChain %ux%u format=%u windowed=%u", sd.BufferDesc.Width,
         sd.BufferDesc.Height, sd.BufferDesc.Format, sd.Windowed);
    com_ret(c, S_OK);
}
// SDK vtables include inherited slots; every unsupported entry has its own
// name, exact x86 argument count and an explicit E_NOTIMPL diagnostic.
DX_STUB(ID3D11Device_CreateBuffer, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateTexture1D, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateTexture2D, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateTexture3D, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateShaderResourceView, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateUnorderedAccessView, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateDepthStencilView, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateInputLayout, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateVertexShader, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateGeometryShader, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateGeometryShaderWithStreamOutput, E_NOTIMPL)
DX_STUB(ID3D11Device_CreatePixelShader, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateHullShader, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateDomainShader, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateComputeShader, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateClassLinkage, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateBlendState, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateDepthStencilState, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateRasterizerState, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateSamplerState, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateQuery, E_NOTIMPL)
DX_STUB(ID3D11Device_CreatePredicate, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateCounter, E_NOTIMPL)
DX_STUB(ID3D11Device_CreateDeferredContext, E_NOTIMPL)
DX_STUB(ID3D11Device_OpenSharedResource, E_NOTIMPL)
DX_STUB(ID3D11Device_CheckFormatSupport, E_NOTIMPL)
DX_STUB(ID3D11Device_CheckMultisampleQualityLevels, E_NOTIMPL)
DX_STUB(ID3D11Device_CheckCounterInfo, E_NOTIMPL)
DX_STUB(ID3D11Device_CheckCounter, E_NOTIMPL)
DX_STUB(ID3D11Device_CheckFeatureSupport, E_NOTIMPL)
DX_STUB(ID3D11Device_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11Device_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11Device_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11Device_GetFeatureLevel, E_NOTIMPL)
DX_STUB(ID3D11Device_GetCreationFlags, E_NOTIMPL)
DX_STUB(ID3D11Device_GetDeviceRemovedReason, E_NOTIMPL)
DX_STUB(ID3D11Device_GetImmediateContext, E_NOTIMPL)
DX_STUB(ID3D11Device_SetExceptionMode, E_NOTIMPL)
DX_STUB(ID3D11Device_GetExceptionMode, E_NOTIMPL)
static const ComMethod ID3D11Device_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"CreateBuffer", 4, ID3D11Device_CreateBuffer},
    {"CreateTexture1D", 4, ID3D11Device_CreateTexture1D},
    {"CreateTexture2D", 4, ID3D11Device_CreateTexture2D},
    {"CreateTexture3D", 4, ID3D11Device_CreateTexture3D},
    {"CreateShaderResourceView", 4, ID3D11Device_CreateShaderResourceView},
    {"CreateUnorderedAccessView", 4, ID3D11Device_CreateUnorderedAccessView},
    {"CreateRenderTargetView", 4, create_rtv},
    {"CreateDepthStencilView", 4, ID3D11Device_CreateDepthStencilView},
    {"CreateInputLayout", 6, ID3D11Device_CreateInputLayout},
    {"CreateVertexShader", 5, ID3D11Device_CreateVertexShader},
    {"CreateGeometryShader", 5, ID3D11Device_CreateGeometryShader},
    {"CreateGeometryShaderWithStreamOutput", 10, ID3D11Device_CreateGeometryShaderWithStreamOutput},
    {"CreatePixelShader", 5, ID3D11Device_CreatePixelShader},
    {"CreateHullShader", 5, ID3D11Device_CreateHullShader},
    {"CreateDomainShader", 5, ID3D11Device_CreateDomainShader},
    {"CreateComputeShader", 5, ID3D11Device_CreateComputeShader},
    {"CreateClassLinkage", 2, ID3D11Device_CreateClassLinkage},
    {"CreateBlendState", 3, ID3D11Device_CreateBlendState},
    {"CreateDepthStencilState", 3, ID3D11Device_CreateDepthStencilState},
    {"CreateRasterizerState", 3, ID3D11Device_CreateRasterizerState},
    {"CreateSamplerState", 3, ID3D11Device_CreateSamplerState},
    {"CreateQuery", 3, ID3D11Device_CreateQuery},
    {"CreatePredicate", 3, ID3D11Device_CreatePredicate},
    {"CreateCounter", 3, ID3D11Device_CreateCounter},
    {"CreateDeferredContext", 3, ID3D11Device_CreateDeferredContext},
    {"OpenSharedResource", 4, ID3D11Device_OpenSharedResource},
    {"CheckFormatSupport", 3, ID3D11Device_CheckFormatSupport},
    {"CheckMultisampleQualityLevels", 4, ID3D11Device_CheckMultisampleQualityLevels},
    {"CheckCounterInfo", 2, ID3D11Device_CheckCounterInfo},
    {"CheckCounter", 10, ID3D11Device_CheckCounter},
    {"CheckFeatureSupport", 4, ID3D11Device_CheckFeatureSupport},
    {"GetPrivateData", 4, ID3D11Device_GetPrivateData},
    {"SetPrivateData", 4, ID3D11Device_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11Device_SetPrivateDataInterface},
    {"GetFeatureLevel", 1, ID3D11Device_GetFeatureLevel},
    {"GetCreationFlags", 1, ID3D11Device_GetCreationFlags},
    {"GetDeviceRemovedReason", 1, ID3D11Device_GetDeviceRemovedReason},
    {"GetImmediateContext", 2, ID3D11Device_GetImmediateContext},
    {"SetExceptionMode", 2, ID3D11Device_SetExceptionMode},
    {"GetExceptionMode", 1, ID3D11Device_GetExceptionMode},
};
static_assert(std::size(ID3D11Device_methods) == 43);
DX_STUB(ID3D11DeviceContext_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_VSSetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_PSSetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_PSSetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_PSSetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_VSSetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DrawIndexed, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_Draw, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_Map, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_Unmap, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_PSSetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_IASetInputLayout, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_IASetVertexBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_IASetIndexBuffer, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DrawIndexedInstanced, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DrawInstanced, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GSSetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GSSetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_IASetPrimitiveTopology, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_VSSetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_VSSetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_Begin, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_End, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GetData, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_SetPredication, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GSSetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GSSetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_OMSetRenderTargets, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_OMSetBlendState, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_OMSetDepthStencilState, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_SOSetTargets, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DrawAuto, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DrawIndexedInstancedIndirect, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DrawInstancedIndirect, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_Dispatch, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DispatchIndirect, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_RSSetState, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_RSSetViewports, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_RSSetScissorRects, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CopySubresourceRegion, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CopyResource, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_UpdateSubresource, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CopyStructureCount, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_ClearUnorderedAccessViewUint, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_ClearUnorderedAccessViewFloat, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_ClearDepthStencilView, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GenerateMips, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_SetResourceMinLOD, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GetResourceMinLOD, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_ResolveSubresource, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_ExecuteCommandList, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_HSSetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_HSSetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_HSSetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_HSSetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DSSetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DSSetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DSSetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DSSetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSSetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSSetUnorderedAccessViews, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSSetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSSetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSSetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_VSGetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_PSGetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_PSGetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_PSGetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_VSGetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_PSGetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_IAGetInputLayout, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_IAGetVertexBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_IAGetIndexBuffer, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GSGetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GSGetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_IAGetPrimitiveTopology, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_VSGetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_VSGetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GetPredication, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GSGetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GSGetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_OMGetRenderTargets, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_OMGetBlendState, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_OMGetDepthStencilState, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_SOGetTargets, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_RSGetState, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_RSGetViewports, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_RSGetScissorRects, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_HSGetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_HSGetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_HSGetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_HSGetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DSGetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DSGetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DSGetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_DSGetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSGetShaderResources, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSGetUnorderedAccessViews, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSGetShader, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSGetSamplers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_CSGetConstantBuffers, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_ClearState, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_Flush, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GetType, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_GetContextFlags, E_NOTIMPL)
DX_STUB(ID3D11DeviceContext_FinishCommandList, E_NOTIMPL)
static const ComMethod ID3D11DeviceContext_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11DeviceContext_GetPrivateData},
    {"SetPrivateData", 4, ID3D11DeviceContext_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11DeviceContext_SetPrivateDataInterface},
    {"VSSetConstantBuffers", 4, ID3D11DeviceContext_VSSetConstantBuffers},
    {"PSSetShaderResources", 4, ID3D11DeviceContext_PSSetShaderResources},
    {"PSSetShader", 4, ID3D11DeviceContext_PSSetShader},
    {"PSSetSamplers", 4, ID3D11DeviceContext_PSSetSamplers},
    {"VSSetShader", 4, ID3D11DeviceContext_VSSetShader},
    {"DrawIndexed", 4, ID3D11DeviceContext_DrawIndexed},
    {"Draw", 3, ID3D11DeviceContext_Draw},
    {"Map", 6, ID3D11DeviceContext_Map},
    {"Unmap", 3, ID3D11DeviceContext_Unmap},
    {"PSSetConstantBuffers", 4, ID3D11DeviceContext_PSSetConstantBuffers},
    {"IASetInputLayout", 2, ID3D11DeviceContext_IASetInputLayout},
    {"IASetVertexBuffers", 6, ID3D11DeviceContext_IASetVertexBuffers},
    {"IASetIndexBuffer", 4, ID3D11DeviceContext_IASetIndexBuffer},
    {"DrawIndexedInstanced", 6, ID3D11DeviceContext_DrawIndexedInstanced},
    {"DrawInstanced", 5, ID3D11DeviceContext_DrawInstanced},
    {"GSSetConstantBuffers", 4, ID3D11DeviceContext_GSSetConstantBuffers},
    {"GSSetShader", 4, ID3D11DeviceContext_GSSetShader},
    {"IASetPrimitiveTopology", 2, ID3D11DeviceContext_IASetPrimitiveTopology},
    {"VSSetShaderResources", 4, ID3D11DeviceContext_VSSetShaderResources},
    {"VSSetSamplers", 4, ID3D11DeviceContext_VSSetSamplers},
    {"Begin", 2, ID3D11DeviceContext_Begin},
    {"End", 2, ID3D11DeviceContext_End},
    {"GetData", 5, ID3D11DeviceContext_GetData},
    {"SetPredication", 3, ID3D11DeviceContext_SetPredication},
    {"GSSetShaderResources", 4, ID3D11DeviceContext_GSSetShaderResources},
    {"GSSetSamplers", 4, ID3D11DeviceContext_GSSetSamplers},
    {"OMSetRenderTargets", 4, ID3D11DeviceContext_OMSetRenderTargets},
    {"OMSetRenderTargetsAndUnorderedAccessViews", 8,
     ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews},
    {"OMSetBlendState", 4, ID3D11DeviceContext_OMSetBlendState},
    {"OMSetDepthStencilState", 3, ID3D11DeviceContext_OMSetDepthStencilState},
    {"SOSetTargets", 4, ID3D11DeviceContext_SOSetTargets},
    {"DrawAuto", 1, ID3D11DeviceContext_DrawAuto},
    {"DrawIndexedInstancedIndirect", 3, ID3D11DeviceContext_DrawIndexedInstancedIndirect},
    {"DrawInstancedIndirect", 3, ID3D11DeviceContext_DrawInstancedIndirect},
    {"Dispatch", 4, ID3D11DeviceContext_Dispatch},
    {"DispatchIndirect", 3, ID3D11DeviceContext_DispatchIndirect},
    {"RSSetState", 2, ID3D11DeviceContext_RSSetState},
    {"RSSetViewports", 3, ID3D11DeviceContext_RSSetViewports},
    {"RSSetScissorRects", 3, ID3D11DeviceContext_RSSetScissorRects},
    {"CopySubresourceRegion", 9, ID3D11DeviceContext_CopySubresourceRegion},
    {"CopyResource", 3, ID3D11DeviceContext_CopyResource},
    {"UpdateSubresource", 7, ID3D11DeviceContext_UpdateSubresource},
    {"CopyStructureCount", 4, ID3D11DeviceContext_CopyStructureCount},
    {"ClearRenderTargetView", 3, clear_rtv},
    {"ClearUnorderedAccessViewUint", 3, ID3D11DeviceContext_ClearUnorderedAccessViewUint},
    {"ClearUnorderedAccessViewFloat", 3, ID3D11DeviceContext_ClearUnorderedAccessViewFloat},
    {"ClearDepthStencilView", 5, ID3D11DeviceContext_ClearDepthStencilView},
    {"GenerateMips", 2, ID3D11DeviceContext_GenerateMips},
    {"SetResourceMinLOD", 3, ID3D11DeviceContext_SetResourceMinLOD},
    {"GetResourceMinLOD", 2, ID3D11DeviceContext_GetResourceMinLOD},
    {"ResolveSubresource", 6, ID3D11DeviceContext_ResolveSubresource},
    {"ExecuteCommandList", 3, ID3D11DeviceContext_ExecuteCommandList},
    {"HSSetShaderResources", 4, ID3D11DeviceContext_HSSetShaderResources},
    {"HSSetShader", 4, ID3D11DeviceContext_HSSetShader},
    {"HSSetSamplers", 4, ID3D11DeviceContext_HSSetSamplers},
    {"HSSetConstantBuffers", 4, ID3D11DeviceContext_HSSetConstantBuffers},
    {"DSSetShaderResources", 4, ID3D11DeviceContext_DSSetShaderResources},
    {"DSSetShader", 4, ID3D11DeviceContext_DSSetShader},
    {"DSSetSamplers", 4, ID3D11DeviceContext_DSSetSamplers},
    {"DSSetConstantBuffers", 4, ID3D11DeviceContext_DSSetConstantBuffers},
    {"CSSetShaderResources", 4, ID3D11DeviceContext_CSSetShaderResources},
    {"CSSetUnorderedAccessViews", 5, ID3D11DeviceContext_CSSetUnorderedAccessViews},
    {"CSSetShader", 4, ID3D11DeviceContext_CSSetShader},
    {"CSSetSamplers", 4, ID3D11DeviceContext_CSSetSamplers},
    {"CSSetConstantBuffers", 4, ID3D11DeviceContext_CSSetConstantBuffers},
    {"VSGetConstantBuffers", 4, ID3D11DeviceContext_VSGetConstantBuffers},
    {"PSGetShaderResources", 4, ID3D11DeviceContext_PSGetShaderResources},
    {"PSGetShader", 4, ID3D11DeviceContext_PSGetShader},
    {"PSGetSamplers", 4, ID3D11DeviceContext_PSGetSamplers},
    {"VSGetShader", 4, ID3D11DeviceContext_VSGetShader},
    {"PSGetConstantBuffers", 4, ID3D11DeviceContext_PSGetConstantBuffers},
    {"IAGetInputLayout", 2, ID3D11DeviceContext_IAGetInputLayout},
    {"IAGetVertexBuffers", 6, ID3D11DeviceContext_IAGetVertexBuffers},
    {"IAGetIndexBuffer", 4, ID3D11DeviceContext_IAGetIndexBuffer},
    {"GSGetConstantBuffers", 4, ID3D11DeviceContext_GSGetConstantBuffers},
    {"GSGetShader", 4, ID3D11DeviceContext_GSGetShader},
    {"IAGetPrimitiveTopology", 2, ID3D11DeviceContext_IAGetPrimitiveTopology},
    {"VSGetShaderResources", 4, ID3D11DeviceContext_VSGetShaderResources},
    {"VSGetSamplers", 4, ID3D11DeviceContext_VSGetSamplers},
    {"GetPredication", 3, ID3D11DeviceContext_GetPredication},
    {"GSGetShaderResources", 4, ID3D11DeviceContext_GSGetShaderResources},
    {"GSGetSamplers", 4, ID3D11DeviceContext_GSGetSamplers},
    {"OMGetRenderTargets", 4, ID3D11DeviceContext_OMGetRenderTargets},
    {"OMGetRenderTargetsAndUnorderedAccessViews", 7,
     ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews},
    {"OMGetBlendState", 4, ID3D11DeviceContext_OMGetBlendState},
    {"OMGetDepthStencilState", 3, ID3D11DeviceContext_OMGetDepthStencilState},
    {"SOGetTargets", 3, ID3D11DeviceContext_SOGetTargets},
    {"RSGetState", 2, ID3D11DeviceContext_RSGetState},
    {"RSGetViewports", 3, ID3D11DeviceContext_RSGetViewports},
    {"RSGetScissorRects", 3, ID3D11DeviceContext_RSGetScissorRects},
    {"HSGetShaderResources", 4, ID3D11DeviceContext_HSGetShaderResources},
    {"HSGetShader", 4, ID3D11DeviceContext_HSGetShader},
    {"HSGetSamplers", 4, ID3D11DeviceContext_HSGetSamplers},
    {"HSGetConstantBuffers", 4, ID3D11DeviceContext_HSGetConstantBuffers},
    {"DSGetShaderResources", 4, ID3D11DeviceContext_DSGetShaderResources},
    {"DSGetShader", 4, ID3D11DeviceContext_DSGetShader},
    {"DSGetSamplers", 4, ID3D11DeviceContext_DSGetSamplers},
    {"DSGetConstantBuffers", 4, ID3D11DeviceContext_DSGetConstantBuffers},
    {"CSGetShaderResources", 4, ID3D11DeviceContext_CSGetShaderResources},
    {"CSGetUnorderedAccessViews", 4, ID3D11DeviceContext_CSGetUnorderedAccessViews},
    {"CSGetShader", 4, ID3D11DeviceContext_CSGetShader},
    {"CSGetSamplers", 4, ID3D11DeviceContext_CSGetSamplers},
    {"CSGetConstantBuffers", 4, ID3D11DeviceContext_CSGetConstantBuffers},
    {"ClearState", 1, ID3D11DeviceContext_ClearState},
    {"Flush", 1, ID3D11DeviceContext_Flush},
    {"GetType", 1, ID3D11DeviceContext_GetType},
    {"GetContextFlags", 1, ID3D11DeviceContext_GetContextFlags},
    {"FinishCommandList", 3, ID3D11DeviceContext_FinishCommandList},
};
static_assert(std::size(ID3D11DeviceContext_methods) == 115);
DX_STUB(ID3D11Texture2D_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11Texture2D_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11Texture2D_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11Texture2D_GetType, E_NOTIMPL)
DX_STUB(ID3D11Texture2D_SetEvictionPriority, E_NOTIMPL)
DX_STUB(ID3D11Texture2D_GetEvictionPriority, E_NOTIMPL)
DX_STUB(ID3D11Texture2D_GetDesc, E_NOTIMPL)
static const ComMethod ID3D11Texture2D_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11Texture2D_GetPrivateData},
    {"SetPrivateData", 4, ID3D11Texture2D_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11Texture2D_SetPrivateDataInterface},
    {"GetType", 2, ID3D11Texture2D_GetType},
    {"SetEvictionPriority", 2, ID3D11Texture2D_SetEvictionPriority},
    {"GetEvictionPriority", 1, ID3D11Texture2D_GetEvictionPriority},
    {"GetDesc", 2, ID3D11Texture2D_GetDesc},
};
static_assert(std::size(ID3D11Texture2D_methods) == 11);
DX_STUB(ID3D11Buffer_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11Buffer_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11Buffer_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11Buffer_GetType, E_NOTIMPL)
DX_STUB(ID3D11Buffer_SetEvictionPriority, E_NOTIMPL)
DX_STUB(ID3D11Buffer_GetEvictionPriority, E_NOTIMPL)
DX_STUB(ID3D11Buffer_GetDesc, E_NOTIMPL)
static const ComMethod ID3D11Buffer_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11Buffer_GetPrivateData},
    {"SetPrivateData", 4, ID3D11Buffer_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11Buffer_SetPrivateDataInterface},
    {"GetType", 2, ID3D11Buffer_GetType},
    {"SetEvictionPriority", 2, ID3D11Buffer_SetEvictionPriority},
    {"GetEvictionPriority", 1, ID3D11Buffer_GetEvictionPriority},
    {"GetDesc", 2, ID3D11Buffer_GetDesc},
};
static_assert(std::size(ID3D11Buffer_methods) == 11);
DX_STUB(ID3D11ShaderResourceView_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11ShaderResourceView_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11ShaderResourceView_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11ShaderResourceView_GetResource, E_NOTIMPL)
DX_STUB(ID3D11ShaderResourceView_GetDesc, E_NOTIMPL)
static const ComMethod ID3D11ShaderResourceView_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11ShaderResourceView_GetPrivateData},
    {"SetPrivateData", 4, ID3D11ShaderResourceView_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11ShaderResourceView_SetPrivateDataInterface},
    {"GetResource", 2, ID3D11ShaderResourceView_GetResource},
    {"GetDesc", 2, ID3D11ShaderResourceView_GetDesc},
};
static_assert(std::size(ID3D11ShaderResourceView_methods) == 9);
DX_STUB(ID3D11RenderTargetView_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11RenderTargetView_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11RenderTargetView_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11RenderTargetView_GetResource, E_NOTIMPL)
DX_STUB(ID3D11RenderTargetView_GetDesc, E_NOTIMPL)
static const ComMethod ID3D11RenderTargetView_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11RenderTargetView_GetPrivateData},
    {"SetPrivateData", 4, ID3D11RenderTargetView_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11RenderTargetView_SetPrivateDataInterface},
    {"GetResource", 2, ID3D11RenderTargetView_GetResource},
    {"GetDesc", 2, ID3D11RenderTargetView_GetDesc},
};
static_assert(std::size(ID3D11RenderTargetView_methods) == 9);
DX_STUB(ID3D11SamplerState_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11SamplerState_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11SamplerState_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11SamplerState_GetDesc, E_NOTIMPL)
static const ComMethod ID3D11SamplerState_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11SamplerState_GetPrivateData},
    {"SetPrivateData", 4, ID3D11SamplerState_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11SamplerState_SetPrivateDataInterface},
    {"GetDesc", 2, ID3D11SamplerState_GetDesc},
};
static_assert(std::size(ID3D11SamplerState_methods) == 8);
DX_STUB(ID3D11BlendState_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11BlendState_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11BlendState_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11BlendState_GetDesc, E_NOTIMPL)
static const ComMethod ID3D11BlendState_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11BlendState_GetPrivateData},
    {"SetPrivateData", 4, ID3D11BlendState_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11BlendState_SetPrivateDataInterface},
    {"GetDesc", 2, ID3D11BlendState_GetDesc},
};
static_assert(std::size(ID3D11BlendState_methods) == 8);
DX_STUB(ID3D11RasterizerState_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11RasterizerState_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11RasterizerState_SetPrivateDataInterface, E_NOTIMPL)
DX_STUB(ID3D11RasterizerState_GetDesc, E_NOTIMPL)
static const ComMethod ID3D11RasterizerState_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11RasterizerState_GetPrivateData},
    {"SetPrivateData", 4, ID3D11RasterizerState_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11RasterizerState_SetPrivateDataInterface},
    {"GetDesc", 2, ID3D11RasterizerState_GetDesc},
};
static_assert(std::size(ID3D11RasterizerState_methods) == 8);
DX_STUB(ID3D11VertexShader_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11VertexShader_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11VertexShader_SetPrivateDataInterface, E_NOTIMPL)
static const ComMethod ID3D11VertexShader_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11VertexShader_GetPrivateData},
    {"SetPrivateData", 4, ID3D11VertexShader_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11VertexShader_SetPrivateDataInterface},
};
static_assert(std::size(ID3D11VertexShader_methods) == 7);
DX_STUB(ID3D11PixelShader_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11PixelShader_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11PixelShader_SetPrivateDataInterface, E_NOTIMPL)
static const ComMethod ID3D11PixelShader_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11PixelShader_GetPrivateData},
    {"SetPrivateData", 4, ID3D11PixelShader_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11PixelShader_SetPrivateDataInterface},
};
static_assert(std::size(ID3D11PixelShader_methods) == 7);
DX_STUB(ID3D11InputLayout_GetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11InputLayout_SetPrivateData, E_NOTIMPL)
DX_STUB(ID3D11InputLayout_SetPrivateDataInterface, E_NOTIMPL)
static const ComMethod ID3D11InputLayout_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, dx11::get_device},
    {"GetPrivateData", 4, ID3D11InputLayout_GetPrivateData},
    {"SetPrivateData", 4, ID3D11InputLayout_SetPrivateData},
    {"SetPrivateDataInterface", 3, ID3D11InputLayout_SetPrivateDataInterface},
};
static_assert(std::size(ID3D11InputLayout_methods) == 7);
} // namespace
void d3d11_register() {
    dx11::define(IF_D3D11_DEVICE, K_D3D11_DEVICE, "d3d11.dll", "ID3D11Device", ID3D11Device_methods,
                 std::size(ID3D11Device_methods), "db6f6ddb-ac77-4e88-8253-819df9bbf140");
    dx11::define(IF_D3D11_CONTEXT, K_D3D11_CONTEXT, "d3d11.dll", "ID3D11DeviceContext",
                 ID3D11DeviceContext_methods, std::size(ID3D11DeviceContext_methods),
                 "c0bfa96c-e089-44fb-8eaf-26f8796190da");
    dx11::define(IF_D3D11_TEXTURE, K_D3D11_TEXTURE, "d3d11.dll", "ID3D11Texture2D",
                 ID3D11Texture2D_methods, std::size(ID3D11Texture2D_methods),
                 "6f15aaf2-d208-4e89-9ab4-489535d34f9c");
    dx11::define(IF_D3D11_BUFFER, K_D3D11_BUFFER, "d3d11.dll", "ID3D11Buffer", ID3D11Buffer_methods,
                 std::size(ID3D11Buffer_methods), "48570b85-d1ee-4fcd-a250-eb350722b037");
    dx11::define(IF_D3D11_SRV, K_D3D11_SRV, "d3d11.dll", "ID3D11ShaderResourceView",
                 ID3D11ShaderResourceView_methods, std::size(ID3D11ShaderResourceView_methods),
                 "b0e06fe0-8192-4e1a-b1ca-36d7414710b2");
    dx11::define(IF_D3D11_RTV, K_D3D11_RTV, "d3d11.dll", "ID3D11RenderTargetView",
                 ID3D11RenderTargetView_methods, std::size(ID3D11RenderTargetView_methods),
                 "dfdba067-0b8d-4865-875b-d7b4516cc164");
    dx11::define(IF_D3D11_SAMPLER, K_D3D11_SAMPLER, "d3d11.dll", "ID3D11SamplerState",
                 ID3D11SamplerState_methods, std::size(ID3D11SamplerState_methods),
                 "da6fea51-564c-4487-9810-f0d0f9b4e3a5");
    dx11::define(IF_D3D11_BLEND, K_D3D11_BLEND, "d3d11.dll", "ID3D11BlendState",
                 ID3D11BlendState_methods, std::size(ID3D11BlendState_methods),
                 "75b68faa-347d-4159-8f45-a0640f01cd9a");
    dx11::define(IF_D3D11_RASTER, K_D3D11_RASTER, "d3d11.dll", "ID3D11RasterizerState",
                 ID3D11RasterizerState_methods, std::size(ID3D11RasterizerState_methods),
                 "9bb4ab81-ab1a-4d8f-b506-fc04200b6ee7");
    dx11::define(IF_D3D11_VS, K_D3D11_VS, "d3d11.dll", "ID3D11VertexShader",
                 ID3D11VertexShader_methods, std::size(ID3D11VertexShader_methods),
                 "3b301d64-d678-4289-8897-22f8928b72f3");
    dx11::define(IF_D3D11_PS, K_D3D11_PS, "d3d11.dll", "ID3D11PixelShader",
                 ID3D11PixelShader_methods, std::size(ID3D11PixelShader_methods),
                 "ea82e40d-51dc-4f33-93d4-db7c9125ae8c");
    dx11::define(IF_D3D11_LAYOUT, K_D3D11_LAYOUT, "d3d11.dll", "ID3D11InputLayout",
                 ID3D11InputLayout_methods, std::size(ID3D11InputLayout_methods),
                 "e4819ddc-4cf0-4025-bd26-5de82a3e07b7");
    static const ImportShim shims[] = {
        {"d3d11.dll", "D3D11CreateDeviceAndSwapChain", 12, create_device}};
    imports_register(shims, std::size(shims));
}
