"""Roads and pavements as the game draws them, in HD (run with Fooocus's Python, after redraw.py and tops.py):
   faithful.py HDDIR
The tile (its base, in the new colours) among 3 x 3 copies of itself as the map lays them, its 1-pixel dithering
blended, made 4x by ESRGAN (no redraw: every marking, groove, light and kerb where the game has it); then a fine
grain of its material (a square that repeats without a seam, laid on the ground) adds detail, not shapes.  Its top
replaces the one in HDDIR/out/NNN.png; its sides stay the redraw's."""
import os, sys
HD = os.path.abspath(sys.argv[1])
FOOOCUS = 'C:/development/fooocus/Fooocus_win64_2-5-0/Fooocus'
sys.argv = [sys.argv[0]]
sys.path.insert(0, FOOOCUS)
os.chdir(FOOOCUS)
import numpy as np
import torch
from PIL import Image, ImageFilter
from modules import config
config.update_files()
from modules.patch import PatchSettings, patch_settings, patch_all
patch_all()
import modules.default_pipeline as pipeline
from modules.upscaler import perform_upscale

W, H, S = 64, 48, 4
ROADS = [100, 102, 103, 104, 106, 107, 108, 109, 110, 111, 112, 113, 119, 120, 121, 122, 123]
PAVED = [35, 36, 41, 42, 43, 176, 203, 207, 251, 252]
GRAINS = {
    'asphalt': ('seamless top-down texture of fine dark asphalt grain, tiny aggregate, subtle wear, uniform, flat, '
                'no lines, no stones, no cracks', ROADS),
    'concrete': ('seamless top-down texture of fine worn concrete grit, subtle stains, uniform, flat, no lines, '
                 'no stones, no tiles', PAVED),
}
STRENGTH = 0.45
# the roads in asphalt greys and white markings (the screenshot's palette makes them teal and pink): its colours
# there -> the road's; its rust and ochre (the grooves, ridges, arrows and bays) in lighter asphalt greys: a road is
# a road, its pattern shows in its greys; the yellow and white markings as they are
ROAD_COLOURS = {(60, 73, 69): (50, 52, 56), (52, 56, 65): (36, 37, 40), (190, 154, 154): (222, 222, 212),
                (77, 89, 121): (72, 74, 80), (101, 101, 101): (92, 92, 94),
                (174, 93, 60): (64, 66, 70), (203, 134, 81): (80, 82, 86), (255, 166, 97): (96, 97, 100),
                (113, 85, 4): (58, 60, 63), (150, 121, 24): (70, 72, 75)}
# how much the grain lightens and darkens
DARKER = 0.88     # as the redrawn tiles
patch_settings[os.getpid()] = PatchSettings(sharpness=2.0, adaptive_cfg=7.0)
neg = pipeline.clip_encode(texts=['text, watermark, lines, stripes, grid, tiles, stones, cobbles, perspective'],
                           pool_top_k=1)


def circular(on):
    for mod in (pipeline.final_unet.model, pipeline.final_vae.first_stage_model):
        for m in mod.modules():
            if isinstance(m, torch.nn.Conv2d):
                m.padding_mode = 'circular' if on else 'zeros'


def grain(prompt, seed):
    """a seamless square of grain, 32 * S, as a high-pass: 0 the mean, about +-1 its detail"""
    pos = pipeline.clip_encode(texts=[prompt], pool_top_k=1)
    circular(True)
    img = pipeline.process_diffusion(positive_cond=pos, negative_cond=neg, steps=24, switch=24, width=1024,
                                     height=1024, image_seed=seed, callback=None, sampler_name='dpmpp_2m_sde_gpu',
                                     scheduler_name='karras', latent=None, denoise=1.0, tiled=False, cfg_scale=4.0,
                                     disable_preview=True)[0]
    circular(False)
    l = np.asarray(Image.fromarray(img).convert('L').resize((32 * S, 32 * S), Image.LANCZOS)).astype(np.float32)
    n = l.shape[0]                                   # seamless: blended with itself moved half its size
    w = np.sin(np.pi * (np.arange(n) + 0.5) / n)
    w = w[:, None] * w[None, :]
    l = l * w + np.roll(l, (n // 2, n // 2), (0, 1)) * (1 - w)
    # the blur of a repeating square, repeating (3 x 3, the middle)
    big = Image.fromarray(np.tile(l, (3, 3)).clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(8))
    low = np.asarray(big).astype(np.float32)[32 * S:64 * S, 32 * S:64 * S]
    hp = (l - low) / max(1.0, l.mean())
    return hp / max(1e-3, np.abs(hp).mean() * 2.5)


def faithful(t):
    """the tile 4x by ESRGAN, among copies of itself (its edges as its neighbours make them)"""
    mean = tuple(int(v) for v in t[..., :3][t[..., 3] > 0].mean(0)) + (255,)
    canvas = Image.new('RGBA', (W * 3, H + 64), mean)
    im = Image.fromarray(t, 'RGBA')
    for j in range(-1, 2):
        for i in range(-1, 2):
            canvas.alpha_composite(im, (W + (i - j) * 32, 32 + (i + j) * 16))
    canvas.alpha_composite(im, (W, 32))
    ctx = np.asarray(canvas.convert('RGB').filter(ImageFilter.GaussianBlur(0.5)))
    up = perform_upscale(ctx)
    up = np.asarray(Image.fromarray(up).resize((W * 3 * S, (H + 64) * S), Image.LANCZOS)).astype(np.float32)
    return up[32 * S:(32 + H) * S, W * S:2 * W * S]


ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
U = np.clip(((fy + (fx - 32) / 2) * S).astype(int), 0, 32 * S - 1)
V = np.clip(((fy - (fx - 32) / 2) * S).astype(int), 0, 32 * S - 1)
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0
for name, (prompt, ids) in GRAINS.items():
    g = grain(prompt, 4000 + len(name))
    Image.fromarray(((g * 0.25 + 0.5).clip(0, 1) * 255).astype(np.uint8)).save(os.path.join(HD, 'ai', 'grain_%s.png' % name))
    for k in ids:
        p = os.path.join(HD, 'out', '%03d.png' % k)
        if not os.path.exists(p):
            continue
        base = np.asarray(Image.open(os.path.join(HD, 'base', '%03d.png' % k)).convert('RGBA')).copy()
        if k in ROADS:
            src = base.copy()
            for a, b in ROAD_COLOURS.items():
                base[..., :3][(src[..., :3] == a).all(-1)] = b
        hd = faithful(base) * DARKER
        hd = hd * (1 + STRENGTH * g[V, U])[..., None]
        out = np.asarray(Image.open(p).convert('RGBA')).copy()
        top = TOP & (out[..., 3] > 0)
        out[..., :3][top] = hd.clip(0, 255).astype(np.uint8)[top]
        Image.fromarray(out, 'RGBA').save(p)
        print('faithful', k, flush=True)
