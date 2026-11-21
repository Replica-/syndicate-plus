/* hooks.h - additions to the original game, hooked onto functions of the recompiled programs (hooks.c). */
#pragma once

#include <stdint.h>

#include "../rt/recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the game programs (each has its own addresses) */
enum {
    GAME_NONE,  /* the launcher, the intros */
    GAME_SYN,   /* SYNDICAT\MAIN.EXE: Syndicate in English, French, Italian */
    GAME_SYN_S, /* SYNDICAT\SMAIN.EXE: Spanish */
    GAME_SYN_G, /* SYNDICAT\GMAIN.EXE: German */
    GAME_AR,    /* DATADISK\MAIN.EXE: American Revolt */
    GAME_AR_S,
    GAME_AR_G
};

/* a hook: called on entry of the function at `addr` of program `game`, with the guest's registers and stack as
 * at the call; returns 1 when it did the function's work itself (and popped the return address), 0 to let the
 * function run */
typedef struct HookDef {
    int game;
    uint32_t addr;
    int (*fn)(uint32_t addr);
} HookDef;

/* additions.c: every hook, ended by one with fn == NULL */
extern const HookDef g_hooks[];

/* game.c */
int game_of_module(const RtModuleDef *def);
/* the GAME_* of the running program */
int game_current(void);
/* the game ends (the census file is written) */
void game_hooks_end(void);

#ifdef __cplusplus
}
#endif
