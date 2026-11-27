/* render.c - the city of a mission drawn by this program from the game's own data (render.h).
 *
 * The game keeps its data decompressed in memory: the map (columns of tile numbers), the tiles (HBLK: 64 x 48 pixels
 * made of six 32 x 16 blocks of bit planes), the sprites (HSPR: rows of 8-pixel blocks, one transparency byte and
 * four bit planes each) and the animations (HFRA frames made of HELE elements, each a sprite at an offset).  Each
 * thing of the level data (people, cars, objects, weapons, effects) says where it is and which frame it shows.
 *
 * The city is drawn back to front: diagonal rows of tiles (tile x + tile y) from the far one, in each tile column
 * from the ground up, and what stands on a tile after it. */
#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rt/rt.h"

/* ---- the game's data in memory (the running program's addresses: render_set_addrs) ---- */
static RenderAddrs g_ra;
#define P_MAP g_ra.map            /* -> max x, max y, max z (32 bits each), then a pointer per column */
#define P_TILES g_ra.tiles        /* -> HBLK: 256 x 6 offsets of 32 x 16 blocks */
#define P_SPRITE_TAB g_ra.sprite_tab /* -> 6 bytes a sprite: pointer to its pixels, width, height */
#define P_FRAMES g_ra.frames      /* -> HFRA: 8 bytes a frame: first element, width, height, flags, next */
#define P_ELEMENTS g_ra.elements  /* -> HELE: 10 bytes: sprite * 6, x offset, y offset, flipped, next */
#define LEVEL g_ra.level          /* the level data (GAMExx.DAT) */
#define VIEW g_ra.view            /* (16 bits x, y) the view, in world units / 128 */

#define TILE_W 64
#define TILE_H 48
#define NB_TILES 256

typedef struct ObjList {
    uint32_t offset, size, count; /* in the level data */
} ObjList;

/* people, cars, objects, weapons, effects (FreeSynd's leveldata.h: LevelDataAll) */
static const ObjList k_lists[] = {
    {0x8008u, 0x5cu, 256},
    {0xdc08u, 0x2au, 64},
    {0xe688u, 0x1eu, 400},
    {0x11568u, 0x24u, 512},
    {0x15d68u, 0x1eu, 256},
};

static void snaps_reset(void);

void render_set_addrs(const RenderAddrs *a)
{
    g_ra = *a;
    snaps_reset();
}

static uint32_t rd32(uint32_t a)
{
    uint32_t v;
    memcpy(&v, M + a, 4);
    return v;
}

static uint16_t rd16(uint32_t a)
{
    uint16_t v;
    memcpy(&v, M + a, 2);
    return v;
}

static uint8_t g_types[256]; /* the tiles' types (COL01.DAT), render_set_tile_types */
static int g_have_types;

/* the images the GPU has (host_city_image, in true colour): the tiles 0-255 (from the HD pack, when it has them),
 * the sprites from 256 */
#define IMAGE_SPRITES 256
static uint8_t g_defined[IMAGE_SPRITES + 4096];

/* ---- tiles: decoded once per tile set (palette indices, 255 transparent) ---- */
static uint8_t g_tiles[NB_TILES][TILE_H][TILE_W];
static uint8_t g_tile_seen[NB_TILES]; /* 1 decoded, 2 decoded and empty */
static uint32_t g_tiles_at;

static void decode_bits(const uint8_t *mask, const uint8_t *planes, int stride, uint8_t *out)
{
    int i;
    for (i = 0; i < 8; i++) {
        int b = 7 - i;
        if ((mask[0] >> b) & 1) {
            out[i] = 255;
        } else {
            out[i] = (uint8_t)(((planes[0] >> b) & 1) | (((planes[stride] >> b) & 1) << 1) |
                               (((planes[2 * stride] >> b) & 1) << 2) | (((planes[3 * stride] >> b) & 1) << 3));
        }
    }
}

static void lights_forget(void);

static const uint8_t *tile(int id)
{
    uint32_t data = rd32(P_TILES);
    int sx, sy, row, blk;
    if (data != g_tiles_at) {
        memset(g_tile_seen, 0, sizeof g_tile_seen);
        g_tiles_at = data;
        memset(g_defined, 0, sizeof g_defined);
        lights_forget();
        host_city_reset();
    }
    if (!g_tile_seen[id]) {
        int any = 0;
        memset(g_tiles[id], 255, sizeof g_tiles[id]);
        for (sx = 0; sx < 2; sx++)
            for (sy = 0; sy < 3; sy++) {
                uint32_t off = rd32(data + (uint32_t)(id * 24 + (sx * 3 + sy) * 4));
                if (off < NB_TILES * 24u) continue; /* no block */
                for (row = 0; row < 16; row++) {
                    const uint8_t *r = M + data + off + (uint32_t)row * 20u;
                    /* the blocks and their rows from the top */
                    int y = sy * 16 + row;
                    for (blk = 0; blk < 4; blk++) decode_bits(r + blk, r + 4 + blk, 4, &g_tiles[id][y][sx * 32 + blk * 8]);
                }
            }
        for (row = 0; row < TILE_H && !any; row++)
            for (sx = 0; sx < TILE_W; sx++)
                if (g_tiles[id][row][sx] != 255) {
                    any = 1;
                    break;
                }
        g_tile_seen[id] = (uint8_t)(any ? 1 : 2);
    }
    return g_tile_seen[id] == 1 ? &g_tiles[id][0][0] : NULL;
}

/* ---- sprites: decoded when first drawn ---- */
#define NB_SPRITES 4096
static uint8_t g_hat_rows[NB_SPRITES]; /* the rows of a sprite with its hat; 0: it has none; 255: not known yet */
static uint8_t *g_sprites[NB_SPRITES];
static uint32_t g_sprites_at;

static const uint8_t *sprite(int id, int *w, int *h)
{
    uint32_t tab = rd32(P_SPRITE_TAB) + (uint32_t)id * 6u, px;
    int x, y, stride;
    if (id < 0 || id >= NB_SPRITES) return NULL;
    if (rd32(P_SPRITE_TAB) != g_sprites_at) {
        int i;
        for (i = 0; i < NB_SPRITES; i++) {
            free(g_sprites[i]);
            g_sprites[i] = NULL;
        }
        g_sprites_at = rd32(P_SPRITE_TAB);
        memset(g_hat_rows, 255, sizeof g_hat_rows);
        memset(g_defined, 0, sizeof g_defined);
        host_city_reset();
    }
    *w = M[tab + 4];
    *h = M[tab + 5];
    if (*w == 0 || *h == 0) return NULL;
    if (g_sprites[id] == NULL) {
        uint8_t *p = (uint8_t *)malloc((size_t)(*w * *h)), block[8];
        if (p == NULL) return NULL;
        px = rd32(tab);
        stride = (*w + 7) / 8;
        for (y = 0; y < *h; y++)
            for (x = 0; x < stride; x++) {
                const uint8_t *b = M + px + (uint32_t)((y * stride + x) * 5);
                int i;
                decode_bits(b, b + 1, 1, block);
                for (i = 0; i < 8 && x * 8 + i < *w; i++) p[y * *w + x * 8 + i] = block[i];
            }
        g_sprites[id] = p;
    }
    return g_sprites[id];
}

/* ---- drawing ---- */
typedef struct Target {
    uint32_t *rgb;
    int w, h, x0, y0, x1, y1; /* the frame and the rectangle drawn into */
    const uint32_t *pal;
    int gpu;                  /* images go to the GPU's list (host_city_frame) instead */
} Target;

/* ---- the GPU's list: images 0-255 the tiles, 256 + n the sprites ---- */
static HostCityQuad *g_quads;
static int g_nquads, g_quads_cap;

static int g_cut = -1;   /* the tiles of this level and above are drawn almost see-through (-1: none) */
static int g_translucent; /* the tile being put is one of them */
static int g_cut_above;   /* and its levels above the cut (the higher, the fainter) */
#define SEE_THROUGH_ALPHA 60 /* of 255, as FreeSynd's */
static uint32_t g_cols;     /* the map being drawn: its columns and size */
static int g_mx, g_my, g_mz;

#define HAT_INDEX 6  /* the hats' colour in the agents' sprites */
#define HAT_SHINE 14 /* the hats' bright red, where the light catches them (in the heads only) */
static uint32_t g_image_pal[256]; /* the palette the GPU's images were made with */
static int is_head(int id);

/* a hat's colour where the light catches it */
/* the agents' heads seen from the side (41, 42 and the same of their other sets): the pale pixel at the hat's edge is
 * its brim catching the light (the hat's colour, lighter), not the red hat's pink */
#define HAT_BRIM 4
static int hat_brim(int id, uint8_t c)
{
    return c == HAT_BRIM && id >= 41 && is_head(id);
}

/* the agents' heads (41 on): their rows from COLLAR_ROW down are the coat's collar and the shoulders (black and dark
 * grey: lost in the game's dark coat, a black line on a coat of another colour): the coat's colour, dark */
#define COLLAR_ROW 3
static int collar(int id, int row, uint8_t c)
{
    return id >= 41 && is_head(id) && row >= COLLAR_ROW && (c == 0 || c == 2 || c == 8);
}

static uint32_t shine(uint32_t c)
{
    uint32_t r = 0;
    int k;
    for (k = 0; k < 24; k += 8) {
        int v = (int)((c >> k) & 255) * 3 / 2 + 20;
        r |= (uint32_t)(v > 255 ? 255 : v) << k;
    }
    return r;
}

/* a pedestrian's own colours (render_ped_look): in the people's heads (sprites 1-32) their hair (blonde 11, grey 10
 * and 4, the dark heads' 8; red is the hats' colour, HAT_INDEX: the hat's colour is the hair's) and skin (9); in their
 * bodies the clothes, in their legs the trousers (every colour but black).  The image's alpha for them: 252 hair, 251
 * skin, 250 clothes, 249 trousers (255: none); their colour channels the pixel's shade (128: the colour itself): its
 * brightness against the part's main colour in that sprite, so the folds and lights stay */
#define LOOK_SKIN 9
#define LOOK_REF 85 /* the brightness a part's own colour is drawn at (its shades: lighter or darker than it) */
#define PEOPLE_SPRITES 458 /* the people's sprites: 1 to 457 (450-457: the upper bodies of those holding a weapon) */
static uint32_t g_obj_look[3]; /* the person being drawn: hair, skin, clothes (0: as drawn) */
static uint8_t g_look_main[NB_SPRITES]; /* a sprite's main colour of its part (+1; 0: not known yet) */

static int is_leg(int id)
{
    return (id >= 181 && id <= 260) || (id >= 361 && id <= 400) || (id >= 421 && id <= 440);
}

static int look_code(int id, uint8_t c)
{
    if (c == 0 || c == 255) return 255;
    if (id >= 1 && id <= 32) {
        if (c == LOOK_SKIN) return 251;
        if (c == 11 || ((id <= 24 && id > 16) && (c == 10 || c == 4)) || (id > 24 && c == 8)) return 252;
        return 255;
    }
    if (id > 32 && id < PEOPLE_SPRITES && !is_head(id) && c != HAT_INDEX && c != 14 && c != 12) return is_leg(id) ? 249 : 250;
    return 255;
}

static int lum8(uint32_t c)
{
    return (int)((((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000);
}

/* the shade (128: 1) of colour c in sprite id (its pixels px): against the part's most used colour */
/* (the people in their own palette in every mission: person_colour) */
static uint32_t person_colour(int sprite, uint8_t c, const uint32_t *pal);
static int look_shade(int id, const uint8_t *px, int n, uint8_t c)
{
    int k, s;
    if (g_look_main[id] == 0) {
        int count[256] = {0}, best = -1, code = look_code(id, c);
        for (k = 0; k < n; k++)
            if (look_code(id, px[k]) == code) count[px[k]]++;
        for (k = 0; k < 256; k++)
            if (count[k] > 0 && (best < 0 || count[k] > count[best])) best = k;
        g_look_main[id] = (uint8_t)(best < 0 ? c : best) + 1;
    }
    /* (against one brightness for every sprite, LOOK_REF: a colour is as light in each frame of an animation) */
    s = lum8(person_colour(id, c, g_image_pal)) * 128 / LOOK_REF;
    return s < 40 ? 40 : s > 230 ? 230 : s; /* 0.3 .. 1.8 */
}

/* a look's colour at a shade */
static uint32_t look_colour(uint32_t c, int shade)
{
    uint32_t r = 0;
    int k, mx = 5;
    if ((c & 0x1000000u) && shade > 102) shade = 102 + (shade - 102) * 35 / 100; /* (bit 24: its highlights subtle) */
    for (k = 0; k < 24; k += 8)
        if ((int)((c >> k) & 255) > mx) mx = (int)((c >> k) & 255);
    for (k = 0; k < 24; k += 8) {
        /* (the pixels lighter than the main colour add light too: a black coat keeps its highlights; that light in
         * the colour's own hue mostly: a dark red's are red, not pink) */
        int ch = (int)((c >> k) & 255), add = shade > 102 ? (shade - 102) * 9 / 10 * (30 + 70 * ch / mx) / 100 : 0;
        int v = ch * shade / 128 + add;
        r |= (uint32_t)(v > 255 ? 255 : v) << k;
    }
    return r;
}

/* an image (palette indices, 255 transparent) to the GPU in true colour: the hat's colours marked (alpha 254 its
 * colour, 253 its shine) */
static int g_hd = 1; /* the HD pack used (render_set_hd) */
/* the people (sprites 1-449) in one palette in every mission (the city's first, HPAL01: the HD pack's look), not the
 * mission's: their coats keep their colour from city to city */
static const uint32_t k_people_pal[16] = {0x000000u, 0xAE5D3Cu, 0x3C4945u, 0xFFA661u, 0xBE9A9Au, 0x715504u, 0xAA2400u,
    0x4D5979u, 0x343841u, 0xCB8651u, 0x656565u, 0xFFBE00u, 0xDFFFFFu, 0x967918u, 0xFF3C08u, 0x008EBAu};
static uint32_t person_colour(int sprite, uint8_t c, const uint32_t *pal)
{
    return sprite >= 1 && sprite < PEOPLE_SPRITES && c < 16 && g_hd ? k_people_pal[c] : pal[c];
}

static int hd_define(int image, const uint8_t *px);
static int hd_sprite(int image, const uint8_t *px, int w, int h);

static void define_image(int image, const uint8_t *px, int w, int h)
{
    static uint32_t rgb[256 * 256];
    int i, head = image >= IMAGE_SPRITES && is_head(image - IMAGE_SPRITES);
    if (w * h > 256 * 256) return;
    if (image < IMAGE_SPRITES && hd_define(image, px)) return;
    if (image >= IMAGE_SPRITES && !head && hd_sprite(image, px, w, h)) return;
    for (i = 0; i < w * h; i++) {
        uint32_t a = px[i] == HAT_INDEX ? 0xFE000000u
                     : head && (px[i] == HAT_SHINE || hat_brim(image - IMAGE_SPRITES, px[i])) ? 0xFD000000u : 0xFF000000u;
        if (image >= IMAGE_SPRITES && a == 0xFF000000u && px[i] != 255) {
            int code = collar(image - IMAGE_SPRITES, i / w, px[i]) ? 250 : look_code(image - IMAGE_SPRITES, px[i]);
            if (code != 255) { /* alpha 160 + 16 x part (hair, skin, clothes, trousers) + shade (0.3 + 0.1 x it) */
                int s = look_shade(image - IMAGE_SPRITES, px, w * h, px[i]), b = (s * 10 / 128 - 3);
                b = b < 0 ? 0 : b > 15 ? 15 : b;
                rgb[i] = (uint32_t)(160 + 16 * (252 - code) + b) << 24 | (g_image_pal[px[i]] & 0xFFFFFFu);
                continue;
            }
        }
        rgb[i] = px[i] == 255 ? 0 : a | (person_colour(image - IMAGE_SPRITES, px[i], g_image_pal) & 0xFFFFFFu);
    }
    host_city_image(image, w, h, 1, rgb);
}

/* a pedestrian's trousers: the clothes' colour darker and greyer */
static uint32_t trousers(uint32_t c)
{
    uint32_t r = 0;
    int k, l = lum8(c);
    for (k = 0; k < 24; k += 8) {
        int v = (((int)((c >> k) & 255) * 2 + l * 3) / 5) * 3 / 5;
        r |= (uint32_t)v << k;
    }
    return c != 0 ? r : 0;
}

/* ---- an agent drawn on Configure Company (the menus have no sprites loaded): a frame from the game's own files
 * (HSPR-0.TAB/.DAT, HFRA-0.ANI, HELE-0.ANI, unpacked: render_preview_files), its hat and coat in the colours chosen,
 * the rest in the people's colours, as a mission draws it ---- */
static uint8_t *g_pv_tab, *g_pv_dat, *g_pv_fra, *g_pv_ele;
static size_t g_pv_ntab, g_pv_ndat, g_pv_nfra, g_pv_nele;

void render_preview_files(uint8_t *tab, size_t ntab, uint8_t *dat, size_t ndat, uint8_t *fra, size_t nfra, uint8_t *ele,
                          size_t nele)
{
    g_pv_tab = tab, g_pv_ntab = ntab, g_pv_dat = dat, g_pv_ndat = ndat;
    g_pv_fra = fra, g_pv_nfra = nfra, g_pv_ele = ele, g_pv_nele = nele;
}

static unsigned pv16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static uint32_t trousers(uint32_t c);

/* frame `frame`, `scale` times its size, into rgb (w x h) with its feet's middle at bx, by; 0 if it cannot be */
int render_preview(uint32_t *rgb, int w, int h, int bx, int by, int scale, int frame, uint32_t hat, uint32_t coat)
{
    static uint8_t px[128 * 128];
    int pass, x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    if (g_pv_tab == NULL || g_pv_dat == NULL || g_pv_fra == NULL || g_pv_ele == NULL || (size_t)frame * 8u + 8u > g_pv_nfra)
        return 0;
    for (pass = 0; pass < 2; pass++) { /* the frame's box, then its sprites drawn round its feet */
        unsigned e = pv16(g_pv_fra + (size_t)frame * 8u);
        int guard = 0;
        while (e != 0 && (size_t)e * 10u + 10u <= g_pv_nele && guard++ < 32) {
            const uint8_t *ea = g_pv_ele + (size_t)e * 10u;
            int id = (int)pv16(ea) / 6, ox = (int16_t)pv16(ea + 2), oy = (int16_t)pv16(ea + 4), flip = pv16(ea + 6) != 0;
            const uint8_t *te = g_pv_tab + (size_t)id * 6u;
            int sw, sh, x, y, stride, main = -1;
            uint32_t off;
            e = pv16(ea + 8);
            if ((size_t)id * 6u + 6u > g_pv_ntab) continue;
            off = (uint32_t)te[0] | (uint32_t)te[1] << 8 | (uint32_t)te[2] << 16 | (uint32_t)te[3] << 24;
            sw = te[4], sh = te[5], stride = (sw + 7) / 8;
            if (sw == 0 || sh == 0 || sw * sh > (int)sizeof px || off + (uint32_t)(stride * sh * 5) > g_pv_ndat) continue;
            if (pass == 0) {
                if (ox < x0) x0 = ox;
                if (oy < y0) y0 = oy;
                if (ox + sw > x1) x1 = ox + sw;
                if (oy + sh > y1) y1 = oy + sh;
                continue;
            }
            for (y = 0; y < sh; y++)
                for (x = 0; x < stride; x++) {
                    uint8_t block[8];
                    int i;
                    decode_bits(g_pv_dat + off + (uint32_t)((y * stride + x) * 5), g_pv_dat + off + (uint32_t)((y * stride + x) * 5) + 1, 1, block);
                    for (i = 0; i < 8 && x * 8 + i < sw; i++) px[y * sw + x * 8 + i] = block[i];
                }
            { /* the coat's main colour in this sprite: its shades against it */
                int count[16] = {0}, k;
                for (k = 0; k < sw * sh; k++)
                    if (px[k] < 16 && look_code(id, px[k]) != 255) count[px[k]]++;
                for (k = 0; k < 16; k++)
                    if (count[k] > 0 && (main < 0 || count[k] > count[main])) main = k;
            }
            for (y = 0; y < sh; y++)
                for (x = 0; x < sw; x++) {
                    uint8_t c = px[y * sw + (flip ? sw - 1 - x : x)];
                    uint32_t col;
                    int code, a, b, X, Y;
                    if (c == 255 || c >= 16) continue;
                    code = collar(id, y, c) ? 250 : look_code(id, c);
                    if (is_head(id) && c == HAT_INDEX) col = hat;
                    else if (is_head(id) && (c == HAT_SHINE || hat_brim(id, c))) col = shine(hat);
                    else if (coat != 0 && (code == 250 || code == 249)) {
                        int s = lum8(k_people_pal[c]) * 128 / LOOK_REF;
                        col = look_colour(code == 249 ? trousers(coat) | (coat & 0x1000000u) : coat, s < 40 ? 40 : s > 230 ? 230 : s);
                    } else col = k_people_pal[c];
                    X = bx + (ox + x - (x0 + x1) / 2) * scale;
                    Y = by + (oy + y - y1) * scale;
                    for (b = 0; b < scale; b++)
                        for (a = 0; a < scale; a++)
                            if (X + a >= 0 && X + a < w && Y + b >= 0 && Y + b < h) rgb[(Y + b) * w + X + a] = col;
                }
        }
        if (x1 <= x0) return 0;
    }
    return 1;
}

/* a pixel of a sprite drawn on the CPU: the hat's colours in `hat`'s top rows (head: a head sprite) */
static int g_put_head; /* the sprite being put is a head (sprite_colour) */
static int g_put_sprite = -1; /* the sprite being put (-1: a tile) */
static uint32_t sprite_colour(const uint32_t *pal, uint8_t c, int row, uint32_t hat, int head)
{
    int code;
    if (row < (int)(hat >> 24) && c == HAT_INDEX) return hat & 0xFFFFFFu;
    if (row < (int)(hat >> 24) && head && (c == HAT_SHINE || hat_brim(g_put_sprite, c))) return shine(hat & 0xFFFFFFu);
    if (g_put_sprite >= 0 && g_look_main[g_put_sprite] != 0 &&
        (code = collar(g_put_sprite, row, c) ? 250 : look_code(g_put_sprite, c)) != 255) {
        uint32_t l = g_obj_look[code == 252 ? 0 : code == 251 ? 1 : 2];
        int s = lum8(person_colour(g_put_sprite, c, pal)) * 128 / LOOK_REF;
        if (code == 249) l = trousers(l) | (l & 0x1000000u);
        if (l != 0) return look_colour(l, s < 40 ? 40 : s > 230 ? 230 : s);
    }
    return person_colour(g_put_sprite, c, pal);
}

static int g_put_tx = -1, g_put_ty; /* the map place of the tile being put (-1: not a tile of the map) */
static int hd_has_materials(int image);

static uint32_t g_tile_shade, g_obj_shade; /* the tile being put, the thing being drawn: HostCityQuad.shade */
static float g_quad_z, g_obj_z; /* the height of what is being put (HostCityQuad.z), of the thing's feet */

static void emit(int image, const uint8_t *px, int w, int h, float x, float y, int flipped, uint32_t hat, int glow)
{
    HostCityQuad *q;
    if (!g_defined[image]) {
        if (px != NULL) define_image(image, px, w, h);
        g_defined[image] = 1;
    }
    if (g_nquads == g_quads_cap) {
        int cap = g_quads_cap ? g_quads_cap * 2 : 4096;
        HostCityQuad *n = (HostCityQuad *)realloc(g_quads, sizeof *n * (size_t)cap);
        if (n == NULL) return;
        g_quads = n;
        g_quads_cap = cap;
    }
    q = &g_quads[g_nquads++];
    q->x = x;
    q->y = y;
    q->image = (uint16_t)image;
    q->flip = (uint8_t)(flipped != 0);
    q->glow = (uint8_t)(glow | (g_translucent ? 0x80 : 0));
    q->shade = (image >= IMAGE_SPRITES ? g_obj_shade : g_tile_shade) | (g_translucent ? (uint32_t)g_cut_above << 20 : 0u);
    q->z = g_quad_z;
    q->look[0] = image >= IMAGE_SPRITES ? g_obj_look[0] : 0;
    q->look[1] = image >= IMAGE_SPRITES ? g_obj_look[1] : 0;
    q->look[2] = image >= IMAGE_SPRITES ? g_obj_look[2] : 0;
    q->hat = image >= IMAGE_SPRITES ? hat /* a sprite: its hat's rows, colour */
             : g_put_tx >= 0 && hd_has_materials(image)
                   ? HOST_CITY_WORLD | (uint32_t)(g_put_ty & 1023) << 10 | (uint32_t)(g_put_tx & 1023)
                   : 0; /* a tile with materials: its place on the map */
}


/* `hat`: a colour for the pixels of colour HAT_INDEX (0: as they are) */
static void blit(const Target *t, const uint8_t *src, int sw, int sh, int dx, int dy, int flipped, uint32_t hat);

/* ---- the zoom: the city drawn `g_zoom` times its size about the point g_zcx, g_zcy of the frame ---- */
static double g_zoom = 1.0;
static float g_zcx, g_zcy;

void render_set_cut(int level)
{
    g_cut = level;
}

void render_set_zoom(double zoom, float cx, float cy)
{
    g_zoom = zoom;
    g_zcx = cx;
    g_zcy = cy;
}

static float zoom_x(float x)
{
    return g_zcx + (x - g_zcx) * (float)g_zoom;
}

static float zoom_y(float y)
{
    return g_zcy + (y - g_zcy) * (float)g_zoom;
}

/* a pixel of a tile or sprite (blended over what is there when it is drawn see-through) */
static void put_pixel(const Target *t, int x, int y, uint32_t c)
{
    uint32_t *p = &t->rgb[y * t->w + x];
    if (g_translucent) {
        uint32_t o = *p, r = 0, k;
        for (k = 0; k < 24; k += 8)
            r |= ((((c >> k) & 0xFF) * SEE_THROUGH_ALPHA + ((o >> k) & 0xFF) * (255u - SEE_THROUGH_ALPHA)) / 255u) << k;
        c = r;
    }
    *p = c;
}

/* nearest pixel: src drawn `z` times its size with its top left corner at ox, oy */
static void blit_scaled(const Target *t, const uint8_t *src, int sw, int sh, float ox, float oy, float z, int flipped,
                        uint32_t hat)
{
    int x, y, xa = (int)floorf(ox), ya = (int)floorf(oy), xb = (int)ceilf(ox + sw * z), yb = (int)ceilf(oy + sh * z);
    if (xa < t->x0) xa = t->x0;
    if (ya < t->y0) ya = t->y0;
    if (xb > t->x1) xb = t->x1;
    if (yb > t->y1) yb = t->y1;
    for (y = ya; y < yb; y++) {
        int sy = (int)(((float)y + 0.5f - oy) / z);
        if (sy < 0 || sy >= sh) continue;
        for (x = xa; x < xb; x++) {
            int sx = (int)(((float)x + 0.5f - ox) / z);
            uint8_t c;
            if (sx < 0 || sx >= sw) continue;
            c = src[sy * sw + (flipped ? sw - 1 - sx : sx)];
            if (c == 255) continue;
            put_pixel(t, x, y, sprite_colour(t->pal, c, sy, hat, g_put_head));
        }
    }
}

/* a tile or a sprite (image number): on the GPU at its exact place, else as blit (both zoomed) */
static int tile_glow(int image);

/* the sprites drawn in the last frame, in order (unzoomed places): the debug readout's sprite under the pointer */
#define NB_SDRAWN 8192
static struct {
    int16_t id, x, y, w, h;
    uint8_t flipped;
    const uint8_t *px;
} g_sdrawn[NB_SDRAWN];
static int g_nsdrawn;

static void put(const Target *t, int image, const uint8_t *src, int sw, int sh, int dx, int dy, float fx, float fy,
                int flipped, uint32_t hat)
{
    if (image >= IMAGE_SPRITES && g_nsdrawn < NB_SDRAWN) {
        g_sdrawn[g_nsdrawn].id = (int16_t)(image - IMAGE_SPRITES);
        g_sdrawn[g_nsdrawn].x = (int16_t)dx;
        g_sdrawn[g_nsdrawn].y = (int16_t)dy;
        g_sdrawn[g_nsdrawn].w = (int16_t)sw;
        g_sdrawn[g_nsdrawn].h = (int16_t)sh;
        g_sdrawn[g_nsdrawn].flipped = (uint8_t)(flipped != 0);
        g_sdrawn[g_nsdrawn].px = src;
        g_nsdrawn++;
    }
    if (t->gpu)
        emit(image, src, sw, sh, g_zoom == 1.0 ? fx : zoom_x(fx), g_zoom == 1.0 ? fy : zoom_y(fy), flipped, hat,
             tile_glow(image));
    else if (g_zoom == 1.0)
        blit(t, src, sw, sh, dx, dy, flipped, hat);
    else
        blit_scaled(t, src, sw, sh, zoom_x(fx), zoom_y(fy), (float)g_zoom, flipped, hat);
}

static void blit(const Target *t, const uint8_t *src, int sw, int sh, int dx, int dy, int flipped, uint32_t hat)
{
    int x, y;
    for (y = 0; y < sh; y++) {
        int ty = dy + y;
        if (ty < t->y0 || ty >= t->y1) continue;
        for (x = 0; x < sw; x++) {
            int tx = dx + x;
            uint8_t c = src[y * sw + (flipped ? sw - 1 - x : x)];
            if (c == 255 || tx < t->x0 || tx >= t->x1) continue;
            put_pixel(t, tx, ty, sprite_colour(t->pal, c, y, hat, g_put_head));
        }
    }
}

/* ---- the HD texture pack (render.h) ---- */
#include "../third_party/stb_image.h" /* (its code: additions.c) */
#include "../third_party/stb_image_write.h" /* (its code: record.c) */
#define HD_SCALE 4
#define HD_W (TILE_W * HD_SCALE)
#define HD_H (TILE_H * HD_SCALE)
static volatile int g_hd_want = 1; /* render_set_hd (any thread); g_hd follows it at the next city drawn */
typedef struct HdTile {
    int state;          /* 0 not looked for, 1 loaded, 2 none */
    uint8_t *rgba;      /* HD_W x HD_H */
    uint8_t *normals;   /* HD_W x HD_H RGB(A) */
    uint8_t *orig;      /* TILE_W x TILE_H RGBA: the colours it was made in */
    int materials;      /* some of its pixels are a material's (alpha HD_MATERIAL + its number) */
} HdTile;
static HdTile g_hd_tiles[NB_TILES];
/* the ground materials (hd/material_KK.png, 1024 x 1024, repeating without a seam, and _n its normals): a tile's
 * pixels of alpha HD_MATERIAL + K show material K, laid on the ground in world space (4 texture pixels a game pixel:
 * 8 x 8 tiles before it repeats), so tiles side by side never show a seam or the same pattern */
#define HD_MATERIAL 240
#define HD_MATERIALS 12
#define HD_MAT_SIZE 1024
static int g_hd_materials_loaded;

static uint8_t *hd_load(int id, const char *suffix, int w, int h)
{
    char path[1200];
    int iw, ih, n;
    uint8_t *p;
    snprintf(path, sizeof path, "%s/hd/tile_%03d%s.png", rt_user_dir, id, suffix);
    p = stbi_load(path, &iw, &ih, &n, 4);
    if (p != NULL && (iw != w || ih != h)) {
        stbi_image_free(p);
        p = NULL;
    }
    return p;
}

static const char *g_dump_tiles;

void render_dump_tiles(const char *path)
{
    g_dump_tiles = path;
}

static void dump_tiles(const uint32_t *pal)
{
    FILE *f = fopen(g_dump_tiles, "wb");
    int i, k;
    g_dump_tiles = NULL;
    if (f == NULL) return;
    for (i = 0; i < NB_TILES; i++) {
        const uint8_t *t = tile(i);
        for (k = 0; k < TILE_W * TILE_H; k++) {
            uint32_t c = t != NULL && t[k] != 255 ? 0xFF000000u | pal[t[k]] : 0;
            fwrite(&c, 4, 1, f);
        }
    }
    fwrite(g_types, 1, sizeof g_types, f);
    fclose(f);
}

void render_set_hd(int on)
{
    g_hd_want = on != 0;
}

static int hd_has_materials(int image)
{
    return g_hd && image >= 0 && image < NB_TILES && g_hd_tiles[image].materials;
}

static void hd_materials(void)
{
    static uint32_t rgb[HD_MAT_SIZE * HD_MAT_SIZE], nrm[HD_MAT_SIZE * HD_MAT_SIZE];
    int k, i;
    g_hd_materials_loaded = 1;
    for (k = 0; k < HD_MATERIALS; k++) {
        char path[1200];
        int w, h, n;
        uint8_t *c, *nm;
        snprintf(path, sizeof path, "%s/hd/material_%02d.png", rt_user_dir, k);
        c = stbi_load(path, &w, &h, &n, 4);
        if (c == NULL) continue;
        if (w != HD_MAT_SIZE || h != HD_MAT_SIZE) {
            stbi_image_free(c);
            continue;
        }
        snprintf(path, sizeof path, "%s/hd/material_%02d_n.png", rt_user_dir, k);
        nm = stbi_load(path, &w, &h, &n, 4);
        if (nm != NULL && (w != HD_MAT_SIZE || h != HD_MAT_SIZE)) {
            stbi_image_free(nm);
            nm = NULL;
        }
        for (i = 0; i < HD_MAT_SIZE * HD_MAT_SIZE; i++) {
            const uint8_t *p = c + i * 4, *q = nm != NULL ? nm + i * 4 : NULL;
            rgb[i] = 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            nrm[i] = q != NULL ? 0xFF000000u | (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2] : 0xFF8080FFu;
        }
        host_city_material(k, HD_MAT_SIZE, HD_MAT_SIZE, rgb, nrm);
        stbi_image_free(c);
        if (nm != NULL) stbi_image_free(nm);
    }
}

/* tile `image` (its pixels px, palette indices) from the pack, with its normals; 0 if not in the pack.  With the tile it
 * was made from (tile_NNN_orig.png) its colours are brought to the tile's (its mean colour, each channel, the tile's
 * mean colour in this palette: as bright and tinted as the game draws it); without, it keeps its own colours in every
 * mission (the time of day still lights it) */
static int hd_define(int image, const uint8_t *px)
{
    static uint32_t rgb[HD_W * HD_H], nrm[HD_W * HD_H];
    HdTile *t = &g_hd_tiles[image];
    float now[3] = {0, 0, 0}, was[3] = {0, 0, 0}, f[3];
    int x, y, ch;
    if (!g_hd) return 0;
    if (!g_hd_materials_loaded) hd_materials();
    if (t->state == 0) {
        t->rgba = hd_load(image, "", HD_W, HD_H);
        t->normals = t->rgba != NULL ? hd_load(image, "_n", HD_W, HD_H) : NULL;
        t->orig = t->rgba != NULL ? hd_load(image, "_orig", TILE_W, TILE_H) : NULL;
        t->state = t->rgba != NULL ? 1 : 2;
    }
    if (t->state != 1) return 0;
    for (y = 0; y < TILE_H * TILE_W; y++)
        if (px[y] != 255)
            for (ch = 0; ch < 3; ch++)
                now[ch] += (float)((g_image_pal[px[y]] >> (16 - 8 * ch)) & 255) * (HD_SCALE * HD_SCALE);
    for (y = 0; y < HD_H; y++)
        for (x = 0; x < HD_W; x++)
            if (t->rgba[(y * HD_W + x) * 4 + 3] >= 128 && px[(y / HD_SCALE) * TILE_W + x / HD_SCALE] != 255)
                for (ch = 0; ch < 3; ch++) was[ch] += (float)t->rgba[(y * HD_W + x) * 4 + ch];
    for (ch = 0; ch < 3; ch++) f[ch] = t->orig != NULL && was[ch] > 0 ? now[ch] / was[ch] : 1.0f;
    for (y = 0; y < HD_H; y++)
        for (x = 0; x < HD_W; x++) {
            const uint8_t *h = t->rgba + (y * HD_W + x) * 4;
            uint32_t out = 0;
            if (h[3] < 128 || px[(y / HD_SCALE) * TILE_W + x / HD_SCALE] == 255) {
                rgb[y * HD_W + x] = 0;
                continue;
            }
            for (ch = 0; ch < 3; ch++) {
                float v = (float)h[ch] * f[ch];
                out |= (uint32_t)(v > 255.0f ? 255.0f : v) << (16 - 8 * ch);
            }
            /* a material's pixel keeps its alpha (the material's number) */
            if (h[3] >= HD_MATERIAL && h[3] < HD_MATERIAL + HD_MATERIALS) {
                rgb[y * HD_W + x] = (uint32_t)h[3] << 24 | out;
                t->materials = 1;
            } else {
                rgb[y * HD_W + x] = 0xFF000000u | out;
            }
            if (t->normals != NULL) {
                const uint8_t *n = t->normals + (y * HD_W + x) * 4;
                nrm[y * HD_W + x] = 0xFF000000u | (uint32_t)n[0] << 16 | (uint32_t)n[1] << 8 | n[2];
            }
        }
    host_city_image(image, HD_W, HD_H, HD_SCALE, rgb);
    if (t->normals != NULL) host_city_normals(image, HD_W, HD_H, nrm);
    return 1;
}

/* the HD sprites (hd/sprite_NNNN.png, HD_SCALE times the sprite's size, RGBA): drawn in its place, in its own colours
 * (the trees, ...); 0 if not in the pack.  SYND_DUMP_SPRITES=DIR writes each sprite the city draws to DIR/NNNN.png in
 * this palette's colours (to redraw them) */
#define HD_FIRE 232
#define HD_SMOKE 233 /* smoke: its density in red (drawn moving, see-through) */
static uint8_t g_hd_sprite_state[NB_SPRITES]; /* 0 not looked for, 1 loaded, 2 none */
static uint8_t *g_hd_sprite[NB_SPRITES];

static void dump_sprite(int id, const uint8_t *px, int w, int h)
{
    static uint8_t done[NB_SPRITES];
    static uint8_t rgba[256 * 256 * 4];
    const char *dir = getenv("SYND_DUMP_SPRITES");
    char path[1200];
    int i;
    if (dir == NULL || done[id] || w * h > 256 * 256) return;
    done[id] = 1;
    if (getenv("SYND_DUMP_ALL_SPRITES") != NULL) { /* every sprite the game has, once */
        static int all;
        if (!all) {
            int k, sw, sh;
            const uint8_t *s;
            all = 1;
            for (k = 0; k < NB_SPRITES; k++)
                if (k != id && (s = sprite(k, &sw, &sh)) != NULL && sw > 0 && sh > 0) dump_sprite(k, s, sw, sh);
        }
    }
    for (i = 0; i < w * h; i++) {
        uint32_t c = g_image_pal[px[i]];
        rgba[i * 4] = (uint8_t)(c >> 16);
        rgba[i * 4 + 1] = (uint8_t)(c >> 8);
        rgba[i * 4 + 2] = (uint8_t)c;
        rgba[i * 4 + 3] = px[i] == 255 ? 0 : 255;
    }
    snprintf(path, sizeof path, "%s/%04d.png", dir, id);
    stbi_write_png(path, w, h, 4, rgba, w * 4);
}

static int hd_sprite(int image, const uint8_t *px, int w, int h)
{
    static uint32_t rgb[256 * HD_SCALE * 256 * HD_SCALE];
    int id = image - IMAGE_SPRITES, i;
    dump_sprite(id, px, w, h);
    if (!g_hd || id < PEOPLE_SPRITES || id >= NB_SPRITES || w * h > 256 * 256) return 0; /* (the people: never the pack's) */
    if (g_hd_sprite_state[id] == 0) {
        char path[1200];
        int iw, ih, n;
        snprintf(path, sizeof path, "%s/hd/sprite_%04d.png", rt_user_dir, id);
        g_hd_sprite[id] = stbi_load(path, &iw, &ih, &n, 4);
        if (g_hd_sprite[id] != NULL && (iw != w * HD_SCALE || ih != h * HD_SCALE)) {
            stbi_image_free(g_hd_sprite[id]);
            g_hd_sprite[id] = NULL;
        }
        g_hd_sprite_state[id] = g_hd_sprite[id] != NULL ? 1 : 2;
    }
    if (g_hd_sprite_state[id] != 1) return 0;
    for (i = 0; i < w * h * HD_SCALE * HD_SCALE; i++) {
        const uint8_t *p = g_hd_sprite[id] + i * 4;
        rgb[i] = p[3] < 128 ? 0 : (p[3] == HD_FIRE || p[3] == HD_SMOKE ? (uint32_t)p[3] << 24 : 0xFF000000u) | (uint32_t)p[0] << 16 |
                                   (uint32_t)p[1] << 8 | p[2]; /* alpha HD_FIRE: fire, its heat in red (drawn moving) */
    }
    host_city_image(image, w * HD_SCALE, h * HD_SCALE, HD_SCALE, rgb);
    return 1;
}

/* `hat`: the colour of the hat: colour 6 of the sprites that are heads, whose colour 6 is all in their top
 * HAT_ROWS rows (what the agents hold uses colour 6 too, anywhere in it) */
#define HAT_ROWS 5

/* the agents' heads (the eight ways they face, in each of their sets: the top sprite of the people's frames): all
 * their colour 6 is the hat, even where it goes lower than HAT_ROWS (a head bowed towards the camera, a cap seen
 * from behind) */
static int is_head(int id)
{
    return (id >= 9 && id <= 16) || (id >= 41 && id <= 64) || (id >= 1121 && id <= 1128);
}

static int hat_rows(int id, const uint8_t *s, int sw, int sh)
{
    if (id < 0 || id >= NB_SPRITES) return 0;
    if (g_hat_rows[id] == 255 && is_head(id)) g_hat_rows[id] = (uint8_t)(sh < 255 ? sh : 254);
    if (g_hat_rows[id] == 255) {
        int x, y, top = -1, last = -1;
        for (y = 0; y < sh; y++)
            for (x = 0; x < sw; x++) {
                uint8_t c = s[y * sw + x];
                if (c == 255) continue;
                if (top < 0) top = y;
                if (c == HAT_INDEX) last = y;
            }
        g_hat_rows[id] = (uint8_t)(last >= 0 && last - top < HAT_ROWS ? last + 1 : 0);
    }
    return g_hat_rows[id];
}
static void add_light(float fx, float fy, float rx, float ry, uint32_t colour);
static void project_f(int32_t x, int32_t y, int32_t z, float *fx, float *fy);

#define FLASH_LIGHT_R 56.0f          /* a flash's light (a muzzle flash, an explosion): its radius */
#define FLASH_LIGHT_COLOUR 0xFFC878u
#define FIRE_LIGHT_COLOUR 0xFF8A3Cu  /* a blaze's: orange-red */
#define FIRE_LIGHT_R 150.0f
/* a sprite with fire in it (the HD pack's dynamic fire: blazes, a destroyed car's flames, the explosion's fireball) */
static int fire_sprite(int id)
{
    return (id >= 1756 && id <= 1759) || (id >= 1005 && id <= 1008) || (id >= 765 && id <= 773) || id == 872 ||
           id == 1552 || (id >= 1562 && id <= 1565); /* (+ the fireballs rising from a burning car, a small flame) */
}

/* the bits rising from a fire (its small flames and smoke wisps), the explosion's smoke, smoke columns: GPU smoke */
static int smoke_sprite(int id)
{
    return (id >= 992 && id <= 1002) || (id >= 774 && id <= 779) || id == 841 || id == 842;
}
/* a flash (a muzzle flash, an explosion, a flame): a small sprite mostly of bright yellow and white pixels */
static int flash_sprite(int id, const uint8_t *s, int sw, int sh)
{
    static uint8_t known[NB_SPRITES], flash[NB_SPRITES];
    if (id < 450 || id >= NB_SPRITES) return 0; /* the people's sprites never (a blonde head is yellow) */
    if (!known[id]) {
        int k, n = 0, hot = 0;
        known[id] = 1;
        for (k = 0; sw <= 40 && sh <= 40 && k < sw * sh; k++) {
            uint32_t c;
            if (s[k] == 255) continue;
            n++;
            c = g_image_pal[s[k]];
            if (((c >> 16) & 255) >= 200 && ((c >> 8) & 255) >= 120) hot++; /* white, yellow, bright orange */
        }
        flash[id] = n >= 4 && hot * 2 >= n;
    }
    return flash[id];
}

/* ---- the GPU's effects (HD): each burning thing's fire and smoke, each explosion, kept by where it is in the world
 * (the game's sprites only say where, and roughly how big: each frame of their animation its own size, so the effect
 * eases to it) and drawn at the display's rate, moving smoothly (no sprite); an explosion runs its own time ---- */
#define NB_FX 96
#define FX_EXPLODE_US 1800000u /* an explosion's life: flash, fireball, smoke rising and thinning */
enum { FX_FIRE = 1, FX_SMOKE, FX_EXPLODE };
typedef struct Fx {
    int kind, wx, wy, wz;
    uint64_t start, seen;           /* when it was first, last seen (its sprites drawn) */
    float bx, by, w, h;             /* its base (from its anchor: frame pixels), half width, height: as drawn */
    float tbx, tby, tw, th;         /* as its sprites are now (what it eases to) */
    uint32_t seed;
} Fx;
static Fx g_fx[NB_FX];
static uint64_t g_fx_now, g_fx_last;
static int g_obj_wx, g_obj_wy, g_obj_wz; /* the object being drawn: its place in the world */
static int g_obj_person;                 /* it is a person (its flash sprites: a muzzle flash) */
#define MUZZLE_FX 0 /* 1: the people's muzzle flashes drawn by the GPU (off: the game's sprites) */

static float saturate_f(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* an explosion's sprites (its fireball, its smoke) */
static int explode_sprite(int id)
{
    return id >= 765 && id <= 779;
}

/* a burning thing's (or an explosion's) sprites drawn this frame: x0..x1, y0..y1 from its anchor */
static void fx_seen(int kind, float x0, float y0, float x1, float y1)
{
    Fx *f = NULL;
    int i, free_i = -1, old_i = -1;
    uint64_t oldest = ~0ull;
    for (i = 0; i < NB_FX; i++) {
        Fx *e = &g_fx[i];
        if (e->kind == 0) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (e->kind == kind && abs(e->wx - g_obj_wx) <= 64 && abs(e->wy - g_obj_wy) <= 64 && abs(e->wz - g_obj_wz) <= 96) {
            f = e;
            break;
        }
        if (e->seen < oldest) oldest = e->seen, old_i = i;
    }
    if (f == NULL) {
        if (free_i < 0) free_i = old_i; /* none free: the one seen longest ago */
        if (free_i < 0) return;
        f = &g_fx[free_i];
        memset(f, 0, sizeof *f);
        f->kind = kind;
        f->start = g_fx_now;
        f->seed = (uint32_t)((g_obj_wx / 48) * 73856093 ^ (g_obj_wy / 48) * 19349663 ^ g_obj_wz * 83492791);
        f->bx = (x0 + x1) / 2.0f;
        f->by = y1;
        f->w = (x1 - x0) / 2.0f;
        f->h = y1 - y0;
    }
    f->wx = g_obj_wx;
    f->wy = g_obj_wy;
    f->wz = g_obj_wz;
    f->seen = g_fx_now;
    f->tbx = (x0 + x1) / 2.0f;
    f->tby = y1;
    f->tw = (x1 - x0) / 2.0f;
    f->th = y1 - y0;
}

static uint32_t fx_colour(uint32_t kind, uint32_t seed, float strong, float progress)
{
    int s = (int)(strong * 255.0f + 0.5f), p = (int)(progress * 255.0f + 0.5f);
    s = s < 0 ? 0 : s > 255 ? 255 : s;
    p = p < 0 ? 0 : p > 255 ? 255 : p;
    return kind | (seed & 255u) << 16 | (uint32_t)s << 8 | (uint32_t)p;
}

/* the effects of this frame, each where its thing is now on the screen */
static void fx_draw(void)
{
    float dt = g_fx_last != 0 && g_fx_now > g_fx_last ? (float)(g_fx_now - g_fx_last) / 1e6f : 0.016f;
    float ease = 1.0f - expf(-dt / 0.2f); /* to the sprites' size in about 0.2 s */
    int i;
    g_fx_last = g_fx_now;
    for (i = 0; i < NB_FX; i++) {
        Fx *f = &g_fx[i];
        float ffx, ffy, ax, ay, since, strong;
        if (f->kind == 0) continue;
        since = (float)(g_fx_now - f->seen) / 1e6f;
        project_f(f->wx, f->wy, f->wz, &ffx, &ffy);
        ax = ffx + (float)render_obj_dx;
        ay = ffy + (float)render_obj_dy;
        if (f->kind == FX_EXPLODE) {
            float pr = (float)(g_fx_now - f->start) / (float)FX_EXPLODE_US, r;
            if (pr >= 1.0f) {
                f->kind = 0;
                continue;
            }
            r = f->w > f->h * 0.5f ? f->w : f->h * 0.5f;
            r = (r < 30.0f ? 30.0f : r) * 1.6f;
            add_light(ax + f->bx, ay + f->by, r, r * 2.0f, fx_colour(HOST_LIGHT_EXPLODE, f->seed, 1.0f, pr));
            if (pr < 0.45f) { /* its light: a flash, then the fireball's glow dying */
                float k = 1.0f - pr / 0.45f, lr = r * (2.0f + 3.0f * (pr < 0.08f ? 1.0f : k));
                add_light(ax + f->bx, ay + f->by - r * 0.6f, lr, lr * 0.6f, pr < 0.1f ? 0xFFF0D0u : FIRE_LIGHT_COLOUR);
            }
            continue;
        }
        /* a fire or smoke: in as its sprites come, out a moment after they go */
        strong = since < 0.25f ? 1.0f : 1.0f - (since - 0.25f) / 0.6f;
        if (strong <= 0.0f) { /* gone */
            f->kind = 0;
            continue;
        }
        strong *= 0.15f + 0.85f * saturate_f((float)(g_fx_now - f->start) / 3e5f);
        f->bx += (f->tbx - f->bx) * ease;
        f->by += (f->tby - f->by) * ease;
        f->w += (f->tw - f->w) * ease;
        f->h += (f->th - f->h) * ease;
        if (f->kind == FX_FIRE) {
            /* (its sprites' box takes in the bits rising from it: the flames lower than it) */
            float fw = f->w * 1.15f + 2.0f, fh = f->h * 0.8f + 6.0f;
            if (f->h < 16.0f) fw = f->w * 0.9f + 1.0f, fh = f->h * 1.1f + 2.0f; /* a small flame: about its sprite's size */
            add_light(ax + f->bx, ay + f->by + fh * 0.08f, fw, fh, fx_colour(HOST_LIGHT_FIRE, f->seed, strong, fw / fh / 4.0f));
            /* its smoke rising from the flames' tops */
            add_light(ax + f->bx, ay + f->by - fh * 0.55f, fw * 1.7f, fh * 2.2f, fx_colour(HOST_LIGHT_SMOKE, f->seed * 7u, strong * 0.85f, 0));
        } else {
            add_light(ax + f->bx, ay + f->by, f->w * 1.2f + 2.0f, f->h * 1.5f + 4.0f, fx_colour(HOST_LIGHT_SMOKE, f->seed, strong, 0));
        }
    }
}

static void draw_frame(const Target *t, int frame, int ax, int ay, float fax, float fay, uint32_t hat)
{
    uint32_t frames = rd32(P_FRAMES), elements = rd32(P_ELEMENTS), f = frames + (uint32_t)frame * 8u;
    uint32_t e = rd16(f), e0 = e;
    int guard = 0, top = 0x7FFFFFFF;
    /* the frame's topmost element: the head (the hat's colour is also in what the agents hold, as the minigun) */
    if (hat != 0)
        for (;;) {
            uint32_t ea = elements + e * 10u;
            int sw, sh, oy = (int16_t)rd16(ea + 4u);
            if (sprite(rd16(ea) / 6, &sw, &sh) != NULL && oy < top) top = oy;
            e = rd16(ea + 8u);
            if (e == 0 || ++guard > 64) break;
        }
    e = e0;
    guard = 0;
    {
    /* the frame's fire, smoke, explosion sprites: their boxes (from the anchor), one effect each (fx_seen) */
    float bx0[4] = {1e9f, 1e9f, 1e9f, 1e9f}, by0[4] = {1e9f, 1e9f, 1e9f, 1e9f}, bx1[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
    float by1[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
    int fx_on = t->gpu && g_hd, kind;
    for (;;) {
        uint32_t ea = elements + e * 10u;
        int sw, sh, id = rd16(ea) / 6, ox = (int16_t)rd16(ea + 2u), oy = (int16_t)rd16(ea + 4u);
        const uint8_t *s = sprite(id, &sw, &sh);
        if (s != NULL) {
            int rows = hat != 0 && (is_head(id) || oy == top) ? hat_rows(id, s, sw, sh) : 0;
            g_put_head = is_head(id);
            g_put_sprite = id;
            g_quad_z = g_obj_z - 8.0f * (float)oy; /* its feet at the anchor, 8 world units a pixel up */
            kind = !fx_on ? 0 : explode_sprite(id) ? FX_EXPLODE : fire_sprite(id) ? FX_FIRE : smoke_sprite(id) ? FX_SMOKE : 0;
            if (kind) { /* the GPU draws it (no sprite) */
                float x0 = (float)ox, y0 = (float)oy, x1 = (float)(ox + sw), y1 = (float)(oy + sh);
                if (kind == FX_FIRE && id >= 1756) x0 += (float)sw * 0.15f, x1 -= (float)sw * 0.15f; /* a blaze: its flames' width */
                if (x0 < bx0[kind]) bx0[kind] = x0;
                if (y0 < by0[kind]) by0[kind] = y0;
                if (x1 > bx1[kind]) bx1[kind] = x1;
                if (y1 > by1[kind]) by1[kind] = y1;
            }
            /* a person's muzzle flash: the GPU draws it (a small teardrop of fire pointing away from the gun, from the
             * person's middle through the flash sprite's), no sprite */
            int muzzle = MUZZLE_FX && fx_on && !kind && g_obj_person && id >= PEOPLE_SPRITES && flash_sprite(id, s, sw, sh);
            if (muzzle) {
                /* (the flash's own pixels: the one nearest the person's middle is at the gun's tip, the teardrop's
                 * point there; it points away from the person) */
                int px, py, n = 0, flip = rd16(ea + 6u) != 0, turn;
                float cx = 0, cy = 0, tx = 0, ty = 0, best = 1e9f, far2 = 0, dx, dy, r, len;
                for (py = 0; py < sh; py++)
                    for (px = 0; px < sw; px++)
                        if (s[py * sw + px] != 255) {
                            float X = (float)ox + (float)(flip ? sw - 1 - px : px) + 0.5f, Y = (float)oy + (float)py + 0.5f;
                            float d2 = X * X + (Y + 14.0f) * (Y + 14.0f);
                            cx += X, cy += Y, n++;
                            if (d2 < best) best = d2, tx = X, ty = Y;
                        }
                if (n == 0) n = 1, tx = (float)ox + (float)sw / 2.0f, ty = (float)oy + (float)sh / 2.0f, cx = tx, cy = ty;
                cx /= (float)n, cy /= (float)n;
                dx = cx, dy = cy + 21.0f; /* away from the person: from its shoulders through the flash's (whatever the
                                           * flash's own shape: a streak, a star) */
                len = sqrtf(dx * dx + dy * dy);
                if (len < 0.01f) dx = 1.0f, dy = 0.0f, len = 1.0f;
                dx /= len, dy /= len;
                for (py = 0; py < sh; py++)
                    for (px = 0; px < sw; px++)
                        if (s[py * sw + px] != 255) {
                            float X = (float)ox + (float)(flip ? sw - 1 - px : px) + 0.5f - tx, Y = (float)oy + (float)py + 0.5f - ty;
                            if (X * X + Y * Y > far2) far2 = X * X + Y * Y;
                        }
                r = sqrtf(far2) * 0.6f + 2.5f; /* a little longer than the flash drawn */
                if (r > 8.0f) r = 8.0f;
                turn = (int)(atan2f(dy, dx) / 6.2831853f * 256.0f) & 255;
                cx = tx + dx * r * 0.9f, cy = ty + dy * r * 0.9f; /* (its point 0.9 of its radius from its middle) */
                add_light(fax + cx, fay + cy, r, r, HOST_LIGHT_FLASH | (uint32_t)turn << 16 |
                                                     (uint32_t)((g_obj_wx * 7 + g_obj_wy * 13) & 255) << 8);
            }
            int burning = fire_sprite(id) && !explode_sprite(id);
            if (t->gpu && (id < 729 || id > 733) && (burning || flash_sprite(id, s, sw, sh))) {
                /* it lights what is round it (by night most): two or three pools a little apart, changing each
                 * frame (a flicker, not a disc); the impacts' sparks light themselves (LIST_EFFECTS) */
                static uint32_t flicker = 2463534242u;
                float cx = fax + (float)ox + (float)sw / 2.0f, cy = fay + (float)oy + (float)sh / 2.0f;
                int n, pools = burning ? 4 : 2 + (int)(flicker & 1u); /* fire: brighter (the lights add up) */
                for (n = 0; n < pools; n++) {
                    float jx, jy, r;
                    flicker ^= flicker << 13;
                    flicker ^= flicker >> 17;
                    flicker ^= flicker << 5;
                    jx = (float)((int)(flicker & 31u) - 16) * 0.9f;
                    jy = (float)((int)((flicker >> 5) & 31u) - 16) * 0.5f;
                    r = FLASH_LIGHT_R * (0.45f + 0.35f * (float)((flicker >> 10) & 15u) / 15.0f);
                    if (burning) r = FIRE_LIGHT_R * (0.75f + 0.35f * (float)((flicker >> 10) & 15u) / 15.0f);
                    add_light(cx + jx, cy + jy + (burning ? (float)sh * 0.3f : 0.0f), r,
                              r * (0.45f + 0.25f * (float)((flicker >> 14) & 7u) / 7.0f),
                              burning ? FIRE_LIGHT_COLOUR : FLASH_LIGHT_COLOUR);
                }
            }
            if ((id >= 729 && id <= 738) || flash_sprite(id, s, sw, sh)) g_obj_shade |= HOST_SHADE_LIGHT; /* no shadow */
            if (!kind && !muzzle)
            put(t, IMAGE_SPRITES + id, s, sw, sh, ax + ox, ay + oy, fax + (float)ox, fay + (float)oy, rd16(ea + 6u) != 0,
                rows > 0 ? (hat & 0xFFFFFFu) | (uint32_t)rows << 24 : 0);
            g_put_head = 0;
            g_put_sprite = -1;
            g_obj_shade &= ~HOST_SHADE_LIGHT;
        }
        e = rd16(ea + 8u);
        if (e == 0 || ++guard > 64) break;
    }
    for (kind = FX_FIRE; kind <= FX_EXPLODE; kind++)
        if (bx1[kind] > bx0[kind]) fx_seen(kind, bx0[kind], by0[kind], bx1[kind], by1[kind]);
    }
}

/* ---- where things are: a snapshot at each tick of the game, drawn on a steady clock ----
 * The ticks start on a steady clock (additions.c, on_frame_wait) and each snapshot is given the time its tick
 * started, but it is taken later, after the game's drawing (0-40 ms).  A playback clock goes through the snapshots
 * at the ticks' rate over the last four seconds, PLAY_BEHIND ticks behind the newest (enough to cover that delay),
 * and is brought back to it by running a little faster or slower (PLAY_GAIN for each tick off, its average over a
 * second; at most PLAY_ADJUST), which the eye does not see.  Measured: every frame within 2% of the same speed. */
#define NB_OBJS (256 + 64 + 400 + 512 + 256)
#define TELEPORT 512   /* a move of more than 2 tiles in one tick is not drawn in between */
#define NB_HIST 16
#define PLAY_BEHIND 2.0
#define PLAY_ADJUST 0.1
#define PLAY_GAIN 0.03
#define PLAY_FILTER_US 1000000.0
#define PLAY_RESET 6.0 /* further off: start again there */
#define RATE_TICKS 80  /* the rate is measured over this many ticks */

typedef struct Snap {
    int32_t vx, vy;              /* the view, in world units */
    int16_t pos[NB_OBJS][3];
    uint16_t frame[NB_OBJS];     /* the frame shown (+0x10): the pose goes with the position */
    uint8_t loc[NB_OBJS];        /* on the map or not (+0xa) */
} Snap;

static Snap g_hist[NB_HIST];
static uint32_t g_nticks;        /* snapshots taken (the newest: g_nticks - 1) */
static uint64_t g_last_tick_us, g_play_us;
static uint64_t g_tick_us[RATE_TICKS + 1]; /* when the last ticks came (since a pause) */
static int g_rate_n;
static double g_period_us = 50000.0; /* the ticks' period lately */
static double g_play = -1.0;     /* the playback clock, in ticks */
static double g_play_off;        /* how far it is from where it should be (ticks, averaged) */
static int32_t g_vx, g_vy;       /* the view of the frame being drawn */
static float g_vfx, g_vfy;       /* its fractions (subtracted: the view is g_v - g_vf) */
static int g_own_view;           /* render_set_view: the view is this program's, not the game's */
static int32_t g_own_vx, g_own_vy;

static float g_own_fx, g_own_fy;

void render_set_view(int own, int32_t vx, int32_t vy)
{
    g_own_view = own;
    g_own_vx = vx;
    g_own_vy = vy;
    g_own_fx = g_own_fy = 0.0f;
}

void render_set_view_f(double vx, double vy)
{
    g_own_view = 1;
    g_own_vx = (int32_t)vx;
    g_own_vy = (int32_t)vy;
    g_own_fx = (float)(g_own_vx - vx); /* <= 0 */
    g_own_fy = (float)(g_own_vy - vy);
}

static void read_view(int32_t *vx, int32_t *vy)
{
    *vx = (int32_t)(int16_t)rd16(VIEW) * 128;
    *vy = (int32_t)(int16_t)rd16(VIEW + 2u) * 128;
}

void render_tick(uint64_t now_us)
{
    Snap *s = &g_hist[g_nticks % NB_HIST];
    int li, n = 0;
    read_view(&s->vx, &s->vy);
    for (li = 0; li < (int)(sizeof k_lists / sizeof k_lists[0]); li++) {
        const ObjList *l = &k_lists[li];
        uint32_t i;
        for (i = 0; i < l->count; i++, n++) {
            uint32_t a = LEVEL + l->offset + i * l->size;
            s->pos[n][0] = (int16_t)rd16(a + 4u);
            s->pos[n][1] = (int16_t)rd16(a + 6u);
            s->pos[n][2] = (int16_t)rd16(a + 8u);
            s->frame[n] = rd16(a + 0x10u);
            s->loc[n] = M[a + 0xau];
        }
    }
    /* the period over the last ticks (a pause, e.g. a menu, starts again) */
    if (g_nticks == 0 || now_us < g_last_tick_us || now_us - g_last_tick_us > 500000u) g_rate_n = 0;
    if (g_rate_n == RATE_TICKS + 1) {
        memmove(g_tick_us, g_tick_us + 1, sizeof g_tick_us[0] * RATE_TICKS);
        g_rate_n--;
    }
    g_tick_us[g_rate_n++] = now_us;
    if (g_rate_n >= 8) g_period_us = (double)(now_us - g_tick_us[0]) / (double)(g_rate_n - 1);
    g_last_tick_us = now_us;
    g_nticks++;
}

static void snaps_reset(void)
{
    g_nticks = 0;
    g_play = -1.0;
}

void render_reset(void)
{
    snaps_reset();
    lights_forget(); /* and the settings file is read again (it may have been changed) */
}

/* the playback clock at `now_us`: the snapshots to draw between and how far (0..256); 0 when there are too few */
static int playback(uint64_t now_us, const Snap **a, const Snap **b, int *t256)
{
    double newest, target, since;
    uint32_t i0;
    if (g_nticks < 2) return 0;
    newest = (double)(g_nticks - 1);
    /* where the clock should be: behind the newest tick, as if the ticks came evenly */
    since = now_us > g_last_tick_us ? (double)(now_us - g_last_tick_us) / g_period_us : 0.0;
    target = newest - PLAY_BEHIND + (since < 1.0 ? since : 1.0);
    if (target < 0) target = 0;
    if (g_play < 0 || g_play > newest || g_play < newest - (NB_HIST - 2) || fabs(g_play - target) > PLAY_RESET) {
        g_play = target;
        g_play_off = 0.0;
    } else if (now_us > g_play_us) {
        double dt = (double)(now_us - g_play_us), adjust;
        g_play_off += (target - g_play - g_play_off) * (dt < PLAY_FILTER_US ? dt / PLAY_FILTER_US : 1.0);
        adjust = g_play_off * PLAY_GAIN;
        if (adjust > PLAY_ADJUST) adjust = PLAY_ADJUST;
        if (adjust < -PLAY_ADJUST) adjust = -PLAY_ADJUST;
        g_play += dt / g_period_us * (1.0 + adjust);
        if (g_play > newest) g_play = newest;
    }
    g_play_us = now_us;
    i0 = (uint32_t)g_play;
    *a = &g_hist[i0 % NB_HIST];
    *b = i0 + 1 < g_nticks ? &g_hist[(i0 + 1) % NB_HIST] : *a;
    *t256 = (int)((g_play - (double)i0) * 256.0);
    return 1;
}

static int32_t lerp(int32_t a, int32_t b, int t) /* t 0..256 */
{
    if (a - b > TELEPORT || b - a > TELEPORT) return b;
    return a + (b - a) * t / 256;
}

void render_image(uint32_t *rgb, int w, int h, const uint32_t *pal, uint32_t pixels, int iw, int ih, int x, int y, int x0,
                  int y0, int x1, int y1)
{
    Target t = {rgb, w, h, x0, y0, x1, y1, pal, 0};
    static uint8_t img[64 * 64];
    int row, blk, stride = (iw + 7) / 8;
    if (iw <= 0 || ih <= 0 || iw > 64 || ih > 64) return;
    for (row = 0; row < ih; row++)
        for (blk = 0; blk < stride; blk++) {
            uint8_t block[8];
            const uint8_t *b = M + pixels + (uint32_t)((row * stride + blk) * 5);
            int i;
            decode_bits(b, b + 1, 1, block);
            for (i = 0; i < 8 && blk * 8 + i < iw; i++) img[row * iw + blk * 8 + i] = block[i];
        }
    blit(&t, img, iw, ih, x, y, 0, 0);
}

const uint8_t *render_sprite_pixels(int id, int *w, int *h)
{
    return sprite(id, w, h);
}

int render_sprite_ink(int id, int *x, int *y)
{
    int sw, sh, i, j, mx = 9999, my = 9999;
    const uint8_t *s = sprite(id, &sw, &sh);
    if (s == NULL) return 0;
    for (j = 0; j < sh; j++)
        for (i = 0; i < sw; i++)
            if (s[j * sw + i] != 255 && s[j * sw + i] != 0) {
                if (i < mx) mx = i;
                if (j < my) my = j;
            }
    if (mx == 9999) return 0;
    *x = mx;
    *y = my;
    return 1;
}

void render_sprite(uint32_t *rgb, int w, int h, const uint32_t *pal, int id, int x, int y, int x0, int y0, int x1, int y1)
{
    Target t = {rgb, w, h, x0, y0, x1, y1, pal, 0};
    int sw, sh;
    const uint8_t *s = sprite(id, &sw, &sh);
    if (s != NULL) blit(&t, s, sw, sh, x, y, 0, 0);
}

/* where a point of the world is in the frame (the game's sprite placement), from the view being drawn */
static void project(int32_t x, int32_t y, int32_t z, int *fx, int *fy)
{
    int32_t dx = x - g_vx, dy = y - g_vy;
    *fx = (dx - dy) >> 3;
    *fy = ((dx + dy) / 2 - z) >> 3;
}

/* the same with the fractions (the GPU draws between pixels) */
static void project_f(int32_t x, int32_t y, int32_t z, float *fx, float *fy)
{
    float dx = (float)(x - g_vx) + g_vfx, dy = (float)(y - g_vy) + g_vfy;
    *fx = (dx - dy) / 8.0f;
    *fy = ((dx + dy) / 2.0f - (float)z) / 8.0f;
}

/* ---- what shines at dusk and at night (the GPU's lighting): lamps in the tiles, found from their pixels in the
 * mission's palette, and the tiles set below ---- */
static const struct {
    uint8_t tile, glow, light, radius; /* radius 0: from the tile */
} k_tile_lights[] = {
    {33, RENDER_GLOW_NONE, RENDER_LIGHT_ON, 0},   {92, RENDER_GLOW_NONE, RENDER_LIGHT_ON, 0},     {95, RENDER_GLOW_NONE, RENDER_LIGHT_ON, 0},
    {119, RENDER_GLOW_FULL, RENDER_LIGHT_ON, 0},  {121, RENDER_GLOW_BRIGHT, RENDER_LIGHT_ON, 0},  {170, RENDER_GLOW_NONE, RENDER_LIGHT_ON, 0},
    {187, RENDER_GLOW_NONE, RENDER_LIGHT_NONE, 0}, {201, RENDER_GLOW_NONE, RENDER_LIGHT_ON, 130}, {207, RENDER_GLOW_FULL, RENDER_LIGHT_ON, 80},
    {226, RENDER_GLOW_NONE, RENDER_LIGHT_ON, 0},
};
/* a lamp (as FreeSynd's): a few very bright pixels, fewer on the ground, none on roads and slopes (their marks) */
#define LAMP_LUMINANCE 199    /* 0.78 of 255 */
#define LAMP_MIN_PIXELS 2
#define LAMP_MAX_PIXELS (TILE_W * TILE_H / 8)
#define LAMP_MAX_GROUND 48
#define LAMP_RADIUS_ON 80     /* a tile whose light is on: from its centre */
#define WINDOW_MAX_PIXELS (TILE_W * TILE_H / 3) /* more: a white wall, not windows */
#define WINDOW_LIGHT_R 56.0f
#define TILE_LIGHTS_FILE "tile_lights.ini"

/* the settings of each tile (render_tile_setting): the defaults above, then tile_lights.ini in the user's folder */
static RenderTileSetting g_tile_set[NB_TILES];
static int g_tile_set_loaded;

static const char *const k_glow_names[3] = {"none", "bright", "full"};
static const char *const k_light_names[3] = {"auto", "none", "on"};

static int name_index(const char *v, const char *const *names, int n)
{
    int k;
    for (k = 0; k < n; k++)
        if (strncmp(v, names[k], strlen(names[k])) == 0) return k;
    return 0;
}

static void tile_light_forget(int id);

/* the settings in a tile_lights.ini over those set before */
static void tile_settings_read(const char *path)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (f == NULL) return;
    /* tile=N glow=none|bright|full light=auto|none|on radius=R colour=auto|r,g,b */
    while (fgets(line, sizeof line, f) != NULL) {
        const char *v;
        RenderTileSetting t;
        int id, r, g, b;
        if (line[0] == '#' || sscanf(line, "tile=%d", &id) != 1 || id < 0 || id >= NB_TILES) continue;
        memset(&t, 0, sizeof t);
        t.set = 1;
        if ((v = strstr(line, "glow=")) != NULL) t.glow = name_index(v + 5, k_glow_names, 3);
        if ((v = strstr(line, "light=")) != NULL) t.light = name_index(v + 6, k_light_names, 3);
        if ((v = strstr(line, "radius=")) != NULL) t.radius = atoi(v + 7);
        if ((v = strstr(line, "colour=")) != NULL && sscanf(v + 7, "%d,%d,%d", &r, &g, &b) == 3)
            t.colour = 0xFF000000u | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255);
        g_tile_set[id] = t;
    }
    fclose(f);
}

/* the defaults, then the HD pack's (hd/tile_lights.ini: its tiles drawn otherwise, as the tower bases' pads without
 * their light), then the user's */
static void tile_settings_load(void)
{
    char path[1200];
    int k;
    g_tile_set_loaded = 1;
    memset(g_tile_set, 0, sizeof g_tile_set);
    for (k = 0; k < (int)(sizeof k_tile_lights / sizeof k_tile_lights[0]); k++) {
        RenderTileSetting *t = &g_tile_set[k_tile_lights[k].tile];
        t->glow = k_tile_lights[k].glow;
        t->light = k_tile_lights[k].light;
        t->radius = k_tile_lights[k].radius;
        t->set = 1;
    }
    if (g_hd) {
        snprintf(path, sizeof path, "%s/hd/%s", rt_user_dir, TILE_LIGHTS_FILE);
        tile_settings_read(path);
    }
    snprintf(path, sizeof path, "%s/%s", rt_user_dir, TILE_LIGHTS_FILE);
    tile_settings_read(path);
}

int render_tile_setting(int id, RenderTileSetting *out)
{
    if (id < 0 || id >= NB_TILES) return 0;
    if (!g_tile_set_loaded) tile_settings_load();
    *out = g_tile_set[id];
    return 1;
}

int render_set_tile_setting(int id, const RenderTileSetting *in)
{
    char path[1200];
    FILE *f;
    int k;
    if (id < 0 || id >= NB_TILES) return 0;
    if (!g_tile_set_loaded) tile_settings_load();
    g_tile_set[id] = *in;
    tile_light_forget(id); /* found again */
    /* all the tiles with settings other than none (the file is the whole list) */
    snprintf(path, sizeof path, "%s/%s", rt_user_dir, TILE_LIGHTS_FILE);
    f = fopen(path, "w");
    if (f == NULL) return 0;
    fprintf(f, "# How the tiles light up at dusk and at night (the light editor: Ctrl+L in a mission)\n"
               "#   glow   = none, bright (its brightest pixels shine), full (all of it)\n"
               "#   light  = auto (when the tile has a small bright spot: a lamp), none, on\n"
               "#   radius = of the light in pixels, 0: from the tile\n"
               "#   colour = of the light: auto (from the tile) or r,g,b\n");
    for (k = 0; k < NB_TILES; k++) {
        const RenderTileSetting *t = &g_tile_set[k];
        if (t->glow == RENDER_GLOW_NONE && t->light == RENDER_LIGHT_AUTO && t->radius == 0 && t->colour == 0) continue;
        fprintf(f, "tile=%d glow=%s light=%s radius=%d colour=", k, k_glow_names[t->glow % 3], k_light_names[t->light % 3],
                t->radius);
        if (t->colour)
            fprintf(f, "%u,%u,%u\n", (t->colour >> 16) & 255u, (t->colour >> 8) & 255u, t->colour & 255u);
        else
            fprintf(f, "auto\n");
    }
    fclose(f);
    return 1;
}
#define NB_LIGHTS 1024

typedef struct TileLight {
    uint8_t known, glow, light, spill; /* spill: windows' light (dimmer, cooler) */
    float ox, oy, radius, ry; /* the light's centre in the tile, its radii */
    uint32_t colour;
} TileLight;
static TileLight g_tile_light[NB_TILES];
static uint32_t g_light_pal;          /* the palette they were found with (a hash) */
static const uint32_t *g_light_palette;
static HostCityLight g_lights[NB_LIGHTS];
static int g_nlights;

static float g_car_dir[64][2];
#define NB_FRAME_LIGHTS 4096
#define NB_LAMP_HUES 3 /* a signal's lamps: red, amber, green */
static struct {
    uint16_t frame, known, lit, bright; /* lit: a few coloured lamps; bright: very bright pixels (a lit window) */
    float ox, oy;
    uint32_t colour;
    uint16_t lamps[NB_LAMP_HUES]; /* its strongly coloured pixels of each hue, their centre */
    float lx[NB_LAMP_HUES], ly[NB_LAMP_HUES];
    float cx, cy;                 /* the centre of all of it */
    uint32_t average;             /* its colour (brightened: the brightest of r, g, b 255) */
    uint8_t flipped;              /* its first image drawn mirrored */
} g_frame_light[NB_FRAME_LIGHTS];

static void lights_forget(void)
{
    memset(g_tile_light, 0, sizeof g_tile_light);
    memset(g_frame_light, 0, sizeof g_frame_light);
    memset(g_car_dir, 0, sizeof g_car_dir);
    g_tile_set_loaded = 0;
}

static void tile_light_forget(int id)
{
    g_tile_light[id].known = 0;
}

static int luminance(uint32_t c)
{
    return (int)(0.299 * ((c >> 16) & 0xFF) + 0.587 * ((c >> 8) & 0xFF) + 0.114 * (c & 0xFF));
}

static int max_lamp_pixels(int type)
{
    if (!g_have_types) return LAMP_MAX_PIXELS;
    if ((type >= 0x01 && type <= 0x04) || (type >= 0x06 && type <= 0x09) || type == 0x0B || type == 0x0E || type == 0x0F)
        return 0;
    return type == 0x05 ? LAMP_MAX_GROUND : LAMP_MAX_PIXELS;
}

static int road_type(int type)
{
    return (type >= 0x06 && type <= 0x09) || type == 0x0B || type == 0x0E || type == 0x0F;
}

static const TileLight *tile_light(int id)
{
    TileLight *l = &g_tile_light[id];
    if (!l->known) {
        const uint8_t *px = tile(id);
        int x, y, n = 0, opaque = 0, top = 0;
        double sx = 0, sy = 0, ox = 0, oy = 0, cr = 0, cg = 0, cb = 0, mx, tr = 0, tg = 0, tb = 0, tn = 0;
        RenderTileSetting set;
        memset(l, 0, sizeof *l);
        l->known = 1;
        render_tile_setting(id, &set);
        /* a road without a setting of its own: its white lights shine */
        l->glow = (uint8_t)(!set.set && g_have_types && road_type(g_types[id]) ? 3 : set.glow);
        l->light = (uint8_t)set.light;
        l->radius = (float)set.radius;
        if (px == NULL || g_light_palette == NULL) {
            l->light = RENDER_LIGHT_NONE;
            return l;
        }
        for (y = 0; y < TILE_H; y++)
            for (x = 0; x < TILE_W; x++) {
                uint8_t c = px[y * TILE_W + x];
                if (c == 255) continue;
                opaque++;
                ox += x;
                oy += y;
                if (luminance(g_light_palette[c]) > top) top = luminance(g_light_palette[c]);
                if (luminance(g_light_palette[c]) >= LAMP_LUMINANCE) {
                    uint32_t v = g_light_palette[c];
                    n++;
                    sx += x;
                    sy += y;
                    cr += (v >> 16) & 0xFF;
                    cg += (v >> 8) & 0xFF;
                    cb += v & 0xFF;
                }
            }
        if (l->light == RENDER_LIGHT_ON) {
            /* turned on: from the centre of the tile, the colour of its brightest pixels, an ellipse on the floor */
            for (y = 0; y < TILE_H; y++)
                for (x = 0; x < TILE_W; x++) {
                    uint8_t c = px[y * TILE_W + x];
                    uint32_t v;
                    if (c == 255 || luminance(g_light_palette[c]) < top * 9 / 10) continue;
                    v = g_light_palette[c];
                    tr += (v >> 16) & 0xFF;
                    tg += (v >> 8) & 0xFF;
                    tb += v & 0xFF;
                    tn++;
                }
            l->ox = opaque ? (float)(ox / opaque) : TILE_W / 2.0f;
            l->oy = opaque ? (float)(oy / opaque) : TILE_H / 2.0f;
            cr = tr;
            cg = tg;
            cb = tb;
            if (l->radius == 0) l->radius = LAMP_RADIUS_ON;
            l->ry = l->radius / 2;
        } else if (l->light == RENDER_LIGHT_AUTO && n >= LAMP_MIN_PIXELS &&
                   n <= max_lamp_pixels(g_have_types ? g_types[id] : 0)) {
            /* a lamp: its bright pixels' centre and colour, a circle of light as big as it is bright */
            l->light = RENDER_LIGHT_ON;
            l->ox = (float)(sx / n);
            l->oy = (float)(sy / n);
            if (l->radius == 0) {
                l->radius = (float)(24.0 + 6.0 * sqrt((double)n));
                if (l->radius < 28) l->radius = 28;
                if (l->radius > 90) l->radius = 90;
            }
            l->ry = l->radius;
        } else if (l->light == RENDER_LIGHT_AUTO && n > max_lamp_pixels(g_have_types ? g_types[id] : 0) &&
                   n <= WINDOW_MAX_PIXELS && g_have_types && g_types[id] != 0x05 && !road_type(g_types[id])) {
            /* rows of lit windows (a wall's bright pixels, more than a lamp's): a soft spill of their light round
             * them, a little below (on the ground in front) */
            l->light = RENDER_LIGHT_ON;
            l->ox = (float)(sx / n);
            l->oy = (float)(sy / n) + 6.0f;
            if (l->radius == 0) l->radius = WINDOW_LIGHT_R;
            l->ry = l->radius * 0.55f;
            l->spill = 1;
        } else {
            l->light = RENDER_LIGHT_NONE;
            return l;
        }
        mx = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
        l->colour = mx > 0 ? (uint32_t)(cr / mx * 255.0) << 16 | (uint32_t)(cg / mx * 255.0) << 8 | (uint32_t)(cb / mx * 255.0)
                           : 0xFFFFFFu;
        if (set.colour != 0) l->colour = set.colour & 0xFFFFFFu;
    }
    return l;
}

static int g_obj_glow; /* how the object being drawn glows */

/* a station platform's edge by the track (tiles 128, 129, one the other's mirror): its white edge lights and its
 * red lights shine; its signal lamp red, or green while a train stands at the platform (platform_lights) */
#define PLATFORM_TILE 128
#define PLATFORM_TILE_M 129
static int tile_glow(int image)
{
    if (image == PLATFORM_TILE || image == PLATFORM_TILE_M) return RENDER_GLOW_BRIGHT | RENDER_GLOW_SIGNAL;
    return image < IMAGE_SPRITES ? tile_light(image)->glow : g_obj_glow;
}

/* the train's carriages this frame (cars drawn with the train's sprites): where, and whether they move */
#define NB_TRAIN 32
static struct {
    int x, y, moving;
} g_train[NB_TRAIN];
static int g_ntrain;

static int train_sprite(int id)
{
    return id >= 1683 && id <= 1687;
}

/* tile id (PLATFORM_TILE or its mirror) at tx, ty, tz, its picture's top left fx, fy: its signal lamp (on its end, at
 * the left of 128, the right of 129) red, green while a carriage stands still within 2 tiles of it */
static void platform_lights(int id, int tx, int ty, int tz, float fx, float fy)
{
    int i, green = 0, next = id == PLATFORM_TILE ? render_map_tile(tx, ty + 1, tz) : render_map_tile(tx + 1, ty, tz);
    float lx = id == PLATFORM_TILE ? 5.0f : 59.0f, ly = 26.0f;
    /* (the lamp at the platform's end only: the next of its tiles covers this one's end) */
    if (next == PLATFORM_TILE || next == PLATFORM_TILE_M) return;
    for (i = 0; i < g_ntrain; i++)
        if (!g_train[i].moving && abs(g_train[i].x - (tx * 256 + 128)) < 640 && abs(g_train[i].y - (ty * 256 + 128)) < 640)
            green = 1;
    if (green) ly += 5.0f; /* the lower lamp */
    add_light(fx + lx, fy + ly, 5.0f, 4.0f, HOST_LIGHT_GLOW | (green ? 0x30FF60u : 0xFF2A18u)); /* the lamp, by day too */
    add_light(fx + lx, fy + ly + 6.0f, 44.0f, 26.0f, green ? 0x40FF70u : 0xFF3020u);     /* its light round it */
}

/* a light at fx, fy of the unzoomed picture */
static void add_light(float fx, float fy, float rx, float ry, uint32_t colour)
{
    HostCityLight *h;
    if (g_nlights == NB_LIGHTS) return;
    h = &g_lights[g_nlights++];
    h->x = zoom_x(fx);
    h->y = zoom_y(fy);
    h->rx = rx * (float)g_zoom;
    h->ry = ry * (float)g_zoom;
    h->colour = colour;
}

/* things that light up what is around them (as FreeSynd's): a car's headlights (ahead of it, the way it last
 * moved, and its red rear lights) */
#define LIST_CARS 1
#define LIST_STATICS 2
#define LIST_EFFECTS 4
/* how far a car reaches from its middle (a tile is 256; its frames reach some 340 ahead and behind, 45 pixels):
 * along the way it faces, and across */
#define CAR_LONG 352
#define CAR_WIDE 96
#define CAR_HEADING 0x1au /* (8 bits) the way it faces: 0 +y, 64 +x, 128 -y, 192 -x */
#define LYING_H 20    /* a lying person's frame is at most this high */
#define HEADLIGHT_COLOUR 0xFFF5D7u
#define HEADLIGHT_STRENGTH 3
#define WRECK_FIRST 1743 /* the sprites of a destroyed car */
#define WRECK_LAST 1744
#define REAR_LIGHT_COLOUR 0xFF281Eu
#define HEADLAMP_GLOW 0xB4AE9Cu      /* the lamps on the car: white (a lens lit, not a ball of light) */
#define TAIL_GLOW 0x5A0E08u          /* red, dim */
#define BRAKE_GLOW 0xB41C10u         /* red, bright */
#define CAR_LAMP_FRONT 205.0f        /* where they are on the car (world units from its middle) */
#define CAR_LAMP_REAR 190.0f
#define CAR_LAMP_SIDE 48.0f
#define CAR_LAMP_HIGH 36
static float g_car_speed[64];
static uint8_t g_car_dead[64]; /* seen burning or wrecked: no lights for the rest of the mission */
static uint32_t g_car_map;

#define SHOT_LIGHT_RX 13.0f          /* an impact's sparks: a small, local light */
#define SHOT_LIGHT_COLOUR 0xFFB860u  /* warm */

#define POLICE_FIRST 1707 /* the police car's sprites */
#define POLICE_LAST 1709
#define POLICE_RED 0xFF2010u
#define POLICE_BLUE 0x2040FFu

/* a frame with one of sprites lo..hi */
static int frame_has(int frame, int lo, int hi)
{
    uint32_t elements = rd32(P_ELEMENTS), e = rd16(rd32(P_FRAMES) + (uint32_t)frame * 8u);
    int g = 0;
    while (e != 0 && g++ < 64) {
        int id = rd16(elements + e * 10u) / 6;
        if (id >= lo && id <= hi) return 1;
        e = rd16(elements + e * 10u + 8u);
    }
    return 0;
}

/* a frame showing a wreck, or a car on fire or blowing up (no lights then) */
static int wreck_frame(int frame)
{
    uint32_t elements = rd32(P_ELEMENTS), e = rd16(rd32(P_FRAMES) + (uint32_t)frame * 8u);
    int g = 0;
    if (e == 0) return 0;
    for (;;) {
        int id = rd16(elements + e * 10u) / 6;
        if ((id >= WRECK_FIRST && id <= WRECK_LAST) || fire_sprite(id) || (id >= 765 && id <= 779) ||
            (id >= 800 && id <= 809) || id == 841 || id == 842) return 1; /* burnt, burning, blowing up */
        e = rd16(elements + e * 10u + 8u);
        if (e == 0 || ++g > 64) return 0;
    }
}

static void object_lights(int li, int i, int32_t x, int32_t y, int32_t z, int32_t vx, int32_t vy, int heading, int frame)
{
    float fx, fy;
    if (li == LIST_CARS && i < 64) { /* destroyed (once seen burning or wrecked: its later frames too): no lights */
        uint32_t map = rd32(P_MAP);
        if (map != g_car_map) { g_car_map = map; memset(g_car_dead, 0, sizeof g_car_dead); }
        if (wreck_frame(frame)) g_car_dead[i] = 1;
        if (g_car_dead[i]) return;
    }
    if (li == LIST_CARS && i < 64) {
        float d = sqrtf((float)vx * vx + (float)vy * vy);
        int k, braking;
        if (d > 2.0f) {
            g_car_dir[i][0] = (float)vx / d;
            g_car_dir[i][1] = (float)vy / d;
        }
        if (g_car_dir[i][0] == 0 && g_car_dir[i][1] == 0) { /* never seen moving: the way it faces */
            g_car_dir[i][0] = sinf((float)heading * 6.2832f / 256.0f);
            g_car_dir[i][1] = cosf((float)heading * 6.2832f / 256.0f);
        }
        if (frame_has(frame, POLICE_FIRST, POLICE_LAST)) {
            /* a police car: its roof's lights, red and blue in turn (each a light round the car and a glow on it) */
            int blue = (int)(host_time_us() / 350000u) & 1, s;
            project_f(x, y, z + 60, &fx, &fy);
            for (s = -1; s <= 1; s += 2) {
                int on = (s < 0) != blue;
                float lx = fx + (float)render_obj_dx + (float)s * 5.0f, ly = fy + (float)render_obj_dy - 6.0f;
                add_light(lx, ly + 10.0f, on ? 70.0f : 0.0f, on ? 45.0f : 0.0f, s < 0 ? POLICE_RED : POLICE_BLUE);
                if (on) add_light(lx, ly, 5.0f, 4.0f, HOST_LIGHT_GLOW | (s < 0 ? POLICE_RED : POLICE_BLUE));
            }
        }
        /* slowing down (its speed a tick under the speed it kept) or standing: braking */
        braking = d < 2.0f || d < g_car_speed[i] * 0.85f;
        g_car_speed[i] = g_car_speed[i] * 0.8f + d * 0.2f;
        /* the light it gives off (no lamps drawn): the headlights' beam ahead (circles growing), the red glow behind,
         * larger while it brakes */
        for (k = 0; k <= 3; k++) {
            float ahead = 240.0f + 200.0f * (float)k;
            project_f(x + (int32_t)(g_car_dir[i][0] * ahead), y + (int32_t)(g_car_dir[i][1] * ahead), z, &fx, &fy);
            int twice;
            for (twice = 0; twice < HEADLIGHT_STRENGTH; twice++) /* the lights add up: as bright as HEADLIGHT_STRENGTH */
                add_light(fx + (float)render_obj_dx, fy + (float)render_obj_dy, 18.0f + 14.0f * (float)k,
                          14.0f + 10.0f * (float)k, HEADLIGHT_COLOUR);
        }
        project_f(x - (int32_t)(g_car_dir[i][0] * 250.0f), y - (int32_t)(g_car_dir[i][1] * 250.0f), z, &fx, &fy);
        add_light(fx + (float)render_obj_dx, fy + (float)render_obj_dy, braking ? 34.0f : 22.0f, braking ? 26.0f : 17.0f,
                  REAR_LIGHT_COLOUR);
    }
}

/* the tiles drawn in the last frame, in order (unzoomed places) */
#define NB_DRAWN 12288
static struct {
    uint8_t id, tz;
    int16_t x, y;
    int16_t tx, ty; /* its place in the map */
} g_drawn[NB_DRAWN];
static int g_ndrawn;

/* a line of the frame (any slope), blended */
static void line(uint32_t *rgb, int w, int h, float x0, float y0, float x1, float y1, int cx0, int cy0, int cx1, int cy1,
                 uint32_t c)
{
    int n = (int)(fabsf(x1 - x0) > fabsf(y1 - y0) ? fabsf(x1 - x0) : fabsf(y1 - y0)) + 1, i;
    (void)h;
    for (i = 0; i <= n; i++) {
        int x = (int)floorf(x0 + (x1 - x0) * (float)i / (float)n + 0.5f), y = (int)floorf(y0 + (y1 - y0) * (float)i / (float)n + 0.5f);
        if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) rgb[y * w + x] = c;
    }
}

void render_floor_outline(uint32_t *rgb, int w, int h, int level, uint32_t colour, int x0, int y0, int x1, int y1,
                          int (*walkable)(int x, int y, int z))
{
    uint32_t map;
    int mx, my, mz, tx, ty;
    if (g_ra.map == 0 || level < 0) return;
    map = rd32(P_MAP);
    mx = (int)rd32(map);
    my = (int)rd32(map + 4u);
    mz = (int)rd32(map + 8u);
    if (mx <= 0 || my <= 0 || mx > 256 || my > 256 || level >= mz) return;
    for (ty = 0; ty < my; ty++)
        for (tx = 0; tx < mx; tx++) {
            static const int nb[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
            /* the corners of the floor of the cell, clockwise from x, y */
            float cx[4], cy[4];
            int k, z = level * 128;
            if (!walkable(tx, ty, level)) continue;
            for (k = 0; k < 4; k++) {
                int wx = (tx + (k == 1 || k == 2)) * 256, wy = (ty + (k >= 2)) * 256;
                project_f(wx, wy, z, &cx[k], &cy[k]);
                cx[k] = zoom_x(cx[k] + (float)render_obj_dx);
                cy[k] = zoom_y(cy[k] + (float)render_obj_dy);
            }
            if (cx[0] < x0 - 64 || cx[0] > x1 + 64 || cy[0] < y0 - 64 || cy[0] > y1 + 64) continue;
            /* an edge where one cannot walk on (on the level, nor just above or below it: stairs) */
            for (k = 0; k < 4; k++) {
                int nx = tx + nb[k][0], ny = ty + nb[k][1];
                if (walkable(nx, ny, level) || walkable(nx, ny, level - 1) || walkable(nx, ny, level + 1)) continue;
                line(rgb, w, h, cx[k], cy[k], cx[(k + 1) & 3], cy[(k + 1) & 3], x0, y0, x1, y1, colour);
            }
        }
}

int render_map_size(int *mx, int *my, int *mz)
{
    uint32_t map;
    if (g_ra.map == 0) return 0;
    map = rd32(P_MAP);
    *mx = (int)rd32(map);
    *my = (int)rd32(map + 4u);
    *mz = (int)rd32(map + 8u);
    return *mx > 0 && *my > 0 && *mz > 0 && *mx <= 256 && *my <= 256 && *mz <= 16;
}

int render_map_tile(int x, int y, int z)
{
    uint32_t map = rd32(P_MAP), col;
    int mx = (int)rd32(map), my = (int)rd32(map + 4u), mz = (int)rd32(map + 8u);
    if (x < 0 || y < 0 || z < 0 || x >= mx || y >= my || z >= mz) return -1;
    col = rd32(map + 12u + (uint32_t)(x + y * mx) * 4u);
    return M[col + (uint32_t)z];
}

/* the tiles drawn in the last frame: how many; tile i: its id, its place in the map, the middle of its top on the
 * frame (zoomed) */
int render_drawn_count(void)
{
    return g_ndrawn;
}

void render_drawn_tile(int i, int *id, int *tx, int *ty, int *tz, int *sx, int *sy)
{
    *id = g_drawn[i].id;
    *tx = g_drawn[i].tx;
    *ty = g_drawn[i].ty;
    *tz = g_drawn[i].tz;
    *sx = (int)floorf(zoom_x((float)g_drawn[i].x + 32.0f));
    *sy = (int)floorf(zoom_y((float)g_drawn[i].y + 16.0f));
}

/* the sprite drawn at the frame's point sx, sy (in front): its number; 0 none */
int render_sprite_at(int sx, int sy, int *id)
{
    float lx = g_zcx + ((float)sx - g_zcx) / (float)g_zoom, ly = g_zcy + ((float)sy - g_zcy) / (float)g_zoom;
    int i;
    for (i = g_nsdrawn - 1; i >= 0; i--) {
        int x = (int)floorf(lx) - g_sdrawn[i].x, y = (int)floorf(ly) - g_sdrawn[i].y;
        if (x < 0 || y < 0 || x >= g_sdrawn[i].w || y >= g_sdrawn[i].h || g_sdrawn[i].px == NULL) continue;
        if (g_sdrawn[i].flipped) x = g_sdrawn[i].w - 1 - x;
        if (g_sdrawn[i].px[y * g_sdrawn[i].w + x] == 255) continue;
        *id = g_sdrawn[i].id;
        return 1;
    }
    return 0;
}

/* the tile drawn at the frame's point sx, sy (in front): its id and its place in the map; 0 none */
int render_tile_place(int sx, int sy, int *id, int *tx, int *ty, int *tz)
{
    float lx = g_zcx + ((float)sx - g_zcx) / (float)g_zoom, ly = g_zcy + ((float)sy - g_zcy) / (float)g_zoom;
    int i;
    for (i = g_ndrawn - 1; i >= 0; i--) {
        int x = (int)floorf(lx) - g_drawn[i].x, y = (int)floorf(ly) - g_drawn[i].y;
        const uint8_t *px;
        if (x < 0 || y < 0 || x >= TILE_W || y >= TILE_H) continue;
        px = tile(g_drawn[i].id);
        if (px == NULL || px[y * TILE_W + x] == 255) continue;
        *id = g_drawn[i].id;
        *tx = g_drawn[i].tx;
        *ty = g_drawn[i].ty;
        *tz = g_drawn[i].tz;
        return 1;
    }
    return 0;
}

int render_tile_at(int sx, int sy, int *id, int *rx, int *ry, int *rw, int *rh)
{
    float lx = g_zcx + ((float)sx - g_zcx) / (float)g_zoom, ly = g_zcy + ((float)sy - g_zcy) / (float)g_zoom;
    int i;
    for (i = g_ndrawn - 1; i >= 0; i--) {
        int x = (int)floorf(lx) - g_drawn[i].x, y = (int)floorf(ly) - g_drawn[i].y;
        const uint8_t *px;
        if (x < 0 || y < 0 || x >= TILE_W || y >= TILE_H) continue;
        px = tile(g_drawn[i].id);
        if (px == NULL || px[y * TILE_W + x] == 255) continue;
        *id = g_drawn[i].id;
        *rx = (int)floorf(zoom_x((float)g_drawn[i].x));
        *ry = (int)floorf(zoom_y((float)g_drawn[i].y));
        *rw = (int)ceilf(TILE_W * (float)g_zoom);
        *rh = (int)ceilf(TILE_H * (float)g_zoom);
        return 1;
    }
    return 0;
}

/* a frame with a few bright, strongly coloured pixels (a traffic light, a signal): a small light of that colour,
 * found once for each frame (its centre from the frame's anchor) */

static int signal_pixel(uint32_t c)
{
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return mx >= 200 && mn * 100 <= mx * 45;
}

/* a signal pixel's hue: 0 red, 1 amber, 2 green, -1 another (blue) */
static int lamp_hue(uint32_t c)
{
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    if (r >= g && r >= b) return g * 2 < r ? 0 : 1;
    if (g >= r && g >= b) return 2;
    return -1;
}

/* r, g, b brightened: the brightest of them 255 */
static uint32_t brightest(double r, double g, double b)
{
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx <= 0) return 0xFFFFFFu;
    return (uint32_t)(r / mx * 255.0) << 16 | (uint32_t)(g / mx * 255.0) << 8 | (uint32_t)(b / mx * 255.0);
}

static int frame_slot(int frame)
{
    int slot = frame % NB_FRAME_LIGHTS;
    if (g_light_palette == NULL || frame <= 0) return -1;
    if (!g_frame_light[slot].known || g_frame_light[slot].frame != frame) {
        uint32_t frames = rd32(P_FRAMES), elements = rd32(P_ELEMENTS), e = rd16(frames + (uint32_t)frame * 8u);
        int guard = 0, n = 0, nb = 0, all = 0, k;
        double sx = 0, sy = 0, cr = 0, cg = 0, cb = 0, mx, lx[NB_LAMP_HUES] = {0}, ly[NB_LAMP_HUES] = {0};
        double ax = 0, ay = 0, ar = 0, ag = 0, ab = 0;
        memset(&g_frame_light[slot], 0, sizeof g_frame_light[slot]);
        g_frame_light[slot].flipped = rd16(elements + e * 10u + 6u) != 0;
        for (;;) {
            uint32_t ea = elements + e * 10u;
            int sw, sh, x, y, ox = (int16_t)rd16(ea + 2u), oy = (int16_t)rd16(ea + 4u), flip = rd16(ea + 6u) != 0;
            const uint8_t *px = sprite(rd16(ea) / 6, &sw, &sh);
            for (y = 0; px != NULL && y < sh; y++)
                for (x = 0; x < sw; x++) {
                    uint8_t c = px[y * sw + (flip ? sw - 1 - x : x)];
                    int hue;
                    if (c == 255) continue;
                    all++;
                    ax += ox + x;
                    ay += oy + y;
                    ar += (g_light_palette[c] >> 16) & 0xFF;
                    ag += (g_light_palette[c] >> 8) & 0xFF;
                    ab += g_light_palette[c] & 0xFF;
                    if (luminance(g_light_palette[c]) >= LAMP_LUMINANCE) nb++;
                    if (!signal_pixel(g_light_palette[c])) continue;
                    if ((hue = lamp_hue(g_light_palette[c])) >= 0) {
                        g_frame_light[slot].lamps[hue]++;
                        lx[hue] += ox + x;
                        ly[hue] += oy + y;
                    }
                    n++;
                    sx += ox + x;
                    sy += oy + y;
                    cr += (g_light_palette[c] >> 16) & 0xFF;
                    cg += (g_light_palette[c] >> 8) & 0xFF;
                    cb += g_light_palette[c] & 0xFF;
                }
            e = rd16(ea + 8u);
            if (e == 0 || ++guard > 64) break;
        }
        g_frame_light[slot].frame = (uint16_t)frame;
        g_frame_light[slot].known = 1;
        g_frame_light[slot].lit = n > 0 && n <= 20;
        g_frame_light[slot].bright = (uint16_t)nb;
        if (n > 0) {
            g_frame_light[slot].ox = (float)(sx / n);
            g_frame_light[slot].oy = (float)(sy / n);
            mx = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
            g_frame_light[slot].colour =
                (uint32_t)(cr / mx * 255.0) << 16 | (uint32_t)(cg / mx * 255.0) << 8 | (uint32_t)(cb / mx * 255.0);
        }
        for (k = 0; k < NB_LAMP_HUES; k++)
            if (g_frame_light[slot].lamps[k] > 0) {
                g_frame_light[slot].lx[k] = (float)(lx[k] / g_frame_light[slot].lamps[k]);
                g_frame_light[slot].ly[k] = (float)(ly[k] / g_frame_light[slot].lamps[k]);
            }
        if (all > 0) {
            g_frame_light[slot].cx = (float)(ax / all);
            g_frame_light[slot].cy = (float)(ay / all);
            g_frame_light[slot].average = brightest(ar, ag, ab);
        }
    }
    return slot;
}

/* the statics' kinds (their byte +0x19) */
#define STATIC_KIND 0x19u
/* a traffic light's lamps: each hue lit shines its colour on the road the side it is on faces (its light pushed
 * out from the middle of the light SEMAPHORE_THROW times as far as the lamps are) */
#define SEMAPHORE_RADIUS 72
#define SEMAPHORE_THROW 3.0f
#define LIGHT_TWICE 2 /* a lamp's and a billboard's lights drawn this many times over (a light adds 0.4 of its colour) */
static const uint32_t k_lamp_colours[NB_LAMP_HUES] = {0xFF2814u, 0xFFB414u, 0x28FF50u};
/* a billboard: its picture shines, and lights what is in front of it (it faces +y, or +x drawn mirrored) in its
 * colour, wider further out: BILLBOARD_STEPS lights from its foot out */
#define STATIC_BILLBOARD 0x1F
#define BILLBOARD_STEPS 5
#define BILLBOARD_STEP 170.0f  /* world units between them */
#define BILLBOARD_RADIUS 40.0f /* the first's (pixels), each further one BILLBOARD_GROW more */
#define BILLBOARD_GROW 18.0f
#define OBJECT_LIGHT_DY (-24) /* a window's or sign's light: above where it stands */
#define WINDOW_LIGHT 0xFFD796u
#define NEON_LIGHT 0xFF82DCu /* a neon sign's light when its picture is not known */
#define STATIC_NEON 0x0A
#define NEON_RADIUS 56.0f

/* a static's light (traffic lights, neon signs, lit windows); returns how it glows in the dark (HostCityQuad.glow) */
static int static_lights(uint32_t a, int frame, float ax, float ay)
{
    int kind = M[a + STATIC_KIND], slot;
    if (kind >= 0x05 && kind <= 0x08) { /* traffic lights: each lamp lit, its colour */
        int k, n;
        slot = frame_slot(frame);
        for (k = 0; slot >= 0 && k < NB_LAMP_HUES; k++) {
            float dx, dy;
            if (g_frame_light[slot].lamps[k] == 0) continue;
            dx = (g_frame_light[slot].lx[k] - g_frame_light[slot].cx) * SEMAPHORE_THROW;
            dy = (g_frame_light[slot].ly[k] - g_frame_light[slot].cy) * SEMAPHORE_THROW;
            for (n = 0; n < LIGHT_TWICE; n++)
                add_light(ax + g_frame_light[slot].cx + dx, ay + g_frame_light[slot].cy + dy + 4.0f, SEMAPHORE_RADIUS,
                          SEMAPHORE_RADIUS * 0.6f, k_lamp_colours[k]);
        }
        return RENDER_GLOW_BRIGHT | RENDER_GLOW_SIGNAL;
    }
    if (kind == STATIC_BILLBOARD) {
        int k, n;
        slot = frame_slot(frame);
        if (slot < 0) return RENDER_GLOW_FULL;
        for (k = 0; k < BILLBOARD_STEPS; k++) {
            /* the step in front of it on the screen: +y is down and left, +x down and right */
            float d = BILLBOARD_STEP * (float)k, r = BILLBOARD_RADIUS + BILLBOARD_GROW * (float)k;
            float dx = (g_frame_light[slot].flipped ? d : -d) / 8.0f, dy = d / 16.0f;
            for (n = 0; n < LIGHT_TWICE; n++)
                add_light(ax + dx, ay + dy + (k == 0 ? OBJECT_LIGHT_DY : 0.0f), r, r * 0.6f, g_frame_light[slot].average);
        }
        return RENDER_GLOW_FULL;
    }
    if (kind == STATIC_NEON) { /* a sign on a wall: it shines, and lights the wall and the ground in its colour */
        int n;
        float dx = 0.0f, dy = OBJECT_LIGHT_DY;
        uint32_t colour = NEON_LIGHT;
        slot = frame_slot(frame);
        if (slot >= 0) {
            dx = g_frame_light[slot].cx;
            dy = g_frame_light[slot].cy;
            colour = g_frame_light[slot].average;
        }
        for (n = 0; n < LIGHT_TWICE; n++) add_light(ax + dx, ay + dy, NEON_RADIUS, NEON_RADIUS * 0.8f, colour);
        return RENDER_GLOW_FULL;
    }
    if (kind >= 0x20 && kind <= 0x26) { /* a window that lights up: lit when its frame has very bright pixels */
        slot = frame_slot(frame);
        if (slot < 0 || g_frame_light[slot].bright == 0) return RENDER_GLOW_NONE;
        add_light(ax, ay + OBJECT_LIGHT_DY, 40, 40, WINDOW_LIGHT);
        return RENDER_GLOW_BRIGHT;
    }
    if (kind == 0x12 || kind == 0x13 || kind == 0x15) return RENDER_GLOW_BRIGHT; /* windows' glass */
    return RENDER_GLOW_NONE;
}

/* ---- the tiles' types (COL01.DAT) ---- */
void render_set_tile_types(const uint8_t types[256])
{
    memcpy(g_types, types, sizeof g_types);
    g_have_types = 1;
}

static int type_at(uint32_t cols, int mx, int my, int mz, int x, int y, int z)
{
    if (x < 0 || y < 0 || z < 0 || x >= mx || y >= my || z >= mz) return 0;
    return g_types[M[rd32(cols + (uint32_t)(x + y * mx) * 4u) + (uint32_t)z]];
}

/* ---- the sun's shadows: traced per pixel on the GPU from each pixel's height (HostCityQuad.z); here where the sun
 * is (it moves through the mission) and which tiles are solid (their face away from the sun darker) ---- */
#define SUN_PERIOD 2400.0 /* seconds the sun takes to go across and back (it moves as the mission goes on) */
int render_shadows = 1;
int render_mission_time;
static float g_sun = 0.3f; /* where the sun is: 0 towards world -x, 1 towards -y */
static uint64_t g_sun_start;
static uint32_t g_sun_map;

/* the sun at a time of the mission (now_us; 0: where it was): it swings slowly between the top left and the top
 * right of the screen and back */
static void sun_at(uint64_t now_us, uint32_t map)
{
    double t;
    if (now_us == 0) return;
    if (map != g_sun_map || g_sun_start == 0 || now_us < g_sun_start) {
        g_sun_map = map;
        g_sun_start = now_us;
    }
    t = fmod((double)(now_us - g_sun_start) / 1e6 / SUN_PERIOD + 0.15, 1.0);
    g_sun = (float)(t < 0.5 ? t * 2 : 2 - t * 2);
}

/* casts a shadow: not empty, not a mesh (fences, handrails), not the space over a train stop */
static int solid_type(int t)
{
    return t != 0x00 && t != 0x0C && t != 0x10;
}

/* a tile whose top diamond is (almost) all drawn: a floor, a road, a roof (a pylon's piece, a fence, a lamp: no) */
static int flat_tile(int id)
{
    static uint8_t known[NB_TILES], flat[NB_TILES];
    if (id < 0 || id >= NB_TILES) return 0;
    if (!known[id]) {
        const uint8_t *px = tile(id);
        int x, y, in = 0, on = 0;
        known[id] = 1;
        for (y = 0; px != NULL && y < 32; y++)
            for (x = 0; x < TILE_W; x++)
                if (abs(2 * x + 1 - 64) * 16 + abs(2 * y + 1 - 32) * 32 <= 32 * 32 - 64) {
                    in++;
                    on += px[y * TILE_W + x] != 255;
                }
        flat[id] = in > 0 && on * 10 >= in * 9;
    }
    return flat[id];
}

/* a tile's HostCityQuad.shade */
static uint32_t tile_shade(uint32_t cols, int mx, int my, int mz, int x, int y, int z, int id)
{
    uint32_t s = HOST_SHADE_TILE | (uint32_t)(g_sun * 255.0f) << 8 | (render_shadows ? HOST_SHADE_ON : 0) |
                 (uint32_t)(render_mission_time & 7) << 24;
    if (render_shadows && g_have_types && solid_type(type_at(cols, mx, my, mz, x, y, z))) s |= HOST_SHADE_SOLID;
    if (flat_tile(id)) s |= HOST_SHADE_FLAT;
    if (g_have_types && type_at(cols, mx, my, mz, x, y, z) == 0x0C) s |= HOST_SHADE_MESH;
    return s;
}

/* a tile drawn with its top left at fx, fy (unzoomed): its light */
static void tile_lights_at(int id, float fx, float fy)
{
    const TileLight *l = tile_light(id);
    HostCityLight *h;
    if (l->light != RENDER_LIGHT_ON || g_nlights == NB_LIGHTS) return;
    h = &g_lights[g_nlights++];
    h->x = zoom_x(fx + l->ox);
    h->y = zoom_y(fy + l->oy);
    h->rx = l->radius * (float)g_zoom;
    h->ry = l->ry * (float)g_zoom;
    h->colour = l->colour;
    if (l->spill) { /* windows: dimmer and a little cool */
        uint32_t c = l->colour;
        h->colour = (uint32_t)(((c >> 16) & 255) * 45 / 100) << 16 | (uint32_t)(((c >> 8) & 255) * 50 / 100) << 8 |
                    (uint32_t)((c & 255) * 58 / 100);
    }
}

void render_project(int32_t x, int32_t y, int32_t z, int *fx, int *fy)
{
    float ffx, ffy;
    project_f(x, y, z, &ffx, &ffy);
    *fx = (int)floorf(zoom_x(ffx + (float)render_obj_dx) + 0.5f);
    *fy = (int)floorf(zoom_y(ffy + (float)render_obj_dy) + 0.5f);
}

int render_gpu_changed;
int render_tile_dx = 96, render_tile_dy = 40; /* a tile's top left corner from its projected corner */
int render_obj_dx = 128, render_obj_dy = 56;  /* a frame's anchor from the projected position */
uint32_t (*render_hat_colour)(int ped);       /* the colour of an agent's hat (0: as drawn) */
int (*render_ped_look)(int ped, uint32_t look[3]); /* a pedestrian's hair, skin, clothes (0: not one) */
uint32_t (*render_agent_coat)(int ped);

typedef struct Obj {
    int key;      /* the diagonal row of tiles it stands in (tile x + tile y) */
    int z;        /* the level it stands on */
    int x, y, zw; /* where it is drawn (world units) */
    uint32_t a;   /* its record */
    int frame;    /* the frame it shows */
    int ped;      /* its number among the people, RENDER_CAR_ID(n) for the cars, -1 for others */
    uint8_t glow; /* how it glows in the dark (HostCityQuad.glow) */
} Obj;

/* back to front: the diagonal row of tiles, the level, then the exact depth (x + y) and height */
static int cmp_obj(const void *a, const void *b)
{
    const Obj *p = (const Obj *)a, *q = (const Obj *)b;
    if (p->key != q->key) return p->key - q->key;
    if (p->z != q->z) return p->z - q->z;
    if (p->x + p->y != q->x + q->y) return (p->x + p->y) - (q->x + q->y);
    return p->zw - q->zw;
}

/* the people's (and the cars') places in the frame last drawn */
#define NB_PLACES RENDER_CAR_ID(64)
static int16_t g_ped_fx[NB_PLACES], g_ped_fy[NB_PLACES];
static uint8_t g_ped_drawn[NB_PLACES];

void render_unproject(int sx, int sy, int32_t z, int32_t *x, int32_t *y)
{
    /* the unzoomed picture, then the projection backwards (project_f), the view of the last city drawn */
    float lx = g_zcx + ((float)sx - g_zcx) / (float)g_zoom, ly = g_zcy + ((float)sy - g_zcy) / (float)g_zoom;
    float a = 8.0f * (lx - (float)render_obj_dx), b = 2.0f * (8.0f * (ly - (float)render_obj_dy) + (float)z);
    *x = (int32_t)floorf((a + b) / 2.0f - g_vfx) + g_vx;
    *y = (int32_t)floorf((b - a) / 2.0f - g_vfy) + g_vy;
}

int render_ped_place(int ped, int *fx, int *fy)
{
    if (ped < 0 || ped >= NB_PLACES || !g_ped_drawn[ped]) return 0;
    *fx = (int)floorf(zoom_x((float)g_ped_fx[ped]) + 0.5f);
    *fy = (int)floorf(zoom_y((float)g_ped_fy[ped]) + 0.5f);
    return 1;
}

int render_ped_frame(uint32_t *rgb, int w, int h, const uint32_t *pal, int frame, int ped, int dx, int dy, int x0, int y0,
                     int x1, int y1)
{
    Target t = {rgb, w, h, x0, y0, x1, y1, pal, 0};
    uint32_t frames = rd32(P_FRAMES), elements = rd32(P_ELEMENTS), e;
    int guard = 0;
    if (ped < 0 || ped >= NB_PLACES || !g_ped_drawn[ped]) return 0;
    e = rd16(frames + (uint32_t)frame * 8u);
    for (;;) {
        uint32_t ea = elements + e * 10u;
        int sw, sh, id = rd16(ea) / 6;
        const uint8_t *s = sprite(id, &sw, &sh);
        /* its place zoomed, its size not */
        float fx = zoom_x((float)(g_ped_fx[ped] + dx + (int16_t)rd16(ea + 2u)));
        float fy = zoom_y((float)(g_ped_fy[ped] + dy + (int16_t)rd16(ea + 4u)));
        if (s != NULL) blit(&t, s, sw, sh, (int)floorf(fx + 0.5f), (int)floorf(fy + 0.5f), rd16(ea + 6u) != 0, 0);
        e = rd16(ea + 8u);
        if (e == 0 || ++guard > 64) break;
    }
    return 1;
}

static void draw_obj(const Target *t, const Obj *o)
{
    int fx, fy;
    float ffx, ffy;
    uint32_t hat = 0;
    project(o->x, o->y, o->zw, &fx, &fy);
    project_f(o->x, o->y, o->zw, &ffx, &ffy);
    if (o->ped >= 0) {
        g_ped_fx[o->ped] = (int16_t)(fx + render_obj_dx);
        g_ped_fy[o->ped] = (int16_t)(fy + render_obj_dy);
        g_ped_drawn[o->ped] = 1;
        if (render_hat_colour != NULL && o->ped < 256) hat = render_hat_colour(o->ped);
        /* a pedestrian: its own hair (the red heads' hair is the hats' colour: the hat's colour is the hair's), skin
         * and clothes */
        if (hat == 0 && render_ped_look != NULL && o->ped < 256 && render_ped_look(o->ped, g_obj_look))
            hat = g_obj_look[0];
        else if (hat != 0 && render_agent_coat != NULL && o->ped < 256) /* the player's agents: the coat chosen */
            g_obj_look[2] = render_agent_coat(o->ped);
    }
    g_obj_glow = o->glow;
    g_obj_shade = (uint32_t)(g_sun * 255.0f) << 8 | (render_shadows ? HOST_SHADE_ON : 0) |
                  (uint32_t)(render_mission_time & 7) << 24; /* the mission's time: every quad carries it */
    g_obj_z = (float)o->zw;
    g_obj_wx = o->x;
    g_obj_wy = o->y;
    g_obj_wz = o->zw;
    g_obj_person = o->ped >= 0 && o->ped < 256;
    draw_frame(t, o->frame, fx + render_obj_dx, fy + render_obj_dy, ffx + (float)render_obj_dx, ffy + (float)render_obj_dy,
               hat);
    g_obj_glow = 0;
    g_obj_look[0] = g_obj_look[1] = g_obj_look[2] = 0;
    g_obj_shade = 0;
}

void render_city(uint32_t *rgb, int w, int h, const uint32_t *pal, int x0, int y0, int x1, int y1, uint64_t now_us)
{
    static Obj objs[2048];
    uint32_t map, cols;
    if (g_ra.map == 0) return;
    g_fx_now = now_us; /* the effects' clock: when this frame is shown */
    if (g_dump_tiles != NULL && g_have_types) dump_tiles(pal);
    if (g_hd != g_hd_want) {
        /* HD textures on or off: the GPU's images made again */
        g_hd = g_hd_want;
        lights_forget(); /* the pack's light settings in or out */
        memset(g_defined, 0, sizeof g_defined);
        host_city_reset();
    }
    if (rgb == NULL && memcmp(g_image_pal, pal, sizeof g_image_pal) != 0) {
        /* another palette: the GPU's images are made again in its colours */
        static int resets;
        memcpy(g_image_pal, pal, sizeof g_image_pal);
        memset(g_defined, 0, sizeof g_defined);
        host_city_reset();
        if (++resets <= 20 || resets % 100 == 0) rt_log("city images made again: another palette (%d)", resets);
    }
    map = rd32(P_MAP);
    sun_at(now_us, map);
    cols = map + 12u;
    int mx = (int)rd32(map), my = (int)rd32(map + 4u), mz = (int)rd32(map + 8u), nobj = 0, li, s, k = 0, n = 0, t256;
    const Snap *sa = NULL, *sb = NULL;
    Target t = {rgb, w, h, x0, y0, x1, y1, pal, rgb == NULL};
    /* the part of the unzoomed picture that the zoom brings into x0..x1, y0..y1 */
    int lx0 = (int)floorf(g_zcx + ((float)x0 - g_zcx) / (float)g_zoom), lx1 = (int)ceilf(g_zcx + ((float)x1 - g_zcx) / (float)g_zoom);
    int ly0 = (int)floorf(g_zcy + ((float)y0 - g_zcy) / (float)g_zoom), ly1 = (int)ceilf(g_zcy + ((float)y1 - g_zcy) / (float)g_zoom);
    /* on the GPU with the sun's shadows the tiles round the view too (what casts the shadows that fall into it):
     * win32_gpu.c's SHADOW_MARGIN */
    int margin = t.gpu && render_shadows ? 160 : 0;
    if (mx <= 0 || my <= 0 || mz <= 0 || mx > 256 || my > 256 || mz > 16) return;
    g_nquads = 0;
    g_nlights = 0;
    g_cols = cols;
    g_mx = mx;
    g_my = my;
    g_mz = mz;
    g_ndrawn = 0;
    g_nsdrawn = 0;
    if (t.gpu) {
        /* a new palette: the lamps are found again */
        uint32_t hash = 2166136261u;
        int i;
        for (i = 0; i < 256; i++) hash = (hash ^ pal[i]) * 16777619u;
        if (hash != g_light_pal || g_light_palette != pal) {
            g_light_pal = hash;
            memset(g_tile_light, 0, sizeof g_tile_light);
        }
        g_light_palette = pal;
    }
    /* on the playback clock (now_us 0: as the game is now) */
    if (now_us != 0 && playback(now_us, &sa, &sb, &t256)) {
        g_vx = g_own_view ? g_own_vx : lerp(sa->vx, sb->vx, t256);
        g_vy = g_own_view ? g_own_vy : lerp(sa->vy, sb->vy, t256);
        g_vfx = g_own_view ? g_own_fx : 0.0f;
        g_vfy = g_own_view ? g_own_fy : 0.0f;
    } else {
        g_vfx = g_vfy = 0.0f;
        t256 = -1;
        read_view(&g_vx, &g_vy);
    }
    g_ntrain = 0;
    /* the things on the map, by tile column then height */
    for (li = 0; li < (int)(sizeof k_lists / sizeof k_lists[0]); li++) {
        const ObjList *l = &k_lists[li];
        uint32_t i;
        for (i = 0; i < l->count; i++, n++) {
            uint32_t a = LEVEL + l->offset + i * l->size;
            /* as it was at the snapshot drawn (the later of the two: what appears or goes, and the pose) */
            uint8_t loc = t256 >= 0 ? sb->loc[n] : M[a + 0xau];
            int x, y, z, frame = t256 >= 0 ? sb->frame[n] : rd16(a + 0x10u);
            if (nobj == 2048) break;
            /* the game's rule (its list of things to draw, 0x40d528): shown unless bit 0 is set */
            if (loc == 0 || (loc & 1u)) continue;
            if (t256 >= 0 && li != LIST_EFFECTS && sa->loc[n] != 0 && !(sa->loc[n] & 1u)) {
                x = lerp(sa->pos[n][0], sb->pos[n][0], t256);
                y = lerp(sa->pos[n][1], sb->pos[n][1], t256);
                z = lerp(sa->pos[n][2], sb->pos[n][2], t256);
            } else if (t256 >= 0) {
                /* an effect (a shot, a ricochet: its place in the list is used again and again), or a thing that
                 * was not on the map at the earlier snapshot: where it is, not sliding from where it was */
                x = sb->pos[n][0];
                y = sb->pos[n][1];
                z = sb->pos[n][2];
            } else {
                x = (int16_t)rd16(a + 4u);
                y = (int16_t)rd16(a + 6u);
                z = (int16_t)rd16(a + 8u);
            }
            if (x < 0 || y < 0 || x >= mx * 256 || y >= my * 256) continue;
            objs[nobj].glow = 0;
            if (t.gpu && li == LIST_STATICS) {
                float sfx, sfy;
                project_f(x, y, z, &sfx, &sfy);
                objs[nobj].glow = (uint8_t)static_lights(a, frame, sfx + (float)render_obj_dx, sfy + (float)render_obj_dy);
            }
            if (t.gpu && li == LIST_EFFECTS) {
                float sfx, sfy;
                project_f(x, y, z, &sfx, &sfy);
                add_light(sfx + (float)render_obj_dx, sfy + (float)render_obj_dy - 2.0f, SHOT_LIGHT_RX, SHOT_LIGHT_RX * 0.6f,
                          SHOT_LIGHT_COLOUR);
            }
            if (li == LIST_CARS && g_ntrain < NB_TRAIN) { /* a carriage of the train: where, moving or not */
                uint32_t fr = rd32(P_FRAMES) + (uint32_t)frame * 8u;
                if (train_sprite(rd16(rd32(P_ELEMENTS) + rd16(fr) * 10u) / 6)) {
                    g_train[g_ntrain].x = x;
                    g_train[g_ntrain].y = y;
                    g_train[g_ntrain].moving = t256 >= 0 && (sb->pos[n][0] != sa->pos[n][0] || sb->pos[n][1] != sa->pos[n][1]);
                    g_ntrain++;
                }
            }
            if (t.gpu && li == LIST_CARS)
                object_lights(li, (int)i, x, y, z, t256 >= 0 ? sb->pos[n][0] - sa->pos[n][0] : 0,
                              t256 >= 0 ? sb->pos[n][1] - sa->pos[n][1] : 0, M[a + CAR_HEADING], frame);
            /* a car is two tiles long and more: drawn with the row of tiles its end nearest the viewer is in (with
             * its middle's, the floor of the rows in front of it is drawn over its nose or its tail) */
            if (li == LIST_CARS) {
                int along_x = ((M[a + CAR_HEADING] + 32u) & 64u) != 0;
                objs[nobj].key = ((x + (along_x ? CAR_LONG : CAR_WIDE)) >> 8) + ((y + (along_x ? CAR_WIDE : CAR_LONG)) >> 8);
            } else {
                objs[nobj].key = (x >> 8) + (y >> 8);
            }
            objs[nobj].z = z >> 7;
            /* a car is about a level high: drawn after the tiles of the level above its own in its row too (a tall
             * tile beside it, a pipe or a wall, no longer covers it with its upper half); what it drives under (a
             * bridge, a walkway) is higher and still covers it */
            if (li == LIST_CARS) objs[nobj].z++;
            /* a person lying (a body, falling or dead: its frame low, wider than high; standing ones are 25 to 36
             * high, firing ones up to 49 wide) reaches into the cells beside it, and its ends below its cell's
             * lower edges, over the floor of the next row: drawn after every level of its own row (not under a
             * raised tile next to it) and after the next row's floor it lies on (not cut off along that floor's
             * edge), before what stands on that floor (nearer the viewer) */
            if (li == 0) {
                uint32_t fr = rd32(P_FRAMES) + (uint32_t)frame * 8u;
                if (M[fr + 3u] <= LYING_H && M[fr + 2u] > M[fr + 3u]) {
                    objs[nobj].key++;
                    objs[nobj].z = z >= 128 ? (z >> 7) - 1 : 0;
                }
            }
            objs[nobj].x = x;
            objs[nobj].y = y;
            objs[nobj].zw = z;
            objs[nobj].a = a;
            objs[nobj].frame = frame;
            objs[nobj].ped = li == 0 ? (int)i : li == LIST_CARS ? RENDER_CAR_ID(i) : -1;
            nobj++;
        }
    }
    qsort(objs, (size_t)nobj, sizeof objs[0], cmp_obj);
    memset(g_ped_drawn, 0, sizeof g_ped_drawn);
    /* back to front: diagonal rows of tiles; in each, level by level, the tiles of the level then what stands on it,
     * nearest last (an object is drawn with its diagonal, not its column: two agents side by side overlap as they
     * stand) */
    for (s = 0; s < mx + my - 1; s++) {
        int tx, tz;
        while (k < nobj && objs[k].key < s) k++;
        for (tz = 0; tz < mz; tz++) {
            for (tx = 0; tx < mx; tx++) {
                int ty = s - tx;
                uint32_t col;
                if (ty < 0 || ty >= my) continue;
                col = rd32(cols + (uint32_t)(tx + ty * mx) * 4u);
                {
                int tid = M[col + (uint32_t)tz];
                const uint8_t *tl = tile(tid);
                int fx, fy;
                if (tl != NULL) {
                    project(tx * 256, ty * 256, tz * 128, &fx, &fy);
                    if (fx + render_tile_dx + TILE_W > lx0 - 2 - margin && fx + render_tile_dx < lx1 + 2 + margin &&
                        fy + render_tile_dy + TILE_H > ly0 - 2 - margin && fy + render_tile_dy < ly1 + 2) {
                        float ffx, ffy;
                        project_f(tx * 256, ty * 256, tz * 128, &ffx, &ffy);
                        g_translucent = g_cut >= 0 && tz >= g_cut;
                        g_cut_above = g_translucent ? (tz - g_cut > 15 ? 15 : tz - g_cut) : 0;
                        g_put_tx = tx;
                        g_put_ty = ty;
                        g_tile_shade = t.gpu ? tile_shade(cols, mx, my, mz, tx, ty, tz, tid) : 0;
                        g_quad_z = (float)((tz + 1) * 128); /* the block's top */
                        put(&t, tid, tl, TILE_W, TILE_H, fx + render_tile_dx, fy + render_tile_dy,
                            ffx + (float)render_tile_dx, ffy + (float)render_tile_dy, 0, 0);
                        g_put_tx = -1;
                        g_tile_shade = 0;
                        g_translucent = 0;
                        if (t.gpu) tile_lights_at(tid, ffx + (float)render_tile_dx, ffy + (float)render_tile_dy);
                        if (t.gpu && (tid == PLATFORM_TILE || tid == PLATFORM_TILE_M))
                            platform_lights(tid, tx, ty, tz, ffx + (float)render_tile_dx, ffy + (float)render_tile_dy);
                        if (g_ndrawn < NB_DRAWN) {
                            g_drawn[g_ndrawn].id = (uint8_t)tid;
                            g_drawn[g_ndrawn].x = (int16_t)(fx + render_tile_dx);
                            g_drawn[g_ndrawn].y = (int16_t)(fy + render_tile_dy);
                            g_drawn[g_ndrawn].tx = (int16_t)tx;
                            g_drawn[g_ndrawn].ty = (int16_t)ty;
                            g_drawn[g_ndrawn].tz = (uint8_t)tz;
                            g_ndrawn++;
                        }
                    }
                }
                }
            }
            for (; k < nobj && objs[k].key == s && objs[k].z <= tz; k++) draw_obj(&t, &objs[k]);
        }
        for (; k < nobj && objs[k].key == s; k++) draw_obj(&t, &objs[k]);
    }
    for (; k < nobj; k++) draw_obj(&t, &objs[k]); /* a body lying in the last row (drawn with the row after it) */
    if (t.gpu) {
        /* a city that moved is a new frame (the frame's pixels may not have changed) */
        static uint32_t prev_hash;
        uint32_t hash = 2166136261u;
        const uint8_t *b = (const uint8_t *)g_quads;
        size_t i, n = sizeof *g_quads * (size_t)g_nquads;
        for (i = 0; i < n; i++) hash = (hash ^ b[i]) * 16777619u;
        render_gpu_changed = hash != prev_hash;
        prev_hash = hash;
        if (g_hd) fx_draw();
        host_city_frame(g_quads, g_nquads, x0, y0, x1, y1, (float)g_zoom, g_lights, g_nlights);
    }
}
