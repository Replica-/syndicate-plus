"""menu_sides.py - the sides of the widescreen menus: the desk outpainted by tools/menu_outpaint.py, at its full size.

    python tools/menu_sides.py GAME_DATA_DIR OUTPAINTED_COLOUR.png src/game

The green menus are the game's 320 x 200 picture over a desk: MSCRENUP.DAT (an FLC file) shows it as a coloured
render first, then wipes a green picture of it over it.  The outpaint is of the coloured desk (its first frame), 576 x
200 at the game's pixel shape, made 3 times larger in square pixels (1728 x 720).  Its sides are kept at that size
(the window draws them at its own resolution, not the game's), each with OVERLAP of the game's columns under the
game's picture to blend into: in colour as they are and in green as the game makes its green desk (each pixel's green
from the brightness of the coloured one blurred a little, fitted on the game's two pictures), as JPEG
(menu_backdrop.h).  menu_sides.h: the palette entries of the two desks (to tell them from what the game draws over
them and each other) and their colours (to follow the game's fades)."""
import io
import struct
import sys

import numpy as np
from PIL import Image, ImageFilter

W, H, CW = 576, 200, 320
SIDE = (W - CW) // 2
OVERLAP = 24        # the game's columns the sides go under the game's picture by
SX, SY = 3, 3.6     # the outpaint's pixels to a game pixel
BLUR = 1.0          # the blur of the green desk, in game pixels (fitted: the least difference)


def frames(path):
    """the frames of one of the game's FLC files (12-byte header): (indices, palette 0..63) after each"""
    d = open(path, 'rb').read()
    nf, w, h = struct.unpack_from('<HHH', d, 6)
    px, pal = bytearray(w * h), bytearray(768)
    p = 12
    for _ in range(nf):
        nch = struct.unpack_from('<H', d, p + 6)[0]
        q = p + 16
        for _ in range(nch):
            cs, ct = struct.unpack_from('<IH', d, q)
            b = q + 6
            if ct == 4:
                n = struct.unpack_from('<H', d, b)[0]
                b += 2
                i = 0
                for _ in range(n):
                    i += d[b]
                    cnt = d[b + 1] or 256
                    b += 2
                    pal[i * 3:(i + cnt) * 3] = d[b:b + cnt * 3]
                    b += cnt * 3
                    i += cnt
            elif ct == 15:
                for y in range(h):
                    b += 1
                    x = 0
                    while x < w:
                        c = struct.unpack_from('b', d, b)[0]
                        b += 1
                        if c < 0:
                            px[y * w + x:y * w + x - c] = d[b:b - c]
                            b -= c
                            x -= c
                        else:
                            px[y * w + x:y * w + x + c] = bytes([d[b]]) * c
                            b += 1
                            x += c
            elif ct == 12:
                y0, nl = struct.unpack_from('<HH', d, b)
                b += 4
                for y in range(y0, y0 + nl):
                    npk = d[b]
                    b += 1
                    x = 0
                    for _ in range(npk):
                        x += d[b]
                        c = struct.unpack_from('b', d, b + 1)[0]
                        b += 2
                        if c > 0:
                            px[y * w + x:y * w + x + c] = d[b:b + c]
                            b += c
                            x += c
                        else:
                            px[y * w + x:y * w + x - c] = bytes([d[b]]) * -c
                            b += 1
                            x -= c
            elif ct == 7:
                nl = struct.unpack_from('<H', d, b)[0]
                b += 2
                y = 0
                while nl > 0:
                    word = struct.unpack_from('<h', d, b)[0]
                    b += 2
                    if word < 0:
                        if word & 0x4000:
                            y -= word
                        else:
                            px[y * w + w - 1] = word & 0xff
                        continue
                    x = 0
                    for _ in range(word):
                        x += d[b]
                        c = struct.unpack_from('b', d, b + 1)[0]
                        b += 2
                        if c > 0:
                            px[y * w + x:y * w + x + 2 * c] = d[b:b + 2 * c]
                            b += 2 * c
                            x += 2 * c
                        else:
                            px[y * w + x:y * w + x - 2 * c] = d[b:b + 2] * -c
                            b += 2
                            x -= 2 * c
                    y += 1
                    nl -= 1
            elif ct == 16:
                px[:] = d[b:b + w * h]
            elif ct == 13:
                px[:] = bytes(w * h)
            q += cs
        p = q
        yield bytes(px), bytes(pal)


def rgb8(pal, i):
    return tuple((pal[i * 3 + k] << 2 | pal[i * 3 + k] >> 4) for k in range(3))


def picture(px, pal):
    return Image.frombytes('RGB', (CW, H), bytes(b for i in px for b in rgb8(pal, i)))


def luminance(im):
    a = np.asarray(im).astype(np.float32)
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def green_lut(colour, green):
    """each brightness of the coloured desk (blurred by BLUR) to the green desk's colour, from their pixels"""
    lum = luminance(colour.filter(ImageFilter.GaussianBlur(BLUR)))
    g = np.asarray(green).astype(np.float32)
    inner = (slice(6, 194), slice(8, 312))
    bins = np.clip(lum[inner].astype(int), 0, 255).ravel()
    n = np.bincount(bins, None, 256)
    lut = np.zeros((256, 3))
    ok = n > 0
    for c in range(3):
        s = np.bincount(bins, g[inner][..., c].ravel(), 256)
        lut[:, c] = np.interp(np.arange(256), np.arange(256)[ok], s[ok] / n[ok])
    return lut


def c_bytes(name, data):
    out = ['static const unsigned char %s[%d] = {' % (name, len(data))]
    for i in range(0, len(data), 24):
        out.append('    ' + ','.join(str(b) for b in data[i:i + 24]) + ',')
    out.append('};')
    return out


def main():
    data, outpainted, out_dir = sys.argv[1:4]
    fr = list(frames(data + '/MSCRENUP.DAT'))
    (cpx, cpal), (gpx, gpal) = fr[0], fr[-1]
    green_set = sorted(set(gpx[y * CW + x] for y in range(5, 195) for x in range(6, 314)))
    counts = {}
    for i in cpx:
        counts[i] = counts.get(i, 0) + 1
    colour_ref = sorted(sorted(counts, key=lambda i: -counts[i])[:32])
    bits = [0] * 32
    for i in counts:
        bits[i >> 3] |= 1 << (i & 7)
    lines = ["/* menu_sides.h - the palette entries of the menus' desk (made by tools/menu_sides.py): k_menu_photo the",
             " * green one's, k_menu_colour the coloured one's most used, with their colours (0..63; the darkest green is the",
             " * background), k_menu_colour_set all the coloured one's */",
             '#define MENU_SIDE_W %d /* the widest side (21:9) */' % SIDE, '#define MENU_PHOTO_N %d' % len(green_set),
             '#define MENU_COLOUR_N %d' % len(colour_ref), 'static const unsigned char k_menu_photo[MENU_PHOTO_N][4] = {']
    lines += ['    {%d, %d, %d, %d},' % ((i,) + tuple(gpal[i * 3:i * 3 + 3])) for i in green_set]
    lines += ['};', 'static const unsigned char k_menu_colour[MENU_COLOUR_N][4] = {']
    lines += ['    {%d, %d, %d, %d},' % ((i,) + tuple(cpal[i * 3:i * 3 + 3])) for i in colour_ref]
    lines += ['};', 'static const unsigned char k_menu_colour_set[32] = {' + ', '.join('0x%02x' % b for b in bits) + '};']
    with open(out_dir + '/menu_sides.h', 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    # the sides at the outpaint's size, in colour and green
    big = Image.open(outpainted).convert('RGB')
    bw, bh = big.size
    lut = green_lut(picture(cpx, cpal), picture(gpx, gpal))
    lum = luminance(big.filter(ImageFilter.GaussianBlur(BLUR * SY)))
    green = Image.fromarray(lut[np.clip(lum.astype(int), 0, 255)].astype(np.uint8))
    sw = int((SIDE + OVERLAP) * SX)
    lines = ["/* menu_backdrop.h - the sides of the widescreen menus at the outpaint's size (made by tools/menu_sides.py), JPEG:",
             ' * [0] left, [1] right in colour, [2] left, [3] right in green; each MENU_BACKDROP_W x MENU_BACKDROP_H, for',
             " * the game's MENU_SIDE_W columns beside its picture and MENU_BACKDROP_OVERLAP under it */",
             '#define MENU_BACKDROP_W %d' % sw, '#define MENU_BACKDROP_H %d' % bh, '#define MENU_BACKDROP_OVERLAP %d' % OVERLAP]
    names = []
    for k, (im, x0) in enumerate(((big, 0), (big, bw - sw), (green, 0), (green, bw - sw))):
        b = io.BytesIO()
        im.crop((x0, 0, x0 + sw, bh)).save(b, 'JPEG', quality=92)
        names.append('k_menu_backdrop_%d' % k)
        lines += c_bytes(names[-1], b.getvalue())
    lines.append('static const unsigned char *const k_menu_backdrop[4] = {%s};' % ', '.join(names))
    lines.append('static const unsigned int k_menu_backdrop_size[4] = {%s};' % ', '.join('sizeof %s' % n for n in names))
    with open(out_dir + '/menu_backdrop.h', 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('done')


if __name__ == '__main__':
    main()
