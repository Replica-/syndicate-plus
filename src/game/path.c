/* path.c - routes for the squad's move orders (path.h).
 *
 * The game walks an agent in a straight line towards where it was sent and stops at what is in the way: sent onto
 * a platform it walks to the point under it and stays there.  Here a route is found over the floors of the map
 * (A* on its tiles, the tile types of COL01.DAT): each step goes to a neighbouring tile on the same level, a step
 * beside a wall costing more (a route keeps to the middle of open ground), and stairs (the slopes) join a level to
 * the next.  The route is then pulled straight: its points are where a straight line from the last one would no
 * longer stay clear of the walls (a band CLEARANCE wide either side of it on floor), and the stairs.  The agent is
 * steered along it (additions.c); the game's own walking does the rest. */
#include "path.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"

/* the tile types (COL01.DAT) */
enum {
    T_NONE = 0x00,
    T_SLOPE_SN = 0x01,
    T_SLOPE_NS = 0x02,
    T_SLOPE_EW = 0x03,
    T_SLOPE_WE = 0x04,
    T_GROUND = 0x05,
    T_ROAD_SIDE_EW = 0x06,
    T_ROAD_SIDE_NS = 0x09,
    T_WALL = 0x0A,
    T_ROAD_CURVE = 0x0B,
    T_RAIL = 0x0C,
    T_ROOF = 0x0D,
    T_CROSSING = 0x0E,
    T_ROAD_MARK = 0x0F,
};

static uint8_t g_types[256];
static int g_have_types;

void path_set_types(const uint8_t types[256])
{
    memcpy(g_types, types, sizeof g_types);
    g_have_types = 1;
}

static int g_mx, g_my, g_mz;

static int type_at(int x, int y, int z)
{
    int id;
    if (x < 0 || y < 0 || z < 0 || x >= g_mx || y >= g_my || z >= g_mz) return T_NONE;
    id = render_map_tile(x, y, z);
    return id < 0 ? T_NONE : g_types[id];
}

static int is_slope(int t)
{
    return t >= T_SLOPE_SN && t <= T_SLOPE_WE;
}

static int is_floor(int t)
{
    return t == T_GROUND || (t >= T_ROAD_SIDE_EW && t <= T_ROAD_SIDE_NS) || t == T_ROAD_CURVE || t == T_ROOF ||
           t == T_CROSSING || t == T_ROAD_MARK;
}

/* one can stand on the tile at x, y, z: a floor or a slope, with room above it */
static int standable(int x, int y, int z)
{
    int t = type_at(x, y, z), above = type_at(x, y, z + 1);
    if (!is_floor(t) && !is_slope(t)) return 0;
    return above == T_NONE || is_slope(above) ? 1 : is_floor(above) ? 0 : above != T_WALL && above != T_RAIL;
}

int path_tile_type(int x, int y, int z)
{
    if (!g_have_types || !render_map_size(&g_mx, &g_my, &g_mz)) return T_NONE;
    return type_at(x, y, z);
}

int path_walkable(int x, int y, int z)
{
    if (!g_have_types || !render_map_size(&g_mx, &g_my, &g_mz)) return 0;
    return z >= 1 && standable(x, y, z - 1);
}

/* a slope's way up (dx, dy): towards the neighbour whose floor is a level higher; 0 if it cannot be told */
static int slope_up(int x, int y, int z, int *dx, int *dy)
{
    int t = type_at(x, y, z), ax = t == T_SLOPE_EW || t == T_SLOPE_WE, s;
    for (s = -1; s <= 1; s += 2) {
        int hx = x + (ax ? s : 0), hy = y + (ax ? 0 : s), lx = x - (ax ? s : 0), ly = y - (ax ? 0 : s);
        int high = standable(hx, hy, z) && !is_slope(type_at(hx, hy, z)), low = standable(lx, ly, z - 1);
        if (high && (low || is_slope(type_at(lx, ly, z - 1)) || standable(lx, ly, z - 1))) {
            *dx = ax ? s : 0;
            *dy = ax ? 0 : s;
            return 1;
        }
    }
    /* stairs of several tiles: up towards the next slope a level higher */
    for (s = -1; s <= 1; s += 2) {
        int hx = x + (ax ? s : 0), hy = y + (ax ? 0 : s);
        if (type_at(hx, hy, z + 1) == t || standable(hx, hy, z)) {
            *dx = ax ? s : 0;
            *dy = ax ? 0 : s;
            return 1;
        }
    }
    return 0;
}

/* the tiles round x, y on level z one cannot stand on (0 to 8): a step beside them costs more, so that a route keeps
 * away from walls where there is room (the shortest one goes along them, and a person catches their corners) */
static int blocked_round(int x, int y, int z)
{
    int dx, dy, n = 0;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if ((dx || dy) && !standable(x + dx, y + dy, z) && !is_slope(type_at(x + dx, y + dy, z + 1)) &&
                !standable(x + dx, y + dy, z - 1))
                n++;
    return n;
}
#define WALL_COST 6 /* for each such tile (a step is 10, a diagonal 14) */

/* ---- A* ---- */
#define MAX_NODES (256 * 256 * 16)
typedef struct Node {
    int32_t g;      /* the cost from the start (-1: not reached) */
    int32_t from;   /* the node it was reached from */
    uint8_t closed;
} Node;
static Node *g_nodes;
static int32_t *g_heap;
static int g_nheap;

static int node_id(int x, int y, int z)
{
    return (z * g_my + y) * g_mx + x;
}

static int32_t g_f(int32_t id, int gx, int gy, int gz)
{
    int x = id % g_mx, y = (id / g_mx) % g_my, z = id / (g_mx * g_my);
    int dx = abs(x - gx), dy = abs(y - gy);
    return g_nodes[id].g + (dx > dy ? 10 * dx + 4 * dy : 10 * dy + 4 * dx) + 10 * abs(z - gz);
}

static void heap_push(int32_t id, int gx, int gy, int gz)
{
    int i = g_nheap++;
    int32_t f = g_f(id, gx, gy, gz);
    while (i > 0) {
        int p = (i - 1) / 2;
        if (g_f(g_heap[p], gx, gy, gz) <= f) break;
        g_heap[i] = g_heap[p];
        i = p;
    }
    g_heap[i] = id;
}

static int32_t heap_pop(int gx, int gy, int gz)
{
    int32_t top = g_heap[0], last = g_heap[--g_nheap];
    int i = 0;
    int32_t f = g_f(last, gx, gy, gz);
    for (;;) {
        int c = 2 * i + 1;
        if (c >= g_nheap) break;
        if (c + 1 < g_nheap && g_f(g_heap[c + 1], gx, gy, gz) < g_f(g_heap[c], gx, gy, gz)) c++;
        if (g_f(g_heap[c], gx, gy, gz) >= f) break;
        g_heap[i] = g_heap[c];
        i = c;
    }
    if (g_nheap > 0) g_heap[i] = last;
    return top;
}

static void relax(int32_t from, int x, int y, int z, int cost, int gx, int gy, int gz)
{
    int32_t id = node_id(x, y, z);
    int32_t g = g_nodes[from].g + cost + WALL_COST * blocked_round(x, y, z);
    if (g_nodes[id].closed || (g_nodes[id].g >= 0 && g_nodes[id].g <= g)) return;
    g_nodes[id].g = g;
    g_nodes[id].from = from;
    heap_push(id, gx, gy, gz);
}

/* the tiles one can step to from x, y, z */
#define STEP_TILE 128 /* a station platform's edge, by the track (its steps drawn on it) */
static void neighbours(int32_t id, int x, int y, int z, int gx, int gy, int gz)
{
    int dx, dy, ux = 0, uy = 0, here = type_at(x, y, z);
    if (is_slope(here) && slope_up(x, y, z, &ux, &uy)) {
        /* on stairs: up (to the next level's floor or stairs) or down */
        if (standable(x + ux, y + uy, z)) relax(id, x + ux, y + uy, z, 10, gx, gy, gz);
        if (is_slope(type_at(x + ux, y + uy, z + 1)) && standable(x + ux, y + uy, z + 1))
            relax(id, x + ux, y + uy, z + 1, 14, gx, gy, gz);
        if (standable(x - ux, y - uy, z - 1)) relax(id, x - ux, y - uy, z - 1, 14, gx, gy, gz);
        if (is_slope(type_at(x - ux, y - uy, z - 1)) && standable(x - ux, y - uy, z - 1))
            relax(id, x - ux, y - uy, z - 1, 14, gx, gy, gz);
        return;
    }
    /* a station platform's edge (STEP_TILE, roof to the tiles' types): a step down onto the track beside it (a level
     * lower), and back up onto it, as the game lets people do */
    {
        static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        int k;
        for (k = 0; k < 4; k++) {
            int nx = x + d4[k][0], ny = y + d4[k][1];
            if (nx < 0 || ny < 0 || nx >= g_mx || ny >= g_my) continue;
            if (z >= 1 && render_map_tile(x, y, z) == STEP_TILE && standable(nx, ny, z - 1) &&
                !is_slope(type_at(nx, ny, z - 1)))
                relax(id, nx, ny, z - 1, 14, gx, gy, gz);
            if (z + 1 < g_mz && render_map_tile(nx, ny, z + 1) == STEP_TILE && standable(nx, ny, z + 1))
                relax(id, nx, ny, z + 1, 14, gx, gy, gz);
        }
    }
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= g_mx || ny >= g_my) continue;
            /* not across a corner */
            if (dx != 0 && dy != 0 && (!standable(x + dx, y, z) || !standable(x, y + dy, z))) continue;
            if (standable(nx, ny, z) && !is_slope(type_at(nx, ny, z))) relax(id, nx, ny, z, dx && dy ? 14 : 10, gx, gy, gz);
            if (dx == 0 || dy == 0) {
                int sx, sy;
                /* onto stairs going up from here (a level higher; the tile under their first step may be floor) or
                 * down (on this level, their low end here) */
                if (is_slope(type_at(nx, ny, z + 1)) && standable(nx, ny, z + 1) && slope_up(nx, ny, z + 1, &sx, &sy) &&
                    sx == dx && sy == dy)
                    relax(id, nx, ny, z + 1, 14, gx, gy, gz);
                if (is_slope(type_at(nx, ny, z)) && standable(nx, ny, z) && slope_up(nx, ny, z, &sx, &sy) &&
                    sx == -dx && sy == -dy)
                    relax(id, nx, ny, z, 14, gx, gy, gz);
            }
        }
}

/* a person can walk on level z at the world point x, y (a floor, not stairs) */
static int floor_at(int32_t x, int32_t y, int z)
{
    int tx = x >> 8, ty = y >> 8;
    return x >= 0 && y >= 0 && standable(tx, ty, z) && !is_slope(type_at(tx, ty, z));
}

#define CLEARANCE 64 /* world units either side of a straight stretch that must be floor (a tile is 256) */
#define LINE_STEP 32

/* a straight walk on level z from a to b (world units) stays on floor, and clear of the walls but near its ends */
static int clear_line(int32_t ax, int32_t ay, int32_t bx, int32_t by, int z)
{
    int32_t dx = bx - ax, dy = by - ay;
    int32_t len = (int32_t)sqrt((double)dx * dx + (double)dy * dy), s;
    double nx = len ? -(double)dy / len : 0, ny = len ? (double)dx / len : 0;
    for (s = 0; s <= len; s += LINE_STEP) {
        int32_t x = ax + (int32_t)((int64_t)dx * s / (len ? len : 1)), y = ay + (int32_t)((int64_t)dy * s / (len ? len : 1));
        if (!floor_at(x, y, z)) return 0;
        if (s >= CLEARANCE && len - s >= CLEARANCE &&
            (!floor_at(x + (int32_t)(nx * CLEARANCE), y + (int32_t)(ny * CLEARANCE), z) ||
             !floor_at(x - (int32_t)(nx * CLEARANCE), y - (int32_t)(ny * CLEARANCE), z)))
            return 0;
    }
    return floor_at(bx, by, z);
}

/* a tile's level from a height (a person stands on the top of the tile under it) */
static int level_of(int32_t z)
{
    int l = z / 128 - 1;
    return l < 0 ? 0 : l;
}

/* the level near z where one can stand at x, y (the person's own, else one above or below) */
static int stand_level(int x, int y, int32_t z)
{
    int l = level_of(z), d;
    for (d = 0; d <= 2; d++) {
        if (l - d >= 0 && standable(x, y, l - d)) return l - d;
        if (l + d < g_mz && standable(x, y, l + d)) return l + d;
    }
    return -1;
}

int path_clear(int32_t ax, int32_t ay, int32_t az, int32_t bx, int32_t by)
{
    int l;
    if (!g_have_types || !render_map_size(&g_mx, &g_my, &g_mz) || ax < 0 || ay < 0) return 0;
    l = stand_level(ax >> 8, ay >> 8, az);
    return l >= 0 && clear_line(ax, ay, bx, by, l);
}

int path_route(int32_t sx, int32_t sy, int32_t sz, int32_t tx, int32_t ty, int32_t tz, PathPoint *out, int max)
{
    int ax, ay, az, bx, by, bz, n = 0, found = 0;
    int32_t id, *route = NULL;
    int nroute = 0, i;
    if (!g_have_types || !render_map_size(&g_mx, &g_my, &g_mz) || g_mx * g_my * g_mz > MAX_NODES) return 0;
    ax = sx >> 8;
    ay = sy >> 8;
    bx = tx >> 8;
    by = ty >> 8;
    if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax >= g_mx || ay >= g_my || bx >= g_mx || by >= g_my) return 0;
    az = stand_level(ax, ay, sz);
    bz = stand_level(bx, by, tz);
    if (az < 0 || bz < 0) return 0;
    /* the game's straight walk will do (clear of the walls) */
    if (az == bz && clear_line(sx, sy, tx, ty, az)) return 0;
    if (g_nodes == NULL) {
        g_nodes = (Node *)malloc(sizeof *g_nodes * MAX_NODES);
        g_heap = (int32_t *)malloc(sizeof *g_heap * MAX_NODES);
        if (g_nodes == NULL || g_heap == NULL) return 0;
    }
    for (i = 0; i < g_mx * g_my * g_mz; i++) {
        g_nodes[i].g = -1;
        g_nodes[i].closed = 0;
    }
    g_nheap = 0;
    id = node_id(ax, ay, az);
    g_nodes[id].g = 0;
    g_nodes[id].from = -1;
    heap_push(id, bx, by, bz);
    while (g_nheap > 0) {
        int x, y, z;
        id = heap_pop(bx, by, bz);
        if (g_nodes[id].closed) continue;
        g_nodes[id].closed = 1;
        x = id % g_mx;
        y = (id / g_mx) % g_my;
        z = id / (g_mx * g_my);
        if (x == bx && y == by && z == bz) {
            found = 1;
            break;
        }
        neighbours(id, x, y, z, bx, by, bz);
    }
    if (!found) return 0;
    for (i = id; i >= 0; i = g_nodes[i].from) nroute++;
    route = (int32_t *)malloc(sizeof *route * (size_t)nroute);
    if (route == NULL) return 0;
    for (i = id, n = nroute; i >= 0; i = g_nodes[i].from) route[--n] = i;
    /* pulled straight: from the last point, on along the route while a straight line to the next tile stays clear
     * of the walls; the tile before stairs, the stairs and the tile after them are points the agent must reach */
    {
        int32_t lx = sx, ly = sy;
        int lz = az, k;
        n = 0;
        for (k = 1; k < nroute && n < max - 1; k++) {
            int qx = route[k] % g_mx, qy = (route[k] / g_mx) % g_my, qz = route[k] / (g_mx * g_my);
            int px = route[k - 1] % g_mx, py = (route[k - 1] / g_mx) % g_my, pz = route[k - 1] / (g_mx * g_my);
            int32_t cx = qx * 256 + 128, cy = qy * 256 + 128, bx0 = px * 256 + 128, by0 = py * 256 + 128;
            int stairs = qz != pz || is_slope(type_at(qx, qy, qz)) || is_slope(type_at(px, py, pz));
            int before = k > 1 && (bx0 != lx || by0 != ly); /* the tile before is not the last point */
            if (!stairs && k < nroute - 1 && clear_line(lx, ly, cx, cy, lz)) continue;
            if (!stairs && k == nroute - 1 && clear_line(lx, ly, tx, ty, lz)) break;
            /* the last tile the line reached, then (stairs, or round a tight corner) this one */
            if (before) {
                out[n].x = bx0;
                out[n].y = by0;
                out[n].z = (pz + 1) * 128;
                out[n].hard = stairs;
                n++;
                lx = bx0;
                ly = by0;
                lz = pz;
                if (!stairs) {
                    k--; /* this tile again from there */
                    continue;
                }
            }
            if (n >= max - 1) break;
            out[n].x = cx;
            out[n].y = cy;
            out[n].z = (qz + 1) * 128;
            out[n].hard = stairs;
            n++;
            lx = cx;
            ly = cy;
            lz = qz;
        }
    }
    free(route);
    /* the last point is where the player sent the agent */
    out[n].x = tx;
    out[n].y = ty;
    out[n].z = tz;
    out[n].hard = 1;
    return n + 1;
}
