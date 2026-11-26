"""HD bullet impacts (a ricochet's sparks), drawn: sprites 729-733 (16 x 4 .. 16 x 11, the game's sequence: a spark,
a flash, a burst, sparks flying, embers) made at 4x in the same size and place (the game's anchors keep them where
the bullet hits):   effects.py HDDIR SPRITEDIR -> HDDIR/sprites/NNNN.png
A white-hot core ringed yellow and orange, bright sparks streaking out from it, orange embers falling."""
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
HD, SRC = sys.argv[1], sys.argv[2]
S, Z = 4, 4                       # 4x; drawn Z times larger again, then made smaller (smooth)
FRAMES = {729: 0.0, 730: 0.25, 731: 0.5, 732: 0.75, 733: 1.0}   # how far through the impact
rng = np.random.default_rng(7)
# ricochet sparks: a few, fanning up and out from the hit (screen angles, up is -pi/2), fast
SPARKS = [(-np.pi / 2 + rng.uniform(-1.25, 1.25), rng.uniform(0.65, 1.0), rng.uniform(0.7, 1.0)) for _ in range(6)]  # angle, speed, size


def hot(t):
    """a spark's colour as it cools: white, yellow, orange, dark red"""
    stops = [(0.0, (255, 255, 240)), (0.3, (255, 226, 120)), (0.6, (255, 140, 40)), (1.0, (150, 40, 10))]
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if t <= b:
            f = (t - a) / (b - a)
            return tuple(int(ca[k] + (cb[k] - ca[k]) * f) for k in range(3))
    return stops[-1][1]


os.makedirs(os.path.join(HD, 'sprites'), exist_ok=True)
for sid, t in FRAMES.items():
    w, h = Image.open(os.path.join(SRC, '%04d.png' % sid)).size
    W, H = w * S * Z, h * S * Z
    cx, cy = W / 2, H * 0.55                       # where the bullet hits (the game's sprite's middle, a little low)
    glow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    lines = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    g, d = ImageDraw.Draw(glow), ImageDraw.Draw(lines)
    unit = W * 0.36                                # how far the sparks fly (thin: the burst looks small)
    # the flash: a white-hot core ringed yellow and orange (opaque: the game's light flashes give the glow round it),
    # big at first, gone by the end
    flash = max(0.0, 1 - t * 2.5)
    if flash > 0 and t == 0.0:                    # only the first frame: a pinpoint (then sparks only)
        r = unit * 0.08
        for k, heat in ((1.0, 0.55), (0.72, 0.3), (0.45, 0.12), (0.22, 0.0)):
            rr = r * k
            d.ellipse((cx - rr, cy - rr * 0.62, cx + rr, cy + rr * 0.62), fill=hot(heat) + (255,))
    # the sparks: out from the hit, streaks (long while fast) with a hot head, falling a little, cooling
    if t > 0.0:
        for ang, speed, size in SPARKS:
            dist = unit * speed * min(1, (t + 0.2) / 0.75)        # straight out, fast
            if t >= 0.95 and size < 0.85:
                continue                                  # the last: one or two
            x = cx + np.cos(ang) * dist
            y = cy + np.sin(ang) * dist * 0.75 + unit * 0.5 * t * t   # falling as they go
            trail = dist * 0.7 * (1 - 0.5 * t) + 1.5 * Z       # a long streak behind each, shortening
            tx, ty = x - np.cos(ang) * trail, y - np.sin(ang) * trail * 0.75
            heat = min(1, t * 0.95 + (1 - speed) * 0.25)
            d.line((tx, ty, x, y), fill=hot(min(1, heat + 0.2)) + (255,), width=max(2, int(Z * 0.9 * size)))
            rr = Z * 0.6 * size
            d.ellipse((x - rr, y - rr, x + rr, y + rr), fill=hot(heat * 0.7) + (255,))
    out = lines.filter(ImageFilter.GaussianBlur(Z * 0.3))
    out = out.resize((w * S, h * S), Image.LANCZOS)
    a = np.asarray(out).astype(np.float32)
    a[..., 3] = np.where(a[..., 3] < 110, 0, 255)    # the game's images: opaque or not
    Image.fromarray(a.astype(np.uint8), 'RGBA').save(os.path.join(HD, 'sprites', '%04d.png' % sid))
    print('impact', sid, w, h)
