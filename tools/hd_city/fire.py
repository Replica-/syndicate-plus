"""HD fire, dynamic (the flames drawn moving by the game's shader from each pixel's heat; the smoke drawn here)
(the burning object's frames: blazes 1756-1759, flame tongues and smoke wisps 992-1002), drawn in the game's
own silhouettes:   fire.py HDDIR SPRITEDIR -> HDDIR/sprites/NNNN.png
Each sprite 4x, its outline the game's (smoothed); inside it turbulent fire: white-yellow where the game's pixels are
hottest, orange, red to the edges and the tips, licks of noise rising; the grey wisps dark, rough smoke."""
import os, sys
import numpy as np
from PIL import Image, ImageFilter
HD, SRC = sys.argv[1], sys.argv[2]
S = 4
IDS = list(range(992, 1003)) + [1756, 1757, 1758, 1759]
SMOKE_IDS = {996, 997}                 # the blaze's frames' grey wisps: its smoke
FIRE = [(0.0, (60, 8, 4)), (0.25, (150, 24, 6)), (0.45, (230, 80, 12)), (0.65, (255, 160, 40)),
        (0.82, (255, 225, 120)), (1.0, (255, 250, 230))]
SMOKE = [(0.0, (30, 28, 27)), (0.6, (62, 58, 55)), (1.0, (96, 90, 86))]


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


os.makedirs(os.path.join(HD, 'sprites'), exist_ok=True)
for sid in IDS:
    spr = Image.open(os.path.join(SRC, '%04d.png' % sid)).convert('RGBA')
    w, h = spr.size
    a = np.asarray(spr).astype(np.float32)
    W, H = w * S, h * S
    # the game's heat: its pixels' brightness, smoothed (hottest where it drew white and yellow)
    lum = a[..., :3].mean(-1) / 255 * (a[..., 3] > 0)
    # spread from the bright cores (a thin flame is mostly black outline: its core's heat for all of it)
    core = Image.fromarray((lum * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
    heat = np.asarray(core.resize((W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(S * 1.0))).astype(np.float32) / 255
    heat = np.clip(heat / max(0.35, float(heat.max())) * 0.95, 0, 1)
    alpha = np.asarray(spr.split()[3].resize((W, H), Image.BILINEAR).filter(ImageFilter.GaussianBlur(S * 0.5))) / 255
    ys = np.linspace(0, 1, H)[:, None]
    licks = noise(H, W, 3 * S, sid) * 0.6 + noise(H, W, S, sid + 7) * 0.4
    sat = a[..., :3].max(-1) - a[..., :3].min(-1)
    lit = (a[..., 3] > 0) & (a[..., :3].max(-1) > 40)       # not the black outline
    grey = sid in SMOKE_IDS or ((sat < 30) & lit).sum() > 0.6 * max(1, lit.sum())
    if grey:                                           # a smoke wisp
        t = heat * 0.6 + licks * 0.4
        col = ramp(SMOKE, t)
    else:
        t = heat * 1.15 + (licks - 0.5) * 0.45 - 0.25 * (1 - ys)   # cooler towards the tips
        col = ramp(FIRE, t)
    shape = (alpha > 0.45) & ((licks > 0.18 + 0.35 * (1 - ys)) | (alpha > 0.85))   # ragged edges, licks at the top
    if grey:                                           # dynamic smoke (alpha 233, its density in red): soft, rising
        dens = np.asarray(spr.split()[3].resize((W, H), Image.BILINEAR).filter(ImageFilter.GaussianBlur(S * 1.5))) / 255
        body = dens > 0.05
        out = np.dstack([np.clip(dens * 1.6, 0, 1) * 255, dens * 0, dens * 0, np.where(body, 233, 0)]).astype(np.uint8)
    else:                                              # dynamic: the game's shader draws the flames (alpha 232: fire,
        heat8 = (np.clip(heat * 1.15, 0, 1) * 255)     # red its heat), moving; the outline a little generous
        body = alpha > 0.3
        out = np.dstack([heat8, heat8 * 0, heat8 * 0, np.where(body, 232, 0)]).astype(np.uint8)
    Image.fromarray(out, 'RGBA').save(os.path.join(HD, 'sprites', '%04d.png' % sid))
    print('fire', sid, 'smoke' if grey else '')

# the burning car's sprites (its flames with their smoke trail 1005-1008, the explosion's puff 765-779, the smoke
# columns 841-842): each pixel fire (alpha 232, heat in red) where the game drew it bright and warm, smoke (alpha 233,
# density in red) where dark and grey; both drawn moving by the game's shader
MIXED = list(range(765, 780)) + [841, 842] + list(range(1005, 1009))
for sid in MIXED:
    p = os.path.join(SRC, '%04d.png' % sid)
    if not os.path.exists(p):
        continue
    spr = Image.open(p).convert('RGBA')
    w, h = spr.size
    a = np.asarray(spr).astype(np.float32)
    W, H = w * S, h * S
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    op = a[..., 3] > 0
    warm = op & (r > 120) & (r - b > 50)
    lum = a[..., :3].mean(-1) / 255
    heat_src = np.where(warm, lum, 0)
    core = Image.fromarray((heat_src * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
    heat = np.asarray(core.resize((W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(S * 0.9))).astype(np.float32) / 255
    if heat.max() > 0:
        heat = np.clip(heat / max(0.35, float(heat.max())) * 0.95, 0, 1)
    fire_m = np.asarray(Image.fromarray((warm * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
                        .resize((W, H), Image.BILINEAR).filter(ImageFilter.GaussianBlur(S * 0.6))) / 255 > 0.3
    body = np.asarray(spr.split()[3].resize((W, H), Image.BILINEAR).filter(ImageFilter.GaussianBlur(S * 1.2))) / 255
    smoke_m = (body > 0.15) & ~fire_m
    dens = np.clip(body * 1.4, 0, 1)
    out = np.zeros((H, W, 4), np.uint8)
    out[..., 0] = np.where(fire_m, heat * 255, dens * 255).astype(np.uint8)
    out[..., 3] = np.where(fire_m, 232, np.where(smoke_m, 233, 0))
    Image.fromarray(out, 'RGBA').save(os.path.join(HD, 'sprites', '%04d.png' % sid))
    print('mixed', sid, int(fire_m.sum()), int(smoke_m.sum()))
