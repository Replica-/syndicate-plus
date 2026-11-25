"""the pavements' tops as paving (after faithful.py and normals.py, before mark.py):   paving.py HDDIR
Square paving slabs laid on the ground's own grid (the user's pick: as the game's intro draws them), grout between
them, their edges bevelled, each a little lighter or darker; their colour the game's tile seen softly (its
checkerboard of slate and brown blended, its stains where it has them), a fine grain and faint blotches over it.
Their relief is made from the same slabs (HDDIR/out/NNN_n.png's top): no guess from the picture.  The slabs go on past
the top's edge over the sides' top rows, coloured from the same place on the tile next to it (the tiles' edges go
through the middle of slabs): tiles side by side join without a line.  Only the top: the sides stay the redraw's."""
import sys, numpy as np
from PIL import Image, ImageFilter
HD = sys.argv[1]
W, H, S = 64, 48, 4
PAVED = [35, 36, 41]
# the versions shown to the user (of 20): slabs along u and v a tile's side, grout and bevel (game pixels), the grout's
# shade, how much the slabs differ, grain, colour (the game's / muted), its blur, bond (1: rows moved half a slab, 2:
# each row moved its own way; 3: the intro's, half a slab from row to row and a little off)
VERSIONS = {2: (2, 2, 0.3, 1.0, 0.72, 0.05, 0.12, 'orig', 2.5, 0),
            3: (2, 2, 0.25, 0.6, 0.85, 0.04, 0.18, 'muted', 2.5, 0),
            6: (4, 2, 0.25, 0.7, 0.75, 0.06, 0.12, 'orig', 0.9, 1),
            # as the game's intro draws its pavements: square slabs, muted terracotta, each its own shade, thin joints
            21: (2, 2, 0.3, 1.3, 0.62, 0.11, 0.16, 'terracotta', 2.5, 3)}
TERRACOTTA = np.array([108, 72, 45], np.float32)              # the slabs' mean colour: the buildings' (as installed, muted), darker
VERSION = int(sys.argv[2]) if len(sys.argv) > 2 else 21       # the user's pick (the intro's look)
N, M, GROUT, BEVEL, GROUT_SHADE, TONE, GRAIN, COLOUR, BLUR, BOND = VERSIONS[VERSION]
PILLOW = 0.1 if VERSION == 21 else 0.0                       # the intro's slabs: lighter in the middle
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 3.0 / 16
u, v = fy + (fx - 32) / 2, fy - (fx - 32) / 2        # along the ground, 0..32 on the top
su, sv = u * N / 32 + 0.5, v * M / 32 + 0.5           # half a slab along: no grout on the game's stepped edge
iv = np.floor(sv).astype(int)
if BOND == 1: su = su + 0.5 * (iv % 2)
if BOND == 2: su = su + (np.sin((iv % M) * 91.7 + 3.1) * 43758.5453) % 1.0
if BOND == 3:   # the intro's: long joints along u, the cross joints half a slab along from row to row, not exactly
    su = su + 0.5 * (iv % 2) + 0.07 * np.sin((iv % M) * 2.3 + 0.7)      # (by the row in the tile: tiles join)
iu = np.floor(su).astype(int)
lu, lv = su - iu, sv - iv
edge = np.minimum(np.minimum(lu, 1 - lu) * 32 / N, np.minimum(lv, 1 - lv) * 32 / M)   # to the slab's edge, game px
h = np.clip((edge - GROUT) / BEVEL, 0, 1)
h = h * h * (3 - 2 * h)
ku, kv = iu % (2 * N), iv % M                         # a slab across two tiles the same in both
tone = 1 + TONE * np.sin(ku * 12.9898 + kv * 78.233) * np.cos(ku * 4.1414 - kv * 2.71)
rng = np.random.default_rng(36)
noise = np.asarray(Image.fromarray(rng.normal(128, 40, (H * S, W * S)).clip(0, 255).astype(np.uint8))
                   .filter(ImageFilter.GaussianBlur(0.7))).astype(np.float32) / 255 - 0.5
blot = np.asarray(Image.fromarray(rng.normal(128, 60, (H * S, W * S)).clip(0, 255).astype(np.uint8))
                  .filter(ImageFilter.GaussianBlur(6))).astype(np.float32) / 255 - 0.5
shade = np.where(edge < GROUT, GROUT_SHADE, (0.86 + 0.14 * h + PILLOW * h) * tone) * (1 + GRAIN * noise) * (1 + 0.12 * GRAIN * blot)
if PILLOW:   # the intro's slabs: the joint soft (no hard line), the face smooth and lighter towards its top corner
    soft = np.clip(edge / (GROUT + BEVEL), 0, 1)
    soft = soft * soft * (3 - 2 * soft)
    face = 1 + 0.07 * (1 - (lu + lv))          # lighter where u and v are small: the slab's top corner
    specks = rng.random((H * S, W * S))
    grit = np.where(specks < 0.025, 0.72, np.where(specks > 0.985, 1.22, 1.0))   # small dark and light specks
    shade = (GROUT_SHADE + (1 - GROUT_SHADE) * soft) * (1 + PILLOW * soft) * face * tone * (1 + GRAIN * noise) * grit
# each slab its own colour: some redder, some browner or greyer (1 everywhere for the other versions)
hue = np.sin(ku * 7.31 + kv * 13.7 + 1.9) * np.cos(ku * 2.17 - kv * 5.3)
SLAB = np.stack([1 + 0.07 * hue, 1 + 0.01 * hue, 1 - 0.06 * hue], -1) if PILLOW else np.ones((H * S, W * S, 3))
gy, gx = np.gradient(h * 0.8 * S)
nrm = np.stack([-gx, -gy, np.ones_like(h) * 4], -1)
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
    soft = lay.convert('RGB').filter(ImageFilter.GaussianBlur(BLUR)).crop((W, 32, 2 * W, 32 + H)) \
        .resize((W * S, H * S), Image.BICUBIC)
    col = np.asarray(soft).astype(np.float32)[WY, WX]
    if COLOUR == 'muted':
        l = col.mean(-1, keepdims=True)
        col = l + (col - l) * 0.45
    if COLOUR == 'terracotta':                  # its own colour; the game's tile only as wear (lighter, darker)
        l = col.mean(-1, keepdims=True)
        col = TERRACOTTA * (0.8 + 0.4 * l / max(1.0, float(l.mean())))
    col = col * (shade * 0.92)[..., None] * SLAB
    out = np.asarray(Image.open(f'{HD}/out/{t:03d}.png').convert('RGBA')).copy()
    sel = TOP & (out[..., 3] > 0)
    out[..., :3][sel] = col.clip(0, 255).astype(np.uint8)[sel]
    Image.fromarray(out, 'RGBA').save(f'{HD}/out/{t:03d}.png')
    n = np.asarray(Image.open(f'{HD}/out/{t:03d}_n.png').convert('RGB')).copy()
    n[sel] = ((nrm * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)[sel]
    Image.fromarray(n).save(f'{HD}/out/{t:03d}_n.png')
print('paved', len(PAVED))
