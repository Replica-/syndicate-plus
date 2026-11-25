"""The ground materials of the HD pack (run with Fooocus's Python):   materials.py HDDIR
Each a 1024 x 1024 square that repeats without a seam, laid on the ground in world space by the game (4 texture pixels
a game pixel: 8 x 8 tiles before it repeats): HDDIR/mat/material_KK.png.  Circular padding in SDXL, then blended with
itself moved half its size (seamless whatever the model made)."""
import os, sys
HD = os.path.abspath(sys.argv[1])
only = [int(a) for a in sys.argv[2:]]
FOOOCUS = 'C:/development/fooocus/Fooocus_win64_2-5-0/Fooocus'
sys.argv = [sys.argv[0]]
sys.path.insert(0, FOOOCUS)
os.chdir(FOOOCUS)
import numpy as np
import torch
from PIL import Image
from modules import config
config.update_files()
from modules.patch import PatchSettings, patch_settings, patch_all
patch_all()
import modules.default_pipeline as pipeline

STYLE = ('seamless tileable top-down orthographic photograph of a surface texture, flat, even diffuse lighting, '
         'gritty dystopian industrial city, realistic, highly detailed, sharp, physically based material scan')
NEG = ('neon, glowing, fluorescent, saturated colours, psychedelic, abstract, perspective, horizon, sky, people, cars, text, watermark, logo, blurry, lowres, cartoon, vignette, frame, '
       'border, grid, tiles, repeated pattern, shadows of objects, blocks, bricks, slabs, paving stones, cobbles')
MATERIALS = [
    'coarse sand and grit, tiny stones, dusty ground',                                     # 0 dirt (see OWN)
    'close-up of uniform dark asphalt tarmac, fine aggregate grain, slightly wet, faint oil stains, seamless, continuous surface',  # 1 asphalt
    'close-up of a continuous poured concrete floor, fine grit, grime and stains, damp patches, seamless',          # 2 concrete
    'dark gunmetal steel roof plates with rivets and seams, rain streaks, scratches',                   # 3 roof plates
    'black tar roof membrane, wet, small puddles, grit',                                                # 4 tar roof
    'close-up of a continuous dark steel diamond tread plate, worn, scratched, damp',                                     # 5 grating
    'patchy grass seen from directly above, olive green tufts and bare dark soil patches',  # 6 grass (see OWN)
]
SMALLER = {6: 2}
# materials made with a style, negative and seed of their own (the user's picks: the dirt, number 4 of 20; the
# grass, number 3 of 4)
GROUND = ('seamless tileable top-down orthographic photograph of a ground texture, flat, even diffuse lighting, '
          'realistic, highly detailed, sharp, physically based material scan')
GROUND_NEG = ('perspective, horizon, sky, people, cars, text, watermark, logo, blurry, lowres, cartoon, vignette, '
              'frame, border, grid, tiles, bricks, slabs, paving stones, cobbles, neon, ')
OWN = {0: (GROUND, GROUND_NEG + 'plants, leaves', 7003), 6: (GROUND, GROUND_NEG + 'flowers, leaves, trees', 8002)}
patch_settings[os.getpid()] = PatchSettings(sharpness=2.0, adaptive_cfg=7.0)
neg = pipeline.clip_encode(texts=[NEG], pool_top_k=1)
os.makedirs(os.path.join(HD, 'mat'), exist_ok=True)


def circular(on):
    for mod in (pipeline.final_unet.model, pipeline.final_vae.first_stage_model):
        for m in mod.modules():
            if isinstance(m, torch.nn.Conv2d):
                m.padding_mode = 'circular' if on else 'zeros'


def wrap(t):
    n = t.shape[0]
    w = np.sin(np.pi * (np.arange(n) + 0.5) / n)
    w = (w[:, None] * w[None, :])[..., None]
    return t * w + np.roll(t, (n // 2, n // 2), (0, 1)) * (1 - w)


for k, prompt in enumerate(MATERIALS):
    if only and k not in only:
        continue
    style, k_neg, seed = OWN.get(k, (STYLE, None, 5000 + k))
    pos = pipeline.clip_encode(texts=[prompt + ', ' + style], pool_top_k=1)
    circular(True)
    img = pipeline.process_diffusion(positive_cond=pos, negative_cond=pipeline.clip_encode(texts=[k_neg], pool_top_k=1)
                                     if k_neg else neg, steps=30, switch=30, width=1024, height=1024,
                                     image_seed=seed, callback=None, sampler_name='dpmpp_2m_sde_gpu',
                                     scheduler_name='karras', latent=None, denoise=1.0, tiled=False, cfg_scale=4.0,
                                     disable_preview=True)[0]
    circular(False)
    out = wrap(np.asarray(img).astype(np.float32))
    if k in SMALLER:                 # made smaller and repeated (its stones as small as the game's)
        n = 1024 // SMALLER[k]
        small = np.asarray(Image.fromarray(out.clip(0, 255).astype(np.uint8)).resize((n, n), Image.LANCZOS))
        out = np.tile(small, (SMALLER[k], SMALLER[k], 1)).astype(np.float32)
    Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(os.path.join(HD, 'mat', 'material_%02d.png' % k))
    print('material', k, flush=True)
