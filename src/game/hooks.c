/* hooks.c - additions to the original game, hooked onto functions of the recompiled programs (RT_ENTER in
 * src/rt/recomp.h).  Each hook names the program it belongs to: the programs of the other languages and of
 * American Revolt have their own addresses.
 *
 * With the environment variable SYND_CENSUS=FILE every function is hooked and, when the program ends, FILE gets
 * one line per function called: its address, the time of its first call (ms from the start) and how often it was
 * called.  This finds the functions behind a screen or an event (development only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rt/rt.h"
#include "hooks.h"

uint8_t rt_hook_bits[RT_HOOK_SPAN / 8u];

static const RtModuleDef *g_def;
static int g_game; /* GAME_* of the running program */
static struct {
    uint32_t addr;
    HookFn fn;
} g_table[64];
static int g_nhooks;

/* ---- census ---- */
static const char *g_census_file;
static uint32_t *g_census_first, *g_census_count;
static uint64_t g_census_start;

static void census_dump(void)
{
    FILE *f;
    uint32_t i;
    if (g_census_first == NULL || g_def == NULL) return;
    f = fopen(g_census_file, "a");
    if (f != NULL) {
        fprintf(f, "# %s\n", g_def->exe_name);
        for (i = 0; i < RT_HOOK_SPAN; i++)
            if (g_census_count[i]) fprintf(f, "%06x %u %u\n", RT_HOOK_BASE + i, g_census_first[i], g_census_count[i]);
        fclose(f);
    }
    memset(g_census_first, 0, sizeof(uint32_t) * RT_HOOK_SPAN);
    memset(g_census_count, 0, sizeof(uint32_t) * RT_HOOK_SPAN);
}

static int census(uint32_t addr)
{
    uint32_t i = addr - RT_HOOK_BASE;
    if (g_census_count[i]++ == 0) g_census_first[i] = (uint32_t)((host_time_us() - g_census_start) / 1000u);
    return 0;
}

/* ---- the hooks of each program ---- */
void hook_add(uint32_t addr, HookFn fn)
{
    uint32_t i = addr - RT_HOOK_BASE;
    if (i >= RT_HOOK_SPAN || g_nhooks == (int)(sizeof g_table / sizeof g_table[0])) {
        rt_log("hooks: cannot hook %08x", addr);
        return;
    }
    rt_hook_bits[i >> 3] |= (uint8_t)(1u << (i & 7u));
    g_table[g_nhooks].addr = addr;
    g_table[g_nhooks].fn = fn;
    g_nhooks++;
}

void game_hooks_install(const RtModuleDef *def)
{
    if (g_census_file == NULL && getenv("SYND_CENSUS") != NULL) {
        g_census_file = getenv("SYND_CENSUS");
        g_census_first = (uint32_t *)calloc(RT_HOOK_SPAN, sizeof(uint32_t));
        g_census_count = (uint32_t *)calloc(RT_HOOK_SPAN, sizeof(uint32_t));
        g_census_start = host_time_us();
        if (g_census_first == NULL || g_census_count == NULL) g_census_file = NULL;
    }
    if (g_census_file != NULL) census_dump(); /* the program that ran until now */
    g_def = def;
    g_game = game_of_module(def);
    memset(rt_hook_bits, 0, sizeof rt_hook_bits);
    g_nhooks = 0;
    if (g_census_file != NULL) {
        memset(rt_hook_bits, 0xFF, sizeof rt_hook_bits);
        return;
    }
    additions_install(g_game);
}

void game_hooks_end(void)
{
    if (g_census_file != NULL) census_dump();
}

int rt_hook(uint32_t addr)
{
    int i;
    if (g_census_file != NULL) return census(addr);
    for (i = 0; i < g_nhooks; i++)
        if (g_table[i].addr == addr) return g_table[i].fn(addr);
    return 0;
}

int game_current(void)
{
    return g_game;
}
