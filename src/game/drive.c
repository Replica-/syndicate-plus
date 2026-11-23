/* drive.c - routes for the squad's cars over the roads of the map (drive.h).
 *
 * A road is three tiles wide: a lane each way and the line between them (COL01.DAT's types: 0x08 a lane to +y,
 * 0x09 to -y, 0x07 to +x, 0x06 to -x, 0x0f the line, 0x0b a junction, 0x0e a crossing, which keeps the way of the
 * lane it is part of).  Traffic keeps to the right: a car drives on the line of its lane nearest the middle of the
 * road.  The game's own driving keeps to the lane it is in and turns at junctions as it can: sent behind it, it
 * drives on away.  Here the route is found over the lanes (Dijkstra on tile and heading): never against a lane's
 * way, never backing up, turning only where the road does (junctions, the loops at the roads' ends), a turn costing
 * a little more than going on.  It ends on the lane nearest the point clicked that can be reached (the side of the
 * road clicked, its traffic's way), where it is cheapest to reach if two are as near.  A car runs on its lanes'
 * lines as on rails: the route's points are where those lines meet at its turns, and its end. */
#include "drive.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"

#define T_NONE 0x00
#define T_LANE_MX 0x06 /* -x */
#define T_LANE_PX 0x07 /* +x */
#define T_LANE_PY 0x08 /* +y */
#define T_LANE_MY 0x09 /* -y */
#define T_JUNCTION 0x0B
#define T_CROSSING 0x0E
#define T_LINE 0x0F

#define ANY 4         /* a junction: any way */
#define TILE_COST 256 /* (world units) */
#define TURN_COST 96
#define GOAL_R 2          /* the lanes this many tiles about the point clicked */
#define GOAL_WEIGHT 64    /* a lane further from it costs this many times the distance more */
#define LANE_IN 0xC0      /* a lane's line: this far into its tile from the side away from the middle */

/* the ways: +y, +x, -y, -x (the game's headings 0, 64, 128, 192) */
static const int k_dx[4] = {0, 1, 0, -1}, k_dy[4] = {1, 0, -1, 0};

static int g_mx, g_my, g_zi;

static int type_at(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_mx || y >= g_my) return T_NONE;
    return path_tile_type(x, y, g_zi);
}

static int lane_way(int t)
{
    return t == T_LANE_PY ? 0 : t == T_LANE_PX ? 1 : t == T_LANE_MY ? 2 : t == T_LANE_MX ? 3 : -1;
}

/* the way a car may drive through the tile x, y: 0-3, ANY, or -1 if it cannot */
static int cell(int x, int y)
{
    int t = type_at(x, y), k, s, w = lane_way(t);
    if (t == T_JUNCTION) return ANY;
    if (w >= 0) {
        /* under a bridge the three tiles of a road are all one lane's: the road's on either side along it tell */
        int a = type_at(x + k_dx[w], y + k_dy[w]), b = type_at(x - k_dx[w], y - k_dy[w]);
        if (a == T_LINE && b == T_LINE) return -1;
        if (lane_way(a) >= 0 && lane_way(a) == lane_way(b) && (lane_way(a) & 1) == (w & 1)) return lane_way(a);
        return w;
    }
    if (t != T_CROSSING) return -1;
    /* a crossing: the way of the lane it is part of (beyond it along the road); on the line, none */
    for (k = 1; k <= 2; k++)
        for (s = 0; s < 4; s++) {
            int n = type_at(x + k * k_dx[s], y + k * k_dy[s]), w = lane_way(n);
            if (n == T_LINE) return -1;
            if (w >= 0 && (w & 1) == (s & 1)) return w;
        }
    return ANY;
}

/* a lane's line, going `way` through the tile x, y (the middle of the road on its left): across that way, the
 * coordinate a car keeps to there */
static int32_t line_at(int x, int y, int way)
{
    static const int k_in[4] = {LANE_IN, 256 - LANE_IN, 256 - LANE_IN, LANE_IN};
    return (way & 1 ? y : x) * 256 + k_in[way];
}

static int32_t clamp_in(int32_t v, int tile)
{
    return v < tile * 256 + 32 ? tile * 256 + 32 : v > tile * 256 + 224 ? tile * 256 + 224 : v;
}

static int add_point(PathPoint *out, int n, int max, int32_t x, int32_t y, int32_t z)
{
    if (n >= max) return n;
    out[n].x = x;
    out[n].y = y;
    out[n].z = z;
    out[n].hard = 0;
    return n + 1;
}

/* ---- Dijkstra over (tile, heading) ---- */
static int32_t *g_cost;
static int32_t *g_from; /* the state before, -1 at the start */
static int *g_heap, g_nheap;
static int g_states;

static int32_t key(int s)
{
    return g_cost[s];
}

static void heap_push(int s)
{
    int i = g_nheap++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (key(g_heap[p]) <= key(s)) break;
        g_heap[i] = g_heap[p];
        i = p;
    }
    g_heap[i] = s;
}

static int heap_pop(void)
{
    int top = g_heap[0], last = g_heap[--g_nheap], i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= g_nheap) break;
        if (c + 1 < g_nheap && key(g_heap[c + 1]) < key(g_heap[c])) c++;
        if (key(last) <= key(g_heap[c])) break;
        g_heap[i] = g_heap[c];
        i = c;
    }
    if (g_nheap > 0) g_heap[i] = last;
    return top;
}

static void relax(int from, int x, int y, int h, int32_t cost)
{
    int s = ((y * g_mx) + x) * 4 + h;
    if (cost >= g_cost[s]) return;
    g_cost[s] = cost;
    g_from[s] = from;
    if (g_nheap < g_states * 2) heap_push(s);
}

int drive_route(int32_t sx, int32_t sy, int32_t sz, int heading, int32_t tx, int32_t ty, PathPoint *out, int max)
{
    int mz, x0 = sx >> 8, y0 = sy >> 8, h0 = ((heading + 32) >> 6) & 3, gx = tx >> 8, gy = ty >> 8, x, y, s, best = -1;
    int n = 0;
    int32_t best_cost = 0x7FFFFFFF, best_x = 0, best_y = 0;
    static int path[65536];
    if (!render_map_size(&g_mx, &g_my, &mz)) return 0;
    g_zi = sz / 128 - 1;
    if (g_zi < 0 || g_zi >= mz || cell(x0, y0) < 0) return 0;
    g_states = g_mx * g_my * 4;
    g_cost = (int32_t *)malloc(sizeof *g_cost * (size_t)g_states);
    g_from = (int32_t *)malloc(sizeof *g_from * (size_t)g_states);
    g_heap = (int *)malloc(sizeof *g_heap * (size_t)g_states * 2);
    if (g_cost == NULL || g_from == NULL || g_heap == NULL) {
        free(g_cost);
        free(g_from);
        free(g_heap);
        return 0;
    }
    for (s = 0; s < g_states; s++) g_cost[s] = 0x7FFFFFFF;
    g_nheap = 0;
    relax(-1, x0, y0, h0, 0);
    while (g_nheap > 0) {
        int cur = heap_pop(), h = cur & 3, c, d;
        int32_t cc = g_cost[cur];
        x = (cur / 4) % g_mx;
        y = (cur / 4) / g_mx;
        if (cc > best_cost) break;
        c = cell(x, y);
        /* on a lane near the point clicked: an end (its distance from that point added) */
        if (abs(x - gx) <= GOAL_R && abs(y - gy) <= GOAL_R) {
            /* on its line, along it as far as the point clicked in the tile (not back where it is, at the start) */
            int32_t px = h & 1 ? clamp_in(tx, x) : line_at(x, y, h), py = h & 1 ? line_at(x, y, h) : clamp_in(ty, y), at, d2;
            if (g_from[cur] < 0) {
                if (h == 0 && py < sy) py = sy;
                if (h == 2 && py > sy) py = sy;
                if (h == 1 && px < sx) px = sx;
                if (h == 3 && px > sx) px = sx;
            }
            d2 = (int32_t)sqrt((double)(px - tx) * (px - tx) + (double)(py - ty) * (py - ty));
            at = cc + d2 * GOAL_WEIGHT;
            if (at < best_cost) {
                best_cost = at;
                best = cur;
                best_x = px;
                best_y = py;
            }
        }
        for (d = 0; d < 4; d++) {
            int nx = x + k_dx[d], ny = y + k_dy[d], nc;
            if (d == ((h + 2) & 3)) continue;                    /* no backing up */
            if (c >= 0 && c < ANY && d == ((c + 2) & 3)) continue; /* nor against the lane */
            if (nx < 0 || ny < 0 || nx >= g_mx || ny >= g_my) continue;
            nc = cell(nx, ny);
            if (nc < 0 || (nc < ANY && d == ((nc + 2) & 3))) continue;
            relax(cur, nx, ny, d, cc + TILE_COST + (d != h ? TURN_COST : 0));
        }
    }
    if (best >= 0) {
        /* the states back to the start, then the corners: where the line it ran on meets the next one */
        int np = 0, i, run_x = x0, run_y = y0, run_h = h0;
        for (s = best; s >= 0 && np < (int)(sizeof path / sizeof path[0]); s = g_from[s]) path[np++] = s;
        for (i = np - 2; i >= 0; i--) {
            int st = path[i], h = st & 3, px0 = (st / 4) % g_mx, py0 = (st / 4) / g_mx;
            if (h != run_h) {
                if (run_h & 1)
                    n = add_point(out, n, max, line_at(px0, py0, h), line_at(run_x, run_y, run_h), sz);
                else
                    n = add_point(out, n, max, line_at(run_x, run_y, run_h), line_at(px0, py0, h), sz);
                run_h = h;
            }
            run_x = px0;
            run_y = py0;
        }
        n = add_point(out, n, max, best_x, best_y, sz);
    }
    free(g_cost);
    free(g_from);
    free(g_heap);
    return n;
}
