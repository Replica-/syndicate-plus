/* win32_gpu.c - the picture on the GPU (win32_gpu.h): Direct3D 11 with a flip-model swap chain.
 *
 * The frame (640 x 480 at most) is a dynamic texture drawn by one triangle over the picture's rectangle.  Its pixels
 * are scaled with "sharp bilinear" filtering: each source pixel is a solid square, and only the one output pixel on
 * the border between two is blended, so the pixels keep the same size and sharp edges at any window size.
 *
 * In missions the city may come as a list of images instead (host_city_frame): the tiles and sprites, in true
 * colour, are packed in one atlas texture; each image is drawn at its exact place (between pixels) at the window's
 * resolution, with the same sharp filtering; the frame is drawn over it, see-through where its pixels say so (their
 * top byte: 255 the city shows).  Shown with vsync.
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt/host.h"

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_swap;
static ID3D11RenderTargetView *g_rtv;
static ID3D11VertexShader *g_vs, *g_city_vs, *g_light_vs;
static ID3D11PixelShader *g_ps, *g_city_ps, *g_city_lit_ps, *g_light_ps, *g_comp_ps, *g_fire_ps, *g_smoke_ps, *g_explode_ps, *g_flash_ps, *g_hmax1_ps, *g_hmax2_ps;
static ID3D11InputLayout *g_city_layout, *g_light_layout;
static ID3D11BlendState *g_add;
static ID3D11Buffer *g_light_vb, *g_comp_cb;
static int g_light_vb_cap;
/* the lit city: its picture, what glows in it, the light map (as big as the picture's rectangle) */
static ID3D11Texture2D *g_lit_tex[6];
static ID3D11RenderTargetView *g_lit_rtv[6]; /* the scene, its glows, the light map, its heights, the heights' block maps (2) */
static ID3D11ShaderResourceView *g_lit_srv[6];
static int g_lit_w, g_lit_h;
static volatile LONG g_time; /* 0 the mission's own look, 1 day, 2 dusk, 3 night, 4 dawn (as FreeSynd's) */
static ID3D11SamplerState *g_linear;
static ID3D11BlendState *g_blend;
static ID3D11BlendState *g_blend_lit; /* the city into the picture and its glows */
static ID3D11RasterizerState *g_scissor;
static ID3D11Buffer *g_cb, *g_city_cb, *g_city_vb;
static ID3D11Texture2D *g_tex, *g_atlas, *g_natlas;
static ID3D11ShaderResourceView *g_srv, *g_atlas_srv, *g_natlas_srv; /* the images, their normals */
/* the ground materials (gpu_city_material): two arrays of NB_MATERIALS squares, their colours and their normals,
 * with their mipmaps; sampled repeating (g_wrap) */
#define NB_MATERIALS 12
static ID3D11Texture2D *g_mat[2];
static ID3D11ShaderResourceView *g_mat_srv[2];
static ID3D11SamplerState *g_wrap;
static int g_mat_size;                            /* their size (0: none yet) */
static uint32_t *g_mat_new[NB_MATERIALS][2];      /* to upload */
static int g_tex_w, g_tex_h, g_city_vb_cap;
static int g_win_w, g_win_h;
/* offscreen (gpu_init(NULL): the headless tests): the picture is drawn into a texture and read back */
static ID3D11Texture2D *g_off, *g_off_read;
/* ---- the widescreen menus' sides (host_menu_sides): at the window's resolution, under the frame ---- */
static const char k_side_shader[] =
    "cbuffer S : register(b0) { uint4 rows[2]; float4 colour; float4 green; };\n"
    "Texture2D ct : register(t0);\n"
    "Texture2D gt : register(t1);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "    V o;\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.uv = uv;\n"
    "    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    /* each of the 200 rows in colour or green, as the game's picture beside it */
    "float4 ps(V i) : SV_Target {\n"
    "    uint row = min((uint)(i.uv.y * 200), 199u), k = (row >> 5) & 3;\n"
    "    uint4 r = rows[row >> 7];\n"
    "    uint w = k == 0 ? r.x : k == 1 ? r.y : k == 2 ? r.z : r.w;\n"
    "    if ((w >> (row & 31)) & 1) return float4(gt.Sample(s, i.uv).rgb * green.rgb, 1);\n"
    "    return float4(ct.Sample(s, i.uv).rgb * colour.rgb, 1);\n"
    "}\n";

typedef struct SideConsts {
    uint32_t rows[8];
    float colour[4], green[4];
} SideConsts;

static ID3D11VertexShader *g_side_vs;
static ID3D11PixelShader *g_side_ps;
static ID3D11Buffer *g_side_cb;
static ID3D11Texture2D *g_side_tex[4];
static ID3D11ShaderResourceView *g_side_srv[4];
static uint32_t *g_side_px[4]; /* to upload (NULL once uploaded) */
static int g_side_w, g_side_h;
static HostMenuSides g_sides; /* under g_city_lock */

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
    "cbuffer C : register(b0) { float2 frame_size; float scale; float bump; float4 sun; float4 shadow; float4 view; float4 target; };\n"
    "Texture2D<float4> atlas : register(t0);\n"
    "Texture2D<float4> normals : register(t1);\n"
    "Texture2DArray<float4> mat : register(t2);\n"
    "Texture2DArray<float4> matn : register(t3);\n"
    "SamplerState wrap : register(s0);\n"
    "struct VI { float2 pos : POSITION; float2 tex : TEXCOORD0; float4 rect : TEXCOORD1; uint hat : TEXCOORD2; uint glow : TEXCOORD3; uint3 look : TEXCOORD4; uint shade : TEXCOORD5; float z : TEXCOORD6; };\n"
    "struct V { float4 pos : SV_Position; float2 tex : TEXCOORD0; nointerpolation float4 rect : TEXCOORD1; nointerpolation uint hat : TEXCOORD2; nointerpolation uint glow : TEXCOORD3; nointerpolation uint3 look : TEXCOORD4; nointerpolation uint shade : TEXCOORD5; nointerpolation float z : TEXCOORD6; };\n"
    "V vs(VI i) {\n"
    "    V o;\n"
    "    float2 t = (i.pos * view.xy + view.zw) / target.xy;\n"
    "    o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);\n"
    "    o.tex = i.tex; o.rect = i.rect; o.hat = i.hat; o.glow = i.glow; o.look = i.look; o.shade = i.shade; o.z = i.z;\n"
    "    return o;\n"
    "}\n"
    "float texels(V i) { return (float)(1u << ((i.glow >> 4) & 3)); }\n" /* the image's texels a frame pixel */
    /* a tile's texel p on the ground (the tile's top a 32 x 32 square of the map, its place in the quad's hat), in
     * materials' squares (8 x 8 tiles); the material of a texel (alpha 240 + its number), -1 none */
    "float2 worldf(float2 t, V i) {\n" /* at a point of the image (texels; texel p's centre p + 0.5) */
    "    float2 l = (t - i.rect.xy) / texels(i);\n"
    "    float u = l.y + (l.x - 32) * 0.5, v = l.y - (l.x - 32) * 0.5;\n"
    "    return float2(u + 32.0 * (i.hat & 1023u), v + 32.0 * ((i.hat >> 10) & 1023u)) / 256.0;\n"
    "}\n"
    "float2 world(int2 p, V i) { return worldf((float2)p + 0.5, i); }\n"
    "int material(float a, V i) {\n"
    "    int k = (int)round(a * 255) - 240;\n"
    "    return (i.hat & 0x100000u) && k >= 0 && k < 12 ? k : -1;\n"
    "}\n"
    "float matlod() { return log2(max(1.0, 4.0 / scale)); }\n" /* 4 texels a game pixel */
    /* the dirt (material DIRT) over big fields: sampled at two sizes blended by a soft noise (its square's repeat no
     * longer lines up), patches of gravel (material GRAVEL) and damp, darker and lighter stretches; all in world space
     * (`w` in materials' squares, 8 tiles each) */
    "#define DIRT 0\n"
    "#define GRAVEL 8\n"
    "#define PAVE 7\n"
    "#define GRASS 6\n"
    "#define DIRT_GRASS 9\n"
    "float hash2(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "float vnoise(float2 p) {\n"
    "    float2 i = floor(p), f = frac(p);\n"
    "    f = f * f * (3 - 2 * f);\n"
    "    return lerp(lerp(hash2(i), hash2(i + float2(1, 0)), f.x), lerp(hash2(i + float2(0, 1)), hash2(i + 1), f.x), f.y);\n"
    "}\n"
    "float fbm(float2 p) { return vnoise(p) * 0.55 + vnoise(p * 2.1 + 5.3) * 0.3 + vnoise(p * 4.3 + 9.1) * 0.15; }\n"
    "float4 ground(float2 w, int m, Texture2DArray<float4> t) {\n"
    "    float4 a, b;\n"
    "    float mix, patch;\n"
    "    if (m != DIRT) return t.SampleLevel(wrap, float3(w, m), matlod());\n"
    "    a = t.SampleLevel(wrap, float3(w, DIRT), matlod());\n"
    "    b = t.SampleLevel(wrap, float3(w * 0.71 + float2(0.37, 0.61), DIRT), matlod());\n"
    "    mix = smoothstep(0.3, 0.7, fbm(w * 1.7 + 11.0));\n"
    "    a = lerp(a, b, mix);\n"
    "    patch = smoothstep(0.52, 0.66, fbm(w * 2.3 + 3.0));\n"
    "    return lerp(a, t.SampleLevel(wrap, float3(w * 1.13, GRAVEL), matlod()), patch);\n"
    "}\n"
    "float damp(float2 w, int m) { return m == DIRT ? 0.82 + 0.3 * fbm(w * 1.1 + 21.0) : 1; }\n"
    /* the dirt fields' variety: dark damp earth, pale dry dust, a warmer or cooler tint over large stretches */
    "float3 dirt_variety(float2 w, float3 c) {\n"
    "    float wet = smoothstep(0.55, 0.75, fbm(w * 1.4 + 91.0));\n"
    "    float dry = smoothstep(0.6, 0.8, fbm(w * 1.1 + 57.0)) * (1 - wet);\n"
    "    float tint = fbm(w * 0.6 + 13.0) - 0.5;\n"
    "    c *= float3(1 + 0.18 * tint, 1 + 0.05 * tint, 1 - 0.15 * tint);\n" /* browner or more olive */
    "    c = lerp(c, c * float3(0.55, 0.52, 0.5), wet * 0.85);\n" /* damp, dark */
    "    c = lerp(c, c * 1.25 + float3(0.03, 0.025, 0.015), dry * 0.7);\n" /* dry, dusty */
    "    return c;\n"
    "}\n"
    /* the pavement's wear: large grimy and worn stretches, and stains (oil, water) a slab or two across */
    /* grass tufts, scattered on the ground in world space (no pattern): roots in a grid of jittered cells (game
     * pixels), each tuft a few blades rising up the picture from its root, leaning, dark at the root, straw at the
     * tip; the root lowest on the picture (nearest) in front */
    /* the dirt's grass patches (DIRT_GRASS), 0..1: the noise turned and warped (its lattice does not show), a few */
    "float patch(float2 w) {\n"
    "    float2 r = float2(w.x * 0.866 - w.y * 0.5, w.x * 0.5 + w.y * 0.866) * 8.0;\n"
    "    r += float2(fbm(w * 3.1 + 1.7), fbm(w * 3.1 + 8.3)) * 1.6;\n"
    "    return smoothstep(0.56, 0.68, fbm(r + 5.0));\n"
    "}\n"
    /* dens: the share of cells with a tuft (0.8 on grass; in the dirt's patches less, none outside them) */
    "float3 tufts(float2 w, float3 c, float dens) {\n"
    "    float2 g = w * 256, cell = floor(g / 2.6);\n"
    "    float best = -1e9;\n"
    "    float3 col = c;\n"
    "    [loop] for (int du = -1; du <= 4; du++)\n"
    "    [loop] for (int dv = -1; dv <= 4; dv++) {\n"
    "        float2 id = cell + float2(du, dv);\n"
    "        float h = hash2(id + 3.7);\n"
    "        float2 root = (id + 0.15 + 0.7 * float2(hash2(id + 1.3), hash2(id + 7.9))) * 2.6;\n"
    "        if (h > dens) continue;\n"
    "        float2 d = root - g;\n"
    "        float up = (d.x + d.y) * 0.5;\n" /* how far the root is below this pixel on the picture */
    "        float dx = (g.x - g.y) - (root.x - root.y);\n" /* across the picture */
    "        float depth = root.x + root.y;\n"
    "        if (up <= 0 || depth < best) continue;\n"
    "        [unroll] for (int k = 0; k < 4; k++) {\n"
    "            float r1 = hash2(id + k * 11.1), r2 = hash2(id + k * 5.3 + 2.1);\n"
    "            float len = 3.2 + 4.4 * r1, lean = (r2 - 0.5) * 1.6, off = (r1 - 0.5) * 1.6;\n"
    "            float wid = 0.65 * (1 - up / len) + 0.08;\n"
    "            if (up < len && abs(dx - off - lean * up) < wid) {\n"
    "                float t = up / len;\n" /* dark at the root, bright straw or green at the tip: they stand out of the grass */
    "                col = lerp(float3(0.05, 0.06, 0.02), lerp(float3(0.52, 0.55, 0.2), float3(0.78, 0.7, 0.38), r2), pow(t, 0.8))\n"
    "                      * (0.8 + 0.4 * r1);\n"
    "                best = depth;\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    return col;\n"
    "}\n"
    "float3 pave_wear(float2 w, float3 c) {\n"
    "    float g = fbm(w * 0.9 + 41.0), worn = fbm(w * 1.7 + 63.0);\n"
    "    c *= 0.88 + 0.22 * smoothstep(0.25, 0.75, g);\n" /* grimy (darker) to clean */
    "    c = lerp(c, c * float3(1.08, 1.05, 1.0) + 0.02, smoothstep(0.62, 0.8, worn) * 0.4);\n" /* worn, paler */
    "    float s = fbm(w * 6.0 + 7.0);\n"
    "    float stain = smoothstep(0.7, 0.78, s) * smoothstep(0.55, 0.7, fbm(w * 2.2 + 17.0));\n"
    "    c = lerp(c, c * float3(0.5, 0.48, 0.47), stain * 0.6);\n" /* oil and water stains */
    "    return c;\n"
    "}\n"
    /* rubble on the dirt: in a grid of `cell` (materials' squares), a stone in some cells (`chance`), its size,
     * shape, turn and tint its own; shaded as a rounded rock lit from the top left, a dark edge where it sits in the
     * dirt; in world space (no seams, no repeats) */
    "float3 stones(float2 w, float3 c, float cell, float chance, float rmin, float rmax, float seed) {\n"
    "    float2 g = w / cell, id = floor(g), f = g - id;\n"
    "    [unroll] for (int dy = -1; dy <= 1; dy++)\n"
    "    [unroll] for (int dx = -1; dx <= 1; dx++) {\n"
    "        float2 cid = id + float2(dx, dy);\n"
    "        float h = hash2(cid + seed);\n"
    "        if (h > chance) continue;\n"
    "        float2 ctr = float2(dx, dy) + 0.2 + 0.6 * float2(hash2(cid + seed + 1.7), hash2(cid + seed + 2.9));\n"
    "        float r = lerp(rmin, rmax, hash2(cid + seed + 3.3)), an = hash2(cid + seed + 4.1) * 6.2832;\n"
    "        float2 d = f - ctr;\n"
    "        d = float2(d.x * cos(an) + d.y * sin(an), d.y * cos(an) - d.x * sin(an)) * float2(1, 1.45);\n"
    "        float q = length(d) / (r * (1 + 0.18 * sin(atan2(d.y, d.x) * 3 + h * 40)));\n"
    "        if (q < 1) {\n"
    "            float3 n = normalize(float3(d / r * 0.9, sqrt(saturate(1 - q * q))));\n"
    "            float lit = 0.45 + 0.75 * saturate(dot(n, normalize(float3(-0.55, -0.45, 0.7))));\n"
    "            float t = hash2(cid + seed + 5.3);\n"
    "            float3 tint = lerp(float3(0.25, 0.235, 0.20), float3(0.29, 0.23, 0.15), t) * (0.7 + 0.45 * hash2(cid + seed + 6.1));\n"
    "            c = tint * lit;\n"
    "        } else if (q < 1.25) c *= lerp(0.55, 1, (q - 1) / 0.25);\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "float3 rubble(float2 w, float3 c) {\n"
    "    c = stones(w, c, 1.0 / 14, 0.07, 0.22, 0.36, 11.0);\n" /* a few rocks, 6 to 10 game pixels */
    "    c = stones(w, c, 1.0 / 40, 0.16, 0.22, 0.40, 37.0);\n" /* stones, 2 to 4 */
    "    return stones(w, c, 1.0 / 90, 0.20, 0.25, 0.45, 71.0);\n" /* pebbles */
    "}\n"
    /* the water (material WATER): black, its ripples (two layers of the material's normals drifting apart, `sun.w`
     * the time in seconds) reflecting the sky (Fresnel: more at a grazing angle) and the sun's glints.  In world
     * space: u, v along the ground, up; the eye is down the picture and 30 degrees up */
    "#define WATER 11\n"
    "float3 water(float2 w) {\n"
    "    float t = sun.w;\n"
    "    float3 a = matn.SampleLevel(wrap, float3(w + float2(0.006, 0.0035) * t, WATER), matlod()).rgb * 2 - 1;\n"
    "    float3 b = matn.SampleLevel(wrap, float3(w * 1.37 + float2(-0.0045, 0.006) * t, WATER), matlod()).rgb * 2 - 1;\n"
    "    float3 n = normalize(float3((a.xy + b.xy) * 0.28, 1));\n"
    "    float3 e = float3(0.6124, 0.6124, 0.5);\n"
    "    float3 r = reflect(-e, n);\n"
    "    float3 l = normalize(sun.x * float3(0.7071, -0.7071, 0) + sun.y * float3(0.3536, 0.3536, -0.866) + sun.z * e);\n"
    "    float fr = 0.02 + 0.98 * pow(1 - saturate(dot(n, e)), 5);\n"
    "    float3 sky = lerp(float3(0.75, 0.82, 0.95), float3(0.12, 0.15, 0.2), saturate(r.z * 1.6));\n"
    "    float spec = pow(saturate(dot(r, l)), 200) * 12 * bump;\n"
    "    return saturate(float3(0.003, 0.004, 0.005) + fr * sky + fr * spec * float3(1, 0.95, 0.85));\n"
    "}\n"
    /* fire (alpha 232, its heat in red): flames rising and flickering (`sun.w` the time), drawn in the sprite's
     * outline, white-yellow where hottest, orange, red at the edges and the tips */
    /* value noise repeating every 256 (so the flames' scroll wraps round without a jump: the time grows large) */
    "float fhash(float2 p) { p = fmod(p, 256); return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "float fnoise(float2 p) {\n"
    "    float2 i = floor(p), f = p - i;\n"
    "    f = f * f * (3 - 2 * f);\n"
    "    return lerp(lerp(fhash(i), fhash(i + float2(1, 0)), f.x), lerp(fhash(i + float2(0, 1)), fhash(i + 1), f.x), f.y);\n"
    "}\n"
    "float4 fire(int2 p, float heat, V i) {\n"
    "    float2 r = ((float2)(p - (int2)i.rect.xy) + 0.5) / max(i.rect.zw - i.rect.xy, 1);\n"
    "    float t = fmod(sun.w, 400);\n"
    "    float sway = sin(t * 1.7 + r.y * 5) * 0.5;\n"
    "    float n = fnoise(float2(r.x * 9 + sway, r.y * 2.6 + fmod(t * 3.2, 256))) * 0.55\n"
    "            + fnoise(float2(r.x * 20 + 13, r.y * 5.7 + fmod(t * 7.0, 256))) * 0.3\n"
    "            + fnoise(float2(r.x * 42 + 5, r.y * 12 + fmod(t * 11.0, 256))) * 0.15;\n"
    "    float h = heat * 1.05 + (n - 0.5) * 1.0 - (1 - r.y) * 0.45;\n"
    "    if (h < 0.10) return 0;\n"
    "    float3 c = h < 0.35 ? lerp(float3(0.55, 0.08, 0.02), float3(0.95, 0.32, 0.05), (h - 0.16) / 0.19)\n"
    "             : h < 0.65 ? lerp(float3(0.95, 0.32, 0.05), float3(1.0, 0.7, 0.2), (h - 0.35) / 0.3)\n"
    "             : lerp(float3(1.0, 0.7, 0.2), float3(1.0, 0.97, 0.85), saturate((h - 0.65) / 0.35));\n"
    "    return float4(c, saturate((h - 0.10) / 0.12));\n"
    "}\n"
    /* smoke (alpha 233, its density in red): dark, soft, rising and thinning, see-through */
    "float4 smoke(int2 p, float dens, V i) {\n"
    "    float2 r = ((float2)(p - (int2)i.rect.xy) + 0.5) / max(i.rect.zw - i.rect.xy, 1);\n"
    "    float t = fmod(sun.w, 400);\n"
    "    float n = fnoise(float2(r.x * 5 + sin(t * 0.6 + r.y * 3) * 0.6, r.y * 2.2 + fmod(t * 0.9, 256))) * 0.6\n"
    "            + fnoise(float2(r.x * 11 + 3, r.y * 4.8 + fmod(t * 1.6, 256))) * 0.4;\n"
    "    float a = saturate(dens * (0.5 + n) - 0.08) * (0.45 + 0.4 * r.y);\n"
    "    if (a < 0.03) return 0;\n"
    "    return float4(lerp(float3(0.07, 0.065, 0.06), float3(0.26, 0.25, 0.24), n), a);\n"
    "}\n"
    "bool on_fire(V i) { return abs(atlas.Load(int3((int2)floor(i.tex), 0)).a * 255 - 232) < 0.5; }\n"
    /* a material's colour at `w` (world(): its place in materials' squares) */
    "float3 material_colour(int m, float2 w) {\n"
    "    if (m == WATER) return water(w);\n"
    "    if (m == GRASS) return tufts(w, ground(w, m, mat).rgb, 0.8);\n"
    "    if (m == DIRT_GRASS) {\n" /* the dirt (as DIRT), patches of grass and tufts in it */
    "        float g = patch(w);\n"
    "        float3 d = rubble(w, dirt_variety(w, ground(w, DIRT, mat).rgb * damp(w, DIRT)));\n"
    "        if (g > 0) d = lerp(d, ground(w, GRASS, mat).rgb * 0.9, g * 0.85);\n"
    "        return g > 0.05 ? tufts(w, d, 0.8 * g) : d;\n"
    "    }\n"
    "    if (m == PAVE) return pave_wear(w, ground(w, m, mat).rgb);\n"
    "    if (m == DIRT) return rubble(w, dirt_variety(w, ground(w, m, mat).rgb * damp(w, m)));\n"
    "    return ground(w, m, mat).rgb * damp(w, m);\n"
    "}\n"
    /* a texel's colour; a material's texel: alpha 2, its material in `mm` (the pixel's colour of it is worked out
     * once, at the pixel's own place: texels4()) */
    "float4 texel(int2 p, V i, inout int mm) {\n"
    "    float4 t;\n"
    "    int m;\n"
    "    if (p.x < (int)i.rect.x || p.y < (int)i.rect.y || p.x >= (int)i.rect.z || p.y >= (int)i.rect.w) return 0;\n"
    "    t = atlas.Load(int3(p, 0));\n"
    "    if (t.a < 0.5) return 0;\n"
    "    if (abs(t.a * 255 - 232) < 0.5) return fire(p, t.r, i);\n"
    "    if (abs(t.a * 255 - 233) < 0.5) return smoke(p, t.r, i);\n"
    "    m = material(t.a, i);\n"
    "    if (m >= 0) { mm = m; return float4(0, 0, 0, 2); }\n"
    "    {\n" /* a pedestrian's part (alpha 160 + 16 x part + shade: 0 hair, 1 skin, 2 clothes, 3 trousers): its own colour at the pixel's shade (0.3 + 0.1 x shade of it), else the pixel as drawn */
    "        uint code = (uint)round(t.a * 255), part = (code - 160) / 16, c = part == 0 ? i.look.x : part == 1 ? i.look.y : i.look.z;\n"
    "        if (code >= 160 && code < 224) {\n"
    "            float3 col = float3((c >> 16) & 255, (c >> 8) & 255, c & 255) / 255.0;\n"
    "            if (c == 0) return float4(t.rgb, 1);\n"
    "            if (part == 3) col = (col * 2 + dot(col, float3(0.299, 0.587, 0.114)) * 3) / 5 * 0.6;\n" /* trousers */
    /* (the pixels lighter than the part's main colour add light too: a black coat keeps its highlights) */
    "            float k = 0.3 + 0.1 * (code & 15);\n"
    "            if (c & 0x1000000u) k = k > 0.8 ? 0.8 + (k - 0.8) * 0.35 : k;\n" /* (bit 24: its highlights subtle) */
    /* (that light in the colour's own hue mostly: a dark red's highlights are red, not pink) */
    "            float3 tint = 0.3 + 0.7 * col / max(max(col.r, col.g), max(col.b, 0.02));\n"
    "            return float4(saturate(col * k + max(0, k - 0.8) * 0.45 * tint), 1);\n"
    "        }\n"
    "    }\n"
    "    if (t.a < 0.998 && t.a > 252.5 / 255 && p.y - (int)i.rect.y < (int)(i.hat >> 24)) {\n" /* alpha 254: the hat's colour, 253 its shine */
    "        float3 h = float3((i.hat >> 16) & 255, (i.hat >> 8) & 255, i.hat & 255) / 255.0;\n"
    "        return float4(t.a > 0.994 ? h : saturate(h * 1.5 + 20.0 / 255), 1);\n"
    "    }\n"
    "    return float4(t.rgb, 1);\n"
    "}\n"
    /* the four texels round the pixel (q its top left one): a material's colour once, at the pixel's place */
    "void texels4(int2 q, V i, out float4 c00, out float4 c10, out float4 c01, out float4 c11) {\n"
    "    int mm = -1;\n"
    "    c00 = texel(q, i, mm); c10 = texel(q + int2(1, 0), i, mm);\n"
    "    c01 = texel(q + int2(0, 1), i, mm); c11 = texel(q + int2(1, 1), i, mm);\n"
    "    if (mm >= 0) {\n"
    "        float4 mc = float4(material_colour(mm, worldf(i.tex, i)), 1);\n"
    "        if (c00.a > 1.5) c00 = mc;\n"
    "        if (c10.a > 1.5) c10 = mc;\n"
    "        if (c01.a > 1.5) c01 = mc;\n"
    "        if (c11.a > 1.5) c11 = mc;\n"
    "    }\n"
    "}\n"
    /* how much of a texel keeps its brightness in the dark (FreeSynd's): all of it (glow 2), its bright pixels
     * by their brightness (1), a road's white lights (3), nothing (0); with 64 its lamps too (bright, strongly
     * coloured: HOST_GLOW_SIGNAL) */
    "float4 glowing(float4 c, uint glow) {\n"
    "    float l = dot(c.rgb, float3(0.299, 0.587, 0.114)), s = 0;\n"
    "    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));\n"
    "    if ((glow & 64) && c.a > 0 && mx >= 0.75 * c.a && mn <= mx * 0.45) return float4(1, 1, 1, 1) * c.a;\n"
    "    glow &= 3;\n"
    "    if (glow == 2) s = 1;\n"
    "    else if (glow == 1) s = saturate((l - 0.62) / 0.18);\n"
    "    else if (glow == 3 && l >= 0.8 && max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)) < 60.0 / 255) s = 0.6;\n"
    /* a road's yellow markings (bright, yellow: red and green well over blue) shine too */
    "    else if (glow == 3 && c.r > 0.55 * c.a && c.g > 0.4 * c.a && c.r - c.b > 0.35 * c.a && c.g - c.b > 0.25 * c.a) s = 0.85;\n"
    "    return float4(s, s, s, 1) * c.a;\n"
    "}\n"
    /* the relief (the image's normals, x right, y down, z out of the picture) in the sun's light of the time of day
     * (`sun`, `bump` how much it shows): brighter facing it, darker away; a flat normal changes nothing */
    "float relief(V i) {\n"
    "    int2 q = (int2)floor(i.tex);\n"
    "    float3 n = normals.Load(int3(q, 0)).rgb * 2 - 1;\n"
    "    int m = material(atlas.Load(int3(q, 0)).a, i);\n"
    "    if (m == WATER) return 1;\n" /* lit by its own reflections */
    "    if (m >= 0) {\n" /* the ground's u goes right and down the picture, its v left and down, its up out of it */
    "        float3 g = ground(world(q, i), m == DIRT_GRASS ? DIRT : m, matn).rgb * 2 - 1;\n"
    "        n = float3((g.x - g.y) * 0.7071, (g.x + g.y) * 0.3536, g.z);\n"
    "    }\n"
    "    float3 l = normalize(sun.xyz);\n"
    "    return lerp(1, saturate(dot(normalize(n), l)) / l.z, bump);\n"
    "}\n"
    /* the sun's shadows (HostCityQuad.shade; shadow.x how dark, 0 none): a tile's place in it (game pixels) */
    "float2 at(V i) { return (i.tex - i.rect.xy) / texels(i); }\n"
    /* a solid tile's face away from the sun (world +x: below the lower right edge of its top) darker */
    "float face(V i) {\n"
    "    float2 r = at(i);\n"
    "    float a = ((i.shade >> 8) & 255) / 255.0;\n"
    "    if (!(i.shade & 1)) return 1;\n"
    "    if (r.x >= 32 && r.y > 32 - (r.x - 32) / 2 + 3) return 1 - shadow.x * 0.6 * (1 - a);\n"
    "    if (r.x < 32 && r.y > 16 + r.x / 2 + 3) return 1 - shadow.x * 0.6 * a;\n"
    "    return 1;\n"
    "}\n"
    /* in the shadow (FreeSynd's: a whole cell is or is not; a tile's top's edges softened against its neighbours
     * in the other state): 0..1 */
    "float shaded(V i) {\n"
    "    float2 r = at(i);\n"
    "    float own = 0, v = own;\n"
    "    if (!(i.shade & 128) || abs(r.x - 32) / 32 + abs(r.y - 16) / 16 > 1) return own;\n"
    "    float2 q = r - float2(32, 0);\n"
    "    float2 w = saturate(float2(q.x / 32 + q.y / 16, q.y / 16 - q.x / 32) * 0.5);\n"
    "    float4 dist = float4(w.x, 1 - w.x, w.y, 1 - w.y);\n"
    "    [unroll] for (int e = 0; e < 4; e++) {\n"
    "        float n = (i.shade >> (e + 2)) & 1;\n"
    "        float edge = lerp(0.5, own, smoothstep(0, 0.2, dist[e]));\n"
    "        if (n != own) v = own > 0 ? min(v, edge) : max(v, edge);\n"
    "    }\n"
    "    return v;\n"
    "}\n"
    /* the light it keeps (FreeSynd's: 45% taken in a shadow, 20% on a slope down from the sun) */
    "float sunlit(V i) {\n"
    "    float l = 0.42 * shaded(i);\n"
    "    if ((i.shade & 128) && (i.shade & 64)) l = max(l, 0.18);\n"
    "    return 1 - shadow.x * l;\n"
    "}\n"
    /* the four texels around a point filtered: their colours by their weights among the opaque ones only (a
     * transparent texel brings no colour in), the coverage that of the nearest (`r` the point's place between
     * them): an image's edge is not half see-through, so tiles side by side meet without a seam */
    "float4 filtered(float4 a, float4 b, float4 c, float4 d, float2 f, float2 r) {\n"
    "    float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y) * float4(a.a, b.a, c.a, d.a);\n"
    "    float s = w.x + w.y + w.z + w.w;\n"
    "    float n = r.y < 0.5 ? (r.x < 0.5 ? a.a : b.a) : (r.x < 0.5 ? c.a : d.a);\n"
    "    if (s <= 0 || n <= 0) return 0;\n"
    "    return float4((a.rgb * w.x + b.rgb * w.y + c.rgb * w.z + d.rgb * w.w) / s, 1) * n;\n"
    "}\n"
    "float4 ps(V i) : SV_Target {\n"
    "    float2 p = i.tex - 0.5;\n"
    "    float2 b = floor(p);\n"
    "    float2 f = saturate((p - b - 0.5) * scale / texels(i) + 0.5);\n"
    "    int2 q = (int2)b;\n"
    "    float4 c00, c10, c01, c11;\n"
    "    texels4(q, i, c00, c10, c01, c11);\n"
    "    float4 c = filtered(c00, c10, c01, c11, f, p - b);\n"
    "    if (!on_fire(i)) c.rgb *= lerp(relief(i) * face(i) * sunlit(i), 1, glowing(c, i.glow).r / max(c.a, 1e-4));\n" /* light is not shaded */
    "    if (i.glow & 128) c *= 0.24 * pow(0.4, (float)((i.shade >> 20) & 15));\n" /* drawn see-through (B held) */
    "    return c;\n" /* premultiplied: the transparent texels are 0 */
    "}\n"
    /* lit: the picture and what glows in it (covering what glowed under it) */
    "float height(V i) {\n"
    "    float2 r = at(i);\n"
    "    float below = r.y - (32 - abs(r.x - 32) / 2);\n"
    "    if ((i.shade & 128) && (i.shade & 2)) return below <= 2.5 ? i.z : i.z - min(below, 16) * 8;\n"
    "    if (i.shade & 128) return i.z - (r.y - 16) * 8;\n"
    "    return i.z - r.y * 8;\n"
    "}\n"
    "struct O { float4 c : SV_Target0; float4 e : SV_Target1; float4 h : SV_Target2; };\n"
    "O ps_lit(V i) {\n"
    "    O o;\n"
    "    float2 p = i.tex - 0.5;\n"
    "    float2 b = floor(p);\n"
    "    float2 f = saturate((p - b - 0.5) * scale / texels(i) + 0.5);\n"
    "    int2 q = (int2)b;\n"
    "    float4 c00, c10, c01, c11;\n"
    "    texels4(q, i, c00, c10, c01, c11);\n"
    "    o.c = filtered(c00, c10, c01, c11, f, p - b);\n"
    "    if (!on_fire(i)) o.c.rgb *= lerp(relief(i) * face(i) * sunlit(i), 1, glowing(o.c, i.glow).r / max(o.c.a, 1e-4));\n" /* light is not shaded */
    "    o.e = lerp(lerp(glowing(c00, i.glow), glowing(c10, i.glow), f.x), lerp(glowing(c01, i.glow), glowing(c11, i.glow), f.x), f.y);\n"
    "    if (on_fire(i)) o.e = float4(1, 1, 1, 1) * o.c.a;\n"
    "    o.h = float4(height(i), (i.shade & 128) && (i.shade & 2) && at(i).y <= 32 - abs(at(i).x - 32) / 2 + 2.5 ? 1 : (i.shade & 128) ? 0 : 0.5, (i.shade & 128) && (i.shade & 2) ? 300 : -14, (i.glow & 128) || (i.shade & 0x60000) || on_fire(i) ? 0 : o.c.a);\n"
    "    if (i.glow & 128) { float k = 0.24 * pow(0.4, (float)((i.shade >> 20) & 15)); o.c *= k; o.e *= k; }\n"
    "    if (o.c.a <= 0 && !any(o.e)) discard;\n" /* nothing drawn (the blending would keep all three as they are) */
    "    return o;\n"
    "}\n";

static const char k_light_shader[] =
    "cbuffer C : register(b0) { float2 frame_size; float strength; float add; float2 origin; float2 margin; float4 sun; float4 ambient; float4 view; float4 target; float4 hmax; };\n"
    "struct LI { float2 pos : POSITION; float2 uv : TEXCOORD0; float3 col : COLOR0; };\n"
    "struct LV { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 col : COLOR0; };\n"
    "LV vs_light(LI i) {\n"
    "    LV o;\n"
    "    float2 t = (i.pos * view.xy + view.zw) / target.xy;\n"
    "    o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);\n"
    "    o.uv = i.uv; o.col = i.col;\n"
    "    return o;\n"
    "}\n"
    "float4 ps_light(LV i) : SV_Target {\n"
    "    float f = saturate(1 - dot(i.uv, i.uv));\n"
    "    return float4(i.col * (f * f * strength), 0);\n"
    "}\n"
    "float fh(float2 p) { p = fmod(p, 256); return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "float fn(float2 p) {\n"
    "    float2 i = floor(p), f = p - i;\n"
    "    f = f * f * (3 - 2 * f);\n"
    "    return lerp(lerp(fh(i), fh(i + float2(1, 0)), f.x), lerp(fh(i + float2(0, 1)), fh(i + 1), f.x), f.y);\n"
    "}\n"
    /* GPU effects: no sprites. A quad from x - rx to x + rx, its bottom at the base, ry tall; uv.x -1..1 across, uv.y -1 */
    /* at its top, 1 at its bottom; col.r its seed, col.g how strong (0..1: fading in or out), col.b an explosion's progress */
    /* (0..1). sun.x the time (seconds). */
    "float fbm(float2 p) {\n"
    "    float s = 0, a = 0.5;\n"
    "    [unroll] for (int k = 0; k < 5; k++) { s += a * fn(p); p = p * 2.03 + float2(17.3, 9.1); a *= 0.5; }\n"
    "    return s;\n"
    "}\n"
    "float fbm3(float2 p) {\n"
    "    float s = 0, a = 0.5;\n"
    "    [unroll] for (int k = 0; k < 3; k++) { s += a * fn(p); p = p * 2.07 + float2(5.9, 13.7); a *= 0.5; }\n"
    "    return s / 0.875;\n"
    "}\n"
    /* the flames' colour from their heat (0..1): deep red, orange, yellow, white at the hottest */
    "float3 flame(float heat) {\n"
    "    return heat < 0.35 ? lerp(float3(0.35, 0.03, 0.01), float3(0.95, 0.25, 0.03), heat / 0.35)\n"
    "         : heat < 0.7 ? lerp(float3(0.95, 0.25, 0.03), float3(1.0, 0.66, 0.16), (heat - 0.35) / 0.35)\n"
    "         : lerp(float3(1.0, 0.66, 0.16), float3(1.0, 0.97, 0.86), saturate((heat - 0.7) / 0.3));\n"
    "}\n"
    /* embers rising from a fire or flung from an explosion: small bright points, `n` of them */
    "float embers(float2 p, float t, float seed, float rate, float spread) {\n"
    "    float e = 0;\n"
    "    [unroll] for (int k = 0; k < 7; k++) {\n"
    "        float a = fh(float2(seed, k * 3.1)), b = fh(float2(k * 7.7, seed + 1.3)), c = fh(float2(seed + k, 9.9));\n"
    "        float life = frac(t * rate * (0.6 + 0.8 * c) + a);\n"
    "        float2 q = float2((a - 0.5) * spread + sin(t * 2.3 + k * 1.7) * 0.12 * life, life * 1.1);\n"
    "        q.x += (b - 0.5) * 0.6 * life;\n"
    "        float d = length((p - q) * float2(1, 0.7));\n"
    "        e += smoothstep(0.035, 0.0, d) * (1 - life) * step(0.25, b);\n"
    "    }\n"
    "    return e;\n"
    "}\n"
    /* fire: tongues of flame licking up (noise warped by noise, rising), narrowing to their tips, flickering, more of */
    /* them side by side the wider it is (col.b: its width over its height), each its own height; embers */
    "float4 ps_fire(LV i) : SV_Target {\n"
    "    float t = fmod(sun.x, 200), seed = i.col.r * 255, strong = i.col.g;\n"
    "    float u = i.uv.x, h = 1 - (i.uv.y + 1) * 0.5;\n"
    "    float2 p = float2(u * 1.3 + seed * 0.37, h * 1.5);\n"
    "    float2 w = float2(fbm3(p * 1.7 + float2(seed, -t * 1.3)), fbm3(p * 1.7 + float2(seed + 4.7, -t * 1.6)));\n"
    "    float n = fbm(p * 2.6 + (w - 0.5) * 1.8 + float2(0, -t * 2.4));\n"
    "    float aspect = max(i.col.b * 4, 0.25), tongues = 1 + aspect * 2.2;\n"
    "    float sway = (fn(float2(h * 1.3 - t * 0.9, seed)) - 0.5) * 0.45 * h / tongues;\n"
    "    float wid = lerp(0.95, 0.3 + 0.12 * min(aspect, 3), pow(saturate(h), 0.8));\n"
    "    float env = saturate(1 - abs(u - sway) / wid);\n"
    "    float tall = 0.45 + 0.75 * fn(float2(u * tongues * 1.6 + seed, t * 0.8));\n"
    "    float f = env * (n * 1.9 - h / tall * 1.25 + 0.15) * (0.75 + 0.25 * strong);\n"
    "    f *= smoothstep(0.0, 0.12, h) * strong;\n"
    "    float heat = saturate(f * 1.6 + (1 - h) * 0.2 - 0.1);\n"
    "    float a = saturate(f * 2.2);\n"
    "    float3 c = flame(heat) * a;\n"
    "    float e = embers(float2(u, h), t, seed, 0.55, 1.2) * strong;\n"
    "    c += float3(1.0, 0.55, 0.15) * e;\n"
    "    a = saturate(a + e * 0.8);\n"
    "    if (a < 0.01) return 0;\n"
    "    return float4(c, a);\n"
    "}\n"
    /* smoke: a column billowing up, widening and thinning as it rises, leaning with the wind, curling (warped noise); */
    /* lit a little from above, darkest where it is thick */
    "float4 ps_smoke(LV i) : SV_Target {\n"
    "    float t = fmod(sun.x, 200), seed = i.col.r * 255, strong = i.col.g;\n"
    "    float u = i.uv.x, h = 1 - (i.uv.y + 1) * 0.5;\n"
    "    float lean = h * h * 0.35 + (fn(float2(t * 0.15, seed)) - 0.5) * 0.25 * h;\n"
    "    float wid = lerp(0.3, 1.0, sqrt(saturate(h)));\n"
    "    float x = (u - lean) / wid;\n"
    "    float2 p = float2(x * 1.2 + seed * 0.21, h * 1.9 - t * 0.32);\n"
    "    float2 w = float2(fbm3(p * 1.3 + float2(seed, t * 0.11)), fbm3(p * 1.3 + float2(seed + 3.3, -t * 0.09)));\n"
    "    float n = fbm(p * 2.1 + (w - 0.5) * 2.2);\n"
    "    float env = saturate(1 - x * x);\n"
    "    float dens = smoothstep(0.26, 0.62, n * (0.6 + 0.8 * env)) * env;\n"
    "    dens *= smoothstep(0.0, 0.18, h) * pow(saturate(1 - h), 1.3) * strong;\n"
    "    float a = dens * 0.92;\n"
    "    if (a < 0.01) return 0;\n"
    "    float lit = saturate(0.35 + (n - 0.5) * 1.2 + h * 0.25);\n"
    "    float3 c = lerp(float3(0.05, 0.048, 0.045), float3(0.34, 0.33, 0.31), lit);\n"
    "    return float4(c * a, a);\n"
    "}\n"
    /* an explosion (col.b its progress over its life): a white flash; a fireball of billowing puffs swelling, rising and */
    /* boiling, cooling from white to orange and red, turning to black smoke from its edges in; the smoke rising on as a */
    /* column with a spreading head; a ring of dust and heat along the ground; embers flung out.  The quad 2 wide and 2 tall */
    /* (in its radius' units), its bottom on the ground */
    "float4 ps_explode(LV i) : SV_Target {\n"
    "    float t = fmod(sun.x, 200), seed = i.col.r * 255, pr = i.col.b;\n"
    "    float2 v = float2(i.uv.x, 1 - i.uv.y);\n"
    "    float grow = sqrt(saturate(pr / 0.25));\n"
    "    float rise = pr * pr * 0.9 + pr * 0.35;\n"
    /* the puffs: one in the middle, five round it, each its own size, rising and spreading as it grows */
    "    float dist = 9.0;\n"
    "    [unroll] for (int k = 0; k < 6; k++) {\n"
    "        float a = k == 0 ? 0 : (k - 1) * 1.2566 + fh(float2(seed, k)) * 0.8;\n"
    "        float off = k == 0 ? 0 : (0.22 + 0.2 * fh(float2(k, seed))) * grow;\n"
    "        float2 c = float2(cos(a) * off * 1.3, 0.32 + 0.45 * grow + sin(a) * off * 0.8 + rise * (0.8 + 0.5 * fh(float2(k * 3, seed))));\n"
    "        float r = (k == 0 ? 0.42 : 0.3 + 0.12 * fh(float2(seed + k, 2))) * (0.25 + 0.75 * grow) * (1 + 0.35 * pr);\n"
    "        dist = min(dist, length((v - c) * float2(1, 1.05)) / r);\n"
    "    }\n"
    /* the column under the head, once it is smoke */
    "    float col_w = 0.07 + 0.08 * pr;\n"
    "    float colum = abs(v.x + (fn(float2(v.y * 2, seed)) - 0.5) * 0.1) / col_w + saturate(0.25 - v.y) * 4;\n"
    "    dist = min(dist, lerp(9, colum + saturate(0.5 - v.y) * 1.5, saturate((pr - 0.35) / 0.3)) * (v.y < 0.4 + rise ? 1 : 9));\n"
    "    float2 w = float2(fbm3(v * 2.4 + float2(seed, t * 0.5 - pr * 2)), fbm3(v * 2.4 + float2(seed + 6.1, -t * 0.4)));\n"
    "    float n = fbm(v * 3.4 + (w - 0.5) * 2.4 + float2(0, -pr * 2.5 - t * 0.3));\n"
    "    float n2 = fbm3(v * 9.0 + (w - 0.5) * 3.0 + float2(seed, -t * 1.5));\n"
    "    dist += (n - 0.5) * 1.6 + (n2 - 0.5) * 0.5;\n"
    "    float body = saturate((1 - dist) * 2.5);\n"
    /* the fire in it: white hot at first and in its core, cooling to orange and red */
    "    float cool = saturate((pr - 0.08) / 0.45);\n"
    "    float core = saturate((1 - dist) * 1.7 + (n - 0.5) * 1.3 + (n2 - 0.5) * 0.6);\n"
    "    float heat = core * (1 - cool * 0.95);\n"
    "    float smoke = saturate((pr - 0.16) / 0.4) * body;\n"
    "    float fire = body * saturate(heat * 1.5) * (1 - smoke * 0.9);\n"
    "    float3 col = flame(saturate(heat * 1.15)) * fire * (0.6 + 0.55 * n2);\n"
    "    float a = fire;\n"
    /* the smoke: black where thick, lit brown-grey at its top, the fire's glow under it at first, thinning at the end */
    "    float sd = smoke * saturate(1.25 - pr) * (0.7 + 0.5 * n);\n"
    "    float lit = saturate(0.25 + (n - 0.5) * 1.4 + (v.y - rise) * 0.3);\n"
    "    float3 sc = lerp(float3(0.025, 0.022, 0.02), float3(0.3, 0.27, 0.24), lit);\n"
    "    sc += float3(0.5, 0.18, 0.04) * saturate(1 - pr * 2.2) * saturate(1 - lit * 1.5);\n"
    "    col = col + sc * sd * (1 - a);\n"
    "    a = a + sd * (1 - a);\n"
    /* the first flash */
    "    float fl = saturate(1 - pr / 0.07) * smoothstep(0.5, 0.05, length(v - float2(0, 0.55)));\n"
    "    col += float3(1.0, 0.96, 0.88) * fl;\n"
    "    a = saturate(a + fl);\n"
    /* the ring along the ground */
    "    float rr = 0.2 + 0.72 * sqrt(saturate(pr / 0.3));\n"
    "    float g = length(float2(v.x, (v.y - 0.1) * 3.2));\n"
    "    float ring = exp(-pow((g - rr) / (0.04 + 0.08 * pr), 2)) * saturate(1 - pr / 0.3) * smoothstep(1.0, 0.85, abs(v.x)) * (0.4 + 0.6 * fn(float2(atan2(v.y - 0.1, v.x) * 5, seed)));\n"
    "    col += float3(0.95, 0.62, 0.3) * ring * 0.8 * (1 - a);\n"
    "    a = saturate(a + ring * 0.5);\n"
    /* embers flung out and falling */
    "    float e = 0;\n"
    "    [unroll] for (int m = 0; m < 10; m++) {\n"
    "        float ang = 0.15 + fh(float2(seed, m * 1.9)) * 2.84, sp = 1.0 + 1.9 * fh(float2(m * 4.3, seed));\n"
    "        float tt = pr * 1.2;\n"
    "        float2 q = float2(cos(ang) * sp * tt, 0.35 + sin(ang) * sp * tt - 2.4 * tt * tt);\n"
    "        e += smoothstep(0.04, 0.0, length(v - q)) * saturate(1 - pr / 0.65) * step(0.03, q.y);\n"
    "    }\n"
    "    col += float3(1.0, 0.62, 0.22) * e * (1 - a * 0.5);\n"
    "    a = saturate(a + e * 0.8);\n"
    "    if (a < 0.01) return 0;\n"
    "    return float4(col, a);\n"
    "}\n"
    /* a muzzle flash (col.r the way it points, a turn; col.g a seed): wide where it leaves the gun, narrowing to a point */
    /* away from it, flickering in length and width; shades of bright yellow, palest inside.  Added to the picture */
    "float4 ps_flash(LV i) : SV_Target {\n"
    "    float a = i.col.r * 6.2831853, seed = i.col.g * 255;\n"
    "    float2 dir = float2(cos(a), sin(a));\n"
    "    float2 q = float2(dot(i.uv, dir), dot(i.uv, float2(-dir.y, dir.x)));\n"
    "    float tick = floor(sun.x * 30);\n"
    "    float len = 0.7 + 0.3 * fh(float2(tick, seed));\n"
    "    float t = (q.x + 0.9) / (1.8 * len);\n"
    "    if (t <= 0 || t >= 1) return 0;\n"
    "    float wid = (0.34 + 0.1 * fh(float2(seed, tick))) * saturate(t / 0.08) * pow(abs(1 - t), 1.3);\n"
    "    float core = saturate(1 - abs(q.y) / max(wid, 0.001));\n"
    "    if (core <= 0) return 0;\n"
    "    float3 c = lerp(float3(1.0, 0.8, 0.12), float3(1.0, 0.98, 0.7), saturate(core * 1.5 - 0.25));\n"
    "    return float4(c * pow(core, 0.6) * 1.5, 0);\n"
    "}\n"
    "Texture2D scene : register(t0);\n"
    "Texture2D emis : register(t1);\n"
    "Texture2D light : register(t2);\n"
    "Texture2D heights : register(t3);\n"
    /* the highest a ray can be and still be shaded by something, in each 16 x 16 block of the heights (hmax_1), then
     * over the blocks round it (hmax_2, hmax.x blocks each way: a stretch of the ray and the rays' spread): where a
     * ray is above it, nothing there shades it (shade() skips that stretch) */
    "Texture2D tops : register(t4);\n"
    "float4 ps_hmax1(float4 pos : SV_Position) : SV_Target {\n"
    "    int2 b = (int2)pos.xy * 16;\n"
    "    float m = -30000;\n"
    "    [loop] for (int y = 0; y < 16; y++)\n"
    "        [loop] for (int x = 0; x < 16; x++) {\n"
    "            float4 h = heights.Load(int3(b + int2(x, y), 0));\n" /* outside: 0, not shading */
    "            if (h.a > 0.5) m = max(m, h.r + max(0, -h.b));\n"
    "        }\n"
    "    return m;\n"
    "}\n"
    "float4 ps_hmax2(float4 pos : SV_Position) : SV_Target {\n"
    "    int2 c = (int2)pos.xy, r = (int)hmax.x, n;\n"
    "    float m = -30000;\n"
    "    tops.GetDimensions(n.x, n.y);\n"
    "    [loop] for (int y = -r.y; y <= r.y; y++)\n"
    "        [loop] for (int x = -r.x; x <= r.x; x++) {\n"
    "            int2 q = c + int2(x, y);\n"
    "            if (all(q >= 0) && all(q < n)) m = max(m, tops.Load(int3(q, 0)).r);\n"
    "        }\n"
    "    return m;\n"
    "}\n"
    "#define STEPS 110\n"
    "float hit_at(int2 p, float2 size, float4 h0, bool blocks, float2 dir, int k) {\n"
    "    int2 q = (int2)floor(p + dir * k + 0.5);\n"
    "    if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) return 0;\n" /* past the edge: (straight) it stays out */
    "    float4 hq = heights.Load(int3(q, 0));\n"
    "    if (hq.a <= 0.5) return 0;\n"
    "    float over = hq.r - (h0.r + sun.z * k);\n"
    /* a floor or block: what stands in front of the ray and above it, up to its depth; an upright thing (a card
     * through its middle: b < 0, its half thickness): the ray passing within it, either side */
    "    if (blocks && hq.b <= 0) return 0;\n"
    "    float hit = hq.b > 0 ? (over > 3 && over < hq.b ? saturate((over - 3) / (5 + 0.12 * k)) : 0)\n"
    "                         : saturate((-hq.b - abs(over)) / (4 + 0.1 * k));\n"
    "    return hit * (1 - 0.55 * k / STEPS);\n"
    "}\n"
    /* four rays side by side, a stretch (hmax.y steps) at a time: a stretch where the ray is above all that is near
     * it (tops) skipped; done when all four are as shaded as anything further can make them (hits further weigh less) */
    "float shade(int2 p, float2 size) {\n"
    "    float4 h0 = heights.Load(int3(p, 0));\n"
    "    if (sun.w <= 0 || h0.a < 0.5 || h0.g < 0.25) return 0;\n"
    "    float2 side = float2(-sun.y, sun.x) * 0.022;\n"
    "    float2 d0 = sun.xy - side * 1.5, d1 = sun.xy - side * 0.5, d2 = sun.xy + side * 0.5, d3 = sun.xy + side * 1.5;\n"
    "    bool blocks = h0.g < 0.75;\n"
    "    int seg = (int)hmax.y;\n"
    "    int2 n;\n"
    "    tops.GetDimensions(n.x, n.y);\n"
    "    float4 occ = 0;\n"
    "    [loop] for (int k0 = 1; k0 <= STEPS; k0 += seg) {\n"
    "        float2 a = p + sun.xy * k0;\n"
    "        float spread = length(side) * 1.5 * k0 + 2;\n"
    "        if (a.x < -spread || a.y < -spread || a.x >= size.x + spread || a.y >= size.y + spread) break;\n"
    "        if (all(occ >= 1 - 0.55 * k0 / STEPS)) break;\n"
    "        if (h0.r + sun.z * k0 >= tops.Load(int3(clamp((int2)floor(a / 16), 0, n - 1), 0)).r) continue;\n"
    "        int k1 = min(k0 + seg, STEPS + 1);\n"
    "        [loop] for (int k = k0; k < k1; k++) {\n"
    "            occ = max(occ, float4(hit_at(p, size, h0, blocks, d0, k), hit_at(p, size, h0, blocks, d1, k),\n"
    "                                  hit_at(p, size, h0, blocks, d2, k), hit_at(p, size, h0, blocks, d3, k)));\n"
    "        }\n"
    "    }\n"
    "    return dot(occ, 0.25) * sun.w;\n"
    "}\n"
    "float4 ps_comp(float4 pos : SV_Position) : SV_Target {\n"
    "    int3 p = int3(pos.xy - origin + margin, 0);\n"
    "    float2 size;\n"
    "    float4 c = scene.Load(p);\n"
    "    heights.GetDimensions(size.x, size.y);\n"
    /* the shadow takes the sun's part (the ambient) only: a light (a muzzle flash, a lamp) lights a shadow too */
    "    float3 l = saturate(light.Load(p).rgb - ambient.rgb * min(shade(p.xy, size), 0.5));\n"
    "    return float4(saturate(c.rgb * clamp(l + emis.Load(p).r, 0, 1.5) + c.rgb * add), c.a);\n" /* strong lights: hotspots */
    "}\n";

typedef struct Consts {
    float tex_size[2], scale[2], sharp, pad[3];
} Consts;

typedef struct CityConsts {
    float frame_size[2], scale, bump, sun[4];
    float shadow[4]; /* x: how dark the sun's shadows are (0: none) */
    float view[4], target[4]; /* the frame in its target: window pixels a frame pixel, offset; the target's size */
} CityConsts;

/* the HD tiles' relief at each time of day (k_times): where the sun is (x right, y down, z out of the picture) and
 * how much the relief shows: from the top left by day, low from the left at dusk and from the right at dawn
 * (longer, stronger relief), faint from above at night */
static const struct {
    float sun[3], bump;
} k_relief[5] = {
    {{-0.45f, -0.55f, 0.7f}, 0.6f},
    {{-0.45f, -0.55f, 0.7f}, 0.6f},
    {{-0.85f, -0.25f, 0.45f}, 0.75f},
    {{0.0f, -0.3f, 0.95f}, 0.3f},
    {{0.85f, -0.25f, 0.45f}, 0.7f},
};

typedef struct CityVertex {
    float x, y, u, v;
    float rect[4];
    uint32_t hat, glow;
    uint32_t look[3];
    uint32_t shade;
    float z;
} CityVertex;

typedef struct LightVertex {
    float x, y, u, v;
    float col[3];
} LightVertex;

#define SHADOW_MARGIN 160.0f /* frame pixels drawn round the view for the shadows (render.c's too) */
#define SHADOW_DARK 0.6f /* how much of the light a shadow takes (the walls away from the sun too; at most half) */
#define SUN_TAN 1.4f /* the sun's height: it rises 1.4 for each unit across (54 degrees up) */
typedef struct LightConsts {
    float frame_size[2], strength, add, origin[2], margin[2]; /* margin: the view's corner in the lit targets */
    float sun[4];     /* the sun's shadows: a step towards the sun (window pixels), the ray's rise a step, how dark */
    float ambient[4]; /* the ambient light (the sun's part, which a shadow takes) */
    float view[4], target[4]; /* as CityConsts' */
    float hmax[4];    /* the shadows' block maps: blocks each way they cover, a ray's stretch (steps) */
} LightConsts;

/* the times of day (FreeSynd's grades): the ambient light the light map starts with, a part of the city added
 * again (brighter), how strong the lights are (0.8 times the scene's brightness, 0.3 at least) */
#define NB_TIMES 5
static const struct {
    float ambient[3], add, strength;
} k_times[NB_TIMES] = {
    {{1.0f, 1.0f, 1.0f}, 0.0f, 0.8f},          /* the mission's own look: the lights add nothing to it */
    {{1.0f, 0.988f, 0.941f}, 0.25f, 0.96f},    /* day */
    {{0.922f, 0.627f, 0.451f}, 0.0f, 0.56f},   /* dusk */
    {{0.34f, 0.38f, 0.60f}, 0.0f, 0.4f},       /* night: moonlit (the city still reads) */
    {{0.98f, 0.765f, 0.784f}, 0.0f, 0.68f},    /* dawn */
};

/* ---- the city's images and list, set on the game thread, drawn on the UI thread ---- */
#define ATLAS 8192 /* the city's images (HD tiles, 4x sprites: a big city's need more than 4096 x 4096) */
#define NB_IMAGES (256 + 4096 + 64) /* the tiles, the sprites, the dirt ground's tiles (render.c) */

typedef struct Image {
    int w, h, ax, ay; /* size, place in the atlas (w 0: not defined) */
    int scale;        /* its pixels to a frame pixel each way: 1, 2 or 4 */
    uint32_t *pixels; /* to upload (NULL once uploaded) */
    uint32_t *normals; /* its normals to upload (NULL: none, or uploaded) */
} Image;

static SRWLOCK g_city_lock = SRWLOCK_INIT;
static Image g_images[NB_IMAGES];
static int g_shelf_x, g_shelf_y, g_shelf_h; /* the atlas packer */
static int g_atlas_used; /* its rows used before a reset (cleared then) */
static int g_atlas_ready; /* its rows made ready (transparent, flat normals): the textures' first rows */
static int g_atlas_reset;                    /* the atlas must be cleared */
static HostCityQuad *g_city_quads;
static int g_city_n, g_city_cap, g_city_on;
static HostCityLight *g_city_lights;
static int g_city_nl, g_city_lcap;
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
        free(g_images[i].normals);
        memset(&g_images[i], 0, sizeof g_images[i]);
    }
    if (g_shelf_y + g_shelf_h > g_atlas_used) g_atlas_used = g_shelf_y + g_shelf_h;
    g_shelf_x = g_shelf_y = g_shelf_h = 0;
    g_atlas_reset = 1;
    ReleaseSRWLockExclusive(&g_city_lock);
}

void gpu_city_image(int id, int w, int h, int scale, const uint32_t *pixels)
{
    Image *im;
    if (id < 0 || id >= NB_IMAGES || w <= 0 || h <= 0 || w > 1024 || h > 1024) return; /* (a train's carriage in HD: 256 x 332) */
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
            static int full;
            ReleaseSRWLockExclusive(&g_city_lock);
            if (++full <= 5 || full % 500 == 0) {
                char line[96];
                snprintf(line, sizeof line, "city atlas full: image %d (%d x %d) not drawn (%d)", id, w, h, full);
                host_log(line);
            }
            return; /* full: not drawn */
        }
        im->ax = g_shelf_x;
        im->ay = g_shelf_y;
        g_shelf_x += w + 1;
        if (h > g_shelf_h) g_shelf_h = h;
    }
    im->w = w;
    im->h = h;
    im->scale = scale == 4 ? 4 : scale == 2 ? 2 : 1;
    free(im->pixels);
    im->pixels = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(w * h));
    if (im->pixels != NULL) memcpy(im->pixels, pixels, sizeof(uint32_t) * (size_t)(w * h));
    ReleaseSRWLockExclusive(&g_city_lock);
}

void gpu_city_normals(int id, int w, int h, const uint32_t *pixels)
{
    Image *im;
    if (id < 0 || id >= NB_IMAGES) return;
    AcquireSRWLockExclusive(&g_city_lock);
    im = &g_images[id];
    if (im->w == w && im->h == h) {
        free(im->normals);
        im->normals = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(w * h));
        if (im->normals != NULL) memcpy(im->normals, pixels, sizeof(uint32_t) * (size_t)(w * h));
    }
    ReleaseSRWLockExclusive(&g_city_lock);
}

void gpu_city_material(int k, int w, int h, const uint32_t *colour, const uint32_t *normals)
{
    int j;
    if (k < 0 || k >= NB_MATERIALS || w != h || w <= 0 || w > 4096) return;
    AcquireSRWLockExclusive(&g_city_lock);
    if (g_mat_size == 0) g_mat_size = w;
    if (w == g_mat_size)
        for (j = 0; j < 2; j++) {
            const uint32_t *src = j ? normals : colour;
            free(g_mat_new[k][j]);
            g_mat_new[k][j] = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)w * (size_t)w);
            if (g_mat_new[k][j] != NULL) memcpy(g_mat_new[k][j], src, sizeof(uint32_t) * (size_t)w * (size_t)w);
        }
    ReleaseSRWLockExclusive(&g_city_lock);
}

/* the materials given since the last city drawn, to the GPU (on the drawing thread, under g_city_lock) */
static void upload_materials(void)
{
    int k, j, any = 0;
    UINT mips = 1;
    for (k = 0; k < NB_MATERIALS; k++) any |= g_mat_new[k][0] != NULL;
    if (!any) return;
    while ((1 << (mips - 1)) < g_mat_size) mips++;
    for (j = 0; j < 2; j++) {
        if (g_mat[j] == NULL) {
            D3D11_TEXTURE2D_DESC td;
            memset(&td, 0, sizeof td);
            td.Width = td.Height = (UINT)g_mat_size;
            td.MipLevels = mips;
            td.ArraySize = NB_MATERIALS;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_mat[j])) ||
                FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_mat[j], NULL, &g_mat_srv[j]))) {
                RELEASE(g_mat[j]);
                RELEASE(g_mat_srv[j]);
                continue;
            }
        }
        for (k = 0; k < NB_MATERIALS; k++)
            if (g_mat_new[k][j] != NULL)
                ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_mat[j],
                                                      (UINT)k * mips /* mip 0 of slice k */, NULL, g_mat_new[k][j],
                                                      (UINT)g_mat_size * 4, 0);
        ID3D11DeviceContext_GenerateMips(g_ctx, g_mat_srv[j]);
    }
    for (k = 0; k < NB_MATERIALS; k++)
        for (j = 0; j < 2; j++) {
            free(g_mat_new[k][j]);
            g_mat_new[k][j] = NULL;
        }
}

void gpu_set_time(int time)
{
    InterlockedExchange(&g_time, time < 0 || time >= NB_TIMES ? 0 : time);
}

void gpu_city_frame(const HostCityQuad *quads, int n, int x0, int y0, int x1, int y1,
                    float scale, const HostCityLight *lights, int nlights)
{
    AcquireSRWLockExclusive(&g_city_lock);
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
    g_city_rect[0] = x0;
    g_city_rect[1] = y0;
    g_city_rect[2] = x1;
    g_city_rect[3] = y1;
    g_city_scale = scale > 0.05f ? scale : 1.0f;
    g_city_on = 1;
    ReleaseSRWLockExclusive(&g_city_lock);
}

void gpu_menu_side_images(const uint32_t *const px[4], int w, int h)
{
    int k;
    AcquireSRWLockExclusive(&g_city_lock);
    for (k = 0; k < 4; k++) {
        free(g_side_px[k]);
        g_side_px[k] = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
        if (g_side_px[k] != NULL) memcpy(g_side_px[k], px[k], sizeof(uint32_t) * (size_t)w * (size_t)h);
    }
    g_side_w = w;
    g_side_h = h;
    ReleaseSRWLockExclusive(&g_city_lock);
}

void gpu_menu_sides(const HostMenuSides *s)
{
    AcquireSRWLockExclusive(&g_city_lock);
    g_sides = *s;
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

/* the compiled shaders kept in shaders\ beside the program, by a hash of their source, entry, target and how they
 * were compiled: a start loads them instead of compiling */
static void cache_path(wchar_t *path, size_t n, const char *src, size_t len, const char *entry, const char *target,
                       UINT flags)
{
    uint64_t h = 1469598103934665603ull;
    size_t k;
    wchar_t *s;
    for (k = 0; k < len; k++) h = (h ^ (uint8_t)src[k]) * 1099511628211ull;
    for (k = 0; entry[k]; k++) h = (h ^ (uint8_t)entry[k]) * 1099511628211ull;
    for (k = 0; target[k]; k++) h = (h ^ (uint8_t)target[k]) * 1099511628211ull;
    h = (h ^ flags) * 1099511628211ull;
    GetModuleFileNameW(NULL, path, (DWORD)n);
    s = wcsrchr(path, L'\\');
    if (s == NULL) {
        path[0] = 0;
        return;
    }
    _snwprintf(s + 1, n - (size_t)(s + 1 - path), L"shaders");
    CreateDirectoryW(path, NULL);
    _snwprintf(s + 1, n - (size_t)(s + 1 - path), L"shaders\\%016llx.cso", (unsigned long long)h);
}

static ID3DBlob *cache_load(const wchar_t *path)
{
    ID3DBlob *code = NULL;
    FILE *f;
    long size;
    if (path[0] == 0 || (f = _wfopen(path, L"rb")) == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || FAILED(D3DCreateBlob((SIZE_T)size, &code)) ||
        fread(ID3D10Blob_GetBufferPointer(code), 1, (size_t)size, f) != (size_t)size)
        RELEASE(code);
    fclose(f);
    return code;
}

static ID3DBlob *compile_now(const char *src, size_t n, const char *entry, const char *target, UINT flags)
{
    ID3DBlob *code = NULL, *err = NULL;
    wchar_t path[MAX_PATH * 2], part[MAX_PATH * 2 + 8];
    FILE *f;
    if (FAILED(D3DCompile(src, n, "synd", NULL, NULL, entry, target, flags, 0, &code, &err))) {
        if (err != NULL) host_log((const char *)ID3D10Blob_GetBufferPointer(err));
        RELEASE(err);
        return NULL;
    }
    RELEASE(err);
    /* written to a .part file first, then renamed: a start never loads a half written one */
    cache_path(path, sizeof path / sizeof path[0], src, n, entry, target, flags);
    _snwprintf(part, sizeof part / sizeof part[0], L"%s.part", path);
    if (path[0] != 0 && (f = _wfopen(part, L"wb")) != NULL) {
        size_t size = ID3D10Blob_GetBufferSize(code);
        int ok = fwrite(ID3D10Blob_GetBufferPointer(code), 1, size, f) == size;
        fclose(f);
        if (!ok || !MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(part);
    }
    return code;
}

/* the city's big shaders, optimised (a minute's work), compiled while the game runs: into the cache, for the starts
 * after this one */
typedef struct LaterJob {
    const char *src, *entry, *target;
    size_t n;
} LaterJob;

static DWORD WINAPI compile_later(LPVOID p)
{
    LaterJob *job = (LaterJob *)p;
    ID3DBlob *code;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    code = compile_now(job->src, job->n, job->entry, job->target, D3DCOMPILE_OPTIMIZATION_LEVEL3);
    RELEASE(code);
    free(job);
    return 0;
}

/* a shader, optimised (level 3); a big one (`slow`) not yet in the cache: compiled without the optimisation now (in
 * seconds; the driver optimises it again too), and optimised in the background for the next start */
static ID3DBlob *compile_shader(const char *src, size_t n, const char *entry, const char *target, int slow)
{
    wchar_t path[MAX_PATH * 2];
    ID3DBlob *code;
    LaterJob *job;
    HANDLE t;
    cache_path(path, sizeof path / sizeof path[0], src, n, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3);
    if ((code = cache_load(path)) != NULL) return code;
    if (!slow) return compile_now(src, n, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3);
    cache_path(path, sizeof path / sizeof path[0], src, n, entry, target, D3DCOMPILE_SKIP_OPTIMIZATION);
    if ((code = cache_load(path)) == NULL) code = compile_now(src, n, entry, target, D3DCOMPILE_SKIP_OPTIMIZATION);
    if (code != NULL && (job = (LaterJob *)malloc(sizeof *job)) != NULL) {
        job->src = src;
        job->n = n;
        job->entry = entry;
        job->target = target;
        t = CreateThread(NULL, 0, compile_later, job, 0, NULL);
        if (t != NULL) CloseHandle(t);
        else free(job);
    }
    return code;
}

static ID3DBlob *compile(const char *src, size_t n, const char *entry, const char *target)
{
    return compile_shader(src, n, entry, target, src == k_city_shader);
}

static int make_city(void)
{
    static const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32_UINT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_UINT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 5, DXGI_FORMAT_R32_UINT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 6, DXGI_FORMAT_R32_FLOAT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0},
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
    ID3DBlob *fpb = compile(k_light_shader, sizeof k_light_shader - 1, "ps_fire", "ps_4_0");
    ID3DBlob *spb = compile(k_light_shader, sizeof k_light_shader - 1, "ps_smoke", "ps_4_0");
    ID3DBlob *epb = compile(k_light_shader, sizeof k_light_shader - 1, "ps_explode", "ps_4_0");
    ID3DBlob *mpb = compile(k_light_shader, sizeof k_light_shader - 1, "ps_flash", "ps_4_0");
    ID3DBlob *h1b = compile(k_light_shader, sizeof k_light_shader - 1, "ps_hmax1", "ps_4_0");
    ID3DBlob *h2b = compile(k_light_shader, sizeof k_light_shader - 1, "ps_hmax2", "ps_4_0");
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
             SUCCEEDED(ID3D11Device_CreateInputLayout(g_dev, layout, sizeof layout / sizeof layout[0],ID3D10Blob_GetBufferPointer(vsb),
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
             fpb != NULL &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(fpb), ID3D10Blob_GetBufferSize(fpb),
                                                      NULL, &g_fire_ps)) &&
             spb != NULL &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(spb), ID3D10Blob_GetBufferSize(spb),
                                                      NULL, &g_smoke_ps)) &&
             epb != NULL &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(epb), ID3D10Blob_GetBufferSize(epb),
                                                      NULL, &g_explode_ps)) &&
             mpb != NULL &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(mpb), ID3D10Blob_GetBufferSize(mpb),
                                                      NULL, &g_flash_ps)) &&
             h1b != NULL && h2b != NULL &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(h1b), ID3D10Blob_GetBufferSize(h1b),
                                                      NULL, &g_hmax1_ps)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(h2b), ID3D10Blob_GetBufferSize(h2b),
                                                      NULL, &g_hmax2_ps)) &&
             SUCCEEDED(ID3D11Device_CreateInputLayout(g_dev, light_layout, 3, ID3D10Blob_GetBufferPointer(lvb),
                                                      ID3D10Blob_GetBufferSize(lvb), &g_light_layout));
    RELEASE(vsb);
    RELEASE(psb);
    RELEASE(litb);
    RELEASE(lvb);
    RELEASE(lpb);
    RELEASE(cpb);
    RELEASE(fpb);
    RELEASE(spb);
    RELEASE(epb);
    RELEASE(mpb);
    RELEASE(h1b);
    RELEASE(h2b);
    if (!ok) return 0;
    memset(&td, 0, sizeof td);
    g_atlas_ready = 0;
    td.Width = td.Height = ATLAS;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_atlas)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_atlas, NULL, &g_atlas_srv)) ||
        FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_natlas)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_natlas, NULL, &g_natlas_srv)))
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
    bl.IndependentBlendEnable = TRUE;
    bl.RenderTarget[1] = bl.RenderTarget[0];
    bl.RenderTarget[2] = bl.RenderTarget[0];
    bl.RenderTarget[2].SrcBlend = D3D11_BLEND_SRC_ALPHA; /* a height where the pixel is opaque, over what is under */
    bl.RenderTarget[2].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bl, &g_blend_lit))) return 0;
    bl.RenderTarget[2] = bl.RenderTarget[3];
    bl.IndependentBlendEnable = FALSE;
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
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &smp, &g_linear))) {
        gpu_shutdown();
        return 0;
    }
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; /* the materials repeat */
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &smp, &g_wrap)) ||
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
    int side;
    if (g_ctx != NULL) ID3D11DeviceContext_ClearState(g_ctx);
    for (side = 0; side < 4; side++) {
        RELEASE(g_side_srv[side]);
        RELEASE(g_side_tex[side]);
    }
    RELEASE(g_side_vs);
    RELEASE(g_side_ps);
    RELEASE(g_side_cb);
    RELEASE(g_srv);
    RELEASE(g_tex);
    RELEASE(g_atlas_srv);
    RELEASE(g_atlas);
    RELEASE(g_natlas_srv);
    RELEASE(g_natlas);
    RELEASE(g_mat_srv[0]);
    RELEASE(g_mat_srv[1]);
    RELEASE(g_mat[0]);
    RELEASE(g_mat[1]);
    RELEASE(g_wrap);
    RELEASE(g_city_vb);
    RELEASE(g_city_cb);
    RELEASE(g_light_vb);
    RELEASE(g_comp_cb);
    RELEASE(g_city_lit_ps);
    RELEASE(g_light_vs);
    RELEASE(g_light_ps);
    RELEASE(g_fire_ps);
    RELEASE(g_smoke_ps);
    RELEASE(g_explode_ps);
    RELEASE(g_flash_ps);
    RELEASE(g_hmax1_ps);
    RELEASE(g_hmax2_ps);
    RELEASE(g_comp_ps);
    RELEASE(g_light_layout);
    RELEASE(g_add);
    {
        int k;
        for (k = 0; k < 6; k++) {
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
    RELEASE(g_blend_lit);
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
    /* the scene, its glows, the light map, its heights; the heights' 16 x 16 blocks' tops, those spread (shade()) */
    static const DXGI_FORMAT formats[6] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                           DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                           DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT};
    D3D11_TEXTURE2D_DESC td;
    int k;
    if (g_lit_tex[0] != NULL && w == g_lit_w && h == g_lit_h) return 1;
    for (k = 0; k < 6; k++) {
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
    for (k = 0; k < 6; k++) {
        td.Format = formats[k];
        td.Width = (UINT)(k < 4 ? w : (w + 15) / 16);
        td.Height = (UINT)(k < 4 ? h : (h + 15) / 16);
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_lit_tex[k])) ||
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_lit_tex[k], NULL, &g_lit_rtv[k])) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_lit_tex[k], NULL, &g_lit_srv[k])))
            return 0;
    }
    g_lit_w = w;
    g_lit_h = h;
    return 1;
}

/* the glows (muzzle flashes): added to the colours of the city drawn, by day too */
/* the glows (added) or the fires (ps g_fire_ps: drawn over, premultiplied), over the picture */
static void draw_glows(RECT dst, int fw, int fh, D3D11_RECT sc, const LightVertex *v, int n, ID3D11PixelShader *ps)
{
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp = {(float)dst.left, (float)dst.top, (float)(dst.right - dst.left), (float)(dst.bottom - dst.top), 0, 1};
    LightConsts lc;
    UINT stride = sizeof(LightVertex), offset = 0;
    if (n > g_light_vb_cap) {
        D3D11_BUFFER_DESC bd;
        RELEASE(g_light_vb);
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (UINT)(sizeof(LightVertex) * (size_t)n * 2);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_light_vb_cap = SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_light_vb)) ? n * 2 : 0;
    }
    if (g_light_vb == NULL || n > g_light_vb_cap ||
        FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_light_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return;
    memcpy(map.pData, v, sizeof(LightVertex) * (size_t)n);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_light_vb, 0);
    memset(&lc, 0, sizeof lc);
    lc.frame_size[0] = (float)fw;
    lc.frame_size[1] = (float)fh;
    lc.strength = 1.0f;
    lc.sun[0] = (float)(GetTickCount64() % 1000000u) / 1000.0f; /* the fires' time */
    lc.view[0] = vp.Width / (float)fw; /* the frame on the back buffer's viewport */
    lc.view[1] = vp.Height / (float)fh;
    lc.target[0] = vp.Width;
    lc.target[1] = vp.Height;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_comp_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &lc, sizeof lc);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_comp_cb, 0);
    }
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_scissor);
    ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_light_layout);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_light_vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_light_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_comp_cb);
    ID3D11DeviceContext_PSSetShader(g_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_comp_cb);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, ps == g_fire_ps || ps == g_smoke_ps || ps == g_explode_ps ? g_blend : g_add, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_Draw(g_ctx, (UINT)n, 0);
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
    static LightVertex *gverts, *fverts, *sverts, *everts, *mverts;
    static int gverts_cap;
    int mission_time = 0;
    int i, n, nv = 0, nlv = 0, ngv = 0, nfv = 0, nsv = 0, nev = 0, nmv = 0, time = (int)g_time, lit_time, sun_on = 0;
    float sun_a = 0;
    float sx = (float)(dst.right - dst.left) / (float)fw, sy = (float)(dst.bottom - dst.top) / (float)fh, zoom;
    AcquireSRWLockExclusive(&g_city_lock);
    if (!g_city_on) {
        ReleaseSRWLockExclusive(&g_city_lock);
        return;
    }
    upload_materials();
    if (g_atlas_reset) {
        /* everything transparent */
        static uint32_t clear[ATLAS], flat[ATLAS]; /* 0: transparent; the normal out of the picture */
        D3D11_BOX box = {0, 0, 0, ATLAS, 1, 1};
        if (flat[0] == 0)
            for (i = 0; i < ATLAS; i++) flat[i] = 0xFF8080FFu;
        for (i = 0; i < g_atlas_used + 2 && i < g_atlas_ready; i++) { /* (the rows used: the rest was never drawn on) */
            box.top = (UINT)i;
            box.bottom = (UINT)i + 1;
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_atlas, 0, &box, clear, ATLAS * 4, 0);
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_natlas, 0, &box, flat, ATLAS * 4, 0);
        }
        g_atlas_reset = 0;
        g_atlas_used = 0;
    }
    {
        /* the rows the images now reach, made ready before they are used: transparent, their normals flat (an image
         * without normals of its own, a sprite, is lit as a flat one: not dark) */
        static uint32_t clear[ATLAS], flat[ATLAS];
        D3D11_BOX box = {0, 0, 0, ATLAS, 1, 1};
        int need = g_shelf_y + g_shelf_h + 2;
        if (need > ATLAS) need = ATLAS;
        if (flat[0] == 0)
            for (i = 0; i < ATLAS; i++) flat[i] = 0xFF8080FFu;
        for (i = g_atlas_ready; i < need; i++) {
            box.top = (UINT)i;
            box.bottom = (UINT)i + 1;
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_atlas, 0, &box, clear, ATLAS * 4, 0);
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_natlas, 0, &box, flat, ATLAS * 4, 0);
        }
        if (need > g_atlas_ready) g_atlas_ready = need;
    }
    for (i = 0; i < NB_IMAGES; i++) {
        Image *im = &g_images[i];
        if (im->pixels != NULL) {
            D3D11_BOX box = {(UINT)im->ax, (UINT)im->ay, 0, (UINT)(im->ax + im->w), (UINT)(im->ay + im->h), 1};
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_atlas, 0, &box, im->pixels,
                                                  (UINT)im->w * 4, 0);
            free(im->pixels);
            im->pixels = NULL;
        }
        if (im->normals != NULL && im->pixels == NULL) {
            D3D11_BOX box = {(UINT)im->ax, (UINT)im->ay, 0, (UINT)(im->ax + im->w), (UINT)(im->ay + im->h), 1};
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_natlas, 0, &box, im->normals,
                                                  (UINT)im->w * 4, 0);
            free(im->normals);
            im->normals = NULL;
        }
    }
    n = g_city_n;
    zoom = g_city_scale;
    /* the sun's shadows: on, and where the sun is (each quad carries it: the first's); the mission's time of day
     * (the first quad that has one) */
    for (i = 0; i < n; i++)
        if (g_city_quads[i].shade & HOST_SHADE_ON) {
            sun_on = 1;
            sun_a = (float)((g_city_quads[i].shade >> 8) & 255) / 255.0f;
            break;
        }
    for (i = 0; i < n && mission_time == 0; i++) mission_time = (int)((g_city_quads[i].shade >> 24) & 7);
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
        x1 = x0 + (float)im->w * zoom / (float)im->scale;
        y1 = y0 + (float)im->h * zoom / (float)im->scale;
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
            c[k].glow = q->glow | (im->scale == 4 ? 0x20u : im->scale == 2 ? 0x10u : 0u);
            c[k].look[0] = q->look[0];
            c[k].look[1] = q->look[1];
            c[k].look[2] = q->look[2];
            c[k].shade = q->shade;
            c[k].z = q->z;
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
    /* the mission's own look: the light map is white, the lights add nothing (only the glows show) */
    if (time == 0) time = mission_time > 0 && mission_time < NB_TIMES ? mission_time : -1; /* the mission's own: its palette's */
    lit_time = time >= 0 ? time : 0;
    /* the lights and the glows: a quad around each ellipse */
    if (g_city_nl * 6 > lverts_cap) {
        LightVertex *v = (LightVertex *)realloc(lverts, sizeof *v * (size_t)g_city_nl * 6);
        LightVertex *g = (LightVertex *)realloc(gverts, sizeof *g * (size_t)g_city_nl * 6);
        if (v != NULL) lverts = v;
        if (g != NULL) gverts = g;
        LightVertex *f = (LightVertex *)realloc(fverts, sizeof *f * (size_t)g_city_nl * 6);
        LightVertex *sm = (LightVertex *)realloc(sverts, sizeof *sm * (size_t)g_city_nl * 6);
        LightVertex *ex = (LightVertex *)realloc(everts, sizeof *ex * (size_t)g_city_nl * 6);
        if (f != NULL) fverts = f;
        if (sm != NULL) sverts = sm;
        LightVertex *mz = (LightVertex *)realloc(mverts, sizeof *mz * (size_t)g_city_nl * 6);
        if (ex != NULL) everts = ex;
        if (mz != NULL) mverts = mz;
        if (v != NULL && g != NULL && f != NULL && sm != NULL && ex != NULL && mz != NULL) lverts_cap = gverts_cap = g_city_nl * 6;
    }
    for (i = 0; i < g_city_nl && (i + 1) * 6 <= lverts_cap; i++) {
        const HostCityLight *l = &g_city_lights[i];
        LightVertex c[4], *out;
        int k, *count, glows = (l->colour & HOST_LIGHT_GLOW) != 0, fire = (l->colour & HOST_LIGHT_FIRE) != 0;
        int smoke = (l->colour & HOST_LIGHT_SMOKE) != 0, boom = (l->colour & HOST_LIGHT_EXPLODE) != 0;
        int muzzle = (l->colour & HOST_LIGHT_FLASH) != 0;
        out = muzzle ? mverts : boom ? everts : smoke ? sverts : fire ? fverts : glows ? gverts : lverts;
        count = muzzle ? &nmv : boom ? &nev : smoke ? &nsv : fire ? &nfv : glows ? &ngv : &nlv;
        fire |= smoke | boom;
        for (k = 0; k < 4; k++) {
            c[k].x = l->x + ((k & 1) ? l->rx : -l->rx);
            c[k].y = fire ? l->y - ((k & 2) ? 0 : l->ry) : l->y + ((k & 2) ? l->ry : -l->ry);
            c[k].u = (k & 1) ? 1.0f : -1.0f;
            c[k].v = (k & 2) ? 1.0f : -1.0f;
            c[k].col[0] = (float)((l->colour >> 16) & 0xFF) / 255.0f;
            c[k].col[1] = (float)((l->colour >> 8) & 0xFF) / 255.0f;
            c[k].col[2] = (float)(l->colour & 0xFF) / 255.0f;
        }
        out[(*count)++] = c[0];
        out[(*count)++] = c[1];
        out[(*count)++] = c[2];
        out[(*count)++] = c[2];
        out[(*count)++] = c[1];
        out[(*count)++] = c[3];
    }
    (void)gverts_cap;
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
    cc.bump = k_relief[lit_time].bump;
    cc.sun[0] = k_relief[lit_time].sun[0];
    cc.sun[1] = k_relief[lit_time].sun[1];
    cc.sun[2] = k_relief[lit_time].sun[2];
    memset(cc.shadow, 0, sizeof cc.shadow);
    cc.shadow[0] = sun_on ? k_times[lit_time].strength : 0; /* the faces away from the sun (the shadows on the
                                                              * ground: traced per pixel in the light map) */
    cc.sun[3] = (float)(GetTickCount64() % 1000000u) / 1000.0f; /* the water's time: its drift repeats each 1000 s */
    /* the frame on the back buffer's viewport (the lit pass sets its own) */
    cc.view[0] = sx;
    cc.view[1] = sy;
    cc.view[2] = cc.view[3] = 0;
    cc.target[0] = (float)(dst.right - dst.left);
    cc.target[1] = (float)(dst.bottom - dst.top);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_city_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &cc, sizeof cc);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_city_cb, 0);
    }
    {
        ID3D11ShaderResourceView *srv[4] = {g_atlas_srv, g_natlas_srv, g_mat_srv[0], g_mat_srv[1]};
        int mx = sun_on ? (int)(SHADOW_MARGIN * sx * zoom) : 0, my = mx;
        int lit = (time >= 0 || sun_on) && lit_targets(dst.right - dst.left + 2 * mx, dst.bottom - dst.top + my);
        rt_set_render_info(lit, time, sun_on, (g_shelf_y + g_shelf_h) * 100 / ATLAS); /* the debug readout */
        ID3D11DeviceContext_IASetInputLayout(g_ctx, g_city_layout);
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_city_vb, &stride, &offset);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_city_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_city_cb);
        ID3D11DeviceContext_PSSetShader(g_ctx, lit ? g_city_lit_ps : g_city_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_city_cb);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 4, srv);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_wrap);
        ID3D11DeviceContext_RSSetState(g_ctx, g_scissor);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, NULL, 0xFFFFFFFFu);
        if (!lit) {
            ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
            ID3D11DeviceContext_Draw(g_ctx, (UINT)nv, 0);
        } else {
            static const float clear[4] = {0, 0, 0, 0};
            ID3D11ShaderResourceView *none[5] = {NULL, NULL, NULL, NULL, NULL};
            static const float no_height[4] = {-30000, 0, 0, 0};
            D3D11_VIEWPORT vp = {0, 0, (float)(dst.right - dst.left + 2 * mx), (float)(dst.bottom - dst.top + my), 0, 1};
            D3D11_VIEWPORT back = {(float)dst.left, (float)dst.top, (float)(dst.right - dst.left), (float)(dst.bottom - dst.top), 0, 1};
            D3D11_RECT lsc = {sc.left - dst.left, sc.top - dst.top, sc.right - dst.left + 2 * mx, sc.bottom - dst.top + my};
            float ambient[4] = {k_times[lit_time].ambient[0], k_times[lit_time].ambient[1], k_times[lit_time].ambient[2], 1};
            ID3D11RenderTargetView *scene_rtv[3] = {g_lit_rtv[0], g_lit_rtv[1], g_lit_rtv[3]};
            LightConsts lc;
            UINT lstride = sizeof(LightVertex);
            if (sun_on) {
                /* the margin drawn only where the shadows' rays go: towards the sun (always up the picture), as far
                 * as they reach (shade(): 110 steps, a frame pixel each, and their spread) */
                float c = cosf(sun_a * 1.5708f), s = sinf(sun_a * 1.5708f), dx = (s - c) / 8.0f;
                float dy = (-(c + s) / 2.0f - SUN_TAN) / 8.0f, len = sqrtf(dx * dx + dy * dy), px = sx * zoom;
                int nx = (int)ceilf((110.0f * fabsf(dx) / len + 8.0f) * px), ny = (int)ceilf((110.0f * fabsf(dy) / len + 8.0f) * px);
                if (nx > mx) nx = mx;
                if (ny > my) ny = my;
                if (dx < 0) lsc.left += mx - nx, lsc.right -= mx;
                else lsc.left += mx, lsc.right -= mx - nx;
                lsc.top += my - ny;
            }
            /* the city and what glows in it, in their textures (with the margin: the frame's 0,0 at mx, my) */
            cc.view[2] = (float)mx;
            cc.view[3] = (float)my;
            cc.target[0] = vp.Width;
            cc.target[1] = vp.Height;
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_city_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                memcpy(map.pData, &cc, sizeof cc);
                ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_city_cb, 0);
            }
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 3, scene_rtv, NULL);
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_lit_rtv[3], no_height);
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_lit_rtv[0], clear);
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_lit_rtv[1], clear);
            ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend_lit, NULL, 0xFFFFFFFFu);
            ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
            ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &lsc);
            ID3D11DeviceContext_Draw(g_ctx, (UINT)nv, 0);
            ID3D11DeviceContext_RSSetState(g_ctx, NULL);
            /* the light map: the ambient light, the lights added */
            memset(&lc, 0, sizeof lc);
            lc.frame_size[0] = (float)fw;
            lc.frame_size[1] = (float)fh;
            lc.strength = k_times[lit_time].strength;
            lc.add = k_times[lit_time].add;
            lc.origin[0] = (float)dst.left;
            lc.origin[1] = (float)dst.top;
            lc.margin[0] = (float)mx;
            lc.margin[1] = (float)my;
            memcpy(lc.view, cc.view, sizeof lc.view);
            memcpy(lc.target, cc.target, sizeof lc.target);
            {
                /* the sun towards world (-cos a, -sin a), rising SUN_TAN for each unit across: on the screen a
                 * frame pixel is (x - y) / 8 across, ((x + y) / 2 - z) / 8 down; a step one frame pixel long */
                float c = cosf(sun_a * 1.5708f), s = sinf(sun_a * 1.5708f), dx = (s - c) / 8.0f;
                float dy = (-(c + s) / 2.0f - SUN_TAN) / 8.0f, len = sqrtf(dx * dx + dy * dy), px = sx * zoom;
                lc.sun[0] = dx / len * px;
                lc.sun[1] = dy / len * px;
                lc.sun[2] = SUN_TAN / len;
                lc.sun[3] = sun_on ? SHADOW_DARK * k_times[lit_time].strength : 0.0f; /* how dark (night: faint) */
                memcpy(lc.ambient, ambient, sizeof lc.ambient);
                /* shade()'s stretches: about 8 window pixels; the blocks round each that its stretch and the rays'
                 * spread (0.022 x 1.5 of a step aside a step, STEPS steps) can reach */
                {
                    float step = px > 0.01f ? px : 0.01f;
                    int seg = (int)(8.0f / step);
                    if (seg < 1) seg = 1;
                    lc.hmax[0] = ceilf(((float)(seg - 1) * step + 0.033f * 110.0f * step + 4.0f) / 16.0f);
                    lc.hmax[1] = (float)seg;
                }
            }
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_comp_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                memcpy(map.pData, &lc, sizeof lc);
                ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_comp_cb, 0);
            }
            if (lc.sun[3] > 0) { /* the heights' block maps (shade()) */
                ID3D11ShaderResourceView *srv1[4] = {NULL, NULL, NULL, g_lit_srv[3]}, *srv2 = g_lit_srv[4], *nosrv[5] = {NULL};
                D3D11_VIEWPORT bv = {0, 0, (float)((g_lit_w + 15) / 16), (float)((g_lit_h + 15) / 16), 0, 1};
                ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
                ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
                ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_comp_cb);
                ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
                ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &bv);
                ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_lit_rtv[4], NULL);
                ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 4, srv1);
                ID3D11DeviceContext_PSSetShader(g_ctx, g_hmax1_ps, NULL, 0);
                ID3D11DeviceContext_Draw(g_ctx, 3, 0);
                ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_lit_rtv[5], NULL);
                ID3D11DeviceContext_PSSetShaderResources(g_ctx, 4, 1, &srv2);
                ID3D11DeviceContext_PSSetShader(g_ctx, g_hmax2_ps, NULL, 0);
                ID3D11DeviceContext_Draw(g_ctx, 3, 0);
                ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 5, nosrv);
                ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
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
            {
                ID3D11ShaderResourceView *comp_srv[5] = {g_lit_srv[0], g_lit_srv[1], g_lit_srv[2], g_lit_srv[3], g_lit_srv[5]};
                ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 5, comp_srv);
            }
            ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, NULL, 0xFFFFFFFFu);
            ID3D11DeviceContext_Draw(g_ctx, 3, 0);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 5, none);
        }
        if (ngv > 0) draw_glows(dst, fw, fh, sc, gverts, ngv, g_light_ps);
        if (nsv > 0) draw_glows(dst, fw, fh, sc, sverts, nsv, g_smoke_ps); /* the smoke under the flames */
        if (nev > 0) draw_glows(dst, fw, fh, sc, everts, nev, g_explode_ps);
        if (nmv > 0) draw_glows(dst, fw, fh, sc, mverts, nmv, g_flash_ps); /* (added to the picture, as the glows) */
        if (nfv > 0) draw_glows(dst, fw, fh, sc, fverts, nfv, g_fire_ps);
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
    /* what of it is on the window (a mission's goes on past its top and bottom) */
    if (dst.left < 0) dst.left = 0;
    if (dst.top < 0) dst.top = 0;
    if (dst.right > (LONG)td.Width) dst.right = (LONG)td.Width;
    if (dst.bottom > (LONG)td.Height) dst.bottom = (LONG)td.Height;
    w = dst.right - dst.left;
    h = dst.bottom - dst.top;
    if (w <= 0 || h <= 0) {
        if (g_swap != NULL) RELEASE(src);
        return;
    }
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

/* the menu's sides, each in its columns of the frame (fw wide in `dst`), before the frame */
static void draw_menu_sides(RECT dst, int fw)
{
    HostMenuSides s;
    int k, side;
    AcquireSRWLockExclusive(&g_city_lock);
    s = g_sides;
    for (k = 0; k < 4; k++)
        if (g_side_px[k] != NULL) {
            D3D11_TEXTURE2D_DESC td;
            D3D11_SUBRESOURCE_DATA sd;
            RELEASE(g_side_srv[k]);
            RELEASE(g_side_tex[k]);
            memset(&td, 0, sizeof td);
            td.Width = (UINT)g_side_w;
            td.Height = (UINT)g_side_h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sd.pSysMem = g_side_px[k];
            sd.SysMemPitch = (UINT)g_side_w * 4u;
            sd.SysMemSlicePitch = 0;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &g_side_tex[k])) ||
                FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_side_tex[k], NULL,
                                                             &g_side_srv[k])))
                RELEASE(g_side_tex[k]);
            free(g_side_px[k]);
            g_side_px[k] = NULL;
        }
    ReleaseSRWLockExclusive(&g_city_lock);
    if (!s.on || fw <= 0 || g_side_srv[0] == NULL || g_side_srv[1] == NULL || g_side_srv[2] == NULL ||
        g_side_srv[3] == NULL)
        return;
    if (g_side_ps == NULL) {
        ID3DBlob *vsb = compile(k_side_shader, sizeof k_side_shader - 1, "vs", "vs_4_0");
        ID3DBlob *psb = compile(k_side_shader, sizeof k_side_shader - 1, "ps", "ps_4_0");
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(SideConsts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (vsb != NULL && psb != NULL &&
            SUCCEEDED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb),
                                                      ID3D10Blob_GetBufferSize(vsb), NULL, &g_side_vs)) &&
            SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_side_cb)))
            ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
                                           NULL, &g_side_ps);
        RELEASE(vsb);
        RELEASE(psb);
        if (g_side_ps == NULL) return;
    }
    for (side = 0; side < 2; side++) {
        D3D11_MAPPED_SUBRESOURCE map;
        D3D11_VIEWPORT vp;
        ID3D11ShaderResourceView *srv[2];
        SideConsts c;
        float sx = (float)(dst.right - dst.left) / (float)fw;
        memset(&c, 0, sizeof c);
        memcpy(c.rows, s.green_rows[side], sizeof s.green_rows[side]);
        for (k = 0; k < 3; k++) {
            c.colour[k] = s.bright[0][k];
            c.green[k] = s.bright[1][k];
        }
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_side_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
            return;
        memcpy(map.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_side_cb, 0);
        vp.TopLeftX = (float)dst.left + (float)s.x0[side] * sx;
        vp.TopLeftY = (float)dst.top;
        vp.Width = (float)(s.x1[side] - s.x0[side]) * sx;
        vp.Height = (float)(dst.bottom - dst.top);
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        srv[0] = g_side_srv[side];
        srv[1] = g_side_srv[2 + side];
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(g_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_side_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_side_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, srv);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_linear);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_side_cb);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    }
}

/* the picture shown: held for as many of the display's refreshes as the game's frames take (60 a second at most:
 * 2 at 120 Hz), so each frame is on screen as long as the one before however long the GPU took for it (a frame
 * that just missed its refresh is not shown a refresh late, then the next one a refresh early); with --log, every
 * 5 seconds how many refreshes apart the frames were shown (all k: even) */
static void present_paced(void)
{
    static UINT last_refresh, last_count;
    static int gaps[5], frames;
    static uint64_t next_report;
    uint64_t vblank, period;
    UINT k = 1;
    DXGI_FRAME_STATISTICS st;
    if (host_display_timing(&vblank, &period) && period >= 2000u && period <= 100000u) {
        k = (UINT)((16667u + period - 1000u) / period);
        if (k < 1) k = 1;
        if (k > 4) k = 4;
    }
    IDXGISwapChain1_Present(g_swap, k, 0);
    if (SUCCEEDED(IDXGISwapChain1_GetFrameStatistics(g_swap, &st))) {
        if (last_count != 0 && st.PresentCount == last_count + 1) {
            UINT gap = st.PresentRefreshCount - last_refresh;
            gaps[gap < 4 ? gap : 4]++;
            frames++;
        }
        if (st.PresentCount != last_count) {
            last_count = st.PresentCount;
            last_refresh = st.PresentRefreshCount;
        }
    }
    if (frames > 0 && host_time_us() >= next_report) {
        char line[128];
        snprintf(line, sizeof line, "pacing: %d frames, %u refreshes each; refreshes apart: 1: %d  2: %d  3: %d  4+: %d",
                 frames, k, gaps[1], gaps[2], gaps[3], gaps[4]);
        if (next_report != 0) host_log(line);
        if (next_report != 0) rt_set_pacing(k < 5 ? gaps[k] : 0, frames - (k < 5 ? gaps[k] : 0));
        memset(gaps, 0, sizeof gaps);
        frames = 0;
        next_report = host_time_us() + 5000000u;
    }
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
    /* the city first (when the frame is see-through over it); a wide menu's sides */
    if (sharp && pixels != NULL && w > 0 && h > 0) draw_city(dst, w, h);
    if (pixels != NULL && h == 200 && w > 320) {
        draw_menu_sides(dst, w);
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    }
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
        present_paced();
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
