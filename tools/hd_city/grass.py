"""the grass tiles (after mark.py):   grass.py HDDIR
Their top the grass material (alpha 240 + GRASS: laid on the ground in world space by the game, no seam), tufts of
blades rising from it (drawn back to front: dark olive at the root, straw at the tip, a little shine where the light
catches them); their sides dark soil.  HDDIR/out/NNN.png and NNN_n.png (the tufts' normals towards the light a little,
the sides' flat; the top's from the material)."""
import sys, numpy as np
from PIL import Image, ImageDraw, ImageFilter
HD = sys.argv[1]
W, H, S = 64, 48, 4
GRASS, TILES = 6, [210]
TUFTS, BLADES = 16, (12, 22)             # tufts a tile, blades a tuft
SOIL = np.array([38, 31, 22], np.float32)
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 1.5 / 16
Z = 4                                     # the tufts drawn Z times larger, then made smaller (smooth blades)
for t in TILES:
    rng = np.random.default_rng(t)
    base = Image.open(f'{HD}/base/{t:03d}.png').convert('RGBA')
    big = np.asarray(base.resize((W * S, H * S), Image.NEAREST))
    opaque = (big[..., 3] > 0) & ~(~TOP & (fy < 16))   # not the game's own blades above the top (ours replace them)
    # the sides: dark soil, the game's light and dark kept a little, a fine grain
    l = np.asarray(base.convert('L').filter(ImageFilter.GaussianBlur(0.8)).resize((W * S, H * S), Image.BICUBIC)) \
        .astype(np.float32)
    grain = rng.normal(0, 0.08, (H * S, W * S))
    soil = SOIL * (0.75 + 0.5 * l / max(1.0, float(l[opaque].mean())))[..., None] * (1 + grain)[..., None]
    out = np.zeros((H * S, W * S, 4), np.uint8)
    out[..., :3] = soil.clip(0, 255).astype(np.uint8)
    out[..., 3] = np.where(opaque | TOP, 255, 0)
    out[..., 3][TOP] = 240 + GRASS
    nrm = np.zeros((H * S, W * S, 3), np.uint8)
    nrm[:] = (128, 128, 255)
    # the tufts, back to front
    layer = Image.new('RGBA', (W * S * Z, H * S * Z), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    roots = []
    while len(roots) < TUFTS:
        u, v = rng.uniform(1.5, 30.5, 2)
        roots.append((u, v))
    roots.sort(key=lambda r: r[0] + r[1])
    for u, v in roots:
        x0, y0 = 32 + (u - v), (u + v) / 2            # game pixels
        n = rng.integers(*BLADES)
        dry = rng.uniform(0, 1)
        for b in range(n):
            ang = np.radians(rng.normal(0, 28))
            length = rng.uniform(3.5, 8.0) * (1 - 0.3 * abs(np.sin(ang)))
            bend = rng.normal(0, 1.0)
            sx = x0 + rng.normal(0, 0.9)
            pts = []
            for k in range(6):
                s = k / 5
                pts.append(((sx + np.sin(ang) * length * s + bend * s * s) * S * Z,
                            (y0 - np.cos(ang) * length * s) * S * Z))
            root = np.array([44, 48, 18]) * rng.uniform(0.8, 1.1)
            tip = np.array([118, 118, 52]) * (1 - dry) + np.array([150, 132, 70]) * dry
            tip = tip * rng.uniform(0.8, 1.1)
            for k in range(5):
                s = (k + 0.5) / 5
                c = root * (1 - s) + tip * s
                wdt = max(1, int(round((2.0 - 1.4 * s) * Z)))
                d.line([pts[k], pts[k + 1]], fill=tuple(int(x) for x in c) + (255,), width=wdt)
    tufts = np.asarray(layer.resize((W * S, H * S), Image.LANCZOS)).astype(np.float32)
    a = tufts[..., 3:] / 255
    rgb = tufts[..., :3] / np.maximum(a, 1e-3)
    on = (a[..., 0] > 0.5) & (opaque | TOP | (fy < 16))
    out[..., :3][on] = rgb.clip(0, 255).astype(np.uint8)[on]
    out[..., 3][on] = 255
    nrm[on] = (128, 104, 236)                           # leaning to the light a little (up the picture, out of it)
    Image.fromarray(out, 'RGBA').save(f'{HD}/out/{t:03d}.png')
    Image.fromarray(nrm).save(f'{HD}/out/{t:03d}_n.png')
print('grass', len(TILES))
