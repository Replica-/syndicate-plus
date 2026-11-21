/* win32_gpu.c - the picture on the GPU (win32_gpu.h): Direct3D 11 with a flip-model swap chain, the frame in a
 * dynamic texture drawn by one triangle that covers the destination rectangle.
 *
 * The game's pixels are scaled with "sharp bilinear" filtering: each source pixel is a solid square, and only the
 * one output pixel on the border between two is blended, so the pixels keep the same size and sharp edges at any
 * window size.  Shown with vsync. */
#define COBJMACROS
#include "win32_gpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <string.h>

#include "../../rt/host.h"

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_swap;
static ID3D11RenderTargetView *g_rtv;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11SamplerState *g_linear;
static ID3D11Buffer *g_cb;
static ID3D11Texture2D *g_tex;
static ID3D11ShaderResourceView *g_srv;
static int g_tex_w, g_tex_h;
static int g_win_w, g_win_h;

static const char k_shader[] =
    "cbuffer C : register(b0) { float2 tex_size; float2 scale; float sharp; float3 pad; };\n"
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "    V o;\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.uv = uv;\n"
    "    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 ps(V i) : SV_Target {\n"
    "    float2 uv = i.uv;\n"
    "    if (sharp > 0.5) {\n"
    "        float2 texel = uv * tex_size;\n"
    "        float2 f = frac(texel) - 0.5;\n"
    "        float2 range = 0.5 - 0.5 / scale;\n"
    "        f = (f - clamp(f, -range, range)) * scale + 0.5;\n"
    "        uv = (floor(texel) + f) / tex_size;\n"
    "    }\n"
    "    return float4(t.Sample(s, uv).rgb, 1);\n"
    "}\n";

typedef struct Consts {
    float tex_size[2], scale[2], sharp, pad[3];
} Consts;

static void release(IUnknown **p)
{
    if (*p != NULL) {
        (*p)->lpVtbl->Release(*p);
        *p = NULL;
    }
}
#define RELEASE(p) release((IUnknown **)&(p))

static int make_target(void)
{
    ID3D11Texture2D *back = NULL;
    if (FAILED(IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&back))) return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)back, NULL, &g_rtv))) {
        RELEASE(back);
        return 0;
    }
    RELEASE(back);
    return 1;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(k_shader, sizeof k_shader - 1, "synd", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                          0, &code, &err))) {
        if (err != NULL) host_log((const char *)ID3D10Blob_GetBufferPointer(err));
        RELEASE(err);
        return NULL;
    }
    RELEASE(err);
    return code;
}

int gpu_init(HWND hwnd)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    IDXGIDevice *dxdev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd;
    ID3DBlob *vsb, *psb;
    D3D11_SAMPLER_DESC smp;
    D3D11_BUFFER_DESC cbd;
    RECT rc;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   3, D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx);
    if (FAILED(hr)) {
        host_log("gpu: no Direct3D 11 hardware device");
        return 0;
    }
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxdev)) ||
        FAILED(IDXGIDevice_GetAdapter(dxdev, &adapter)) ||
        FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory))) {
        RELEASE(dxdev);
        RELEASE(adapter);
        gpu_shutdown();
        return 0;
    }
    GetClientRect(hwnd, &rc);
    g_win_w = rc.right > 1 ? rc.right : 1;
    g_win_h = rc.bottom > 1 ? rc.bottom : 1;
    memset(&sd, 0, sizeof sd);
    sd.Width = (UINT)g_win_w;
    sd.Height = (UINT)g_win_h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_dev, hwnd, &sd, NULL, NULL, &g_swap);
    /* Alt+Enter is the window's own (borderless full screen), not DXGI's */
    if (SUCCEEDED(hr)) IDXGIFactory2_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    RELEASE(factory);
    RELEASE(adapter);
    RELEASE(dxdev);
    if (FAILED(hr) || !make_target()) {
        host_log("gpu: no swap chain");
        gpu_shutdown();
        return 0;
    }
    vsb = compile("vs", "vs_4_0");
    psb = compile("ps", "ps_4_0");
    if (vsb == NULL || psb == NULL ||
        FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
                                               NULL, &g_vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
                                              NULL, &g_ps))) {
        RELEASE(vsb);
        RELEASE(psb);
        host_log("gpu: the shaders do not compile");
        gpu_shutdown();
        return 0;
    }
    RELEASE(vsb);
    RELEASE(psb);
    memset(&smp, 0, sizeof smp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    memset(&cbd, 0, sizeof cbd);
    cbd.ByteWidth = sizeof(Consts);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &smp, &g_linear)) ||
        FAILED(ID3D11Device_CreateBuffer(g_dev, &cbd, NULL, &g_cb))) {
        gpu_shutdown();
        return 0;
    }
    host_log("gpu: Direct3D 11");
    return 1;
}

void gpu_shutdown(void)
{
    if (g_ctx != NULL) ID3D11DeviceContext_ClearState(g_ctx);
    RELEASE(g_srv);
    RELEASE(g_tex);
    RELEASE(g_cb);
    RELEASE(g_linear);
    RELEASE(g_ps);
    RELEASE(g_vs);
    RELEASE(g_rtv);
    RELEASE(g_swap);
    RELEASE(g_ctx);
    RELEASE(g_dev);
    g_tex_w = g_tex_h = 0;
}

void gpu_resize(int w, int h)
{
    if (g_swap == NULL || w <= 0 || h <= 0 || (w == g_win_w && h == g_win_h)) return;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    RELEASE(g_rtv);
    if (FAILED(IDXGISwapChain1_ResizeBuffers(g_swap, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0))) return;
    g_win_w = w;
    g_win_h = h;
    make_target();
}

/* a dynamic texture of w x h (made again when the size changes) */
static int texture(int w, int h)
{
    D3D11_TEXTURE2D_DESC td;
    if (g_tex != NULL && w == g_tex_w && h == g_tex_h) return 1;
    RELEASE(g_srv);
    RELEASE(g_tex);
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; /* 0x00RRGGBB in memory: B, G, R, (unused) */
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_tex, NULL, &g_srv))) {
        RELEASE(g_tex);
        return 0;
    }
    g_tex_w = w;
    g_tex_h = h;
    return 1;
}

void gpu_present(const uint32_t *pixels, int w, int h, RECT dst, int sharp)
{
    static const float black[4] = {0, 0, 0, 1};
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp;
    int y;
    if (g_rtv == NULL) return;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, black);
    if (pixels != NULL && w > 0 && h > 0 && texture(w, h) &&
        SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        Consts c;
        for (y = 0; y < h; y++) memcpy((uint8_t *)map.pData + (size_t)y * map.RowPitch, pixels + (size_t)y * w, (size_t)w * 4u);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_tex, 0);
        memset(&c, 0, sizeof c);
        c.tex_size[0] = (float)w;
        c.tex_size[1] = (float)h;
        c.scale[0] = (float)(dst.right - dst.left) / (float)w;
        c.scale[1] = (float)(dst.bottom - dst.top) / (float)h;
        if (c.scale[0] < 1.0f) c.scale[0] = 1.0f;
        if (c.scale[1] < 1.0f) c.scale[1] = 1.0f;
        c.sharp = sharp ? 1.0f : 0.0f;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &c, sizeof c);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
        }
        vp.TopLeftX = (float)dst.left;
        vp.TopLeftY = (float)dst.top;
        vp.Width = (float)(dst.right - dst.left);
        vp.Height = (float)(dst.bottom - dst.top);
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_linear);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    }
    IDXGISwapChain1_Present(g_swap, 1, 0);
}
