"""HD explosions and burnt things (run after upscale_sprites.py):   explode.py HDDIR SPRITEDIR
FIREBALLS (a tree's or a thing's blast, its frames): billowing fire in the game's outline: a hot core where the game
drew yellow, orange flame, dark red and black smoke rolling at its edges.  EMBERS (burnt stumps, wrecks): the
upscaled sprite kept, its orange and red pixels made glowing embers (hot cores, soft glow, a little flicker of
brightness between them).  -> HDDIR/sprites/NNNN.png"""
import os, sys
import numpy as np
from PIL import Image, ImageFilter
HD, SRC = sys.argv[1], sys.argv[2]
S = 4
FIREBALLS = [1552, 1562, 1563, 1564, 1565]
EMBERS = [1575, 1576, 1577, 1578, 1592, 1593, 1594, 1595, 1596, 1597, 1598, 1599, 1743, 1744]
FIRE = [(0.0, (14, 10, 10)), (0.18, (40, 22, 16)), (0.32, (110, 26, 8)), (0.5, (220, 82, 14)),
        (0.68, (255, 160, 40)), (0.85, (255, 228, 130)), (1.0, (255, 252, 235))]


def ramp(stops, t):
    t = np.clip(t, 0, 1)
    out = np.zeros(t.shape + (3,), np.float32)
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        m = (t >= a) & (t <= b)
        f = ((t - a) / (b - a))[m][:, None]
        out[m] = np.array(ca) + (np.array(cb) - np.array(ca)) * f
    return out


def noise(h, w, scale, seed):
    rng = np.random.default_rng(seed)
    n = rng.random((max(2, h // scale + 2), max(2, w // scale + 2))).astype(np.float32)
    return np.asarray(Image.fromarray((n * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)).astype(np.float32) / 255


def heat_of(a):
    """the game's heat: bright, warm pixels hot (its yellows and oranges), black cold"""
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    warm = np.clip((r - b) / 180, 0, 1)
    return np.clip((r * 0.5 + g * 0.5) / 255 * (0.4 + 0.6 * warm), 0, 1) * (a[..., 3] > 0)


for sid in FIREBALLS:
    spr = Image.open(os.path.join(SRC, '%04d.png' % sid)).convert('RGBA')
    w, h = spr.size
    a = np.asarray(spr).astype(np.float32)
    W, H = w * S, h * S
    heat = np.asarray(Image.fromarray((heat_of(a) * 255).astype(np.uint8)).resize((W, H), Image.BICUBIC)
                      .filter(ImageFilter.GaussianBlur(S * 0.7))).astype(np.float32) / 255
    alpha = np.asarray(spr.split()[3].resize((W, H), Image.BILINEAR).filter(ImageFilter.GaussianBlur(S * 0.6))) / 255
    billow = noise(H, W, 3 * S, sid) * 0.45 + noise(H, W, S, sid + 3) * 0.35 + noise(H, W, max(1, S // 2), sid + 9) * 0.2
    t = heat * 1.9 + (billow - 0.5) * 0.5 + 0.08
    col = ramp(FIRE, t)
    shape = (alpha > 0.35) & ((billow > 0.25) | (alpha > 0.8))
    Image.fromarray(np.dstack([col, np.where(shape, 255, 0)]).astype(np.uint8), 'RGBA').save(
        os.path.join(HD, 'sprites', '%04d.png' % sid))
    print('fireball', sid)

for sid in EMBERS:
    p = os.path.join(HD, 'sprites', '%04d.png' % sid)
    up = np.asarray(Image.open(p).convert('RGBA')).astype(np.float32)
    H, W = up.shape[:2]
    c = up[..., :3]
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    ember = (r > 110) & (r - b > 60) & (up[..., 3] > 0)                 # the game's orange and red pixels
    e = Image.fromarray((ember * 255).astype(np.uint8))
    core = np.asarray(e.filter(ImageFilter.GaussianBlur(S * 0.4))).astype(np.float32) / 255
    glow = np.asarray(e.filter(ImageFilter.GaussianBlur(S * 1.6))).astype(np.float32) / 255
    flick = 0.7 + 0.6 * noise(H, W, 2 * S, sid)
    t = np.clip(core * 1.1 * flick, 0, 1)
    hot = ramp(FIRE, 0.45 + 0.55 * t)
    k = np.clip(core * 1.4, 0, 1)[..., None]
    c = c * (1 - k) + hot * k
    c = c + np.array([255, 110, 30], np.float32) * (glow * 0.35 * (1 - k[..., 0]))[..., None]   # the glow on the char
    up[..., :3] = c
    Image.fromarray(up.clip(0, 255).astype(np.uint8), 'RGBA').save(p)
    print('embers', sid)
