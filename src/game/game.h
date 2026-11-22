/* game.h - the Syndicate Plus program: which recompiled DOS programs it contains and how it starts. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameOptions {
    const char *game_dir;  /* folder with SYND.EXE, DATA, SYNDICAT, DATADISK */
    const char *user_dir;  /* writable folder (saves, setup, files the game writes) */
    const char *start;     /* NULL: the original launcher (SYND.EXE); else a DOS command line to run first */
    int verbose;
    int fixed_clock;       /* the guest's date and time start at a fixed point (repeatable test runs) */
    const char *music_dir; /* songs played from files instead of the FM music (src/rt/music.c); NULL: the
                            * "music" folder of the game folder, when there is one */
    const char *cheats;    /* "COMPANY:PLAYER": names given to the player's company and the player when the world
                            * map first comes up, so that their cheats apply (NULL: none) */
} GameOptions;

/* run the game on the calling thread until it exits or the host asks to quit; returns the exit code */
int game_main(const GameOptions *opt);

/* check that `dir` holds the original game files this program was built from; returns NULL when it does,
 * else a sentence describing what is missing or different */
const char *game_check_dir(const char *dir);

/* the player's company is shown in this colour (0xFFRRGGBB: its agents' hats in missions); 0: the game's own
 * (any thread) */
void game_set_player_colour(uint32_t rgb);
/* the squad's move orders walked as routes over the map's floors when the game's straight walk cannot do them
 * (on by default) */
void game_set_routes(int on);

#ifdef __cplusplus
}
#endif
