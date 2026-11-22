/* script.c - scripted input (rt.h: rt_script_set): keys, clicks, moves and the wheel at set times, as
 * "ms:event,..." (the headless program's --input, the window's too):
 *   a key, pressed and released 100 ms later, as a scancode in hex (1c = Enter) or a name (enter, esc, space,
 *   up, down, left, right, f1..f10, letters and digits, ...); "key:down" / "key:up" to press or release only;
 *   "click@x;y" / "rclick@x;y" (held 150 ms) and "move@x;y" with x, y fractions 0..65535 of the picture (on a wide
 *   menu, of the game's 320 x 200 in it);
 *   "wheel@N" turns the mouse wheel N notches (negative: towards the user). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "rt.h"

#ifndef _WIN32
#define strtok_s strtok_r
#endif

typedef struct Ev {
    uint32_t ms;
    int kind; /* 0 key, 1 click, 2 right click, 3 move, 4 buttons up (the end of a click, 150 ms later), 5 wheel */
    uint32_t code;
    int up;
    int x, y;
} Ev;
static Ev g_ev[256];
static int g_nev, g_iev;

static uint32_t key_code(const char *k)
{
    static const struct {
        const char *name;
        uint32_t code;
    } names[] = {{"enter", 0x1C}, {"esc", 0x01},  {"space", 0x39}, {"up", 0xE048}, {"down", 0xE050},
                 {"left", 0xE04B}, {"right", 0xE04D}, {"f1", 0x3B}, {"f2", 0x3C},   {"f3", 0x3D},
                 {"f4", 0x3E},     {"f5", 0x3F},    {"f6", 0x40},  {"f7", 0x41},   {"f8", 0x42},
                 {"f9", 0x43},     {"f10", 0x44},   {"tab", 0x0F}, {"bksp", 0x0E}, {"y", 0x15},
                 {"n", 0x31},      {"1", 0x02},     {"2", 0x03},   {"3", 0x04},    {"4", 0x05},
                 {"5", 0x06},      {"6", 0x07},     {"a", 0x1E},   {"s", 0x1F},    {"d", 0x20},
                 {"q", 0x10},      {"p", 0x19},     {"x", 0x2D},   {"c", 0x2E}};
    static const char *rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const uint32_t row_start[] = {0x02, 0x10, 0x1E, 0x2C};
    static const struct {
        const char *name;
        uint32_t code;
    } more[] = {{"ctrl", 0x1D}, {"shift", 0x2A}, {"alt", 0x38}, {"pgup", 0xE049}, {"pgdn", 0xE051},
                {"home", 0xE047}, {"end", 0xE04F}, {"ins", 0xE052}, {"del", 0xE053}, {"minus", 0x0C},
                {"plus", 0x0D}};
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(k, names[i].name) == 0) return names[i].code;
    for (i = 0; i < sizeof more / sizeof more[0]; i++)
        if (strcmp(k, more[i].name) == 0) return more[i].code;
    if (k[0] && !k[1]) {
        for (i = 0; i < 4; i++) {
            const char *p = strchr(rows[i], k[0]);
            if (p) return row_start[i] + (uint32_t)(p - rows[i]);
        }
    }
    return (uint32_t)strtoul(k, NULL, 16);
}

void rt_script_set(const char *s)
{
    char buf[4096], *tok, *save = NULL;
    snprintf(buf, sizeof buf, "%s", s);
    for (tok = strtok_s(buf, ",", &save); tok && g_nev < 256; tok = strtok_s(NULL, ",", &save)) {
        Ev *e = &g_ev[g_nev];
        char *c = strchr(tok, ':');
        if (!c) continue;
        *c = 0;
        e->ms = (uint32_t)atoi(tok);
        c++;
        if (!strncmp(c, "wheel@", 6)) {
            e->kind = 5;
            e->x = atoi(c + 6);
        } else if (!strncmp(c, "click@", 6) || !strncmp(c, "rclick@", 7) || !strncmp(c, "move@", 5)) {
            e->kind = c[0] == 'c' ? 1 : c[0] == 'r' ? 2 : 3;
            c = strchr(c, '@') + 1;
            e->x = atoi(c);
            e->y = atoi(strchr(c, ';') ? strchr(c, ';') + 1 : c);
            if (e->kind != 3 && g_nev + 1 < 256) { /* a click: the button is held for 150 ms, as by a hand */
                Ev *u = &g_ev[++g_nev];
                *u = *e;
                u->kind = 4;
                u->ms = e->ms + 150;
            }
        } else {
            /* "key": pressed and released 100 ms later; "key:down" or "key:up": only that */
            char *u = strchr(c, ':');
            int only = 0;
            if (u) {
                *u = 0;
                only = strcmp(u + 1, "up") == 0 ? 2 : 1;
            }
            e->code = key_code(c);
            e->up = only == 2;
            if (only == 0 && g_nev + 1 < 256) {
                Ev *r = &g_ev[++g_nev];
                *r = *e;
                r->up = 1;
                r->ms = e->ms + 100;
            }
        }
        g_nev++;
    }
    /* in time order (a click's release may come after later events) */
    {
        int i, j;
        for (i = 1; i < g_nev; i++)
            for (j = i; j > 0 && g_ev[j - 1].ms > g_ev[j].ms; j--) {
                Ev t = g_ev[j];
                g_ev[j] = g_ev[j - 1];
                g_ev[j - 1] = t;
            }
    }
}

/* the events due by `ms` (from the start), given to the game */
void rt_script_poll(uint32_t ms)
{
    while (g_iev < g_nev && g_ev[g_iev].ms <= ms) {
        Ev *e = &g_ev[g_iev++];
        if (e->kind == 0) {
            rt_key_event(e->code, !e->up);
        } else if (e->kind == 5) {
            rt_mouse_wheel(e->x);
        } else {
            rt_mouse_move(rt_mouse_window_x(e->x), e->y); /* on a wide menu: of the game's picture in it */
            if (e->kind == 1)
                rt_mouse_buttons(1);
            else if (e->kind == 2)
                rt_mouse_buttons(2);
            else if (e->kind == 4)
                rt_mouse_buttons(0);
        }
    }
}
