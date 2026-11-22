/* path.h - routes for the squad's move orders over the floors of the map (path.c). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PathPoint {
    int32_t x, y, z; /* world units */
    int hard;        /* to be reached, not cut (stairs, the end) */
} PathPoint;

/* the type of each tile (the game's COL01.DAT, unpacked) */
void path_set_types(const uint8_t types[256]);
/* a person can be in the cell x, y, z (standing on the tile under it) */
int path_walkable(int x, int y, int z);
/* a route from the person at sx, sy, sz to tx, ty, tz: its points in `out` (the last one tx, ty, tz), at most `max`;
 * 0 when the game's straight walk will do (clear of the walls), or no route was found */
int path_route(int32_t sx, int32_t sy, int32_t sz, int32_t tx, int32_t ty, int32_t tz, PathPoint *out, int max);

#ifdef __cplusplus
}
#endif
