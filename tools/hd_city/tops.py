"""Seamless tops for the flat tiles that lie side by side (run with Fooocus's Python, after redraw.py):
   tops.py HDDIR
For each group: the top (a 64 x 32 diamond) of its first tile unfolded into the 32 x 32 square it is on the ground
(its outline ring dropped), made 4x and redrawn with circular padding (it repeats without a seam), its colour brought
to the base's; then laid on the top of every tile of the group (HDDIR/out/NNN.png), over the redraw's."""
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
import modules.core as core
from modules.upscaler import perform_upscale

W, H, S = 64, 48, 4
STYLE = ('seamless top-down ground texture, dystopian cyberpunk industrial city, gritty, weathered, dark moody, '
         'highly detailed, sharp, physically based material')
NEG = 'blurry, lowres, text, watermark, people, cars, perspective, green, cartoon, pixel art, dithering, grid, tiles, lines, stones, pebbles, cobbles, rocks'
# (tiles, what it is, the colour its mean is graded to (None: the base top's), the tile whose top it is made from)
GROUPS = [
    ([13, 14], 'fine dry dark brown earth soil, dusty compacted dirt, faint tyre marks, no stones, no pebbles, no cobbles', (86, 64, 46), 13),
    ([76, 79, 160, 168, 169, 170], 'black steel rooftop plates with square raised panels and rivets', None, 160),
    ([159], 'black steel floor plate with a fine perforated mesh', None, 159),
    ([156, 158], 'matte black tar roof surface, fine grit, faint dust', None, 156),
]
# the colours of the base kept over the shared top (markings, stripes, lamps): yellow, white, red, cyan
KEEP = [(255, 190, 0), (223, 255, 255), (255, 60, 8), (0, 142, 186), (170, 36, 0), (150, 121, 24)]
patch_settings[os.getpid()] = PatchSettings(sharpness=2.0, adaptive_cfg=7.0)
neg = pipeline.clip_encode(texts=[NEG], pool_top_k=1)


def circular(on):
    for mod in (pipeline.final_unet.model, pipeline.final_vae.first_stage_model):
        for m in mod.modules():
            if isinstance(m, torch.nn.Conv2d):
                m.padding_mode = 'circular' if on else 'zeros'


def unfold(t):
    sq = np.zeros((32, 32, 3), np.uint8)
    for v in range(32):
        for u in range(32):
            sq[v, u] = t[min((u + v) // 2, H - 1), min(max(32 + u - v, 0), W - 1), :3]
    return sq


def wrap(t):
    """seamless by construction: blended with itself moved half its size, its own middle at the edges (where the
    moved copy joins) and the moved copy's middle where the first wraps"""
    n = t.shape[0]
    w = np.sin(np.pi * (np.arange(n) + 0.5) / n)
    w = (w[:, None] * w[None, :])[..., None]
    return t * w + np.roll(t, (n // 2, n // 2), (0, 1)) * (1 - w)


def fold(top, base, orig):
    out = base.copy()
    keep = np.zeros(orig.shape[:2], bool)
    for c in KEEP:
        keep |= (np.abs(orig[..., :3].astype(int) - c).sum(-1) < 12) & (orig[..., 3] > 0)
    keep = np.asarray(Image.fromarray(keep.astype(np.uint8) * 255).resize((W * S, H * S), Image.NEAREST)) > 0
    ys, xs = np.mgrid[0:H * S, 0:W * S]
    fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
    inside = (np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.06) & (base[..., 3] > 0) & ~keep
    u = np.clip(((fy + (fx - 32) / 2) * S).astype(int), 0, 32 * S - 1)
    v = np.clip(((fy - (fx - 32) / 2) * S).astype(int), 0, 32 * S - 1)
    out[..., :3][inside] = top[v[inside], u[inside]]
    return out


for ids, prompt, tint, src in GROUPS:
    t = np.asarray(Image.open(os.path.join(HD, 'base', '%03d.png' % src)).convert('RGBA'))
    sq = unfold(t)[1:31, 1:31]
    sq = np.asarray(Image.fromarray(sq).resize((32, 32), Image.LANCZOS).filter(ImageFilter.GaussianBlur(1.0)))
    big = perform_upscale(np.tile(sq, (3, 3, 1)))                       # 4x of 96 x 96
    big = np.asarray(Image.fromarray(big).resize((96 * 8, 96 * 8), Image.LANCZOS))[256:512, 256:512]
    big = np.asarray(Image.fromarray(big).resize((1024, 1024), Image.LANCZOS).filter(ImageFilter.GaussianBlur(6)))
    latent = core.encode_vae(vae=pipeline.final_vae, pixels=core.numpy_to_pytorch(big))
    pos = pipeline.clip_encode(texts=[prompt + ', ' + STYLE], pool_top_k=1)
    circular(True)
    img = pipeline.process_diffusion(positive_cond=pos, negative_cond=neg, steps=24, switch=24, width=1024, height=1024,
                                     image_seed=3000 + src, callback=None, sampler_name='dpmpp_2m_sde_gpu',
                                     scheduler_name='karras', latent=latent, denoise=1.0 if tint is not None else 0.6, tiled=False, cfg_scale=4.0,
                                     disable_preview=True)[0]
    circular(False)
    top = wrap(np.asarray(Image.fromarray(img).resize((32 * S, 32 * S), Image.LANCZOS)).astype(np.float32))
    # its colour (each channel's mean) brought to the base's top, a little darker as the other tiles
    if tint is None:
        want = sq.reshape(-1, 3).astype(np.float32).mean(0) * 0.9
        top = top * (want / np.maximum(top.reshape(-1, 3).mean(0), 1))
    else:
        # graded: its light (and a little of its own colour) in the colour wanted
        l = top.mean(-1, keepdims=True)
        top = l / max(1, l.mean()) * np.array(tint, np.float32) * 0.8 + top * (np.array(tint).mean() / max(1, top.mean())) * 0.2
    top = np.clip(top, 0, 255).astype(np.uint8)
    Image.fromarray(top).save(os.path.join(HD, 'ai', 'top_%03d.png' % src))
    for k in ids:
        p = os.path.join(HD, 'out', '%03d.png' % k)
        if os.path.exists(p):
            orig = np.asarray(Image.open(os.path.join(HD, 'base', '%03d.png' % k)).convert('RGBA'))
            Image.fromarray(fold(top, np.asarray(Image.open(p).convert('RGBA')), orig), 'RGBA').save(p)
            print('top', k, flush=True)
