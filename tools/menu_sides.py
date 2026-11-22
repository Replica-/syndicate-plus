"""menu_sides.py - the sides of the widescreen menus (src/game/menu_sides.h) from the desk photo outpainted by
tools/menu_outpaint.py.

    python tools/menu_sides.py GAME_DATA_DIR OUTPAINTED.png src/game/menu_sides.h

The green menus are the game's 320 x 200 picture over a desk photo (the last frame of MSCRENUP.DAT, an FLC file);
the outpainted photo is 576 x 200 at that pixel shape (shown 3 times larger, square pixels: 1728 x 720).  Its two
sides (128 columns each) are written in true colour as they are, scaled to the menus' pixels, with the palette
entries of the photo (the same in every menu) to follow the game's fades by.  Only the new sides are written,
nothing of the game's picture."""
import struct
import sys

from PIL import Image

W, H, CW = 576, 200, 320
SIDE = (W - CW) // 2


def last_frame(path):
    """the last frame of one of the game's FLC files (12-byte header): (indices, palette 0..63)"""
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
    return bytes(px), bytes(pal)


def main():
    data, outpainted, out = sys.argv[1:4]
    px, pal = last_frame(data + '/MSCRENUP.DAT')
    used = sorted(set(px[y * CW + x] for y in range(5, 195) for x in range(6, 314)))
    six = lambda i: tuple(pal[i * 3 + k] for k in range(3))
    wide = Image.open(outpainted).convert('RGB').resize((W, H), Image.LANCZOS)
    sides = [wide.crop((x0, 0, x0 + SIDE, H)) for x0 in (0, W - SIDE)]
    lines = ['/* menu_sides.h - the sides of the widescreen menus: the desk photo outpainted, in true colour (made by',
             ' * tools/menu_sides.py).  k_menu_sides[side: 0 left, 1 right][row][column]: 0xRRGGBB; k_menu_photo: the',
             ' * palette entries of the photo and their colours (0..63) as the menus have them, the sides as bright as',
             ' * those are (the game fades its menus in and out with the palette) */',
             '#define MENU_SIDE_W %d' % SIDE, '#define MENU_SIDE_H %d' % H, '#define MENU_PHOTO_N %d' % len(used),
             'static const unsigned char k_menu_photo[MENU_PHOTO_N][4] = {']
    lines += ['    {%d, %d, %d, %d},' % ((i,) + six(i)) for i in used]
    lines += ['};', 'static const unsigned int k_menu_sides[2][MENU_SIDE_H][MENU_SIDE_W] = {']
    for s in sides:
        lines.append('    {')
        for y in range(H):
            row = [s.getpixel((x, y)) for x in range(SIDE)]
            lines.append('        {' + ','.join('0x%06x' % (r << 16 | g << 8 | b) for r, g, b in row) + '},')
        lines.append('    },')
    lines.append('};')
    with open(out, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(out)


if __name__ == '__main__':
    main()
