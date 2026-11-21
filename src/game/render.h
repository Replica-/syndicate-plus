/* render.h - the city of a mission drawn by this program from the game's own data (render.c). */
#pragma once

#include <stdint.h>

/* where the running program keeps what the renderer reads */
typedef struct RenderAddrs {
    uint32_t map, tiles, sprite_tab, frames, elements; /* the pointers to the map, HBLK, HSPR's table, HFRA, HELE */
    uint32_t level;                                    /* the level data */
    uint32_t view;                                     /* the view (16 bits x, y) */
} RenderAddrs;

void render_set_addrs(const RenderAddrs *a);

/* rgb NULL: the city goes to the GPU (host_city_frame) instead, at its exact place */
/* draws the city into the rectangle x0..x1, y0..y1 of `rgb` (w x h, 0x00RRGGBB), in the game's projection (a
 * 640 x 480 frame: the map from x 128, y 40), with the colours of `pal`.  now_us 0: as the game is now; else as it
 * is shown at that time on a steady playback clock about 1.5 ticks behind the game (render_tick), so that things
 * move smoothly and evenly at any frame rate */
void render_city(uint32_t *rgb, int w, int h, const uint32_t *pal, int x0, int y0, int x1, int y1, uint64_t now_us);
/* the view to draw from (world units) instead of the game's (own: 1), so that it moves at once with the keys */
void render_set_view(int own, int32_t vx, int32_t vy);
/* the same with fractions of a world unit (the GPU draws between pixels) */
void render_set_view_f(double vx, double vy);
/* the city drawn `zoom` times its size about the point cx, cy of the frame (render_project and render_ped_place
 * give the zoomed places) */
void render_set_zoom(double zoom, float cx, float cy);
/* the tiles of level `level` (z / 128) and above are drawn almost see-through, to see inside the buildings; -1:
 * none are */
void render_set_cut(int level);
/* the tiles' types (the game's COL01.DAT): the sun's shadows */
void render_set_tile_types(const uint8_t types[256]);
/* the outline of the cells one can walk in at level `level` (`walkable`), in the last city's view */
void render_floor_outline(uint32_t *rgb, int w, int h, int level, uint32_t colour, int x0, int y0, int x1, int y1,
                          int (*walkable)(int x, int y, int z));

/* how a tile lights up at dusk and at night (the light editor) */
enum { RENDER_GLOW_NONE, RENDER_GLOW_BRIGHT, RENDER_GLOW_FULL };   /* what of it shines */
enum { RENDER_LIGHT_AUTO, RENDER_LIGHT_NONE, RENDER_LIGHT_ON };    /* whether it lights what is around it */
typedef struct RenderTileSetting {
    int glow, light, radius;   /* radius 0: from the tile */
    uint32_t colour;           /* 0xFFRRGGBB, or 0: from the tile */
} RenderTileSetting;
int render_tile_setting(int id, RenderTileSetting *out);
/* changes a tile's setting (at once) and saves them all in the user's folder (tile_lights.ini) */
int render_set_tile_setting(int id, const RenderTileSetting *in);
/* the mission's map: its size in tiles (0 when there is none), the tile at x, y, z (-1 outside) */
int render_map_size(int *mx, int *my, int *mz);
/* how dark the mission's own palette makes it (0 day .. 1 night): its lights glow that much by default
 * (host_city_frame's `night`) */
void render_set_mission_night(float night);
int render_map_tile(int x, int y, int z);
/* the tile seen at sx, sy of the frame in the last city drawn: its number and where it is drawn */
int render_tile_at(int sx, int sy, int *id, int *rx, int *ry, int *rw, int *rh);
/* a tick of the mission: where things are now */
void render_tick(uint64_t now_us);
/* a new mission: no tick before */
void render_reset(void);
/* where a point of the world is in the frame last drawn (its view) */
void render_project(int32_t x, int32_t y, int32_t z, int *fx, int *fy);

/* draws sprite `id` (HSPR) with its top left corner at x, y, inside x0..x1, y0..y1 */
void render_sprite(uint32_t *rgb, int w, int h, const uint32_t *pal, int id, int x, int y, int x0, int y0, int x1, int y1);
/* draws an image in the sprites' format (rows of 8-pixel blocks: a transparency byte and 4 bit planes) at guest
 * address `pixels`, iw x ih, with its top left corner at x, y, inside x0..x1, y0..y1 */
void render_image(uint32_t *rgb, int w, int h, const uint32_t *pal, uint32_t pixels, int iw, int ih, int x, int y, int x0,
                  int y0, int x1, int y1);
/* the top left of the coloured pixels of sprite `id` (not black, not transparent); 0 when it has none */
int render_sprite_ink(int id, int *x, int *y);
/* draws frame `frame` (HFRA: its elements) anchored `dy` pixels below where ped `ped` was drawn (unzoomed), each
 * element at its zoomed place but at its own size, inside x0..x1, y0..y1; 0 if the ped was not drawn */
int render_ped_frame(uint32_t *rgb, int w, int h, const uint32_t *pal, int frame, int ped, int dy, int x0, int y0, int x1,
                     int y1);
/* sprite `id` as palette indices (255 transparent), w x h; NULL if there is none */
const uint8_t *render_sprite_pixels(int id, int *w, int *h);
/* where person `ped` (its number in the level data) was drawn in the last frame; 0 when it was not */
int render_ped_place(int ped, int *fx, int *fy);

/* where the tiles and the frames go from their projected points (the calibration) */
extern int render_tile_dx, render_tile_dy, render_obj_dx, render_obj_dy;
/* set by render_city on the GPU: the city differs from the last one */
extern int render_gpu_changed;
/* the colour of the hat of a person (0: as drawn), for agents */
extern uint32_t (*render_hat_colour)(int ped);
