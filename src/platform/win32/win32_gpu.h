/* win32_gpu.h - the picture on the GPU (Direct3D 11): a texture drawn into a rectangle of the window. */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>

#include "../../rt/host.h"

/* 0 when Direct3D 11 cannot be used (the window then draws with GDI) */
int gpu_init(HWND hwnd);
void gpu_shutdown(void);
/* the window's client area changed size */
void gpu_resize(int w, int h);
/* draws `pixels` (w x h, 0x00RRGGBB) into `dst` of the client area (black around it) and shows it; with `sharp`
 * the pixels keep sharp edges at any scale (sharp bilinear), else the image is drawn as it is (1:1) */
void gpu_present(const uint32_t *pixels, int w, int h, RECT dst, int sharp);
/* the city under the frame (host_city_*), drawn by gpu_present with `sharp`, where the frame is see-through; any
 * thread */
void gpu_city_reset(void);
void gpu_city_image(int id, int w, int h, const uint8_t *pixels);
void gpu_city_frame(const HostCityQuad *quads, int n, const uint32_t *palette, int x0, int y0, int x1, int y1);
/* no city (a frame without one) */
void gpu_city_off(void);
