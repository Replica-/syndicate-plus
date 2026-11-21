/* music.c - songs from files in place of the game's FM music, and the music and sound volumes.
 *
 * A folder may hold files for the game's songs (music_set_dir): intro, assassinate (the city), danger (combat),
 * game_completed, mission_failed and mission_completed, as .mp3, .ogg, .flac or .wav.  When the game starts one
 * of its XMIDI sequences (audio.c sees the driver call), the file of that song plays instead and the FM music is
 * silenced; songs without a file play as before.  Each song resumes where it stopped (the city song after a
 * fight), with a short fade in.  Everything here runs on the game thread, except rt_set_volumes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"
#include "rt.h"

#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "../third_party/dr_flac.h"
#include "../third_party/dr_mp3.h"
#include "../third_party/dr_wav.h"
#undef STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
/* stb_vorbis leaves these short names defined */
#undef L
#undef C
#undef R

static const char *const k_song_names[MUSIC_NB_SONGS] = {"intro",          "assassinate",    "danger",
                                                         "game_completed", "mission_failed", "mission_completed"};

typedef enum { DEC_NONE, DEC_MP3, DEC_FLAC, DEC_WAV, DEC_OGG } DecKind;

typedef struct Track {
    char path[1100];
    DecKind kind;
    void *dec;          /* drmp3 *, drflac *, drwav *, stb_vorbis * */
    uint32_t rate;
    int channels;
    int16_t buf[4096 * 2]; /* decoded stereo frames */
    int buf_n, buf_i;
    int16_t prev[2], cur[2];
    double phase;
    int ended;
} Track;

static Track g_tracks[MUSIC_NB_SONGS];
static int g_song = MUSIC_NONE;
static int g_loop;
static uint32_t g_fade_total, g_fade_left; /* output frames */
static volatile int g_vol_music = 100, g_vol_sound = 100;

void rt_set_volumes(int music, int sound)
{
    g_vol_music = music < 0 ? 0 : music > 100 ? 100 : music;
    g_vol_sound = sound < 0 ? 0 : sound > 100 ? 100 : sound;
}

int music_volume(void)
{
    return g_vol_music;
}

int sound_volume(void)
{
    return g_vol_sound;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

void music_set_dir(const char *dir)
{
    static const char *const exts[] = {"mp3", "ogg", "flac", "wav"};
    static const DecKind kinds[] = {DEC_MP3, DEC_OGG, DEC_FLAC, DEC_WAV};
    int s, e;
    for (s = 0; s < MUSIC_NB_SONGS; s++) {
        Track *t = &g_tracks[s];
        t->kind = DEC_NONE;
        if (dir == NULL || dir[0] == '\0') continue;
        for (e = 0; e < 4; e++) {
            snprintf(t->path, sizeof t->path, "%s/%s.%s", dir, k_song_names[s], exts[e]);
            if (file_exists(t->path)) {
                t->kind = kinds[e];
                rt_log("music: %s is played from %s", k_song_names[s], t->path);
                break;
            }
        }
    }
}

static void track_close(Track *t)
{
    if (t->dec == NULL) return;
    switch (t->kind) {
    case DEC_MP3: drmp3_uninit((drmp3 *)t->dec); free(t->dec); break;
    case DEC_FLAC: drflac_close((drflac *)t->dec); break;
    case DEC_WAV: drwav_uninit((drwav *)t->dec); free(t->dec); break;
    case DEC_OGG: stb_vorbis_close((stb_vorbis *)t->dec); break;
    default: break;
    }
    t->dec = NULL;
}

static int track_open(Track *t)
{
    if (t->dec != NULL) return 1;
    switch (t->kind) {
    case DEC_MP3: {
        drmp3 *d = (drmp3 *)malloc(sizeof *d);
        if (d == NULL || !drmp3_init_file(d, t->path, NULL)) {
            free(d);
            break;
        }
        t->dec = d;
        t->rate = d->sampleRate;
        t->channels = (int)d->channels;
        break;
    }
    case DEC_FLAC: {
        drflac *d = drflac_open_file(t->path, NULL);
        if (d == NULL) break;
        t->dec = d;
        t->rate = d->sampleRate;
        t->channels = d->channels;
        break;
    }
    case DEC_WAV: {
        drwav *d = (drwav *)malloc(sizeof *d);
        if (d == NULL || !drwav_init_file(d, t->path, NULL)) {
            free(d);
            break;
        }
        t->dec = d;
        t->rate = d->sampleRate;
        t->channels = d->channels;
        break;
    }
    case DEC_OGG: {
        int err = 0;
        stb_vorbis *d = stb_vorbis_open_filename(t->path, &err, NULL);
        stb_vorbis_info info;
        if (d == NULL) break;
        info = stb_vorbis_get_info(d);
        t->dec = d;
        t->rate = info.sample_rate;
        t->channels = info.channels;
        break;
    }
    default:
        return 0;
    }
    if (t->dec == NULL || t->rate == 0 || t->channels <= 0) {
        rt_log("music: cannot play %s", t->path);
        track_close(t);
        t->kind = DEC_NONE;
        return 0;
    }
    t->buf_n = t->buf_i = 0;
    t->phase = 0.0;
    t->ended = 0;
    memset(t->prev, 0, sizeof t->prev);
    memset(t->cur, 0, sizeof t->cur);
    return 1;
}

static void track_rewind(Track *t)
{
    switch (t->kind) {
    case DEC_MP3: drmp3_seek_to_pcm_frame((drmp3 *)t->dec, 0); break;
    case DEC_FLAC: drflac_seek_to_pcm_frame((drflac *)t->dec, 0); break;
    case DEC_WAV: drwav_seek_to_pcm_frame((drwav *)t->dec, 0); break;
    case DEC_OGG: stb_vorbis_seek_start((stb_vorbis *)t->dec); break;
    default: break;
    }
    t->buf_n = t->buf_i = 0;
    t->ended = 0;
}

/* decodes the next frames into t->buf (as stereo); 0 at the end of the file */
static int track_decode(Track *t)
{
    static int16_t raw[4096 * 8];
    int max = (int)(sizeof raw / sizeof raw[0]) / t->channels, n = 0, i;
    if (max > 4096) max = 4096;
    switch (t->kind) {
    case DEC_MP3: n = (int)drmp3_read_pcm_frames_s16((drmp3 *)t->dec, (drmp3_uint64)max, raw); break;
    case DEC_FLAC: n = (int)drflac_read_pcm_frames_s16((drflac *)t->dec, (drflac_uint64)max, raw); break;
    case DEC_WAV: n = (int)drwav_read_pcm_frames_s16((drwav *)t->dec, (drwav_uint64)max, raw); break;
    case DEC_OGG:
        n = stb_vorbis_get_samples_short_interleaved((stb_vorbis *)t->dec, t->channels, raw, max * t->channels);
        break;
    default: break;
    }
    for (i = 0; i < n; i++) {
        if (t->channels == 1) {
            t->buf[i * 2] = t->buf[i * 2 + 1] = raw[i];
        } else {
            t->buf[i * 2] = raw[i * t->channels];
            t->buf[i * 2 + 1] = raw[i * t->channels + 1];
        }
    }
    t->buf_n = n;
    t->buf_i = 0;
    return n > 0;
}

/* the next frame of the file, at the file's rate; 0 when the song is over */
static int track_next(Track *t, int16_t out[2])
{
    if (t->buf_i >= t->buf_n && !track_decode(t)) {
        if (!g_loop) return 0;
        track_rewind(t);
        if (!track_decode(t)) return 0;
    }
    out[0] = t->buf[t->buf_i * 2];
    out[1] = t->buf[t->buf_i * 2 + 1];
    t->buf_i++;
    return 1;
}

int music_has_file(int song)
{
    return song >= 0 && song < MUSIC_NB_SONGS && g_tracks[song].kind != DEC_NONE;
}

void music_play(int song, int from_start)
{
    Track *t;
    int resume;
    if (song == g_song && !from_start) return;
    g_song = MUSIC_NONE;
    if (!music_has_file(song)) return;
    t = &g_tracks[song];
    if (!track_open(t)) return;
    resume = !from_start && !t->ended && (t->buf_n > 0 || t->phase > 0.0);
    if (!resume) track_rewind(t);
    g_song = song;
    /* the city and combat songs loop, the others play once */
    g_loop = song == MUSIC_ASSASSINATE || song == MUSIC_DANGER;
    /* a short fade in when a song starts, a longer one when it comes back */
    g_fade_total = g_fade_left = (uint32_t)host_audio_rate() * (resume ? 1500u : 500u) / 1000u;
    rt_log("music: %s (%s)", k_song_names[song], resume ? "resumed" : "from the start");
}

void music_stop(void)
{
    g_song = MUSIC_NONE;
}

int music_playing(void)
{
    return g_song != MUSIC_NONE;
}

void music_mix(int32_t *out, int n, int rate)
{
    Track *t;
    double step;
    int i, vol = g_vol_music;
    if (g_song == MUSIC_NONE) return;
    t = &g_tracks[g_song];
    step = (double)t->rate / rate;
    for (i = 0; i < n; i++) {
        int32_t l, r;
        t->phase += step;
        while (t->phase >= 1.0) {
            t->prev[0] = t->cur[0];
            t->prev[1] = t->cur[1];
            if (!track_next(t, t->cur)) {
                t->ended = 1;
                g_song = MUSIC_NONE;
                return;
            }
            t->phase -= 1.0;
        }
        l = t->prev[0] + (int32_t)((t->cur[0] - t->prev[0]) * t->phase);
        r = t->prev[1] + (int32_t)((t->cur[1] - t->prev[1]) * t->phase);
        if (g_fade_left > 0) {
            uint32_t done = g_fade_total - g_fade_left--;
            l = (int32_t)((int64_t)l * done / g_fade_total);
            r = (int32_t)((int64_t)r * done / g_fade_total);
        }
        out[i * 2] += l * vol / 100;
        out[i * 2 + 1] += r * vol / 100;
    }
}

/* ---- following the game's music driver ---- */
extern const RtModuleDef drv_fm_module;

/* GAMEFM.DLL, the AIL/32 XMIDI driver (the same file in both games): the functions the songs follow, at their
 * link addresses (from the driver's index of AIL function numbers, at file offset 0xE2F) */
#define DRV_REGISTER_SEQUENCE 0x13593u /* 151 (driver, file image, sequence number, state) -> handle */
#define DRV_START_SEQUENCE 0x136d1u    /* 170 (driver, handle): from the start */
#define DRV_STOP_SEQUENCE 0x1372bu     /* 171 (driver, handle): stops where it is */
#define DRV_RESUME_SEQUENCE 0x13779u   /* 173 (driver, handle): goes on from there */

static struct {
    uint32_t handle;
    int song;
} g_handles[16];
static int g_nhandles;
static uint32_t g_reg_seq;

int music_is_fm_driver(const RtFunc *f)
{
    return f >= drv_fm_module.funcs && f < drv_fm_module.funcs + drv_fm_module.nfuncs;
}

static int handle_song(uint32_t handle)
{
    int i;
    for (i = 0; i < g_nhandles; i++)
        if (g_handles[i].handle == handle) return g_handles[i].song;
    return MUSIC_NONE;
}

void music_driver_call(uint32_t fn, const char *exe, int after)
{
    static uint32_t arg2, arg3; /* the arguments after the driver's, read before the call */
    int song, i;
    if (!after) {
        arg2 = RD32(R.esp + 8u); /* above the return address and the driver */
        arg3 = RD32(R.esp + 12u);
        return;
    }
    switch (fn) {
    case DRV_REGISTER_SEQUENCE:
        /* (file image, sequence number) -> the handle in eax.  INTRO.EXE plays INTRO.XMI, the games
         * SYNGAME.XMI. */
        if (strstr(exe, "INTRO") != NULL)
            song = arg3 == 0 ? MUSIC_INTRO : MUSIC_NONE;
        else
            song = arg3 + 1 < MUSIC_NB_SONGS ? (int)arg3 + 1 : MUSIC_NONE;
        for (i = 0; i < g_nhandles && g_handles[i].handle != R.eax; i++) {
        }
        if (i == 16) break;
        if (i == g_nhandles) g_nhandles++;
        g_handles[i].handle = R.eax;
        g_handles[i].song = song;
        break;
    case DRV_START_SEQUENCE:
    case DRV_RESUME_SEQUENCE:
        song = handle_song(arg2);
        if (song != MUSIC_NONE)
            music_play(song, fn == DRV_START_SEQUENCE);
        else
            music_stop();
        break;
    case DRV_STOP_SEQUENCE:
        if (handle_song(arg2) == g_song) music_stop();
        break;
    default:
        break;
    }
}

void music_shutdown(void)
{
    int s;
    for (s = 0; s < MUSIC_NB_SONGS; s++) track_close(&g_tracks[s]);
    g_song = MUSIC_NONE;
    g_nhandles = 0;
}
