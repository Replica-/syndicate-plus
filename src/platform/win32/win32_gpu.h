/* win32_gpu.h - the picture on the GPU (Direct3D 11): a texture drawn into a rectangle of the window. */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>

/* 0 when Direct3D 11 cannot be used (the window then draws with GDI) */
int gpu_init(HWND hwnd);
void gpu_shutdown(void);
/* the window's client area changed size */
void gpu_resize(int w, int h);
/* draws `pixels` (w x h, 0x00RRGGBB) into `dst` of the client area (black around it) and shows it; with `sharp`
 * the pixels keep sharp edges at any scale (sharp bilinear), else the image is drawn as it is (1:1) */
void gpu_present(const uint32_t *pixels, int w, int h, RECT dst, int sharp);
