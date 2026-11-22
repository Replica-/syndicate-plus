/* drive.c - routes for the squad's cars over the roads of the map (drive.h).
 *
 * A road is three tiles wide: a lane each way and the line between them (COL01.DAT's types: 0x08 a lane to +y,
 * 0x09 to -y, 0x07 to +x, 0x06 to -x, 0x0f the line, 0x0b a junction, 0x0e a crossing, which keeps the way of the
 * lane it is part of).  Traffic keeps to the right: a car drives on the line of its lane nearest the middle of the
 * road.  The game's own driving keeps to the lane it is in and turns at junctions as it can: sent behind it, it
 * drives on away.  Here the route is found over the lanes (Dijkstra on tile and heading): never against a lane's
 * way, never backing up, a turn costing a little more than going on; turning round across the line is allowed
 * where there is no other way, at a high cost.  It ends on the lane nearest the point clicked (the side of the road
 * clicked, its traffic's way), where it is cheapest to reach if two are as near. */
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
#define UTURN_COST (24 * TILE_COST)
#define GOAL_R 2          /* the lanes this many tiles about the point clicked */
#define GOAL_WEIGHT 64    /* a lane further from it costs this many times the distance more */
#define LANE_IN 0xC0      /* a lane's line: this far into its tile from the side away from the middle */
#define UTURN_AHEAD 64    /* round across the line: from this far on along its lane ... */
#define UTURN_OVER 224    /* ... over the line this far on, back onto the other lane */

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

/* where on the tile x, y a car drives: on its lane's line (the middle of the road on its left), the middle of the
 * tile along it */
static void lane_point(int x, int y, int32_t *px, int32_t *py)
{
    static const int k_in[4] = {LANE_IN, 256 - LANE_IN, 256 - LANE_IN, LANE_IN}; /* across its way, by way */
    int c = cell(x, y);
    *px = x * 256 + 128;
    *py = y * 256 + 128;
    if (c < 0 || c == ANY) return;
    if (c & 1)
        *py = y * 256 + k_in[c];
    else
        *px = x * 256 + k_in[c];
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
    int32_t best_cost = 0x7FFFFFFF, gpx[2 * GOAL_R + 1][2 * GOAL_R + 1], gpy[2 * GOAL_R + 1][2 * GOAL_R + 1];
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
            int32_t px, py, at, d2;
            lane_point(x, y, &px, &py);
            /* along the lane: as far as the point clicked, in the tile */
            if (c == ANY || (c & 1) == 0) py = ty < y * 256 + 32 ? y * 256 + 32 : ty > y * 256 + 224 ? y * 256 + 224 : ty;
            if (c == ANY || (c & 1) == 1) px = tx < x * 256 + 32 ? x * 256 + 32 : tx > x * 256 + 224 ? x * 256 + 224 : tx;
            d2 = (int32_t)sqrt((double)(px - tx) * (px - tx) + (double)(py - ty) * (py - ty));
            at = cc + d2 * GOAL_WEIGHT;
            if (at < best_cost) {
                best_cost = at;
                best = cur;
                gpx[y - gy + GOAL_R][x - gx + GOAL_R] = px;
                gpy[y - gy + GOAL_R][x - gx + GOAL_R] = py;
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
        /* round across the line, to the other lane */
        if (c >= 0 && c < ANY && h == c)
            for (d = 1; d <= 3; d += 2) {
                int side = (c + d) & 3, mx1 = x + k_dx[side], my1 = y + k_dy[side];
                int ox = x + 2 * k_dx[side], oy = y + 2 * k_dy[side], mt = type_at(mx1, my1);
                if ((mt == T_LINE || mt == T_CROSSING) && cell(ox, oy) == ((c + 2) & 3) &&
                    cell(x + k_dx[c], y + k_dy[c]) == c && cell(ox + k_dx[c], oy + k_dy[c]) == ((c + 2) & 3))
                    relax(cur, ox, oy, (c + 2) & 3, cc + UTURN_COST);
            }
    }
    if (best >= 0) {
        /* the states back to the start, then the points: a lane's line in each tile (not the junctions'), the ones
         * where the way does not change left out; round across the line, an arc ahead of where it turns */
        int np = 0, i;
        for (s = best; s >= 0 && np < (int)(sizeof path / sizeof path[0]); s = g_from[s]) path[np++] = s;
        for (i = np - 2; i >= 0; i--) {
            int st = path[i], h = st & 3, px0 = (st / 4) % g_mx, py0 = (st / 4) / g_mx, c = cell(px0, py0);
            int prev = path[i + 1], ph = prev & 3, qx = (prev / 4) % g_mx, qy = (prev / 4) / g_mx;
            int32_t px, py;
            if (h == ((ph + 2) & 3)) {
                lane_point(qx, qy, &px, &py);
                n = add_point(out, n, max, px + k_dx[ph] * UTURN_AHEAD, py + k_dy[ph] * UTURN_AHEAD, sz);
                n = add_point(out, n, max, (qx + px0) / 2 * 256 + 128 + k_dx[ph] * UTURN_OVER,
                              (qy + py0) / 2 * 256 + 128 + k_dy[ph] * UTURN_OVER, sz);
                lane_point(px0, py0, &px, &py);
                n = add_point(out, n, max, px + k_dx[ph] * UTURN_AHEAD, py + k_dy[ph] * UTURN_AHEAD, sz);
                if (i > 0) continue;
            }
            if (i == 0) {
                px = gpx[py0 - gy + GOAL_R][px0 - gx + GOAL_R];
                py = gpy[py0 - gy + GOAL_R][px0 - gx + GOAL_R];
            } else {
                if (c == ANY) continue;
                lane_point(px0, py0, &px, &py);
                /* on a straight run (the same way before and after) */
                if (n > 0 && ph == h && (path[i - 1] & 3) == h && ((h & 1) ? out[n - 1].y == py : out[n - 1].x == px))
                    continue;
            }
            n = add_point(out, n, max, px, py, sz);
        }
    }
    free(g_cost);
    free(g_from);
    free(g_heap);
    return n;
}
