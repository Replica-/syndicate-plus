"""the pavement as a ground material (run with Fooocus's Python):   pavemat.py HDDIR [version...]
The slabs (paving.py's layout: 2 a tile's side, half a slab along from row to row and a little off, each its own shade
and tint, the buildings' brown) drawn top-down on a square that repeats without a seam (1024 texels = 8 x 8 tiles, 16 x
16 slabs), then redrawn by SDXL (img2img, circular padding) for the redrawn buildings' worn, gritty material.  The
result's colour brought back to the layout's mean (the buildings' brown).
   HDDIR/mat/pave/base.png, HDDIR/mat/pave/NN.png (the versions: strength and prompt)"""
import os, sys
HD = os.path.abspath(sys.argv[1])
only = [int(a) for a in sys.argv[2:]]
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

N = 1024                       # texels; 4 a game pixel
SLAB = 128                     # texels as SDXL draws them: 8 x 8 slabs, then made half the size and repeated 2 x 2
                               # (16 game pixels a slab: 2 a tile's side)
GROUT, BEVEL = 0.3 * 8, 1.3 * 8
COLOUR = np.array([108, 72, 45], np.float32)
PROMPTS = [
    'top-down orthographic photograph of old weathered terracotta paving slabs, rust brown, worn and gritty, '
    'stains and grime, dirt in the joints, chipped edges, dystopian industrial city street',
    'top-down orthographic photograph of grimy rust-brown stone pavers, scuffed, cracked corners, oil stains, '
    'dust and grit, dark dirty joints, gritty urban ground',
]
VERSIONS = [(0.50, 0), (0.60, 0), (0.70, 0), (0.50, 1), (0.60, 1), (0.70, 1)]   # (strength, prompt)
NEG = ('perspective, horizon, sky, people, cars, text, watermark, logo, blurry, lowres, cartoon, vignette, frame, '
       'border, grass, plants, neon, saturated, shiny, glossy, new, clean')


def layout():
    ys, xs = np.mgrid[0:N, 0:N]
    su, sv = (xs + 0.5) / SLAB, (ys + 0.5) / SLAB
    iv = np.floor(sv).astype(int)
    su = su + 0.5 * (iv % 2) + 0.07 * np.sin((iv % 2) * 2.3 + 0.7)
    iu = np.floor(su).astype(int)
    lu, lv = su - iu, sv - iv
    ku, kv = iu % (N // SLAB), iv % (N // SLAB)
    edge = np.minimum(np.minimum(lu, 1 - lu), np.minimum(lv, 1 - lv)) * SLAB
    soft = np.clip(edge / (GROUT + BEVEL), 0, 1)
    soft = soft * soft * (3 - 2 * soft)
    tone = 1 + 0.11 * np.sin(ku * 12.9898 + kv * 78.233) * np.cos(ku * 4.1414 - kv * 2.71)
    hue = np.sin(ku * 7.31 + kv * 13.7 + 1.9) * np.cos(ku * 2.17 - kv * 5.3)
    tint = np.stack([1 + 0.07 * hue, 1 + 0.01 * hue, 1 - 0.06 * hue], -1)
    face = 1 + 0.07 * (1 - (lu + lv))
    shade = (0.62 + 0.38 * soft) * (1 + 0.1 * soft) * face * tone
    # grime for the model to build on: blotches, a grain, dark and light specks (repeating: by wrapped noise)
    rng = np.random.default_rng(7)
    def noise(sigma):
        n = rng.normal(0, 1, (N, N))
        f = np.fft.fft2(n) * np.exp(-(np.fft.fftfreq(N)[:, None] ** 2 + np.fft.fftfreq(N)[None, :] ** 2) * (np.pi * sigma) ** 2 * 2)
        n = np.real(np.fft.ifft2(f))
        return n / max(1e-6, n.std())
    sp = rng.random((N, N))
    grime = (1 + 0.10 * noise(40)) * (1 + 0.07 * noise(1.2)) * np.where(sp < 0.02, 0.7, np.where(sp > 0.99, 1.2, 1.0))
    return (COLOUR * (shade * grime)[..., None] * tint).clip(0, 255)


def circular(on):
    for mod in (pipeline.final_unet.model, pipeline.final_vae.first_stage_model):
        for m in mod.modules():
            if isinstance(m, torch.nn.Conv2d):
                m.padding_mode = 'circular' if on else 'zeros'


def wrap(t):
    """blended with itself moved half its size (8 slabs, 8 rows: the layout lines up), so it repeats seamlessly"""
    n = t.shape[0]
    w = np.sin(np.pi * (np.arange(n) + 0.5) / n)
    w = (w[:, None] * w[None, :])[..., None]
    return t * w + np.roll(t, (n // 2, n // 2), (0, 1)) * (1 - w)


os.makedirs(os.path.join(HD, 'mat', 'pave'), exist_ok=True)
base = layout()
Image.fromarray(base.astype(np.uint8)).save(os.path.join(HD, 'mat', 'pave', 'base.png'))
patch_settings[os.getpid()] = PatchSettings(sharpness=2.0, adaptive_cfg=7.0)
neg = pipeline.clip_encode(texts=[NEG], pool_top_k=1)
pos = [pipeline.clip_encode(texts=[p], pool_top_k=1) for p in PROMPTS]
circular(True)
latent = core.encode_vae(vae=pipeline.final_vae, pixels=core.numpy_to_pytorch(base.astype(np.uint8)))
for k, (strength, p) in enumerate(VERSIONS):
    if only and k + 1 not in only:
        continue
    img = pipeline.process_diffusion(positive_cond=pos[p], negative_cond=neg, steps=30, switch=30, width=N, height=N,
                                     image_seed=9000 + k, callback=None, sampler_name='dpmpp_2m_sde_gpu',
                                     scheduler_name='karras', latent=latent, denoise=strength, tiled=False,
                                     cfg_scale=4.0, disable_preview=True)[0]
    out = wrap(np.asarray(img).astype(np.float32))
    out = out * (base.mean((0, 1)) / np.maximum(out.mean((0, 1)), 1))       # the buildings' brown
    half = np.asarray(Image.fromarray(out.clip(0, 255).astype(np.uint8)).resize((N // 2, N // 2), Image.LANCZOS))
    out = np.tile(half, (2, 2, 1)).astype(np.float32)
    Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(os.path.join(HD, 'mat', 'pave', '%02d.png' % (k + 1)))
    print('pave', k + 1, flush=True)
circular(False)
