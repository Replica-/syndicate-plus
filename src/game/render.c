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
    uint8_t on_map[3]; /* values of the byte at +0xa that mean "on the map" (0: none) */
} ObjList;

/* people, cars, objects, weapons, effects (FreeSynd's leveldata.h: LevelDataAll) */
static const ObjList k_lists[] = {
    {0x8008u, 0x5cu, 256, {4, 0, 0}},
    {0xdc08u, 0x2au, 64, {4, 0x0c, 0}},
    {0xe688u, 0x1eu, 400, {4, 6, 7}},
    {0x11568u, 0x24u, 512, {4, 0, 0}},
    {0x15d68u, 0x1eu, 256, {4, 0, 0}},
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

static const uint8_t *tile(int id)
{
    uint32_t data = rd32(P_TILES);
    int sx, sy, row, blk;
    if (data != g_tiles_at) {
        memset(g_tile_seen, 0, sizeof g_tile_seen);
        g_tiles_at = data;
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
} Target;

#define HAT_INDEX 6 /* the hats' colour in the agents' sprites */

/* `hat`: a colour for the pixels of colour HAT_INDEX (0: as they are) */
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
            t->rgb[ty * t->w + tx] = hat != 0 && c == HAT_INDEX ? hat : t->pal[c];
        }
    }
}

static void draw_frame(const Target *t, int frame, int ax, int ay, uint32_t hat)
{
    uint32_t frames = rd32(P_FRAMES), elements = rd32(P_ELEMENTS), f = frames + (uint32_t)frame * 8u;
    uint32_t e = rd16(f);
    int guard = 0;
    for (;;) {
        uint32_t ea = elements + e * 10u;
        int sw, sh;
        const uint8_t *s = sprite(rd16(ea) / 6, &sw, &sh);
        if (s != NULL)
            blit(t, s, sw, sh, ax + (int16_t)rd16(ea + 2u), ay + (int16_t)rd16(ea + 4u), rd16(ea + 6u) != 0, hat);
        e = rd16(ea + 8u);
        if (e == 0 || ++guard > 64) break;
    }
}

/* ---- where things are: two snapshots, one a tick, to draw between them ---- */
#define NB_OBJS (256 + 64 + 400 + 512 + 256)
#define TELEPORT 512 /* a move of more than 2 tiles in one tick is not drawn in between */

typedef struct Snap {
    uint64_t us;
    int32_t vx, vy;              /* the view, in world units */
    int16_t pos[NB_OBJS][3];
} Snap;

static Snap g_snap[2];           /* [0] the tick before, [1] the last */
static int g_nsnaps;
static int32_t g_vx, g_vy;       /* the view of the frame being drawn */

static void read_view(int32_t *vx, int32_t *vy)
{
    *vx = (int32_t)(int16_t)rd16(VIEW) * 128;
    *vy = (int32_t)(int16_t)rd16(VIEW + 2u) * 128;
}

void render_tick(uint64_t now_us)
{
    int li, n = 0;
    g_snap[0] = g_snap[1];
    g_snap[1].us = now_us;
    read_view(&g_snap[1].vx, &g_snap[1].vy);
    for (li = 0; li < (int)(sizeof k_lists / sizeof k_lists[0]); li++) {
        const ObjList *l = &k_lists[li];
        uint32_t i;
        for (i = 0; i < l->count; i++, n++) {
            uint32_t a = LEVEL + l->offset + i * l->size;
            g_snap[1].pos[n][0] = (int16_t)rd16(a + 4u);
            g_snap[1].pos[n][1] = (int16_t)rd16(a + 6u);
            g_snap[1].pos[n][2] = (int16_t)rd16(a + 8u);
        }
    }
    if (g_nsnaps < 2) g_nsnaps++;
}

static void snaps_reset(void)
{
    g_nsnaps = 0;
}

void render_reset(void)
{
    snaps_reset();
}

static int32_t lerp(int32_t a, int32_t b, int t) /* t 0..256 */
{
    if (a - b > TELEPORT || b - a > TELEPORT) return b;
    return a + (b - a) * t / 256;
}

/* where a point of the world is in the frame (the game's sprite placement), from the view being drawn */
static void project(int32_t x, int32_t y, int32_t z, int *fx, int *fy)
{
    int32_t dx = x - g_vx, dy = y - g_vy;
    *fx = (dx - dy) >> 3;
    *fy = ((dx + dy) / 2 - z) >> 3;
}

void render_project(int32_t x, int32_t y, int32_t z, int *fx, int *fy)
{
    project(x, y, z, fx, fy);
    *fx += render_obj_dx;
    *fy += render_obj_dy;
}

int render_tile_dx = 96, render_tile_dy = 40; /* a tile's top left corner from its projected corner */
int render_obj_dx = 128, render_obj_dy = 56;  /* a frame's anchor from the projected position */
uint32_t (*render_hat_colour)(int ped);       /* the colour of an agent's hat (0: as drawn) */

typedef struct Obj {
    int key;      /* tile column (x + y * max x) */
    int z;        /* the level it stands on */
    int x, y, zw; /* where it is drawn (world units) */
    uint32_t a;   /* its record */
    int ped;      /* its number among the people, -1 for others */
} Obj;

static int cmp_obj(const void *a, const void *b)
{
    const Obj *p = (const Obj *)a, *q = (const Obj *)b;
    return p->key != q->key ? p->key - q->key : p->z - q->z;
}

/* the people's places in the frame last drawn */
static int16_t g_ped_fx[256], g_ped_fy[256];
static uint8_t g_ped_drawn[256];

int render_ped_place(int ped, int *fx, int *fy)
{
    if (ped < 0 || ped >= 256 || !g_ped_drawn[ped]) return 0;
    *fx = g_ped_fx[ped];
    *fy = g_ped_fy[ped];
    return 1;
}

static void draw_obj(const Target *t, const Obj *o)
{
    int fx, fy;
    uint32_t hat = 0;
    project(o->x, o->y, o->zw, &fx, &fy);
    if (o->ped >= 0) {
        g_ped_fx[o->ped] = (int16_t)(fx + render_obj_dx);
        g_ped_fy[o->ped] = (int16_t)(fy + render_obj_dy);
        g_ped_drawn[o->ped] = 1;
        if (render_hat_colour != NULL) hat = render_hat_colour(o->ped);
    }
    draw_frame(t, rd16(o->a + 0x10u), fx + render_obj_dx, fy + render_obj_dy, hat);
}

void render_city(uint32_t *rgb, int w, int h, const uint32_t *pal, int x0, int y0, int x1, int y1, uint64_t now_us)
{
    static Obj objs[2048];
    uint32_t map, cols;
    if (g_ra.map == 0) return;
    map = rd32(P_MAP);
    cols = map + 12u;
    int mx = (int)rd32(map), my = (int)rd32(map + 4u), mz = (int)rd32(map + 8u), nobj = 0, li, s, k = 0, n = 0, t256;
    Target t = {rgb, w, h, x0, y0, x1, y1, pal};
    if (mx <= 0 || my <= 0 || mz <= 0 || mx > 256 || my > 256 || mz > 16) return;
    /* between the two last ticks (now_us 0: as the game is now) */
    if (now_us != 0 && g_nsnaps == 2 && g_snap[1].us > g_snap[0].us) {
        uint64_t dt = g_snap[1].us - g_snap[0].us, since = now_us > g_snap[1].us ? now_us - g_snap[1].us : 0;
        t256 = since >= dt ? 256 : (int)(since * 256u / dt);
        g_vx = lerp(g_snap[0].vx, g_snap[1].vx, t256);
        g_vy = lerp(g_snap[0].vy, g_snap[1].vy, t256);
    } else {
        t256 = -1;
        read_view(&g_vx, &g_vy);
    }
    /* the things on the map, by tile column then height */
    for (li = 0; li < (int)(sizeof k_lists / sizeof k_lists[0]); li++) {
        const ObjList *l = &k_lists[li];
        uint32_t i;
        for (i = 0; i < l->count; i++, n++) {
            uint32_t a = LEVEL + l->offset + i * l->size;
            uint8_t loc = M[a + 0xau];
            int x, y, z;
            if (nobj == 2048) break;
            if (loc == 0 || (loc != l->on_map[0] && loc != l->on_map[1] && loc != l->on_map[2])) continue;
            if (t256 >= 0) {
                x = lerp(g_snap[0].pos[n][0], g_snap[1].pos[n][0], t256);
                y = lerp(g_snap[0].pos[n][1], g_snap[1].pos[n][1], t256);
                z = lerp(g_snap[0].pos[n][2], g_snap[1].pos[n][2], t256);
            } else {
                x = (int16_t)rd16(a + 4u);
                y = (int16_t)rd16(a + 6u);
                z = (int16_t)rd16(a + 8u);
            }
            if (x < 0 || y < 0 || x >= mx * 256 || y >= my * 256) continue;
            objs[nobj].key = (x >> 8) + (y >> 8) * mx;
            objs[nobj].z = z >> 7;
            objs[nobj].x = x;
            objs[nobj].y = y;
            objs[nobj].zw = z;
            objs[nobj].a = a;
            objs[nobj].ped = li == 0 ? (int)i : -1;
            nobj++;
        }
    }
    qsort(objs, (size_t)nobj, sizeof objs[0], cmp_obj);
    memset(g_ped_drawn, 0, sizeof g_ped_drawn);
    /* back to front: diagonal rows, then each column from the ground up with what stands in it */
    for (s = 0; s < mx + my - 1; s++) {
        int tx;
        for (tx = 0; tx < mx; tx++) {
            int ty = s - tx, tz, key;
            uint32_t col;
            if (ty < 0 || ty >= my) continue;
            key = tx + ty * mx;
            col = rd32(cols + (uint32_t)key * 4u);
            {
                int lo = 0, hi = nobj;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (objs[mid].key < key) lo = mid + 1;
                    else hi = mid;
                }
                k = lo;
            }
            for (tz = 0; tz < mz; tz++) {
                const uint8_t *tl = tile(M[col + (uint32_t)tz]);
                int fx, fy;
                if (tl != NULL) {
                    project(tx * 256, ty * 256, tz * 128, &fx, &fy);
                    if (fx + render_tile_dx + TILE_W > x0 && fx + render_tile_dx < x1 && fy + render_tile_dy + TILE_H > y0 &&
                        fy + render_tile_dy < y1)
                        blit(&t, tl, TILE_W, TILE_H, fx + render_tile_dx, fy + render_tile_dy, 0, 0);
                }
                for (; k < nobj && objs[k].key == key && objs[k].z <= tz; k++) draw_obj(&t, &objs[k]);
            }
            for (; k < nobj && objs[k].key == key; k++) draw_obj(&t, &objs[k]);
        }
    }
}
