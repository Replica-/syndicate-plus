/* path.c - routes for the squad's move orders (path.h).
 *
 * The game walks an agent in a straight line towards where it was sent and stops at what is in the way: sent onto
 * a platform it walks to the point under it and stays there.  Here a route is found over the floors of the map
 * (A* on its tiles, the tile types of COL01.DAT): each step goes to a neighbouring tile on the same level, and
 * stairs (the slopes) join a level to the next.  The route is made of its turning points, which the agent is sent
 * to one after the other (additions.c); the game's own walking does the rest. */
#include "path.h"

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
    int32_t g = g_nodes[from].g + cost;
    if (g_nodes[id].closed || (g_nodes[id].g >= 0 && g_nodes[id].g <= g)) return;
    g_nodes[id].g = g;
    g_nodes[id].from = from;
    heap_push(id, gx, gy, gz);
}

/* the tiles one can step to from x, y, z */
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
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= g_mx || ny >= g_my) continue;
            /* not across a corner */
            if (dx != 0 && dy != 0 && (!standable(x + dx, y, z) || !standable(x, y + dy, z))) continue;
            if (standable(nx, ny, z) && !is_slope(type_at(nx, ny, z))) {
                relax(id, nx, ny, z, dx && dy ? 14 : 10, gx, gy, gz);
            } else if (dx == 0 || dy == 0) {
                int sx, sy;
                /* onto stairs going up from here (a level higher) or down (on this level, their low end here) */
                if (is_slope(type_at(nx, ny, z + 1)) && standable(nx, ny, z + 1) && slope_up(nx, ny, z + 1, &sx, &sy) &&
                    sx == dx && sy == dy)
                    relax(id, nx, ny, z + 1, 14, gx, gy, gz);
                if (is_slope(type_at(nx, ny, z)) && standable(nx, ny, z) && slope_up(nx, ny, z, &sx, &sy) &&
                    sx == -dx && sy == -dy)
                    relax(id, nx, ny, z, 14, gx, gy, gz);
            }
        }
}

/* a straight walk on one level from a to b (tile centres) crosses only floor */
static int straight(int ax, int ay, int bx, int by, int z)
{
    int n = abs(bx - ax) > abs(by - ay) ? abs(bx - ax) : abs(by - ay), i;
    for (i = 0; i <= n * 2; i++) {
        /* half-tile steps, the tiles either side of a diagonal */
        int x = ax + (n ? (bx - ax) * i / (2 * n) : 0), y = ay + (n ? (by - ay) * i / (2 * n) : 0);
        int x2 = ax + (n ? ((bx - ax) * i + n) / (2 * n) : 0), y2 = ay + (n ? ((by - ay) * i + n) / (2 * n) : 0);
        if (!standable(x, y, z) || is_slope(type_at(x, y, z)) || !standable(x2, y2, z) || is_slope(type_at(x2, y2, z)))
            return 0;
    }
    return 1;
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

int path_route(int32_t sx, int32_t sy, int32_t sz, int32_t tx, int32_t ty, int32_t tz, PathPoint *out, int max)
{
    int ax, ay, az, bx, by, bz, n = 0, found = 0;
    int32_t id, *route = NULL;
    int nroute = 0, i, last;
    if (!g_have_types || !render_map_size(&g_mx, &g_my, &g_mz) || g_mx * g_my * g_mz > MAX_NODES) return 0;
    ax = sx >> 8;
    ay = sy >> 8;
    bx = tx >> 8;
    by = ty >> 8;
    if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax >= g_mx || ay >= g_my || bx >= g_mx || by >= g_my) return 0;
    az = stand_level(ax, ay, sz);
    bz = stand_level(bx, by, tz);
    if (az < 0 || bz < 0) return 0;
    /* the game's straight walk will do */
    if (az == bz && straight(ax, ay, bx, by, az)) return 0;
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
    /* the route back to front, then its turning points: a point is kept when the straight walk from the last kept
     * one to the next would leave the floor, or the level changes */
    for (i = id; i >= 0; i = g_nodes[i].from) nroute++;
    route = (int32_t *)malloc(sizeof *route * (size_t)nroute);
    if (route == NULL) return 0;
    for (i = id, n = nroute; i >= 0; i = g_nodes[i].from) route[--n] = i;
    n = 0;
    last = 0;
    for (i = 1; i < nroute && n < max; i++) {
        int px = route[last] % g_mx, py = (route[last] / g_mx) % g_my, pz = route[last] / (g_mx * g_my);
        int qx = route[i] % g_mx, qy = (route[i] / g_mx) % g_my, qz = route[i] / (g_mx * g_my);
        int keep = i == nroute - 1;
        if (!keep && (qz != pz || is_slope(type_at(qx, qy, qz)))) keep = 1;
        if (!keep) {
            int nx = route[i + 1] % g_mx, ny = (route[i + 1] / g_mx) % g_my, nz = route[i + 1] / (g_mx * g_my);
            keep = nz != pz || !straight(px, py, nx, ny, pz);
        }
        if (!keep) continue;
        out[n].x = qx * 256 + 128;
        out[n].y = qy * 256 + 128;
        out[n].z = (qz + 1) * 128;
        n++;
        last = i;
    }
    free(route);
    /* the last point is where the player sent the agent */
    if (n > 0) {
        out[n - 1].x = tx;
        out[n - 1].y = ty;
        out[n - 1].z = tz;
    }
    return n;
}
