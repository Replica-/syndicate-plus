/* render.h - the city of a mission drawn by this program from the game's own data (render.c). */
#pragma once

#include <stdint.h>

/* draws the city as the game sees it into the rectangle x0..x1, y0..y1 of `rgb` (w x h, 0x00RRGGBB), in the
 * game's projection (a 640 x 480 frame: the map from x 128, y 40), with the colours of `pal` */
void render_city(uint32_t *rgb, int w, int h, const uint32_t *pal, int x0, int y0, int x1, int y1);

/* where the tiles and the frames go from their projected points (the calibration) */
extern int render_tile_dx, render_tile_dy, render_obj_dx, render_obj_dy;
