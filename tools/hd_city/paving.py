"""the pavements' tops as paving (after faithful.py and normals.py, before mark.py):   paving.py HDDIR
Small square paving stones laid on the ground's own grid (8 a tile's side: the game's pavement reads as that grid),
grout between them, their edges bevelled, each a little lighter or darker; their colour the game's tile seen softly
(its checkerboard of slate and brown blended, its flecks and stains where it has them), a fine grain over it.  Their
relief is made from the same stones (HDDIR/out/NNN_n.png's top): no guess from the picture.  Only the top: the sides
stay the redraw's."""
import sys, numpy as np
from PIL import Image, ImageFilter
HD = sys.argv[1]
W, H, S = 64, 48, 4
PAVED = [35, 36, 41]
N = 8                      # stones a tile's side
GROUT, BEVEL = 0.06, 0.22  # of a stone's side
DARKER = 0.92
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 3.0 / 16   # and over the sides' top rows: the stones
                                                                     # go on there (no line where tiles meet)
u, v = fy + (fx - 32) / 2, fy - (fx - 32) / 2        # along the ground, 0..32 on the top
su, sv = u * N / 32 + 0.5, v * N / 32 + 0.5      # half a stone along: the tiles' edges go through the middle of
                                                 # stones (no grout on the game's stepped edge)
iu, iv = np.floor(su).astype(int), np.floor(sv).astype(int)
lu, lv = su - iu, sv - iv                             # the place in its stone, 0..1
edge = np.minimum(np.minimum(lu, 1 - lu), np.minimum(lv, 1 - lv))
# the stone's height: 0 in the grout, up its bevel, 1 on its face (rounded)
h = np.clip((edge - GROUT) / BEVEL, 0, 1)
h = h * h * (3 - 2 * h)
grout = edge < GROUT
# each stone a little different (by its place on the tile: tiles side by side go on with the same grid)
ku, kv = iu % N, iv % N                               # a stone across two tiles the same in both
tone = 1 + 0.07 * np.sin(ku * 12.9898 + kv * 78.233) * np.cos(ku * 4.1414 - kv * 2.71)
rng = np.random.default_rng(36)
grain = 1 + np.asarray(Image.fromarray((rng.normal(128, 40, (H * S, W * S))).clip(0, 255).astype(np.uint8))
                       .filter(ImageFilter.GaussianBlur(0.7))).astype(np.float32) / 255 * 0.16 - 0.08
shade = np.where(grout, 0.74, (0.86 + 0.14 * h) * tone) * grain * DARKER
# the relief: the height's slope in the picture (x right, y down), z out of it
hp = h * 1.0
gy, gx = np.gradient(hp)
nrm = np.stack([-gx * 1.0, -gy * 1.0, np.ones_like(hp)], -1)
nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
# the colour of a texel past the top's edge: the same place on the top (the tile next to it, wrapped)
wu, wv = np.clip(u % 32, 0.3, 31.7), np.clip(v % 32, 0.3, 31.7)
WX = np.clip(((32 + (wu - wv)) * S).astype(int), 0, W * S - 1)
WY = np.clip(((wu + wv) / 2 * S).astype(int), 0, H * S - 1)
for t in PAVED:
    base = Image.open(f'{HD}/base/{t:03d}.png').convert('RGBA')
    lay = Image.new('RGBA', (W * 3, H + 64), (0, 0, 0, 255))      # among copies of itself, as the map lays them
    for d in range(0, 5):
        for i in range(0, d + 1):
            if i < 3 and d - i < 3:
                lay.alpha_composite(base, (W + (i - (d - i)) * 32, (i + (d - i)) * 16))
    soft = lay.convert('RGB').filter(ImageFilter.GaussianBlur(0.9)).crop((W, 32, 2 * W, 32 + H))         .resize((W * S, H * S), Image.BICUBIC)
    col = np.asarray(soft).astype(np.float32)[WY, WX] * shade[..., None]
    out = np.asarray(Image.open(f'{HD}/out/{t:03d}.png').convert('RGBA')).copy()
    sel = TOP & (out[..., 3] > 0)
    out[..., :3][sel] = col.clip(0, 255).astype(np.uint8)[sel]
    Image.fromarray(out, 'RGBA').save(f'{HD}/out/{t:03d}.png')
    n = np.asarray(Image.open(f'{HD}/out/{t:03d}_n.png').convert('RGB')).copy()
    n[sel] = ((nrm * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)[sel]
    Image.fromarray(n).save(f'{HD}/out/{t:03d}_n.png')
print('paved', len(PAVED))
