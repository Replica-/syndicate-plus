/* drive.h - routes for the squad's cars over the roads of the map (drive.c). */
#pragma once

#include <stdint.h>

#include "path.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a route for the car at sx, sy, sz heading `heading` (the game's: 0 +y, 64 +x, 128 -y, 192 -x) to the lane nearest
 * tx, ty: its points in `out` (along the lanes' lines; the last one where it stops), at most `max`; 0 when the car
 * is not on a road or no lane near tx, ty can be reached */
int drive_route(int32_t sx, int32_t sy, int32_t sz, int heading, int32_t tx, int32_t ty, PathPoint *out, int max);

#ifdef __cplusplus
}
#endif
