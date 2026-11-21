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

/* draws the city into the rectangle x0..x1, y0..y1 of `rgb` (w x h, 0x00RRGGBB), in the game's projection (a
 * 640 x 480 frame: the map from x 128, y 40), with the colours of `pal`.  now_us 0: as the game is now; else as it
 * is shown at that time on a steady playback clock about 1.5 ticks behind the game (render_tick), so that things
 * move smoothly and evenly at any frame rate */
void render_city(uint32_t *rgb, int w, int h, const uint32_t *pal, int x0, int y0, int x1, int y1, uint64_t now_us);
/* a tick of the mission: where things are now */
void render_tick(uint64_t now_us);
/* a new mission: no tick before */
void render_reset(void);
/* where a point of the world is in the frame last drawn (its view) */
void render_project(int32_t x, int32_t y, int32_t z, int *fx, int *fy);

/* draws sprite `id` (HSPR) with its top left corner at x, y, inside x0..x1, y0..y1 */
void render_sprite(uint32_t *rgb, int w, int h, const uint32_t *pal, int id, int x, int y, int x0, int y0, int x1, int y1);
/* the top left of the coloured pixels of sprite `id` (not black, not transparent); 0 when it has none */
int render_sprite_ink(int id, int *x, int *y);
/* where person `ped` (its number in the level data) was drawn in the last frame; 0 when it was not */
int render_ped_place(int ped, int *fx, int *fy);

/* where the tiles and the frames go from their projected points (the calibration) */
extern int render_tile_dx, render_tile_dy, render_obj_dx, render_obj_dy;
/* the colour of the hat of a person (0: as drawn), for agents */
extern uint32_t (*render_hat_colour)(int ped);
