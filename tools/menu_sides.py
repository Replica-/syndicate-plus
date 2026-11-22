"""menu_sides.py - the sides of the widescreen menus (src/game/menu_sides.h) from the desk outpainted by
tools/menu_outpaint.py.

    python tools/menu_sides.py GAME_DATA_DIR OUTPAINTED_COLOUR.png src/game/menu_sides.h

The green menus are the game's 320 x 200 picture over a desk: MSCRENUP.DAT (an FLC file) shows it as a coloured
render first, then wipes a green picture of it over it.  The outpaint is of the coloured desk (its first frame), 576 x
200 at the game's pixel shape (shown 3 times larger, square pixels: 1728 x 720).  Its two sides (128 columns each) are
written in true colour as they are, scaled to the menus' pixels, and in green as the game makes its green desk (each
pixel's green from the brightness of the coloured one blurred a little: fitted on the game's two pictures), with the
palette entries of the two desks to follow the game's fades by and to tell them apart.  Only the new sides are
written, nothing of the game's pictures."""
import struct
import sys

import numpy as np
from PIL import Image, ImageFilter

W, H, CW = 576, 200, 320
SIDE = (W - CW) // 2
BLUR = 1.0  # the blur of the green desk (fitted: the least difference)


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


def green_of(colour, green):
    """how the game makes its green desk from the colour one: each pixel's green colour from the brightness of the
    colour picture blurred a little (fitted on their pixels inside the frame; within a level of the game's)"""
    lum = luminance(colour.filter(ImageFilter.GaussianBlur(BLUR)))
    g = np.asarray(green).astype(np.float32)
    inner = (slice(6, 194), slice(8, 312))
    bins = np.clip(lum[inner].astype(int), 0, 255).ravel()
    n = np.bincount(bins, None, 256)
    lut = np.zeros((256, 3))
    for c in range(3):
        s = np.bincount(bins, g[inner][..., c].ravel(), 256)
        ok = n > 0
        lut[:, c] = np.interp(np.arange(256), np.arange(256)[ok], s[ok] / n[ok])
    return lambda im: Image.fromarray(lut[np.clip(luminance(im.filter(ImageFilter.GaussianBlur(BLUR))).astype(int), 0,
                                                  255)].astype(np.uint8))


def c_table(name, side_images):
    out = ['static const unsigned int %s[2][MENU_SIDE_H][MENU_SIDE_W] = {' % name]
    for s in side_images:
        out.append('    {')
        for y in range(H):
            out.append('        {' + ','.join('0x%02x%02x%02x' % s.getpixel((x, y)) for x in range(SIDE)) + '},')
        out.append('    },')
    out.append('};')
    return out


def main():
    data, outpainted, out = sys.argv[1:4]
    fr = list(frames(data + '/MSCRENUP.DAT'))
    (cpx, cpal), (gpx, gpal) = fr[0], fr[-1]
    green_set = sorted(set(gpx[y * CW + x] for y in range(5, 195) for x in range(6, 314)))
    counts = {}
    for i in cpx:
        counts[i] = counts.get(i, 0) + 1
    colour_set = sorted(counts)
    colour_ref = sorted(sorted(counts, key=lambda i: -counts[i])[:32])
    # the colour desk 576 wide: the outpaint's sides, the game's own picture between them (only to make the green
    # sides from: the blur reaches across); its green as the game makes it
    wide = Image.open(outpainted).convert('RGB').resize((W, H), Image.LANCZOS)
    wide.paste(picture(cpx, cpal), (SIDE, 0))
    green = green_of(picture(cpx, cpal), picture(gpx, gpal))(wide)
    sides = lambda im: [im.crop((x0, 0, x0 + SIDE, H)) for x0 in (0, W - SIDE)]
    lines = ['/* menu_sides.h - the sides of the widescreen menus: the desk outpainted in true colour (made by',
             ' * tools/menu_sides.py from the desk before the green takes over, its green made as the game makes it).',
             ' * k_menu_colour_sides, k_menu_green_sides [side: 0 left, 1 right][row][column]: 0xRRGGBB.  The palette',
             ' * entries the desk is in, green (k_menu_photo) and in colour (k_menu_colour: the most used; the set of all',
             ' * of them, k_menu_colour_set), with their colours (0..63): the sides as bright as those are (the game fades',
             ' * its menus in and out with the palette), each row of them as the game picture has it */',
             '#define MENU_SIDE_W %d' % SIDE, '#define MENU_SIDE_H %d' % H,
             '#define MENU_PHOTO_N %d' % len(green_set), '#define MENU_COLOUR_N %d' % len(colour_ref),
             'static const unsigned char k_menu_photo[MENU_PHOTO_N][4] = {']
    lines += ['    {%d, %d, %d, %d},' % ((i,) + tuple(gpal[i * 3:i * 3 + 3])) for i in green_set]
    lines += ['};', 'static const unsigned char k_menu_colour[MENU_COLOUR_N][4] = {']
    lines += ['    {%d, %d, %d, %d},' % ((i,) + tuple(cpal[i * 3:i * 3 + 3])) for i in colour_ref]
    bits = [0] * 32
    for i in colour_set:
        bits[i >> 3] |= 1 << (i & 7)
    lines += ['};', 'static const unsigned char k_menu_colour_set[32] = {' + ', '.join('0x%02x' % b for b in bits) + '};']
    lines += c_table('k_menu_colour_sides', sides(wide)) + c_table('k_menu_green_sides', sides(green))
    with open(out, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(out)


if __name__ == '__main__':
    main()
