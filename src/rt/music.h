/* music.h - songs from files in place of the game's FM music (music.c), and the volumes. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the game's songs: INTRO.XMI's, then SYNGAME.XMI's (sequence number + 1) */
enum {
    MUSIC_NONE = -1,
    MUSIC_INTRO = 0,
    MUSIC_ASSASSINATE,       /* SYNGAME.XMI 0: the city */
    MUSIC_DANGER,            /* 1: rival agents near ours */
    MUSIC_GAME_COMPLETED,    /* 2 */
    MUSIC_MISSION_FAILED,    /* 3 */
    MUSIC_MISSION_COMPLETED, /* 4 */
    MUSIC_NB_SONGS
};

/* the folder of the song files (NULL or "" for none) */
void music_set_dir(const char *dir);
int music_has_file(int song);
/* plays the song's file (when it has one), from the start or from where it stopped; MUSIC_NONE or a song
 * without a file stops the file being played */
void music_play(int song, int from_start);
void music_stop(void);
/* 1 while a file plays (the FM music is then silent) */
int music_playing(void);
/* adds `n` stereo frames of the song at `rate` to `out` */
void music_mix(int32_t *out, int n, int rate);
void music_shutdown(void);

/* the calls into the game's FM music driver (core.c): `f` is the function called, music_driver_call() is called
 * before (after = 0) and after it with the guest's stack as at the call */
#define MUSIC_DRV_FIRST 0x13593u
#define MUSIC_DRV_END 0x137c5u
struct RtFunc;
int music_is_fm_driver(const struct RtFunc *f);
void music_driver_call(uint32_t fn, const char *exe, int after);

/* 0..100 (rt_set_volumes in host.h sets them) */
int music_volume(void);
int sound_volume(void);

#ifdef __cplusplus
}
#endif
