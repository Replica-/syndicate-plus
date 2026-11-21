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

/* a hook: called on entry of a function of the running program, with the guest's registers and stack as at the
 * call; returns 1 when it did the function's work itself (and popped the return address), 0 to let the function
 * run */
typedef int (*HookFn)(uint32_t addr);

/* hooks.c: hooks `addr` of the running program */
void hook_add(uint32_t addr, HookFn fn);
/* additions.c: adds the hooks of program `game` (GAME_*) */
void additions_install(int game);

/* game.c */
int game_of_module(const RtModuleDef *def);
/* GameOptions.cheats ("" when none) */
const char *game_cheats(void);
/* the GAME_* of the running program */
int game_current(void);
/* the game ends (the census file is written) */
void game_hooks_end(void);

#ifdef __cplusplus
}
#endif
