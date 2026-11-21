/* host.h - the boundary between the portable game core (src/rt, src/game, gen) and the platform layer
 * (src/platform/win32, src/platform/macos, src/platform/headless).
 *
 * The core runs the recompiled DOS programs on one thread (the "game thread").  It calls the host_*
 * functions below; the platform calls the rt_* entry points (rt_key_event, rt_mouse_event, ...) from its
 * own UI thread, so those are thread-safe.  Nothing in this header depends on an OS header. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- implemented by the platform -------------------------------------------------------------------------- */

/* monotonic time in microseconds */
uint64_t host_time_us(void);
/* sleep the game thread for about `us` microseconds */
void host_sleep_us(uint32_t us);

/* a finished frame: `w` x `h` pixels 0x00RRGGBB (row stride `w`), in true colour (the game's 256 colours with
 * what this version draws over them). Every VGA mode the games use fills a 4:3 screen (320x200 has tall
 * pixels). Called on the game thread; the platform copies what it needs before returning. */
void host_present(const uint32_t *pixels, int w, int h);
/* the width of a 480-line picture as wide as the display shows it (640: 4:3); in missions the frames may be that
 * wide (the city fills the rest), at most HOST_CANVAS_MAX */
#define HOST_CANVAS_MAX 1280
int host_canvas_width(void);

/* audio: the core produces signed 16-bit stereo at host_audio_rate() and pushes it here */
int host_audio_rate(void);
void host_audio_push(const int16_t *frames, size_t count);

/* one lock shared by the input queues (the UI thread feeds them, the game thread drains them) */
void host_lock(void);
void host_unlock(void);

/* log a line (UTF-8, no newline needed) */
void host_log(const char *line);

/* the display's timing: the time of a recent vertical blank (host_time_us's clock) and the refresh period, both in
 * microseconds; 0 when it is not known (frames are then made at a steady rate) */
int host_display_timing(uint64_t *vblank_us, uint64_t *period_us);

/* ---- the city on the GPU (optional) ----
 * In missions the core can hand the platform the city as a list of images to draw, at the display's resolution,
 * under the frame: the frame's pixels then carry a transparency in their top byte (0 opaque, 255: the city shows
 * through).  Images are palette indices (255 transparent), defined once; positions are in the frame's pixels
 * (640 x 480), with fractions. */
typedef struct HostCityQuad {
    float x, y;       /* the image's top left corner in the frame */
    uint16_t image;
    uint8_t flip;     /* drawn mirrored */
    uint8_t glow;     /* in the dark: 0 nothing shines, 1 its brightest pixels, 2 all of it */
    uint32_t hat;     /* a colour for the pixels of colour 6 (0x00RRGGBB with 0xFF000000 set), or 0 */
} HostCityQuad;
/* a light in the city (a lamp): a soft ellipse of light, centre x, y, radii rx, ry (frame pixels) */
typedef struct HostCityLight {
    float x, y, rx, ry;
    uint32_t colour;  /* 0x00RRGGBB */
} HostCityLight;
/* 1 when the platform draws the city now (it may decline, e.g. while it records a video) */
int host_city_gpu(void);
/* forget every image (new data); then define image `image` (w x h palette indices, row stride w) */
void host_city_reset(void);
void host_city_image(int image, int w, int h, const uint8_t *pixels);
/* the city of the next frame: quads drawn in order, inside the rectangle x0..x1, y0..y1 of the frame, with the
 * frame's palette, each image `scale` times its size (the zoom); the lights shine when the platform shows the city
 * at dusk or at night */
void host_city_frame(const HostCityQuad *quads, int n, const uint32_t *palette, int x0, int y0, int x1, int y1,
                     float scale, const HostCityLight *lights, int nlights);

/* the original game files built into the program (a pack written by tools/pack_game.py), used when no game
 * folder is given; NULL when the program has none */
const uint8_t *host_game_pack(size_t *size);

/* the core cannot go on (an emulation error): tell the player and end the process; does not return */
void host_fatal(const char *text);

/* the game thread calls this regularly; non-zero when the player closed the window (the core then ends) */
int host_should_quit(void);

/* file system helpers the C library does not offer portably (UTF-8 paths, '/' separators accepted) */
int host_mkdir(const char *path);              /* 0 on success or when it exists */
int host_is_dir(const char *path);
typedef struct HostDirEntry {
    char name[260];
    uint32_t size;
    int is_dir;
    uint16_t dos_time, dos_date;
} HostDirEntry;
void *host_dir_open(const char *path);         /* NULL when it does not exist */
int host_dir_next(void *dir, HostDirEntry *e); /* 1 while there is an entry */
void host_dir_close(void *dir);
/* case-insensitive lookup of one path component in `dir`: writes the real name; 1 when found */
int host_find_name(const char *dir, const char *name, char *out, size_t out_size);

/* ---- implemented by the core (thread-safe, callable from the UI thread) --------------------------------- */

/* PC/AT scancode set 1 (make code; extended keys with the 0xE0 prefix as 0xE000 | code) */
void rt_key_event(uint32_t scancode, int down);
/* mouse position as a fraction of the displayed game picture (0..65535 each way; the core maps it onto
 * the range the game gave the mouse driver); buttons bit 0 left, bit 1 right, bit 2 middle */
void rt_mouse_move(int fx, int fy);
void rt_mouse_buttons(int buttons);
/* the mouse wheel turned `notches` (positive: away from the user) */
void rt_mouse_wheel(int notches);
/* relative motion in mickeys (used when the game reads motion counters) */
void rt_mouse_motion(int dx, int dy);
/* non-zero while the running program uses the mouse (reads its position, buttons or motion, or has an event
 * handler); such a program draws its own pointer, so the front end may hide the system's.  The launcher does
 * not use the mouse and draws no pointer. */
int rt_mouse_used(void);
/* pause/resume the game thread at its next poll (e.g. while a modal dialog is open) */
void rt_set_paused(int paused);
/* the volumes of the music and of the other sounds, 0..100 (100 at first) */
void rt_set_volumes(int music, int sound);
/* the speed of the emulated CPU in instructions a millisecond, as DOSBox's "cycles" (GOG's Syndicate Plus: 8000):
 * the game paces itself on its timer and draws when its work is done, so on a CPU without limit it does a few
 * updates at once and then waits, and the motion jumps.  0: as fast as the host goes (at first) */
void rt_set_cpu_speed(int instructions_per_ms);
/* the number of different frames the game showed in the last second, at the top right of the picture (off at
 * first) */
void rt_set_show_fps(int on);
/* how many frames a second the core makes (the display's refresh rate; 70 at first): in missions the city moves
 * between the game's updates, so each refresh of the display can show a new picture */
void rt_set_frame_rate(int hz);

#ifdef __cplusplus
}
#endif
