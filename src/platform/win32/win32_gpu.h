/* win32_gpu.h - the picture on the GPU (Direct3D 11): a texture drawn into a rectangle of the window. */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>

#include "../../rt/host.h"

/* 0 when Direct3D 11 cannot be used (the game then cannot run); hwnd NULL: offscreen, as big as gpu_resize
 * said before, read with gpu_read (the headless tests) */
int gpu_init(HWND hwnd);
/* offscreen: the last picture presented (w x h of gpu_resize, 0x??RRGGBB); 0 if there is none */
int gpu_read(uint32_t *pixels);
/* the next picture presented is kept (its rectangle `dst`): gpu_capture_take then gives it (malloc'd, 0x??RRGGBB) */
void gpu_capture(void);
int gpu_capture_take(uint32_t **pixels, int *w, int *h);
void gpu_shutdown(void);
/* the window's client area changed size */
void gpu_resize(int w, int h);
/* draws `pixels` (w x h, 0x00RRGGBB) into `dst` of the client area (black around it) and shows it; with `sharp`
 * the pixels keep sharp edges at any scale (sharp bilinear), else the image is drawn as it is (1:1) */
void gpu_present(const uint32_t *pixels, int w, int h, RECT dst, int sharp);
/* the city under the frame (host_city_*), drawn by gpu_present with `sharp`, where the frame is see-through; any
 * thread */
void gpu_city_reset(void);
void gpu_city_image(int id, int w, int h, int scale, const uint32_t *pixels);
void gpu_city_frame(const HostCityQuad *quads, int n, int x0, int y0, int x1, int y1,
                    float scale, const HostCityLight *lights, int nlights);
/* the time of day the city is lit for: 0 as the mission is (its colours; its lights glow as dark as its palette is),
 * 1 off (as the game draws it), 2 dusk, 3 night */
void gpu_set_time(int time);
/* the sun's shadows on the city's tiles (on by default) */
/* no city (a frame without one) */
void gpu_city_off(void);
