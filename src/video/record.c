/* record.c - the video recorder (record.h): ffmpeg through a pipe, fed from a thread of its own. */
#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "../third_party/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK Lock;
typedef CONDITION_VARIABLE Cond;
typedef HANDLE Thread;
#define LOCK_INIT SRWLOCK_INIT
#define lock(l) AcquireSRWLockExclusive(l)
#define unlock(l) ReleaseSRWLockExclusive(l)
#define cond_wait(c, l, ms) SleepConditionVariableSRW(c, l, ms, 0)
#define cond_signal(c) WakeConditionVariable(c)
#define NULL_DEVICE "NUL"
#else
#include <pthread.h>
#include <sys/stat.h>
typedef pthread_mutex_t Lock;
typedef pthread_cond_t Cond;
typedef pthread_t Thread;
#define LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)
#define cond_signal(c) pthread_cond_signal(c)
#define NULL_DEVICE "/dev/null"
static void cond_wait(Cond *c, Lock *l, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(c, l, &ts);
}
#endif

#define VID_W 640
#define VID_H 480
#define MAX_QUEUED 20                  /* frames waiting for ffmpeg; more and frames are skipped */
#define MAX_REPEAT (RECORD_FPS * 10)   /* a frame is repeated at most this often (after a long pause) */
#define SOUND_CHUNK (44100 * 4)

typedef struct Frame {
    uint8_t rgb[VID_W * VID_H * 3];
    int count;
} Frame;

static Lock g_lock = LOCK_INIT;
#ifdef _WIN32
static Cond g_wake = CONDITION_VARIABLE_INIT;
#else
static Cond g_wake = PTHREAD_COND_INITIALIZER;
#endif
static volatile int g_recording;
static int g_stopping;
static char g_out[1100], g_video[1100], g_sound[1100];
static int g_rate;
static FILE *g_pipe, *g_sound_file;
static Thread g_writer;
static int g_writer_on;
static uint64_t g_start_us;
static int64_t g_nframes;
static Frame *g_queue[MAX_QUEUED];
static int g_nqueue;
static uint8_t *g_snd;
static size_t g_snd_n, g_snd_cap;

/* ---- the platform's bits: running a command, making a folder, a thread ---- */
#ifdef _WIN32
static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (w != NULL) {
        if (n <= 0)
            w[0] = 0;
        else
            MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    }
    return w;
}

/* cmd.exe wants the whole command in quotes when its program's arguments have quotes */
static FILE *run(const char *command, const char *mode)
{
    size_t n = strlen(command) + 3;
    char *q = (char *)malloc(n);
    wchar_t *w, *wm;
    FILE *f = NULL;
    if (q == NULL) return NULL;
    snprintf(q, n, "\"%s\"", command);
    w = wide(q);
    wm = wide(mode);
    if (w != NULL && wm != NULL) f = _wpopen(w, wm);
    free(w);
    free(wm);
    free(q);
    return f;
}
#define finish(f) _pclose(f)

static int system_u8(const char *command)
{
    FILE *f = run(command, "r");
    char buf[256];
    if (f == NULL) return -1;
    while (fgets(buf, sizeof buf, f) != NULL) {
    }
    return finish(f);
}

static FILE *open_u8(const char *path, const char *mode)
{
    wchar_t *w = wide(path), *wm = wide(mode);
    FILE *f = (w != NULL && wm != NULL) ? _wfopen(w, wm) : NULL;
    free(w);
    free(wm);
    return f;
}

static void remove_u8(const char *path)
{
    wchar_t *w = wide(path);
    if (w != NULL) _wremove(w);
    free(w);
}

static int rename_u8(const char *from, const char *to)
{
    wchar_t *a = wide(from), *b = wide(to);
    int ok = a != NULL && b != NULL && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    free(a);
    free(b);
    return ok;
}

static void mkdir_u8(const char *path)
{
    wchar_t *w = wide(path);
    if (w != NULL) CreateDirectoryW(w, NULL);
    free(w);
}

static DWORD WINAPI writer_main(LPVOID unused);
static DWORD WINAPI finisher_main(LPVOID job);
static void start_thread(Thread *t, int writer, void *arg)
{
    *t = CreateThread(NULL, 0, writer ? writer_main : finisher_main, arg, 0, NULL);
}
static void join_thread(Thread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
static FILE *run(const char *command, const char *mode)
{
    return popen(command, mode);
}
#define finish(f) pclose(f)
static int system_u8(const char *command)
{
    return system(command);
}
#define open_u8 fopen
#define remove_u8 remove
static int rename_u8(const char *from, const char *to)
{
    return rename(from, to) == 0;
}
static void mkdir_u8(const char *path)
{
    mkdir(path, 0755);
}
static void *writer_main(void *unused);
static void *finisher_main(void *job);
static void start_thread(Thread *t, int writer, void *arg)
{
    pthread_create(t, NULL, writer ? writer_main : finisher_main, arg);
}
static void join_thread(Thread t)
{
    pthread_join(t, NULL);
}
#endif

/* ---- the videos being finished ---- */
typedef struct Job {
    FILE *pipe;
    int with_sound, rate;
    char out[1100], video[1100], sound[1100];
} Job;
#define MAX_JOBS 16
static Thread g_jobs[MAX_JOBS];
static int g_njobs;

static void job_run(Job *j)
{
    int video_ok = finish(j->pipe) == 0;
    if (!video_ok) {
        fprintf(stderr, "record: ffmpeg could not encode %s\n", j->video);
    } else if (!j->with_sound) {
        rename_u8(j->video, j->out);
    } else {
        char cmd[4000];
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -y -loglevel error -i \"%s\" -f s16le -ar %d -ac 2 -i \"%s\" -c:v copy -c:a aac -b:a 192k "
                 "-shortest \"%s\" 2>" NULL_DEVICE,
                 j->video, j->rate, j->sound, j->out);
        if (system_u8(cmd) == 0) {
            remove_u8(j->video);
            remove_u8(j->sound);
        }
    }
    free(j);
}

#ifdef _WIN32
static DWORD WINAPI finisher_main(LPVOID job)
{
    job_run((Job *)job);
    return 0;
}
#else
static void *finisher_main(void *job)
{
    job_run((Job *)job);
    return NULL;
}
#endif

/* ---- the writer ---- */
static void writer_loop(void)
{
    Frame *frames[MAX_QUEUED];
    int n, i, k, failed = 0;
    uint8_t *snd = NULL;
    size_t snd_n = 0, snd_cap = 0;
    for (;;) {
        int stopping;
        lock(&g_lock);
        if (g_nqueue == 0 && !g_stopping) cond_wait(&g_wake, &g_lock, 100);
        n = g_nqueue;
        memcpy(frames, g_queue, sizeof(Frame *) * (size_t)n);
        g_nqueue = 0;
        /* the sound: swap the buffers */
        {
            uint8_t *t = snd;
            size_t c = snd_cap;
            snd = g_snd;
            snd_n = g_snd_n;
            snd_cap = g_snd_cap;
            g_snd = t;
            g_snd_cap = c;
            g_snd_n = 0;
        }
        stopping = g_stopping;
        unlock(&g_lock);
        if (g_sound_file != NULL && snd_n > 0) fwrite(snd, 1, snd_n, g_sound_file);
        for (i = 0; i < n; i++) {
            for (k = 0; k < frames[i]->count && !failed; k++)
                if (fwrite(frames[i]->rgb, 1, sizeof frames[i]->rgb, g_pipe) != sizeof frames[i]->rgb) failed = 1;
            free(frames[i]);
        }
        if (stopping) break;
    }
    free(snd);
}

#ifdef _WIN32
static DWORD WINAPI writer_main(LPVOID unused)
{
    (void)unused;
    writer_loop();
    return 0;
}
#else
static void *writer_main(void *unused)
{
    (void)unused;
    writer_loop();
    return NULL;
}
#endif

/* ---- the interface ---- */
int record_active(void)
{
    return g_recording;
}

const char *record_file(void)
{
    return g_out;
}

int record_start(const char *folder, int rate)
{
    char cmd[2000], base[1100];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (g_recording) return 1;
    if (system_u8("ffmpeg -version >" NULL_DEVICE " 2>&1") != 0) {
        fprintf(stderr, "record: ffmpeg was not found in the PATH\n");
        return 0;
    }
    { /* the folder and the ones above it */
        char dir[1100], *c;
        snprintf(dir, sizeof dir, "%s", folder);
        for (c = dir + 1; *c; c++) {
            if (*c == '/' || *c == '\\') {
                char sep = *c;
                *c = 0;
                if (c[-1] != ':') mkdir_u8(dir);
                *c = sep;
            }
        }
        mkdir_u8(dir);
    }
    snprintf(base, sizeof base, "%s/syndicate_%04d%02d%02d_%02d%02d%02d", folder, tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    snprintf(g_out, sizeof g_out, "%s.mp4", base);
    snprintf(g_video, sizeof g_video, "%s.video.mp4", base);
    snprintf(g_sound, sizeof g_sound, "%s.sound.raw", base);
    /* 640x480 in, twice that out with sharp pixels */
    snprintf(cmd, sizeof cmd,
             "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s %dx%d -framerate %d -i - "
             "-vf scale=%d:%d:flags=neighbor -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p \"%s\" 2>" NULL_DEVICE,
             VID_W, VID_H, RECORD_FPS, VID_W * 2, VID_H * 2, g_video);
    g_pipe = run(cmd, "wb");
    if (g_pipe == NULL) {
        fprintf(stderr, "record: cannot start ffmpeg\n");
        return 0;
    }
    g_sound_file = rate > 0 ? open_u8(g_sound, "wb") : NULL;
    g_rate = rate;
    g_nframes = 0;
    g_start_us = 0;
    g_nqueue = 0;
    g_snd_n = 0;
    g_stopping = 0;
    start_thread(&g_writer, 1, NULL);
    g_writer_on = 1;
    g_recording = 1;
    return 1;
}

void record_stop(void)
{
    Job *j;
    if (!g_recording) return;
    lock(&g_lock);
    g_recording = 0;
    g_stopping = 1;
    cond_signal(&g_wake);
    unlock(&g_lock);
    if (g_writer_on) join_thread(g_writer);
    g_writer_on = 0;
    if (g_sound_file != NULL) fclose(g_sound_file);
    j = (Job *)calloc(1, sizeof *j);
    if (j == NULL) return;
    j->pipe = g_pipe;
    j->with_sound = g_sound_file != NULL;
    j->rate = g_rate;
    g_pipe = NULL;
    g_sound_file = NULL;
    snprintf(j->out, sizeof j->out, "%s", g_out);
    snprintf(j->video, sizeof j->video, "%s", g_video);
    snprintf(j->sound, sizeof j->sound, "%s", g_sound);
    if (g_njobs == MAX_JOBS) record_wait();
    start_thread(&g_jobs[g_njobs++], 0, j);
}

void record_wait(void)
{
    int i;
    for (i = 0; i < g_njobs; i++) join_thread(g_jobs[i]);
    g_njobs = 0;
}

void record_frame(const uint32_t *pixels, int w, int h, uint64_t now_us)
{
    int64_t expected;
    int count, x, y, top = 0, rows = VID_H;
    Frame *f;
    if (!g_recording || w <= 0 || h <= 0) return;
    if (g_nframes == 0) g_start_us = now_us;
    expected = (int64_t)((now_us - g_start_us) * RECORD_FPS / 1000000u) + 1;
    count = (int)(expected - g_nframes);
    if (count <= 0) return;
    if (count > MAX_REPEAT) count = MAX_REPEAT;
    lock(&g_lock);
    if (g_nqueue >= MAX_QUEUED || !g_recording) {
        /* ffmpeg is late: this frame is skipped, the next one is repeated */
        unlock(&g_lock);
        return;
    }
    unlock(&g_lock);
    f = (Frame *)malloc(sizeof *f);
    if (f == NULL) return;
    /* nearest pixel to 640x480 (a 4:3 picture whatever the game's resolution); a wider picture (480 lines, more than
     * 640 across: a mission in a wide window) keeps its shape, with black above and below */
    if (h == 480 && w > 640) {
        rows = VID_W * h / w;
        top = (VID_H - rows) / 2;
    }
    memset(f->rgb, 0, sizeof f->rgb);
    for (y = 0; y < rows; y++) {
        const uint32_t *row = pixels + (size_t)(y * h / rows) * (size_t)w;
        uint8_t *o = f->rgb + (size_t)(y + top) * VID_W * 3;
        for (x = 0; x < VID_W; x++) {
            uint32_t p = row[x * w / VID_W];
            o[x * 3] = (uint8_t)(p >> 16);
            o[x * 3 + 1] = (uint8_t)(p >> 8);
            o[x * 3 + 2] = (uint8_t)p;
        }
    }
    f->count = count;
    lock(&g_lock);
    g_queue[g_nqueue++] = f;
    g_nframes += count;
    cond_signal(&g_wake);
    unlock(&g_lock);
}

void record_audio(const int16_t *frames, size_t count)
{
    size_t bytes = count * 4u;
    if (!g_recording || g_nframes == 0) return; /* the sound starts with the first frame */
    lock(&g_lock);
    if (g_snd_n + bytes > g_snd_cap) {
        size_t cap = (g_snd_n + bytes) * 2 + SOUND_CHUNK;
        uint8_t *b = (uint8_t *)realloc(g_snd, cap);
        if (b == NULL) {
            unlock(&g_lock);
            return;
        }
        g_snd = b;
        g_snd_cap = cap;
    }
    memcpy(g_snd + g_snd_n, frames, bytes);
    g_snd_n += bytes;
    unlock(&g_lock);
}

int record_screenshot(const char *path, const uint32_t *pixels, int w, int h)
{
    /* the game's frames twice their size (4:3 whatever their resolution; a wide mission frame keeps its shape); a
     * picture as the window shows it (larger) as it is */
    int ow = w <= 640 ? 1280 : h == 480 ? w * 2 : w, oh = w <= 640 || h == 480 ? 960 : h;
    uint8_t *rgb = (uint8_t *)malloc((size_t)ow * oh * 3);
    int x, y, ok;
    if (rgb == NULL || w <= 0 || h <= 0) {
        free(rgb);
        return 0;
    }
    for (y = 0; y < oh; y++) {
        const uint32_t *row = pixels + (size_t)(y * h / oh) * (size_t)w;
        for (x = 0; x < ow; x++) {
            uint32_t p = row[x * w / ow];
            uint8_t *o = rgb + ((size_t)y * ow + x) * 3;
            o[0] = (uint8_t)(p >> 16);
            o[1] = (uint8_t)(p >> 8);
            o[2] = (uint8_t)p;
        }
    }
    ok = stbi_write_png(path, ow, oh, 3, rgb, ow * 3) != 0;
    free(rgb);
    return ok;
}
