/* win32_gpu.c - the picture on the GPU (win32_gpu.h): Direct3D 11 with a flip-model swap chain.
 *
 * The frame (640 x 480 at most) is a dynamic texture drawn by one triangle over the picture's rectangle.  Its pixels
 * are scaled with "sharp bilinear" filtering: each source pixel is a solid square, and only the one output pixel on
 * the border between two is blended, so the pixels keep the same size and sharp edges at any window size.
 *
 * In missions the city may come as a list of images instead (host_city_frame): the tiles and sprites, palette
 * indices, are packed in one atlas texture; each image is drawn at its exact place (between pixels) at the window's
 * resolution, through the frame's palette, with the same sharp filtering; the frame is drawn over it, see-through
 * where its pixels say so (their top byte: 255 the city shows).  Shown with vsync.
 *
 * At dusk and at night (gpu_set_time) the city is drawn into a texture of its own, and its glowing pixels (the tiles
 * that glow: their brightest pixels or all of them) into another; a light map is cleared to the time's ambient
 * light and the lights (lamps) are added to it as soft ellipses; the city is then drawn multiplied by the light map,
 * plus what glows. */
#define COBJMACROS
#include "win32_gpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <stdlib.h>
#include <string.h>

#include "../../rt/host.h"

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_swap;
static ID3D11RenderTargetView *g_rtv;
static ID3D11VertexShader *g_vs, *g_city_vs, *g_light_vs;
static ID3D11PixelShader *g_ps, *g_city_ps, *g_city_lit_ps, *g_light_ps, *g_comp_ps;
static ID3D11InputLayout *g_city_layout, *g_light_layout;
static ID3D11BlendState *g_add;
static ID3D11Buffer *g_light_vb, *g_comp_cb;
static int g_light_vb_cap;
/* the lit city: its picture, what glows in it, the light map (as big as the picture's rectangle) */
static ID3D11Texture2D *g_lit_tex[3];
static ID3D11RenderTargetView *g_lit_rtv[3];
static ID3D11ShaderResourceView *g_lit_srv[3];
static int g_lit_w, g_lit_h;
static volatile LONG g_time; /* 0 the mission's own, 1 off (the game's own look), 2 dusk, 3 night */
static volatile LONG g_shadows = 1; /* the sun's shadows */
static ID3D11SamplerState *g_linear;
static ID3D11BlendState *g_blend;
static ID3D11RasterizerState *g_scissor;
static ID3D11Buffer *g_cb, *g_city_cb, *g_city_vb;
static ID3D11Texture2D *g_tex, *g_atlas, *g_paltex;
static ID3D11ShaderResourceView *g_srv, *g_atlas_srv, *g_pal_srv;
static int g_tex_w, g_tex_h, g_city_vb_cap;
static int g_win_w, g_win_h;
/* offscreen (gpu_init(NULL): the headless tests): the picture is drawn into a texture and read back */
static ID3D11Texture2D *g_off, *g_off_read;
/* a capture (a screenshot) of the next picture */
static volatile LONG g_capture_wanted;
static uint32_t *g_capture;
static int g_capture_w, g_capture_h;

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
    /* premultiplied: the top byte (alpha) is the transparency */
    "float4 ps(V i) : SV_Target {\n"
    "    float2 uv = i.uv;\n"
    "    float4 c;\n"
    "    if (sharp > 0.5) {\n"
    "        float2 texel = uv * tex_size;\n"
    "        float2 f = frac(texel) - 0.5;\n"
    "        float2 range = 0.5 - 0.5 / scale;\n"
    "        f = (f - clamp(f, -range, range)) * scale + 0.5;\n"
    "        uv = (floor(texel) + f) / tex_size;\n"
    "    }\n"
    "    c = t.Sample(s, uv);\n"
    "    return float4(c.rgb * (1 - c.a), 1 - c.a);\n"
    "}\n";

/* the city: an image's corner, its texels there, its rectangle in the atlas, its hat colour */
static const char k_city_shader[] =
    "cbuffer C : register(b0) { float2 frame_size; float scale; float shadow; };\n"
    "Texture2D<uint> atlas : register(t0);\n"
    "Texture2D<float4> pal : register(t1);\n"
    "struct VI { float2 pos : POSITION; float2 tex : TEXCOORD0; float4 rect : TEXCOORD1; uint hat : TEXCOORD2;"
    " uint glow : TEXCOORD3; };\n"
    "struct V { float4 pos : SV_Position; float2 tex : TEXCOORD0; nointerpolation float4 rect : TEXCOORD1;"
    " nointerpolation uint hat : TEXCOORD2; nointerpolation uint glow : TEXCOORD3; };\n"
    "V vs(VI i) {\n"
    "    V o;\n"
    "    o.pos = float4(i.pos.x / frame_size.x * 2 - 1, 1 - i.pos.y / frame_size.y * 2, 0, 1);\n"
    "    o.tex = i.tex; o.rect = i.rect; o.hat = i.hat; o.glow = i.glow;\n"
    "    return o;\n"
    "}\n"
    "float4 texel(int2 p, V i) {\n"
    "    uint c;\n"
    "    if (p.x < (int)i.rect.x || p.y < (int)i.rect.y || p.x >= (int)i.rect.z || p.y >= (int)i.rect.w) return 0;\n"
    "    c = atlas.Load(int3(p, 0));\n"
    "    if (c == 255) return 0;\n"
    "    if (c == 6 && (i.hat >> 24) != 0)\n"
    "        return float4(((i.hat >> 16) & 255) / 255.0, ((i.hat >> 8) & 255) / 255.0, (i.hat & 255) / 255.0, 1);\n"
    "    return float4(pal.Load(int3(c, 0, 0)).rgb * (1 - shadow * ((i.glow >> 8) & 127) / 127.0), 1);\n"
    "}\n"
    /* what of a texel glows: all of it (glow 2), or its colour if bright (1) */
    "float4 glowing(float4 c, uint glow) {\n"
    "    glow &= 3;\n"
    "    if (glow == 2 || (glow == 1 && dot(c.rgb, float3(0.3, 0.6, 0.1)) > 0.7)) return c;\n"
    "    return float4(0, 0, 0, c.a);\n"
    "}\n"
    "float4 ps(V i) : SV_Target {\n"
    "    float2 p = i.tex - 0.5;\n"
    "    float2 b = floor(p);\n"
    "    float2 f = saturate((p - b - 0.5) * scale + 0.5);\n"
    "    int2 q = (int2)b;\n"
    "    float4 c = lerp(lerp(texel(q, i), texel(q + int2(1, 0), i), f.x),\n"
    "                    lerp(texel(q + int2(0, 1), i), texel(q + int2(1, 1), i), f.x), f.y);\n"
    "    return c;\n" /* premultiplied: the transparent texels are 0 */
    "}\n"
    /* lit: the picture, and what glows in it (covering what glowed under it) */
    "struct O { float4 c : SV_Target0; float4 e : SV_Target1; };\n"
    "O ps_lit(V i) {\n"
    "    O o;\n"
    "    float2 p = i.tex - 0.5;\n"
    "    float2 b = floor(p);\n"
    "    float2 f = saturate((p - b - 0.5) * scale + 0.5);\n"
    "    int2 q = (int2)b;\n"
    "    float4 c00 = texel(q, i), c10 = texel(q + int2(1, 0), i), c01 = texel(q + int2(0, 1), i);\n"
    "    float4 c11 = texel(q + int2(1, 1), i);\n"
    "    o.c = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);\n"
    "    o.e = lerp(lerp(glowing(c00, i.glow), glowing(c10, i.glow), f.x),\n"
    "               lerp(glowing(c01, i.glow), glowing(c11, i.glow), f.x), f.y);\n"
    "    return o;\n"
    "}\n";

/* the light map: soft ellipses added; then the city multiplied by it, plus what glows */
static const char k_light_shader[] =
    "cbuffer C : register(b0) { float2 frame_size; float strength; float glow; float2 origin; float2 pad; };\n"
    "struct LI { float2 pos : POSITION; float2 uv : TEXCOORD0; float3 col : COLOR0; };\n"
    "struct LV { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 col : COLOR0; };\n"
    "LV vs_light(LI i) {\n"
    "    LV o;\n"
    "    o.pos = float4(i.pos.x / frame_size.x * 2 - 1, 1 - i.pos.y / frame_size.y * 2, 0, 1);\n"
    "    o.uv = i.uv; o.col = i.col;\n"
    "    return o;\n"
    "}\n"
    "float4 ps_light(LV i) : SV_Target {\n"
    "    float f = saturate(1 - dot(i.uv, i.uv));\n"
    "    return float4(i.col * (f * f * strength), 0);\n"
    "}\n"
    "Texture2D scene : register(t0);\n"
    "Texture2D emis : register(t1);\n"
    "Texture2D light : register(t2);\n"
    "float4 ps_comp(float4 pos : SV_Position) : SV_Target {\n"
    "    int3 p = int3(pos.xy - origin, 0);\n"
    "    float4 c = scene.Load(p);\n"
    "    return float4(c.rgb * light.Load(p).rgb + emis.Load(p).rgb * glow, c.a);\n"
    "}\n";

typedef struct Consts {
    float tex_size[2], scale[2], sharp, pad[3];
} Consts;

typedef struct CityConsts {
    float frame_size[2], scale, shadow;
} CityConsts;

typedef struct CityVertex {
    float x, y, u, v;
    float rect[4];
    uint32_t hat, glow;
} CityVertex;

typedef struct LightVertex {
    float x, y, u, v;
    float col[3];
} LightVertex;

typedef struct LightConsts {
    float frame_size[2], strength, glow, origin[2], pad[2];
} LightConsts;

/* the times of day: the ambient light, how strong the lights are, how much what glows adds */
static const struct {
    float ambient[3], strength, glow;
} k_times[4] = {
    {{1, 1, 1}, 0.6f, 0.8f}, /* the mission's own: its colours; strength and glow times how dark it is */
    {{1, 1, 1}, 0, 0},
    {{0.80f, 0.60f, 0.52f}, 0.55f, 0.45f},
    {{0.24f, 0.27f, 0.45f}, 0.8f, 1.0f},
};

/* ---- the city's images and list, set on the game thread, drawn on the UI thread ---- */
#define ATLAS 2048
#define NB_IMAGES (256 + 4096)

typedef struct Image {
    int w, h, ax, ay; /* size, place in the atlas (w 0: not defined) */
    uint8_t *pixels;  /* to upload (NULL once uploaded) */
} Image;

static SRWLOCK g_city_lock = SRWLOCK_INIT;
static Image g_images[NB_IMAGES];
static int g_shelf_x, g_shelf_y, g_shelf_h; /* the atlas packer */
static int g_atlas_reset;                    /* the atlas must be cleared */
static HostCityQuad *g_city_quads;
static int g_city_n, g_city_cap, g_city_on;
static HostCityLight *g_city_lights;
static float g_city_night; /* how dark the mission's palette is */
static int g_city_nl, g_city_lcap;
static uint32_t g_city_pal[256];
static int g_city_rect[4];
static float g_city_scale = 1.0f;

static void release(IUnknown **p)
{
    if (*p != NULL) {
        (*p)->lpVtbl->Release(*p);
        *p = NULL;
    }
}
#define RELEASE(p) release((IUnknown **)&(p))

void gpu_city_reset(void)
{
    int i;
    AcquireSRWLockExclusive(&g_city_lock);
    for (i = 0; i < NB_IMAGES; i++) {
        free(g_images[i].pixels);
        memset(&g_images[i], 0, sizeof g_images[i]);
    }
    g_shelf_x = g_shelf_y = g_shelf_h = 0;
    g_atlas_reset = 1;
    ReleaseSRWLockExclusive(&g_city_lock);
}

void gpu_city_image(int id, int w, int h, const uint8_t *pixels)
{
    Image *im;
    if (id < 0 || id >= NB_IMAGES || w <= 0 || h <= 0 || w > 256 || h > 256) return;
    AcquireSRWLockExclusive(&g_city_lock);
    im = &g_images[id];
    if (im->w == 0) {
        /* shelves, with a transparent pixel between images */
        if (g_shelf_x + w + 1 > ATLAS) {
            g_shelf_x = 0;
            g_shelf_y += g_shelf_h + 1;
            g_shelf_h = 0;
        }
        if (g_shelf_y + h + 1 > ATLAS) {
            ReleaseSRWLockExclusive(&g_city_lock);
            return; /* full: not drawn */
        }
        im->ax = g_shelf_x;
        im->ay = g_shelf_y;
        g_shelf_x += w + 1;
        if (h > g_shelf_h) g_shelf_h = h;
    }
    im->w = w;
    im->h = h;
    free(im->pixels);
    im->pixels = (uint8_t *)malloc((size_t)(w * h));
    if (im->pixels != NULL) memcpy(im->pixels, pixels, (size_t)(w * h));
    ReleaseSRWLockExclusive(&g_city_lock);
}

void gpu_set_shadows(int on)
{
    InterlockedExchange(&g_shadows, on != 0);
}

void gpu_set_time(int time)
{
    InterlockedExchange(&g_time, time < 0 ? 0 : time > 3 ? 3 : time);
}

void gpu_city_frame(const HostCityQuad *quads, int n, const uint32_t *palette, int x0, int y0, int x1, int y1,
                    float scale, const HostCityLight *lights, int nlights, float night)
{
    AcquireSRWLockExclusive(&g_city_lock);
    g_city_night = night;
    if (nlights > g_city_lcap) {
        HostCityLight *l = (HostCityLight *)realloc(g_city_lights, sizeof *l * (size_t)nlights);
        if (l != NULL) {
            g_city_lights = l;
            g_city_lcap = nlights;
        }
    }
    g_city_nl = nlights <= g_city_lcap ? nlights : 0;
    if (g_city_nl > 0) memcpy(g_city_lights, lights, sizeof *lights * (size_t)g_city_nl);
    if (n > g_city_cap) {
        HostCityQuad *q = (HostCityQuad *)realloc(g_city_quads, sizeof *q * (size_t)n);
        if (q == NULL) {
            ReleaseSRWLockExclusive(&g_city_lock);
            return;
        }
        g_city_quads = q;
        g_city_cap = n;
    }
    if (n > 0) memcpy(g_city_quads, quads, sizeof *quads * (size_t)n);
    g_city_n = n;
    memcpy(g_city_pal, palette, sizeof g_city_pal);
    g_city_rect[0] = x0;
    g_city_rect[1] = y0;
    g_city_rect[2] = x1;
    g_city_rect[3] = y1;
    g_city_scale = scale > 0.05f ? scale : 1.0f;
    g_city_on = 1;
    ReleaseSRWLockExclusive(&g_city_lock);
}

void gpu_city_off(void)
{
    AcquireSRWLockExclusive(&g_city_lock);
    g_city_on = 0;
    ReleaseSRWLockExclusive(&g_city_lock);
}

/* ---- set up ---- */
/* the offscreen picture, g_win_w x g_win_h, and its copy the CPU reads */
static int make_offscreen(void)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)g_win_w;
    td.Height = (UINT)g_win_h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_off)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_off, NULL, &g_rtv)))
        return 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_off_read));
}

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

static ID3DBlob *compile(const char *src, size_t n, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(src, n, "synd", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        if (err != NULL) host_log((const char *)ID3D10Blob_GetBufferPointer(err));
        RELEASE(err);
        return NULL;
    }
    RELEASE(err);
    return code;
}

static int make_city(void)
{
    static const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32_UINT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    static const D3D11_INPUT_ELEMENT_DESC light_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3DBlob *litb = compile(k_city_shader, sizeof k_city_shader - 1, "ps_lit", "ps_4_0");
    ID3DBlob *lvb = compile(k_light_shader, sizeof k_light_shader - 1, "vs_light", "vs_4_0");
    ID3DBlob *lpb = compile(k_light_shader, sizeof k_light_shader - 1, "ps_light", "ps_4_0");
    ID3DBlob *cpb = compile(k_light_shader, sizeof k_light_shader - 1, "ps_comp", "ps_4_0");
    ID3DBlob *vsb = compile(k_city_shader, sizeof k_city_shader - 1, "vs", "vs_4_0");
    ID3DBlob *psb = compile(k_city_shader, sizeof k_city_shader - 1, "ps", "ps_4_0");
    D3D11_TEXTURE2D_DESC td;
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    D3D11_RASTERIZER_DESC rs;
    int ok = vsb != NULL && psb != NULL &&
             SUCCEEDED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
                                                       NULL, &g_city_vs)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
                                                      NULL, &g_city_ps)) &&
             SUCCEEDED(ID3D11Device_CreateInputLayout(g_dev, layout, 5, ID3D10Blob_GetBufferPointer(vsb),
                                                      ID3D10Blob_GetBufferSize(vsb), &g_city_layout)) &&
             litb != NULL && lvb != NULL && lpb != NULL && cpb != NULL &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(litb), ID3D10Blob_GetBufferSize(litb),
                                                      NULL, &g_city_lit_ps)) &&
             SUCCEEDED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(lvb), ID3D10Blob_GetBufferSize(lvb),
                                                       NULL, &g_light_vs)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(lpb), ID3D10Blob_GetBufferSize(lpb),
                                                      NULL, &g_light_ps)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(cpb), ID3D10Blob_GetBufferSize(cpb),
                                                      NULL, &g_comp_ps)) &&
             SUCCEEDED(ID3D11Device_CreateInputLayout(g_dev, light_layout, 3, ID3D10Blob_GetBufferPointer(lvb),
                                                      ID3D10Blob_GetBufferSize(lvb), &g_light_layout));
    RELEASE(vsb);
    RELEASE(psb);
    RELEASE(litb);
    RELEASE(lvb);
    RELEASE(lpb);
    RELEASE(cpb);
    if (!ok) return 0;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = ATLAS;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_atlas)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_atlas, NULL, &g_atlas_srv)))
        return 0;
    td.Width = 256;
    td.Height = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_paltex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_paltex, NULL, &g_pal_srv)))
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(CityConsts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_city_cb))) return 0;
    bd.ByteWidth = sizeof(LightConsts);
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_comp_cb))) return 0;
    /* premultiplied alpha: the city's transparent texels and the frame's see-through pixels */
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bl, &g_blend))) return 0;
    /* the lights add up */
    bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bl, &g_add))) return 0;
    memset(&rs, 0, sizeof rs);
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.ScissorEnable = TRUE;
    rs.DepthClipEnable = TRUE;
    return SUCCEEDED(ID3D11Device_CreateRasterizerState(g_dev, &rs, &g_scissor));
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
    if (FAILED(hr) && hwnd == NULL) /* offscreen: Windows' software device will do */
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                               D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx);
    if (FAILED(hr)) {
        host_log("gpu: no Direct3D 11 hardware device");
        return 0;
    }
    if (hwnd == NULL) {
        if (g_win_w <= 0 || g_win_h <= 0 || !make_offscreen()) {
            gpu_shutdown();
            return 0;
        }
        goto shaders;
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
shaders:
    vsb = compile(k_shader, sizeof k_shader - 1, "vs", "vs_4_0");
    psb = compile(k_shader, sizeof k_shader - 1, "ps", "ps_4_0");
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
    if (!make_city()) {
        host_log("gpu: the city cannot be drawn on the GPU");
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
    RELEASE(g_atlas_srv);
    RELEASE(g_atlas);
    RELEASE(g_pal_srv);
    RELEASE(g_paltex);
    RELEASE(g_city_vb);
    RELEASE(g_city_cb);
    RELEASE(g_light_vb);
    RELEASE(g_comp_cb);
    RELEASE(g_city_lit_ps);
    RELEASE(g_light_vs);
    RELEASE(g_light_ps);
    RELEASE(g_comp_ps);
    RELEASE(g_light_layout);
    RELEASE(g_add);
    {
        int k;
        for (k = 0; k < 3; k++) {
            RELEASE(g_lit_srv[k]);
            RELEASE(g_lit_rtv[k]);
            RELEASE(g_lit_tex[k]);
        }
        g_lit_w = g_lit_h = 0;
        g_light_vb_cap = 0;
    }
    RELEASE(g_city_layout);
    RELEASE(g_city_vs);
    RELEASE(g_city_ps);
    RELEASE(g_blend);
    RELEASE(g_scissor);
    RELEASE(g_cb);
    RELEASE(g_linear);
    RELEASE(g_ps);
    RELEASE(g_vs);
    RELEASE(g_rtv);
    RELEASE(g_off);
    RELEASE(g_off_read);
    RELEASE(g_swap);
    RELEASE(g_ctx);
    RELEASE(g_dev);
    g_tex_w = g_tex_h = 0;
    g_city_vb_cap = 0;
}

void gpu_resize(int w, int h)
{
    if (g_dev == NULL) { /* before gpu_init(NULL): the offscreen size */
        g_win_w = w;
        g_win_h = h;
        return;
    }
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
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; /* 0xTTRRGGBB in memory: B, G, R, the transparency */
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

/* the textures of the lit city, w x h */
static int lit_targets(int w, int h)
{
    static const DXGI_FORMAT formats[3] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                           DXGI_FORMAT_R16G16B16A16_FLOAT};
    D3D11_TEXTURE2D_DESC td;
    int k;
    if (g_lit_tex[0] != NULL && w == g_lit_w && h == g_lit_h) return 1;
    for (k = 0; k < 3; k++) {
        RELEASE(g_lit_srv[k]);
        RELEASE(g_lit_rtv[k]);
        RELEASE(g_lit_tex[k]);
    }
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (k = 0; k < 3; k++) {
        td.Format = formats[k];
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_lit_tex[k])) ||
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_lit_tex[k], NULL, &g_lit_rtv[k])) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_lit_tex[k], NULL, &g_lit_srv[k])))
            return 0;
    }
    g_lit_w = w;
    g_lit_h = h;
    return 1;
}

/* the city of the last frame from the game thread: images to upload, then its quads */
static void draw_city(RECT dst, int fw, int fh)
{
    static CityVertex *verts;
    static int verts_cap;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_RECT sc;
    CityConsts cc;
    static LightVertex *lverts;
    static int lverts_cap;
    UINT stride = sizeof(CityVertex), offset = 0;
    int i, n, nv = 0, nlv = 0, time = (int)g_time;
    float strength, glow;
    float sx = (float)(dst.right - dst.left) / (float)fw, sy = (float)(dst.bottom - dst.top) / (float)fh, zoom;
    AcquireSRWLockExclusive(&g_city_lock);
    if (!g_city_on) {
        ReleaseSRWLockExclusive(&g_city_lock);
        return;
    }
    if (g_atlas_reset) {
        /* everything transparent */
        static uint8_t clear[ATLAS];
        D3D11_BOX box = {0, 0, 0, ATLAS, 1, 1};
        memset(clear, 255, sizeof clear);
        for (i = 0; i < ATLAS; i++) {
            box.top = (UINT)i;
            box.bottom = (UINT)i + 1;
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_atlas, 0, &box, clear, ATLAS, 0);
        }
        g_atlas_reset = 0;
    }
    for (i = 0; i < NB_IMAGES; i++) {
        Image *im = &g_images[i];
        if (im->pixels != NULL) {
            D3D11_BOX box = {(UINT)im->ax, (UINT)im->ay, 0, (UINT)(im->ax + im->w), (UINT)(im->ay + im->h), 1};
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_atlas, 0, &box, im->pixels, (UINT)im->w, 0);
            free(im->pixels);
            im->pixels = NULL;
        }
    }
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_paltex, 0, NULL, g_city_pal, 256 * 4, 0);
    n = g_city_n;
    zoom = g_city_scale;
    if (n * 6 > verts_cap) {
        CityVertex *v = (CityVertex *)realloc(verts, sizeof *v * (size_t)n * 6);
        if (v == NULL) {
            ReleaseSRWLockExclusive(&g_city_lock);
            return;
        }
        verts = v;
        verts_cap = n * 6;
    }
    for (i = 0; i < n; i++) {
        const HostCityQuad *q = &g_city_quads[i];
        const Image *im = &g_images[q->image];
        float x0 = q->x, y0 = q->y, x1, y1, u0, u1, v0 = (float)im->ay, v1 = (float)(im->ay + im->h);
        CityVertex c[4];
        int k;
        if (im->w == 0) continue;
        x1 = x0 + (float)im->w * zoom;
        y1 = y0 + (float)im->h * zoom;
        u0 = (q->flip & 1) ? (float)(im->ax + im->w) : (float)im->ax;
        u1 = (q->flip & 1) ? (float)im->ax : (float)(im->ax + im->w);
        for (k = 0; k < 4; k++) {
            c[k].x = (k & 1) ? x1 : x0;
            c[k].y = (k & 2) ? y1 : y0;
            c[k].u = (k & 1) ? u1 : u0;
            c[k].v = (k & 2) ? v1 : v0;
            c[k].rect[0] = (float)im->ax;
            c[k].rect[1] = (float)im->ay;
            c[k].rect[2] = (float)(im->ax + im->w);
            c[k].rect[3] = (float)(im->ay + im->h);
            c[k].hat = q->hat;
            c[k].glow = q->glow | (uint32_t)(q->flip >> 1) << 8; /* the shadow above the glow */
        }
        verts[nv++] = c[0];
        verts[nv++] = c[1];
        verts[nv++] = c[2];
        verts[nv++] = c[2];
        verts[nv++] = c[1];
        verts[nv++] = c[3];
    }
    sc.left = dst.left + (LONG)((float)g_city_rect[0] * sx);
    sc.top = dst.top + (LONG)((float)g_city_rect[1] * sy);
    sc.right = dst.left + (LONG)((float)g_city_rect[2] * sx);
    sc.bottom = dst.top + (LONG)((float)g_city_rect[3] * sy);
    /* off; or the mission's own look, lit as dark as it is (a day mission: as the game draws it) */
    strength = k_times[time].strength;
    glow = k_times[time].glow;
    if (time == 0) {
        strength *= g_city_night;
        glow *= g_city_night;
    }
    if (time == 1 || (time == 0 && g_city_night < 0.02f)) time = -1;
    /* the lights: a quad around each ellipse */
    if (time >= 0 && g_city_nl * 6 > lverts_cap) {
        LightVertex *v = (LightVertex *)realloc(lverts, sizeof *v * (size_t)g_city_nl * 6);
        if (v != NULL) {
            lverts = v;
            lverts_cap = g_city_nl * 6;
        }
    }
    for (i = 0; time >= 0 && i < g_city_nl && (i + 1) * 6 <= lverts_cap; i++) {
        const HostCityLight *l = &g_city_lights[i];
        LightVertex c[4];
        int k;
        for (k = 0; k < 4; k++) {
            c[k].x = l->x + ((k & 1) ? l->rx : -l->rx);
            c[k].y = l->y + ((k & 2) ? l->ry : -l->ry);
            c[k].u = (k & 1) ? 1.0f : -1.0f;
            c[k].v = (k & 2) ? 1.0f : -1.0f;
            c[k].col[0] = (float)((l->colour >> 16) & 0xFF) / 255.0f;
            c[k].col[1] = (float)((l->colour >> 8) & 0xFF) / 255.0f;
            c[k].col[2] = (float)(l->colour & 0xFF) / 255.0f;
        }
        lverts[nlv++] = c[0];
        lverts[nlv++] = c[1];
        lverts[nlv++] = c[2];
        lverts[nlv++] = c[2];
        lverts[nlv++] = c[1];
        lverts[nlv++] = c[3];
    }
    ReleaseSRWLockExclusive(&g_city_lock);
    if (nv == 0) return;
    if (nv > g_city_vb_cap) {
        D3D11_BUFFER_DESC bd;
        RELEASE(g_city_vb);
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (UINT)(sizeof(CityVertex) * (size_t)nv * 2);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_city_vb))) return;
        g_city_vb_cap = nv * 2;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_city_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, verts, sizeof(CityVertex) * (size_t)nv);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_city_vb, 0);
    cc.frame_size[0] = (float)fw;
    cc.frame_size[1] = (float)fh;
    cc.scale = sx * zoom > 1.0f ? sx * zoom : 1.0f; /* window pixels a texel */
    cc.shadow = g_shadows ? 0.42f : 0.0f; /* how dark the sun's shadows are */
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_city_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &cc, sizeof cc);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_city_cb, 0);
    }
    {
        ID3D11ShaderResourceView *srv[2] = {g_atlas_srv, g_pal_srv};
        int lit = time >= 0 && lit_targets(dst.right - dst.left, dst.bottom - dst.top);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, g_city_layout);
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_city_vb, &stride, &offset);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_city_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_city_cb);
        ID3D11DeviceContext_PSSetShader(g_ctx, lit ? g_city_lit_ps : g_city_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_city_cb);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, srv);
        ID3D11DeviceContext_RSSetState(g_ctx, g_scissor);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, NULL, 0xFFFFFFFFu);
        if (!lit) {
            ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
            ID3D11DeviceContext_Draw(g_ctx, (UINT)nv, 0);
        } else {
            static const float clear[4] = {0, 0, 0, 0};
            ID3D11ShaderResourceView *none[3] = {NULL, NULL, NULL};
            D3D11_VIEWPORT vp = {0, 0, (float)(dst.right - dst.left), (float)(dst.bottom - dst.top), 0, 1};
            D3D11_VIEWPORT back = {(float)dst.left, (float)dst.top, vp.Width, vp.Height, 0, 1};
            D3D11_RECT lsc = {sc.left - dst.left, sc.top - dst.top, sc.right - dst.left, sc.bottom - dst.top};
            float ambient[4] = {k_times[time].ambient[0], k_times[time].ambient[1], k_times[time].ambient[2], 1};
            LightConsts lc;
            UINT lstride = sizeof(LightVertex);
            /* the city and what glows in it, in their textures */
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 2, g_lit_rtv, NULL);
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_lit_rtv[0], clear);
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_lit_rtv[1], clear);
            ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
            ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &lsc);
            ID3D11DeviceContext_Draw(g_ctx, (UINT)nv, 0);
            ID3D11DeviceContext_RSSetState(g_ctx, NULL);
            /* the light map: the ambient light, the lights added */
            memset(&lc, 0, sizeof lc);
            lc.frame_size[0] = (float)fw;
            lc.frame_size[1] = (float)fh;
            lc.strength = strength;
            lc.glow = glow;
            lc.origin[0] = (float)dst.left;
            lc.origin[1] = (float)dst.top;
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_comp_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                memcpy(map.pData, &lc, sizeof lc);
                ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_comp_cb, 0);
            }
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_lit_rtv[2], NULL);
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_lit_rtv[2], ambient);
            if (nlv > 0) {
                if (nlv > g_light_vb_cap) {
                    D3D11_BUFFER_DESC bd;
                    RELEASE(g_light_vb);
                    memset(&bd, 0, sizeof bd);
                    bd.ByteWidth = (UINT)(sizeof(LightVertex) * (size_t)nlv * 2);
                    bd.Usage = D3D11_USAGE_DYNAMIC;
                    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    g_light_vb_cap = SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_light_vb)) ? nlv * 2 : 0;
                }
                if (g_light_vb != NULL && nlv <= g_light_vb_cap &&
                    SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_light_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    memcpy(map.pData, lverts, sizeof(LightVertex) * (size_t)nlv);
                    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_light_vb, 0);
                    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_light_layout);
                    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_light_vb, &lstride, &offset);
                    ID3D11DeviceContext_VSSetShader(g_ctx, g_light_vs, NULL, 0);
                    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_comp_cb);
                    ID3D11DeviceContext_PSSetShader(g_ctx, g_light_ps, NULL, 0);
                    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_comp_cb);
                    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_add, NULL, 0xFFFFFFFFu);
                    ID3D11DeviceContext_Draw(g_ctx, (UINT)nlv, 0);
                }
            }
            /* the city, lit, into the picture */
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &back);
            ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
            ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(g_ctx, g_comp_ps, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_comp_cb);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 3, g_lit_srv);
            ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, NULL, 0xFFFFFFFFu);
            ID3D11DeviceContext_Draw(g_ctx, 3, 0);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 3, none);
        }
        ID3D11DeviceContext_RSSetState(g_ctx, NULL);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    }
}

/* the picture's rectangle `dst`, before it is shown */
static void capture(RECT dst)
{
    ID3D11Texture2D *src = g_off, *read = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    int w = dst.right - dst.left, h = dst.bottom - dst.top, y;
    if (w <= 0 || h <= 0) return;
    if (g_swap != NULL && FAILED(IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&src))) return;
    ID3D11Texture2D_GetDesc(src, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &read))) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)read, (ID3D11Resource *)src);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)read, 0, D3D11_MAP_READ, 0, &map))) {
            free(g_capture);
            g_capture = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
            if (g_capture != NULL) {
                for (y = 0; y < h; y++)
                    memcpy(g_capture + (size_t)y * w,
                           (const uint8_t *)map.pData + (size_t)(dst.top + y) * map.RowPitch + (size_t)dst.left * 4u,
                           (size_t)w * 4u);
                g_capture_w = w;
                g_capture_h = h;
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)read, 0);
        }
        RELEASE(read);
    }
    if (g_swap != NULL) RELEASE(src);
}

void gpu_capture(void)
{
    InterlockedExchange(&g_capture_wanted, 1);
}

int gpu_capture_take(uint32_t **pixels, int *w, int *h)
{
    if (g_capture == NULL) return 0;
    *pixels = g_capture;
    *w = g_capture_w;
    *h = g_capture_h;
    g_capture = NULL;
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
    vp.TopLeftX = (float)dst.left;
    vp.TopLeftY = (float)dst.top;
    vp.Width = (float)(dst.right - dst.left);
    vp.Height = (float)(dst.bottom - dst.top);
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    /* the city first (when the frame is see-through over it) */
    if (sharp && pixels != NULL && w > 0 && h > 0) draw_city(dst, w, h);
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
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_linear);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    }
    if (g_capture_wanted) {
        InterlockedExchange(&g_capture_wanted, 0);
        capture(dst);
    }
    if (g_swap != NULL)
        IDXGISwapChain1_Present(g_swap, 1, 0);
    else
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_off_read, (ID3D11Resource *)g_off);
}

int gpu_read(uint32_t *pixels)
{
    D3D11_MAPPED_SUBRESOURCE map;
    int y;
    if (g_off_read == NULL || FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_off_read, 0, D3D11_MAP_READ, 0, &map)))
        return 0;
    for (y = 0; y < g_win_h; y++)
        memcpy(pixels + (size_t)y * g_win_w, (const uint8_t *)map.pData + (size_t)y * map.RowPitch, (size_t)g_win_w * 4u);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_off_read, 0);
    return 1;
}
