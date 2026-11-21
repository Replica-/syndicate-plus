/* record.h - records the game as an MP4 video with its sound, using ffmpeg (it must be in the PATH).
 *
 * The front end hands over every frame it shows (record_frame) and every bit of sound it plays (record_audio).
 * The frames go to ffmpeg at RECORD_FPS while the game goes on (from a thread, so a slow encoder does not slow
 * the game); a frame is repeated when the game shows none for a while, so the video keeps in time with the
 * sound.  Every frame is made 640x480 first (menus at 320x200 too), and the video shows it at twice that size
 * with sharp pixels.  The sound is written to a file beside the video; when the recording stops, ffmpeg puts
 * both together in the background. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_FPS 30

/* starts recording into a new file "syndicate_<date>_<time>.mp4" of `folder` (UTF-8, made if need be), the
 * sound being 16-bit stereo at `rate`; 0 when ffmpeg cannot be found or started */
int record_start(const char *folder, int rate);
/* stops: the video is finished in the background */
void record_stop(void);
int record_active(void);
/* the file being recorded or recorded last (UTF-8) */
const char *record_file(void);
/* a frame shown at `now_us` (any thread; pixels 0x00RRGGBB, row stride w) */
void record_frame(const uint32_t *pixels, int w, int h, uint64_t now_us);
/* sound played (any thread; `count` stereo frames) */
void record_audio(const int16_t *frames, size_t count);
/* waits until the videos being finished are ready (when quitting) */
void record_wait(void);

/* writes `pixels` (w x h, 0x00RRGGBB) as a PNG file shown at 4:3 (each pixel scaled up to fill 1280x960);
 * 1 on success */
int record_screenshot(const char *path, const uint32_t *pixels, int w, int h);

#ifdef __cplusplus
}
#endif
