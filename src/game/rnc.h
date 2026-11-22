/* rnc.h - the game's packed files (RNC ProPack, method 1) unpacked here (rnc.c). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the size `packed` (n bytes, starting "RNC" 1) unpacks to; 0 if it is not such a file */
size_t rnc_size(const uint8_t *packed, size_t n);
/* `packed` unpacked into `out` (at least rnc_size bytes): the bytes written, 0 if it is damaged */
size_t rnc_unpack(const uint8_t *packed, size_t n, uint8_t *out, size_t max);

#ifdef __cplusplus
}
#endif
