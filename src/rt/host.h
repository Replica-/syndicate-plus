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
/* the width of a 200-line menu picture as wide as the display shows it (320: 4:3; its pixels as tall as in 4:3) */
int host_menu_width(void);

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
 * through).  Images are in true colour, defined once; positions are in the frame's pixels (640 x 480), with
 * fractions. */
typedef struct HostCityQuad {
    float x, y;       /* the image's top left corner in the frame */
    uint16_t image;
    uint8_t flip;     /* drawn mirrored */
    uint8_t glow;     /* in the dark: 0 nothing shines, 1 its brightest pixels, 2 all of it, 3 a road's white lights;
                       * bit 7: drawn almost see-through */
    uint32_t hat;     /* a colour (0x00RRGGBB) for the image's hat pixels (alpha 254) in its top rows (the top byte:
                       * how many; 0: none) */
    uint32_t look[3]; /* a pedestrian's hair, skin and clothes (0x00RRGGBB; 0: as drawn) for the image's pixels of
                       * alpha 160 + 16 x part (0 hair, 1 skin, 2 clothes, 3 trousers: the clothes' darker and
                       * greyer) + shade (the colour at 0.3 + 0.1 x shade) */
    uint32_t shade;   /* the sun's shadows (HOST_SHADE_*): bits 0-7 */
    float z;          /* how high it stands (world units, a level 128): a tile, its top; anything else (upright),
                       * its top row, each row below 8 lower: the sun's shadows are traced from each pixel's */
} HostCityQuad;
/* HostCityQuad.shade: a solid tile (its face away from the sun darker); in the sun's shadow (a tile's cell, or the
 * cell what stands there is on); a tile's 4 neighbours in it (bits 2-5: x - 1, x + 1, y - 1, y + 1: its top's edges
 * softened against them); a slope going down from the sun; a tile; bits 8-15 where the sun is (0: towards world -x,
 * the top left of the screen, 255: towards -y, the top right) */
#define HOST_SHADE_SOLID 0x01u
#define HOST_SHADE_SHADOW 0x02u
#define HOST_SHADE_FLAT 0x02u /* a tile whose top is a whole floor or roof (it takes the shadows) */
#define HOST_SHADE_SLOPE 0x40u
#define HOST_SHADE_TILE 0x80u
#define HOST_SHADE_ON 0x10000u /* the sun's shadows on (per pixel, the GPU's) */
/* HostCityQuad.hat of a tile with material pixels: this bit, the tile's map y in bits 10-19, its x in bits 0-9 */
#define HOST_CITY_WORLD 0x100000u
#define HOST_GLOW_SIGNAL 0x40u /* HostCityQuad.glow: its bright, strongly coloured pixels shine too (lamps) */
/* a light in the city (a lamp): a soft ellipse of light, centre x, y, radii rx, ry (frame pixels) */
typedef struct HostCityLight {
    float x, y, rx, ry;
    uint32_t colour;  /* 0x00RRGGBB; HOST_LIGHT_GLOW: a glow, added to the colours (a muzzle flash, seen by day too) */
} HostCityLight;
#define HOST_LIGHT_GLOW 0x01000000u
/* the widescreen menus' sides, drawn at the display's resolution (not the frame's) under the frame where it is
 * see-through: 4 images set once (left, right in colour; left, right in green: w x h 0x00RRGGBB), 0 when the
 * platform does not draw them; then before each frame each side's place in the frame's columns (x0 to x1, all its
 * rows; may go past it), which of its 200 rows are green (bit y of green_rows) and how bright colour (bright[0]) and
 * green (bright[1]) are, r g b 0..1 */
typedef struct HostMenuSides {
    int on;
    int x0[2], x1[2];
    uint32_t green_rows[2][7];
    float bright[2][3];
} HostMenuSides;
int host_menu_side_images(const uint32_t *const px[4], int w, int h);
void host_menu_sides(const HostMenuSides *s);
/* 1 when the platform draws the city now (it may decline, e.g. while it records a video) */
int host_city_gpu(void);
/* forget every image (new data); then define image `image`: w x h pixels 0xAARRGGBB, row stride w, `scale` (1, 2
 * or 4) of them to a pixel of the frame each way; alpha 255 opaque, 254 a hat pixel (HostCityQuad.hat's colour), 253 a
 * hat's shine (its colour brighter), 160-223 a pedestrian's (HostCityQuad.look), 240 + K (in a tile drawn with
 * HOST_CITY_WORLD) material K's, 0 transparent */
void host_city_reset(void);
void host_city_image(int image, int w, int h, int scale, const uint32_t *pixels);
/* the normals of image `image` (defined just before; w x h 0x??XXYYZZ, x right, y down, z out of the picture, 128 0):
 * the light picks out its relief.  Without: flat */
void host_city_normals(int image, int w, int h, const uint32_t *pixels);
/* ground material `k` (0-11): w x h pixels 0x00RRGGBB repeating without a seam, and its normals (x along its rows,
 * y down its columns, z up out of the ground); a tile's pixels of alpha 240 + k show it at their place on the ground
 * (HostCityQuad.hat: HOST_CITY_WORLD and the tile's map place), 4 of its pixels a game pixel.  Kept across
 * host_city_reset */
void host_city_material(int k, int w, int h, const uint32_t *colour, const uint32_t *normals);
/* the city of the next frame: quads drawn in order, inside the rectangle x0..x1, y0..y1 of the frame, each image
 * `scale` times its size (the zoom); the lights show when the platform lights the city for a time of day darker
 * than the mission's own look */
void host_city_frame(const HostCityQuad *quads, int n, int x0, int y0, int x1, int y1, float scale,
                     const HostCityLight *lights, int nlights);

/* ---- the options a mission's menu (Escape) shows: the platform's settings ----
 * host_options fills `o` with the options (at most `max`) and returns how many: a name, the value now; any thread.
 * host_option_step changes option `index` to its next (dir 1) or previous (dir -1) value, as the platform's own
 * menu does (it may do it a little later, on its own thread). */
typedef struct HostOption {
    char name[32];
    char value[32];
} HostOption;
int host_options(HostOption *o, int max);
void host_option_step(int index, int dir);

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
/* a left click at x, y of the game's screen (its coordinates), given to its mouse handler; the pointer stays */
void rt_mouse_click_at(int x, int y);
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
