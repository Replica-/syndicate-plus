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

/* the images the GPU has (host_city_image): the tiles 0-255, the sprites from 256 */
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
#define SEE_THROUGH_ALPHA 60 /* of 255, as FreeSynd's */
static uint32_t g_shade; /* the shadow at the corners of the tile being put (HostCityQuad.shade) */

static void emit(int image, const uint8_t *px, int w, int h, float x, float y, int flipped, uint32_t hat, int glow)
{
    HostCityQuad *q;
    if (!g_defined[image]) {
        host_city_image(image, w, h, px);
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
    q->shade = image < IMAGE_SPRITES ? g_shade : 0;
    q->glow = (uint8_t)(glow | (g_translucent ? 0x80 : 0));
    q->hat = hat; /* its top byte: the rows of the image with the hat */
}

#define HAT_INDEX 6 /* the hats' colour in the agents' sprites */

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
            put_pixel(t, x, y, c == HAT_INDEX && sy < (int)(hat >> 24) ? hat & 0xFFFFFFu : t->pal[c]);
        }
    }
}

/* a tile or a sprite (image number): on the GPU at its exact place, else as blit (both zoomed) */
static int tile_glow(int image);

static void put(const Target *t, int image, const uint8_t *src, int sw, int sh, int dx, int dy, float fx, float fy,
                int flipped, uint32_t hat)
{
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
            put_pixel(t, tx, ty, c == HAT_INDEX && y < (int)(hat >> 24) ? hat & 0xFFFFFFu : t->pal[c]);
        }
    }
}

/* `hat`: the colour of the hat: colour 6 of the sprites that are heads, whose colour 6 is all in their top
 * HAT_ROWS rows (what the agents hold uses colour 6 too, anywhere in it) */
#define HAT_ROWS 5

/* the agents' heads (the eight ways they face, in each of their sets): all their colour 6 is the hat, even where it
 * goes lower than HAT_ROWS (a head bowed towards the camera) */
static int is_head(int id)
{
    return (id >= 41 && id <= 56) || (id >= 1121 && id <= 1128);
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
static void draw_frame(const Target *t, int frame, int ax, int ay, float fax, float fay, uint32_t hat)
{
    uint32_t frames = rd32(P_FRAMES), elements = rd32(P_ELEMENTS), f = frames + (uint32_t)frame * 8u;
    uint32_t e = rd16(f);
    int guard = 0;
    for (;;) {
        uint32_t ea = elements + e * 10u;
        int sw, sh, id = rd16(ea) / 6, ox = (int16_t)rd16(ea + 2u), oy = (int16_t)rd16(ea + 4u);
        const uint8_t *s = sprite(id, &sw, &sh);
        if (s != NULL) {
            int rows = hat != 0 ? hat_rows(id, s, sw, sh) : 0;
            put(t, IMAGE_SPRITES + id, s, sw, sh, ax + ox, ay + oy, fax + (float)ox, fay + (float)oy, rd16(ea + 6u) != 0,
                rows > 0 ? (hat & 0xFFFFFFu) | (uint32_t)rows << 24 : 0);
        }
        e = rd16(ea + 8u);
        if (e == 0 || ++guard > 64) break;
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

static void tile_settings_load(void)
{
    char path[1200], line[256];
    FILE *f;
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
    snprintf(path, sizeof path, "%s/%s", rt_user_dir, TILE_LIGHTS_FILE);
    f = fopen(path, "r");
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
    uint8_t known, glow, light;
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
static struct {
    uint16_t frame, known, lit, bright; /* lit: a few coloured lamps; bright: very bright pixels (a lit window) */
    float ox, oy;
    uint32_t colour;
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

static int tile_glow(int image)
{
    return image < IMAGE_SPRITES ? tile_light(image)->glow : g_obj_glow;
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
#define CAR_REACH 160 /* how far a car reaches from its middle towards the viewer (a tile is 256) */
#define HEADLIGHT_COLOUR 0xFFF5D7u
#define REAR_LIGHT_COLOUR 0xFF281Eu

static void object_lights(int li, int i, int32_t x, int32_t y, int32_t z, int32_t vx, int32_t vy)
{
    float fx, fy;
    if (li == LIST_CARS && i < 64) {
        float d = sqrtf((float)vx * vx + (float)vy * vy);
        int k;
        if (d > 2.0f) {
            g_car_dir[i][0] = (float)vx / d;
            g_car_dir[i][1] = (float)vy / d;
        }
        if (g_car_dir[i][0] == 0 && g_car_dir[i][1] == 0) return;
        /* the cone of the headlights (circles growing ahead), their lamps, the rear lights */
        for (k = 0; k <= 3; k++) {
            float ahead = 160.0f + 180.0f * (float)k;
            project_f(x + (int32_t)(g_car_dir[i][0] * ahead), y + (int32_t)(g_car_dir[i][1] * ahead), z, &fx, &fy);
            add_light(fx + (float)render_obj_dx, fy + (float)render_obj_dy, k == 0 ? 14.0f : 14.0f + 12.0f * k,
                      k == 0 ? 14.0f : 10.0f + 8.0f * k, HEADLIGHT_COLOUR);
        }
        project_f(x - (int32_t)(g_car_dir[i][0] * 160.0f), y - (int32_t)(g_car_dir[i][1] * 160.0f), z, &fx, &fy);
        add_light(fx + (float)render_obj_dx, fy + (float)render_obj_dy, 16, 16, REAR_LIGHT_COLOUR);
    }
}

/* the tiles drawn in the last frame, in order (unzoomed places) */
#define NB_DRAWN 12288
static struct {
    uint8_t id;
    int16_t x, y;
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

static int frame_slot(int frame)
{
    int slot = frame % NB_FRAME_LIGHTS;
    if (g_light_palette == NULL || frame <= 0) return -1;
    if (!g_frame_light[slot].known || g_frame_light[slot].frame != frame) {
        uint32_t frames = rd32(P_FRAMES), elements = rd32(P_ELEMENTS), e = rd16(frames + (uint32_t)frame * 8u);
        int guard = 0, n = 0, nb = 0;
        double sx = 0, sy = 0, cr = 0, cg = 0, cb = 0, mx;
        for (;;) {
            uint32_t ea = elements + e * 10u;
            int sw, sh, x, y, ox = (int16_t)rd16(ea + 2u), oy = (int16_t)rd16(ea + 4u), flip = rd16(ea + 6u) != 0;
            const uint8_t *px = sprite(rd16(ea) / 6, &sw, &sh);
            for (y = 0; px != NULL && y < sh; y++)
                for (x = 0; x < sw; x++) {
                    uint8_t c = px[y * sw + (flip ? sw - 1 - x : x)];
                    if (c != 255 && luminance(g_light_palette[c]) >= LAMP_LUMINANCE) nb++;
                    if (c == 255 || !signal_pixel(g_light_palette[c])) continue;
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
    }
    return slot;
}

/* the statics' kinds (their byte +0x19) */
#define STATIC_KIND 0x19u
#define SEMAPHORE_RADIUS 56
#define OBJECT_LIGHT_DY (-24) /* a window's or sign's light: above where it stands */
#define WINDOW_LIGHT 0xFFD796u
#define NEON_LIGHT 0xFF82DCu

/* a static's light (traffic lights, neon signs, lit windows); returns how it glows in the dark (HostCityQuad.glow) */
static int static_lights(uint32_t a, int frame, float ax, float ay)
{
    int kind = M[a + STATIC_KIND], slot;
    if (kind >= 0x05 && kind <= 0x08) { /* traffic lights: the colour of the lamp lit */
        slot = frame_slot(frame);
        if (slot >= 0 && g_frame_light[slot].lit)
            add_light(ax + g_frame_light[slot].ox, ay + g_frame_light[slot].oy, SEMAPHORE_RADIUS, SEMAPHORE_RADIUS,
                      g_frame_light[slot].colour);
        return RENDER_GLOW_BRIGHT;
    }
    if (kind == 0x0A) {
        add_light(ax, ay + OBJECT_LIGHT_DY, 56, 56, NEON_LIGHT);
        return RENDER_GLOW_NONE;
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

/* ---- the sun's shadows: the sun high at the top left of the screen (from world -x, 45 degrees up), as FreeSynd's:
 * a surface seen from above (a floor, a road, a slope, a roof) with a solid tile above it towards the sun is in its
 * shadow, the less the further away that tile is (up to SHADOW_LENGTH tiles); the shade of the 4 cells around
 * each corner of a tile's top is blended across it (on the GPU), so the shadows fade pixel by pixel ---- */
#define SHADOW_LENGTH 8

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

/* casts a shadow: not empty, not a mesh (fences, handrails), not the space over a train stop */
static int solid_type(int t)
{
    return t != 0x00 && t != 0x0C && t != 0x10;
}

/* is seen from above: the floors, roads, slopes and roofs */
static int surface_type(int t)
{
    return (t >= 0x01 && t <= 0x09) || t == 0x0B || t == 0x0D || t == 0x0E || t == 0x0F;
}

/* how deep in shadow the surface of the cell x, y, z is: 0..255 */
static int cell_shade(uint32_t cols, int mx, int my, int mz, int x, int y, int z)
{
    int d;
    if (!surface_type(type_at(cols, mx, my, mz, x, y, z)) || solid_type(type_at(cols, mx, my, mz, x, y, z + 1))) return 0;
    for (d = 1; d <= SHADOW_LENGTH && z + d < mz; d++)
        if (solid_type(type_at(cols, mx, my, mz, x - d, y, z + d))) return 255 * (SHADOW_LENGTH + 1 - d) / SHADOW_LENGTH;
    return 0;
}

/* the shade at the 4 corners of the tile's top (HostCityQuad.shade): each the mean of the cells around it, those
 * with a surface at this level */
static uint32_t sun_shade(uint32_t cols, int mx, int my, int mz, int tx, int ty, int tz)
{
    static const int corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    uint32_t out = 0;
    int k;
    if (!g_have_types || !surface_type(type_at(cols, mx, my, mz, tx, ty, tz))) return 0;
    for (k = 0; k < 4; k++) {
        int dx, dy, sum = 0, n = 0;
        for (dy = -1; dy <= 0; dy++)
            for (dx = -1; dx <= 0; dx++) {
                int x = tx + corner[k][0] + dx, y = ty + corner[k][1] + dy;
                if (!surface_type(type_at(cols, mx, my, mz, x, y, tz))) continue;
                sum += cell_shade(cols, mx, my, mz, x, y, tz);
                n++;
            }
        out |= (uint32_t)(n ? sum / n : 0) << (8 * k);
    }
    return out;
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

typedef struct Obj {
    int key;      /* the diagonal row of tiles it stands in (tile x + tile y) */
    int z;        /* the level it stands on */
    int x, y, zw; /* where it is drawn (world units) */
    uint32_t a;   /* its record */
    int frame;    /* the frame it shows */
    int ped;      /* its number among the people, -1 for others */
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

/* the people's places in the frame last drawn */
static int16_t g_ped_fx[256], g_ped_fy[256];
static uint8_t g_ped_drawn[256];

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
    if (ped < 0 || ped >= 256 || !g_ped_drawn[ped]) return 0;
    *fx = (int)floorf(zoom_x((float)g_ped_fx[ped]) + 0.5f);
    *fy = (int)floorf(zoom_y((float)g_ped_fy[ped]) + 0.5f);
    return 1;
}

int render_ped_frame(uint32_t *rgb, int w, int h, const uint32_t *pal, int frame, int ped, int dy, int x0, int y0, int x1,
                     int y1)
{
    Target t = {rgb, w, h, x0, y0, x1, y1, pal, 0};
    uint32_t frames = rd32(P_FRAMES), elements = rd32(P_ELEMENTS), e;
    int guard = 0;
    if (ped < 0 || ped >= 256 || !g_ped_drawn[ped]) return 0;
    e = rd16(frames + (uint32_t)frame * 8u);
    for (;;) {
        uint32_t ea = elements + e * 10u;
        int sw, sh, id = rd16(ea) / 6;
        const uint8_t *s = sprite(id, &sw, &sh);
        /* its place zoomed, its size not */
        float fx = zoom_x((float)(g_ped_fx[ped] + (int16_t)rd16(ea + 2u)));
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
        if (render_hat_colour != NULL) hat = render_hat_colour(o->ped);
    }
    g_obj_glow = o->glow;
    draw_frame(t, o->frame, fx + render_obj_dx, fy + render_obj_dy, ffx + (float)render_obj_dx, ffy + (float)render_obj_dy,
               hat);
    g_obj_glow = 0;
}

void render_city(uint32_t *rgb, int w, int h, const uint32_t *pal, int x0, int y0, int x1, int y1, uint64_t now_us)
{
    static Obj objs[2048];
    uint32_t map, cols;
    if (g_ra.map == 0) return;
    map = rd32(P_MAP);
    cols = map + 12u;
    int mx = (int)rd32(map), my = (int)rd32(map + 4u), mz = (int)rd32(map + 8u), nobj = 0, li, s, k = 0, n = 0, t256;
    const Snap *sa = NULL, *sb = NULL;
    Target t = {rgb, w, h, x0, y0, x1, y1, pal, rgb == NULL};
    /* the part of the unzoomed picture that the zoom brings into x0..x1, y0..y1 */
    int lx0 = (int)floorf(g_zcx + ((float)x0 - g_zcx) / (float)g_zoom), lx1 = (int)ceilf(g_zcx + ((float)x1 - g_zcx) / (float)g_zoom);
    int ly0 = (int)floorf(g_zcy + ((float)y0 - g_zcy) / (float)g_zoom), ly1 = (int)ceilf(g_zcy + ((float)y1 - g_zcy) / (float)g_zoom);
    if (mx <= 0 || my <= 0 || mz <= 0 || mx > 256 || my > 256 || mz > 16) return;
    g_nquads = 0;
    g_nlights = 0;
    g_ndrawn = 0;
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
            if (t.gpu && li == LIST_CARS)
                object_lights(li, (int)i, x, y, z, t256 >= 0 ? sb->pos[n][0] - sa->pos[n][0] : 0,
                              t256 >= 0 ? sb->pos[n][1] - sa->pos[n][1] : 0);
            /* a car is longer than a tile: drawn with the row of tiles its front end is in (with its middle's,
             * the floor of the row in front of it is drawn over its front) */
            objs[nobj].key = li == LIST_CARS ? ((x + CAR_REACH) >> 8) + ((y + CAR_REACH) >> 8) : (x >> 8) + (y >> 8);
            objs[nobj].z = z >> 7;
            objs[nobj].x = x;
            objs[nobj].y = y;
            objs[nobj].zw = z;
            objs[nobj].a = a;
            objs[nobj].frame = frame;
            objs[nobj].ped = li == 0 ? (int)i : -1;
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
                    if (fx + render_tile_dx + TILE_W > lx0 - 2 && fx + render_tile_dx < lx1 + 2 &&
                        fy + render_tile_dy + TILE_H > ly0 - 2 && fy + render_tile_dy < ly1 + 2) {
                        float ffx, ffy;
                        project_f(tx * 256, ty * 256, tz * 128, &ffx, &ffy);
                        g_shade = t.gpu ? sun_shade(cols, mx, my, mz, tx, ty, tz) : 0;
                        g_translucent = g_cut >= 0 && tz >= g_cut;
                        put(&t, tid, tl, TILE_W, TILE_H, fx + render_tile_dx, fy + render_tile_dy,
                            ffx + (float)render_tile_dx, ffy + (float)render_tile_dy, 0, 0);
                        g_shade = 0;
                        g_translucent = 0;
                        if (t.gpu) tile_lights_at(tid, ffx + (float)render_tile_dx, ffy + (float)render_tile_dy);
                        if (g_ndrawn < NB_DRAWN) {
                            g_drawn[g_ndrawn].id = (uint8_t)tid;
                            g_drawn[g_ndrawn].x = (int16_t)(fx + render_tile_dx);
                            g_drawn[g_ndrawn].y = (int16_t)(fy + render_tile_dy);
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
    if (t.gpu) {
        /* a city that moved is a new frame (the frame's pixels may not have changed) */
        static uint32_t prev_hash;
        uint32_t hash = 2166136261u;
        const uint8_t *b = (const uint8_t *)g_quads;
        size_t i, n = sizeof *g_quads * (size_t)g_nquads;
        for (i = 0; i < n; i++) hash = (hash ^ b[i]) * 16777619u;
        render_gpu_changed = hash != prev_hash;
        prev_hash = hash;
        host_city_frame(g_quads, g_nquads, pal, x0, y0, x1, y1, (float)g_zoom, g_lights, g_nlights);
    }
}
