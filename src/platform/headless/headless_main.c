/* headless_main.c - a test front end without a window: runs the game on the main thread, writes chosen
 * frames as BMP files, feeds scripted input, and stops after a time limit.  Used for development and
 * regression checks (tests/).
 *
 *   synd_headless --game-dir DIR [--user-dir DIR] [--start "SYNDICAT\MAIN.EXE /args"] [--seconds N]
 *                 [--dump DIR] [--dump-every MS] [--input "ms:key[:up],..."] [--verbose N] [--log FILE]
 *                 [--wav FILE] [--virtual-time STEP_US] [--record DIR] [--music-dir DIR]
 *                 [--mem-dump MS:FILE]... [--cheats "COMPANY:PLAYER"] [--cycles N] [--fps]
 *
 * The frames are 32-bit BMP files (true colour, with what this version draws over the game).
 *
 * --virtual-time runs on a virtual clock (each clock reading advances it by STEP_US microseconds, a sleep by
 * its length): runs repeat exactly and go as fast as the machine allows; times in --seconds/--input are then
 * virtual.
 *
 * events in --input ("ms:event", comma separated, ms from the start): a key, held for 100 ms, as a scancode in
 * hex (1c = Enter) or a name (enter, esc, space, up, down, left, right, f1..f10, some letters and digits);
 * "key:down" / "key:up" to press or release only; "click@x;y" / "rclick@x;y" (held 150 ms) and "move@x;y"
 * with x, y fractions 0..65535 of the picture.
 *
 * --cycles N limits the emulated CPU to N instructions a ms, as the window does (8000; without it the game runs as
 * fast as it can, as the tests want); --fps shows the frames a second at the top right.
 *
 * --record DIR records the run as an MP4 video into DIR (src/video/record.c, needs ffmpeg).  --music-dir plays
 * the songs from the files of DIR (src/rt/music.c); without it the FM music plays, so test runs do not depend on
 * the music folder of the game.  --mem-dump writes the guest's memory from 4 MiB to 16 MiB (the program's image
 * , its data and what it allocates) to FILE at MS (to find where the game keeps something: dump before and after it changes). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#define strtok_s strtok_r /* the same function under its POSIX name */
#endif

#include "../../game/game.h"
#include "../../rt/host.h"
#include "../../video/record.h"

extern FILE *g_logfile;
extern uint32_t g_virtual_step; /* win32_sys.c / posix_sys.c */
static const char *g_dump_dir;
static uint32_t g_dump_every = 1000;
static uint64_t g_start_us, g_end_us, g_next_dump;
static int g_dump_n;

typedef struct Ev {
    uint32_t ms;
    int kind; /* 0 key, 1 click, 2 right click, 3 move, 4 buttons up (the end of a click, 150 ms later) */
    uint32_t code;
    int up;
    int x, y;
} Ev;
static Ev g_ev[256];
static struct {
    uint32_t ms;
    const char *file;
} g_dumps[16];
static int g_ndumps;
static int g_nev, g_iev;

int host_audio_rate(void)
{
    return 44100;
}

/* --wav FILE: the audio output as a 16-bit stereo WAV file */
static FILE *g_wav;
static uint32_t g_wav_frames;

static void wav_header(FILE *f, uint32_t frames)
{
    uint8_t h[44];
    uint32_t data = frames * 4u, rate = 44100u;
    memcpy(h, "RIFF", 4);
    h[4] = (uint8_t)(data + 36u); h[5] = (uint8_t)((data + 36u) >> 8); h[6] = (uint8_t)((data + 36u) >> 16);
    h[7] = (uint8_t)((data + 36u) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = (uint8_t)rate; h[25] = (uint8_t)(rate >> 8); h[26] = (uint8_t)(rate >> 16); h[27] = 0;
    h[28] = (uint8_t)(rate * 4u); h[29] = (uint8_t)((rate * 4u) >> 8); h[30] = (uint8_t)((rate * 4u) >> 16); h[31] = 0;
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)data; h[41] = (uint8_t)(data >> 8); h[42] = (uint8_t)(data >> 16); h[43] = (uint8_t)(data >> 24);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

void host_audio_push(const int16_t *frames, size_t count)
{
    record_audio(frames, count);
    if (g_wav == NULL) return;
    fwrite(frames, 4, count, g_wav);
    g_wav_frames += (uint32_t)count;
}

/* a 32-bit BMP (bottom-up, 0x00RRGGBB as B, G, R, 0) */
static void write_bmp(const char *path, const uint32_t *pix, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int y;
    uint32_t size = 54u + (uint32_t)(w * h) * 4u;
    uint8_t hdr[54] = {'B', 'M'};
    if (f == NULL) return;
    hdr[2] = (uint8_t)size;
    hdr[3] = (uint8_t)(size >> 8);
    hdr[4] = (uint8_t)(size >> 16);
    hdr[5] = (uint8_t)(size >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (uint8_t)w;
    hdr[19] = (uint8_t)(w >> 8);
    hdr[22] = (uint8_t)h;
    hdr[23] = (uint8_t)(h >> 8);
    hdr[26] = 1;
    hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    for (y = h - 1; y >= 0; y--) fwrite(pix + y * w, 4, (size_t)w, f); /* little-endian: B, G, R, 0 */
    fclose(f);
}

void host_present(const uint32_t *pixels, int w, int h)
{
    uint64_t now = host_time_us();
    record_frame(pixels, w, h, now);
    if (g_dump_dir != NULL && now >= g_next_dump) {
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%05u.bmp", g_dump_dir, (unsigned)((now - g_start_us) / 1000u));
        write_bmp(path, pixels, w, h);
        g_next_dump = now + (uint64_t)g_dump_every * 1000u;
        g_dump_n++;
    }
}

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

static void parse_input(const char *s)
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
        if (!strncmp(c, "click@", 6) || !strncmp(c, "rclick@", 7) || !strncmp(c, "move@", 5)) {
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

/* --builtin FILE: the game files come from a pack file (as the embedded files in syndicate64.exe do) */
static uint8_t *g_pack;
static size_t g_pack_size;

const uint8_t *host_game_pack(size_t *size)
{
    *size = g_pack_size;
    return g_pack;
}

static int load_pack(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_pack = (uint8_t *)malloc((size_t)n);
    if (g_pack == NULL || fread(g_pack, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return 0;
    }
    fclose(f);
    g_pack_size = (size_t)n;
    return 1;
}

void host_fatal(const char *text)
{
    fprintf(stderr, "fatal: %s\n", text);
    exit(3);
}

int host_should_quit(void)
{
    uint64_t now = host_time_us();
    uint32_t ms = (uint32_t)((now - g_start_us) / 1000u);
    while (g_iev < g_nev && g_ev[g_iev].ms <= ms) {
        Ev *e = &g_ev[g_iev++];
        if (e->kind == 0) {
            rt_key_event(e->code, !e->up);
        } else {
            rt_mouse_move(e->x, e->y);
            if (e->kind == 1)
                rt_mouse_buttons(1);
            else if (e->kind == 2)
                rt_mouse_buttons(2);
            else if (e->kind == 4)
                rt_mouse_buttons(0);
        }
    }
    {
        int k;
        for (k = 0; k < g_ndumps; k++) {
            if (g_dumps[k].file != NULL && ms >= g_dumps[k].ms) {
                extern uint8_t *M;
                FILE *f = fopen(g_dumps[k].file, "wb");
                if (f != NULL) {
                    fwrite(M + 0x400000u, 1, 0xC00000u, f);
                    fclose(f);
                }
                g_dumps[k].file = NULL;
            }
        }
    }
    return g_end_us != 0 && now >= g_end_us;
}

int main(int argc, char **argv)
{
    GameOptions opt;
    int i, rc;
    double seconds = 0;
    memset(&opt, 0, sizeof opt);
    opt.game_dir = "_references/dos";
    opt.user_dir = "out/user";
    opt.music_dir = "";
    g_logfile = stdout;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--game-dir")) opt.game_dir = v, i++;
        else if (!strcmp(a, "--builtin")) {
            if (!load_pack(v)) {
                fprintf(stderr, "cannot read %s\n", v);
                return 2;
            }
            opt.game_dir = NULL;
            i++;
        }
        else if (!strcmp(a, "--user-dir")) opt.user_dir = v, i++;
        else if (!strcmp(a, "--start")) opt.start = v, i++;
        else if (!strcmp(a, "--seconds")) seconds = atof(v), i++;
        else if (!strcmp(a, "--dump")) g_dump_dir = v, host_mkdir(v), i++;
        else if (!strcmp(a, "--dump-every")) g_dump_every = (uint32_t)atoi(v), i++;
        else if (!strcmp(a, "--wav")) {
            g_wav = fopen(v, "wb");
            if (g_wav != NULL) wav_header(g_wav, 0);
            i++;
        }
        else if (!strcmp(a, "--input")) parse_input(v), i++;
        else if (!strcmp(a, "--virtual-time")) g_virtual_step = (uint32_t)atoi(v), i++;
        else if (!strcmp(a, "--verbose")) opt.verbose = atoi(v), i++;
        else if (!strcmp(a, "--log")) g_logfile = fopen(v, "w"), i++;
        else if (!strcmp(a, "--music-dir")) opt.music_dir = v, i++;
        else if (!strcmp(a, "--cheats")) opt.cheats = v, i++;
        else if (!strcmp(a, "--cycles")) rt_set_cpu_speed(atoi(v)), i++;
        else if (!strcmp(a, "--fps")) rt_set_show_fps(1);
        else if (!strcmp(a, "--mem-dump") && g_ndumps < 16 && strchr(v, ':')) {
            g_dumps[g_ndumps].ms = (uint32_t)atoi(v);
            g_dumps[g_ndumps].file = strchr(v, ':') + 1;
            g_ndumps++;
            i++;
        }
        else if (!strcmp(a, "--record")) {
            if (!record_start(v, 44100)) {
                fprintf(stderr, "cannot record (is ffmpeg in the PATH?)\n");
                return 2;
            }
            i++;
        }
        else {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }
    opt.fixed_clock = g_virtual_step != 0; /* a virtual-time run repeats exactly */
    g_start_us = host_time_us();
    g_end_us = seconds > 0 ? g_start_us + (uint64_t)(seconds * 1e6) : 0;
    rc = game_main(&opt);
    if (g_wav != NULL) {
        wav_header(g_wav, g_wav_frames);
        fclose(g_wav);
    }
    if (record_active()) {
        record_stop();
        record_wait();
        fprintf(stderr, "video: %s\n", record_file());
    }
    fprintf(stderr, "exit %d after %u ms, %d frames dumped\n", rc,
            (unsigned)((host_time_us() - g_start_us) / 1000u), g_dump_n);
    return rc;
}
