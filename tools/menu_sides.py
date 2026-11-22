"""menu_sides.py - the palette entries of the menus' desk (src/game/menu_sides.h), for their widescreen sides.

    python tools/menu_sides.py GAME_DATA_DIR src/game/menu_sides.h

The green menus are the game's 320 x 200 picture over a desk: MSCRENUP.DAT (an FLC file) shows it as a coloured
render first, then wipes a green picture of it over it.  The entries each picture is drawn in (the same in every
menu) tell the desk from what the game draws over it, and the green one from the coloured one: the sides are the
desk's background colour, its picture fading into it at the game's edges.  Only palette entry numbers are written,
nothing of the game's pictures."""
import struct
import sys


W, H, CW = 576, 200, 320
SIDE = (W - CW) // 2


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


def main():
    data, out = sys.argv[1:3]
    fr = list(frames(data + '/MSCRENUP.DAT'))
    (cpx, cpal), (gpx, gpal) = fr[0], fr[-1]
    green_set = sorted(set(gpx[y * CW + x] for y in range(5, 195) for x in range(6, 314)))
    bits = [0] * 32
    for i in set(cpx):
        bits[i >> 3] |= 1 << (i & 7)
    lines = ['/* menu_sides.h - the palette entries of the menus\' desk (made by tools/menu_sides.py): k_menu_photo the',
             ' * green one\'s, with their colours (0..63; the darkest is its background), k_menu_colour_set the coloured',
             ' * one\'s */',
             '#define MENU_SIDE_W %d /* the widest side (21:9) */' % SIDE, '#define MENU_PHOTO_N %d' % len(green_set),
             'static const unsigned char k_menu_photo[MENU_PHOTO_N][4] = {']
    lines += ['    {%d, %d, %d, %d},' % ((i,) + tuple(gpal[i * 3:i * 3 + 3])) for i in green_set]
    lines += ['};', 'static const unsigned char k_menu_colour_set[32] = {' + ', '.join('0x%02x' % b for b in bits) + '};']
    with open(out, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(out)


if __name__ == '__main__':
    main()
