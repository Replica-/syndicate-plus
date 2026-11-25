"""The HD pack's base tiles: each tile a mission uses, recoloured in another city palette.
   export.py TILES.bin MISSION_PAL.bin TARGET_PAL.bin HD [IDS.txt]
TILES.bin: `synd_headless ... --dump-tiles TILES.bin` in the mission (256 tiles of 64 x 48 pixels 0xAARRGGBB in the
mission's colours, 0 transparent, then the 256 tile types).  MISSION_PAL.bin / TARGET_PAL.bin: the city palettes
(768 bytes, 6-bit VGA), unpacked from SYNDICAT/DATA/HPALxx.DAT with unrnc.c (Mission 1: HPAL02; the look of the pack:
HPAL01).  Each pixel's colour is taken back to its palette index through the mission's palette, then drawn in the
target's.  IDS.txt: the tiles to export (spaces between; mission1_tiles.txt: Mission 1's), else every tile that has
pixels.  Writes HD/base/NNN.png (RGBA) and HD/sheetN.png (contact sheets, the numbers on them)."""
import os
import struct
import sys

from PIL import Image, ImageDraw

W, H = 64, 48


def palette(path):
    p = open(path, 'rb').read()
    return [tuple(min(255, x * 4 + x // 16) for x in p[i * 3:i * 3 + 3]) for i in range(16)]


tiles_path, mpal_path, tpal_path, hd = sys.argv[1:5]
d = open(tiles_path, 'rb').read()
mission, target = palette(mpal_path), palette(tpal_path)
index = {c: i for i, c in enumerate(mission)}
tiles = [struct.unpack('<%dI' % (W * H), d[t * W * H * 4:(t + 1) * W * H * 4]) for t in range(256)]
if len(sys.argv) > 5:
    ids = [int(v) for v in open(sys.argv[5]).read().split()]
else:
    ids = [t for t in range(256) if any(p >> 24 for p in tiles[t])]
os.makedirs(os.path.join(hd, 'base'), exist_ok=True)


def nearest(c):
    return min(range(16), key=lambda i: sum((a - b) ** 2 for a, b in zip(c, mission[i])))


for t in ids:
    px = []
    for p in tiles[t]:
        if not p >> 24:
            px.append((0, 0, 0, 0))
            continue
        c = ((p >> 16) & 255, (p >> 8) & 255, p & 255)
        px.append(target[index[c] if c in index else nearest(c)] + (255,))
    im = Image.new('RGBA', (W, H))
    im.putdata(px)
    im.save(os.path.join(hd, 'base', '%03d.png' % t))
Z, per = 3, 8
for sheet in range(0, len(ids), 32):
    part = ids[sheet:sheet + 32]
    rows = (len(part) + per - 1) // per
    sh = Image.new('RGB', (per * (W * Z + 6), rows * (H * Z + 18)), (255, 0, 255))
    dr = ImageDraw.Draw(sh)
    for k, t in enumerate(part):
        x, y = (k % per) * (W * Z + 6), (k // per) * (H * Z + 18)
        im = Image.open(os.path.join(hd, 'base', '%03d.png' % t)).resize((W * Z, H * Z), Image.NEAREST)
        sh.paste(im, (x, y + 16), im)
        dr.text((x + 2, y + 2), str(t), fill=(255, 255, 255))
    sh.save(os.path.join(hd, 'sheet%d.png' % (sheet // 32)))
print(len(ids), 'tiles')
