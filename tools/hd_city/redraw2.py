"""HD redraw of the Mission 1 tiles with Fooocus (SDXL): run with Fooocus's own Python.
   redraw.py HDDIR [ids...] [--denoise D]
Each base tile (64x48, HPAL01 colours) is blown up 16x, softened (the dithering dissolved), redrawn img2img with its
description, brought down to 4x (256x192), its colours locked to the base's, cut to the base's exact outline."""
import os, sys, random
HD = os.path.abspath(sys.argv[1])
args = sys.argv[2:]
denoise = 0.55
if '--denoise' in args:
    i = args.index('--denoise'); denoise = float(args[i + 1]); del args[i:i + 2]
sys.path.insert(0, HD)
from desc import DESC
ids = [int(a) for a in args] or sorted(DESC)

FOOOCUS = 'C:/development/fooocus/Fooocus_win64_2-5-0/Fooocus'
sys.argv = [sys.argv[0]]
sys.path.insert(0, FOOOCUS)
os.chdir(FOOOCUS)
import numpy as np
from PIL import Image, ImageFilter
from modules import config
config.update_files()
from modules.patch import PatchSettings, patch_settings, patch_all
patch_all()
import modules.default_pipeline as pipeline
import modules.core as core

STYLE = ('isometric game asset, orthographic three-quarter top-down view, dystopian cyberpunk megacity, '
         'dark moody, rain-slicked, dark steel, wet concrete, weathered rust, small cyan and magenta neon accent lights, '
         'highly detailed, sharp, physically based materials')
NEG = ('blurry, lowres, jpeg artifacts, watermark, text, logo, signature, people, cars, perspective, green, '
       'cartoon, pixel art, dithering, noise, bright daylight, pastel')
patch_settings[os.getpid()] = PatchSettings(sharpness=2.0, adaptive_cfg=7.0)
neg = pipeline.clip_encode(texts=[NEG], pool_top_k=1)
os.makedirs(os.path.join(HD, 'ai'), exist_ok=True)
os.makedirs(os.path.join(HD, 'out'), exist_ok=True)


def ycc(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    return y, b - y, r - y


def rgb(y, cb, cr):
    r = cr + y; b = cb + y; g = (y - 0.299 * r - 0.114 * b) / 0.587
    return np.clip(np.stack([r, g, b], -1), 0, 255)


for t in ids:
    src = os.path.join(HD, 'base', '%03d.png' % t)
    if t not in DESC or not os.path.exists(src):
        continue
    base = Image.open(src).convert('RGBA')
    a = np.asarray(base).astype(np.float32)
    if a[..., 3].max() == 0:
        continue
    # the transparent part filled with the tile's own mean colour (no magenta for the model to keep)
    m = a[..., 3:] / 255
    mean = (a[..., :3] * m).sum((0, 1)) / max(1, m.sum())
    flat = Image.fromarray((a[..., :3] * m + mean * (1 - m)).astype(np.uint8))
    big = flat.resize((1024, 768), Image.NEAREST).filter(ImageFilter.GaussianBlur(7))
    latent = core.encode_vae(vae=pipeline.final_vae, pixels=core.numpy_to_pytorch(np.asarray(big)))
    pos = pipeline.clip_encode(texts=[DESC[t] + ', ' + STYLE], pool_top_k=1)
    img = pipeline.process_diffusion(positive_cond=pos, negative_cond=neg, steps=24, switch=24, width=1024, height=768,
                                     image_seed=1000 + t, callback=None, sampler_name='dpmpp_2m_sde_gpu',
                                     scheduler_name='karras', latent=latent, denoise=denoise, tiled=False,
                                     cfg_scale=4.0, disable_preview=True)[0]
    Image.fromarray(img).save(os.path.join(HD, 'ai', '%03d.png' % t))
    # down to 4x; the colour (and a part of the light) from the base, softened; the detail from the redraw
    res = np.asarray(Image.fromarray(img).resize((256, 192), Image.LANCZOS)).astype(np.float32)
    ref = np.asarray(flat.resize((256, 192), Image.NEAREST).filter(ImageFilter.GaussianBlur(3))).astype(np.float32)
    ry, rcb, rcr = ycc(res)
    by, bcb, bcr = ycc(ref)
    ry_soft = np.asarray(Image.fromarray(np.clip(ry, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3))).astype(np.float32)
    y = by * 0.3 + ry_soft * 0.7 + (ry - ry_soft) * 1.15  # mostly the redraw's light and detail
    out = rgb(y * 0.85, bcb * 0.25 + rcb * 0.75, bcr * 0.25 + rcr * 0.75)  # the redraw's colours
    alpha = np.asarray(base.resize((256, 192), Image.NEAREST))[..., 3:]
    Image.fromarray(np.concatenate([out, alpha], -1).astype(np.uint8), 'RGBA').save(os.path.join(HD, 'out', '%03d.png' % t))
    print('tile', t, flush=True)
