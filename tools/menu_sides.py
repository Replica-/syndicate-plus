"""menu_sides.py - the sides of the widescreen menus (src/game/menu_sides.h) from the desk photo outpainted by
tools/menu_outpaint.py.

    python tools/menu_sides.py GAME_DATA_DIR OUTPAINTED.png src/game/menu_sides.h

The green menus are the game's 320 x 200 picture over a desk photo (the last frame of MSCRENUP.DAT, an FLC file);
the outpainted photo is 576 x 200 at that pixel shape (shown 3 times larger, square pixels: 1728 x 720).  Its two
sides (128 columns each) are brought back to the photo's own 48 colours of the menus' palette (they are the same in
every menu) and written as palette indices, so that the game's palette colours them as it does the rest (its fades
too).  Only the new sides are written, nothing of the game's picture."""
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
    rgb = lambda i: tuple((pal[i * 3 + k] << 2 | pal[i * 3 + k] >> 4) for k in range(3))
    palimg = Image.new('P', (1, 1))
    flat = []
    for i in used:
        flat += rgb(i)
    palimg.putpalette(flat + list(rgb(used[0])) * (256 - len(used)))
    wide = Image.open(outpainted).convert('RGB').resize((W, H), Image.LANCZOS)
    sides = []
    for x0 in (0, W - SIDE):
        q = wide.crop((x0, 0, x0 + SIDE, H)).quantize(palette=palimg, dither=Image.Dither.FLOYDSTEINBERG)
        sides.append([used[v] if v < len(used) else used[0] for v in q.getdata()])
    with open(out, 'w', newline='\n') as f:
        f.write('/* menu_sides.h - the sides of the widescreen menus: the desk photo outpainted, in the palette indices of\n'
                ' * the menus (made by tools/menu_sides.py).  [side: 0 left, 1 right][row][column] */\n')
        f.write('#define MENU_SIDE_W %d\n#define MENU_SIDE_H %d\n' % (SIDE, H))
        f.write('static const unsigned char k_menu_sides[2][MENU_SIDE_H][MENU_SIDE_W] = {\n')
        for s in sides:
            f.write('    {\n')
            for y in range(H):
                f.write('        {' + ','.join(str(v) for v in s[y * SIDE:(y + 1) * SIDE]) + '},\n')
            f.write('    },\n')
        f.write('};\n')
    print('%s: %d colours' % (out, len(used)))


if __name__ == '__main__':
    main()
