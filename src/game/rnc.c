/* rnc.c - the game's packed files (rnc.h): RNC ProPack, method 1.
 *
 * A file is an 18-byte header ("RNC" 1, the unpacked and packed sizes and their CRCs, big-endian; the leeway and
 * the number of chunks) and the packed data.  The data is read as a stream of bits, low bit first, from 16-bit
 * little-endian words, with the literal bytes in the stream itself: after the first 2 bits (unused), each chunk
 * has three Huffman tables (the lengths of the literal runs, the distances, the lengths of the copies) and the
 * number of its parts; a part is a run of literal bytes, then (but for the last) a copy of what was unpacked
 * `distance` + 1 bytes back, `length` + 2 bytes long.  A table is its number of codes (5 bits) and the length of
 * each code (4 bits, 0: not used); the codes are given in the order of their lengths, then of their values, each
 * read with its bits reversed.  A value v of 2 or more stands for 2^(v-1) plus the v-1 bits that follow.  The
 * unpacked bytes are checked against the header's CRC. */
#include "rnc.h"

#include <string.h>

#define HEADER 18
#define MAX_CODES 32

typedef struct {
    const uint8_t *data;
    size_t n, at; /* the bytes, and where the last word put in `bits` is */
    uint32_t bits;
    int count; /* the bits in `bits` */
} Bits;

typedef struct {
    int n;
    struct {
        uint32_t code;
        int len, value;
    } c[MAX_CODES];
} Table;

static uint32_t word(const Bits *b, size_t at)
{
    uint32_t lo = at < b->n ? b->data[at] : 0, hi = at + 1 < b->n ? b->data[at + 1] : 0;
    return lo | hi << 8;
}

static void bits_init(Bits *b, const uint8_t *data, size_t n, size_t at)
{
    b->data = data;
    b->n = n;
    b->at = at;
    b->bits = word(b, at);
    b->count = 16;
}

static void bits_skip(Bits *b, int n)
{
    b->bits >>= n;
    b->count -= n;
    if (b->count < 16) {
        b->at += 2;
        b->bits |= word(b, b->at) << b->count;
        b->count += 16;
    }
}

static uint32_t bits_read(Bits *b, int n)
{
    uint32_t v = b->bits & ((1u << n) - 1u);
    bits_skip(b, n);
    return v;
}

/* after a run of literal bytes (from b->at on): the word now there takes the place of the one they were */
static void bits_fix(Bits *b)
{
    b->count -= 16;
    b->bits &= (1u << b->count) - 1u;
    b->bits |= word(b, b->at) << b->count;
    b->count += 16;
}

static uint32_t reversed(uint32_t v, int n)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < n; i++) r |= ((v >> i) & 1u) << (n - 1 - i);
    return r;
}

static void table_read(Table *t, Bits *b)
{
    int len[MAX_CODES], n = (int)bits_read(b, 5), i, j;
    uint32_t code = 0;
    t->n = 0;
    for (i = 0; i < n; i++) len[i] = (int)bits_read(b, 4);
    for (i = 1; i <= 16; i++) {
        for (j = 0; j < n; j++)
            if (len[j] == i) {
                t->c[t->n].code = reversed(code, i);
                t->c[t->n].len = i;
                t->c[t->n].value = j;
                t->n++;
                code++;
            }
        code <<= 1;
    }
}

/* a value of table t; -1: no code matches */
static long table_value(const Table *t, Bits *b)
{
    int i, v;
    for (i = 0; i < t->n; i++)
        if ((b->bits & ((1u << t->c[i].len) - 1u)) == t->c[i].code) break;
    if (i == t->n) return -1;
    bits_skip(b, t->c[i].len);
    v = t->c[i].value;
    if (v < 2) return v;
    return (long)((1u << (v - 1)) | bits_read(b, v - 1));
}

/* the CRC of the header's (CRC-16, reflected, polynomial 0xA001, from 0) */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) crc = (uint16_t)((crc & 1u) ? (crc >> 1) ^ 0xA001u : crc >> 1);
    }
    return crc;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

size_t rnc_size(const uint8_t *packed, size_t n)
{
    if (n < HEADER || memcmp(packed, "RNC\1", 4) != 0) return 0;
    return be32(packed + 4);
}

size_t rnc_unpack(const uint8_t *packed, size_t n, uint8_t *out, size_t max)
{
    size_t size = rnc_size(packed, n), o = 0;
    Bits b;
    Table raw, dist, len;
    if (size == 0 || size > max) return 0;
    if (HEADER + (size_t)be32(packed + 8) < n) n = HEADER + (size_t)be32(packed + 8);
    bits_init(&b, packed, n, HEADER);
    bits_skip(&b, 2);
    while (o < size) {
        long parts;
        table_read(&raw, &b);
        table_read(&dist, &b);
        table_read(&len, &b);
        parts = (long)bits_read(&b, 16);
        for (;;) {
            long run = table_value(&raw, &b), d, l;
            if (run < 0 || o + (size_t)run > size || b.at + (size_t)run > n) return 0;
            if (run > 0) {
                memcpy(out + o, packed + b.at, (size_t)run);
                o += (size_t)run;
                b.at += (size_t)run;
                bits_fix(&b);
            }
            if (--parts <= 0) break;
            d = table_value(&dist, &b);
            l = table_value(&len, &b);
            if (d < 0 || l < 0) return 0;
            d += 1;
            l += 2;
            if ((size_t)d > o || o + (size_t)l > size) return 0;
            for (; l > 0; l--, o++) out[o] = out[o - (size_t)d];
            if (o == size) break; /* a copy can end the file before the chunk's count of parts */
        }
    }
    if (crc16(out, o) != (uint16_t)(packed[12] << 8 | packed[13])) return 0;
    return o;
}
