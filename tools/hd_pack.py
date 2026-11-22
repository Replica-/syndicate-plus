# hd_pack.py - the HD texture pack (src/game/render.c loads it from USER_DIR/hd): the ground tiles (ground, roads,
# crossings, road marks) 4 times their size with Fooocus (its upscaler, then a light SDXL pass that keeps the
# original's look), each with a normal map.  Each tile is upscaled among copies of itself (as the map lays them), so its
# edges meet its neighbours.  Run with Fooocus's python_embeded
# from the Fooocus folder:
#
#   synd_headless ... --dump-tiles tiles.bin        (in a mission: the tiles in its palette, and their types)
#   set HD_SRC=tiles.bin & set HD_OUT=USER_DIR\hd & set FOOOCUS_DIR=...\Fooocus
#   ..\python_embeded\python.exe -s hd_pack.py
#
# Writes HD_OUT/tile_NNN.png (RGBA), tile_NNN_n.png (its normals) and tile_NNN_orig.png (the tile it was made from:
# the game re-tints the HD tile by the mission's colours of it).  HD_TILES=13,14 makes only those.  Flat ground's top
# is made as a square that repeats without a seam (Fooocus with circular padding), then laid back on the tile.
import os
import sys

FOO = os.environ.get('FOOOCUS_DIR', os.getcwd())
SRC = os.environ['HD_SRC']
OUT = os.environ['HD_OUT']
os.chdir(FOO)
sys.path.insert(0, FOO)
sys.argv = [sys.argv[0], '--disable-in-browser']

import args_manager  # noqa: F401
from modules.patch import patch_all

patch_all()
import numpy as np
from PIL import Image, ImageFilter

import modules.config as config
import modules.core as core
import modules.default_pipeline as pipeline
import modules.patch as patch
from modules.upscaler import perform_upscale

GROUND = (5, 6, 7, 8, 9, 0x0B, 0x0E, 0x0F)
PROMPTS = {5: 'top-down video game ground texture, dry rough dirt and gravel, small stones, detailed, sharp, same colours',
           0x0E: 'top-down video game texture, asphalt road with painted pedestrian crossing, detailed, sharp, same colours',
           0x0F: 'top-down video game texture, asphalt road with painted road markings, detailed, sharp, same colours'}
ROAD = 'top-down video game texture, dark asphalt road and kerb, detailed, sharp, same colours'
NEGATIVE = ('blurry, smooth, painterly, text, watermark, people, cars, noise, jpeg artifacts, stripes, lines, plants, '
            'leaves, moss, green, balls')
DENOISE = 0.35
ROAD_DENOISE = 0.3
W, H, S = 64, 48, 4

os.makedirs(OUT, exist_ok=True)
patch.patch_settings[os.getpid()] = patch.PatchSettings(sharpness=config.default_sample_sharpness)
conds = {}
negative = pipeline.clip_encode(texts=[NEGATIVE], pool_top_k=1)

d = open(SRC, 'rb').read()
n = 256 * W * H * 4
types = d[n:n + 256]
tiles = [np.frombuffer(d[i * W * H * 4:(i + 1) * W * H * 4], dtype=np.uint8).reshape(H, W, 4) for i in range(256)]


def bgra_to_rgba(t):
    return t[..., [2, 1, 0, 3]]


def context(t):
    """the tile among 3 x 3 copies of itself, as the map lays them; the gaps its mean colour"""
    cw, ch = W * 3, H + 64
    canvas = Image.new('RGBA', (cw, ch), tuple(int(v) for v in t[..., :3][t[..., 3] > 0].mean(0)) + (255,))
    im = Image.fromarray(t, 'RGBA')
    for j in range(-1, 2):
        for i in range(-1, 2):
            canvas.alpha_composite(im, (W + (i - j) * 32, 32 + (i + j) * 16))
    im_c = canvas.copy()
    im_c.alpha_composite(im, (W, 32))  # the middle one on top
    # the game's 1-pixel dithering blended into the colour it stands for (an upscaler takes it for detail)
    return np.array(im_c.convert('RGB').filter(ImageFilter.GaussianBlur(0.8))), (W, 32)


def circular(on):
    import torch
    for mod in (pipeline.final_unet.model, pipeline.final_vae.first_stage_model):
        for m in mod.modules():
            if isinstance(m, torch.nn.Conv2d):
                m.padding_mode = 'circular' if on else 'zeros'


def unfold(t):
    """a ground tile's top (a diamond 64 x 32) as the square it is on the ground, 32 x 32 (u along one edge, v along
    the other): the pixel at (32 + u - v, (u + v) / 2)"""
    sq = np.zeros((32, 32, 3), np.uint8)
    for v in range(32):
        for u in range(32):
            sq[v, u] = t[min((u + v) // 2, H - 1), min(max(32 + u - v, 0), W - 1), :3]
    return sq


def seamless_top(t, seed, prompt):
    """the top's square 4 times its size, repeating without a seam"""
    sq = np.array(Image.fromarray(unfold(t)).filter(ImageFilter.GaussianBlur(0.8)))
    ctx = np.tile(sq, (3, 3, 1))
    big = perform_upscale(ctx)
    big = np.array(Image.fromarray(big).resize((96 * S, 96 * S), Image.LANCZOS))[32 * S:64 * S, 32 * S:64 * S]
    circular(True)
    out = sdxl(big, seed, prompt, DENOISE)
    circular(False)
    return np.array(Image.fromarray(out).resize((32 * S, 32 * S), Image.LANCZOS))


def fold(top, base):
    """the top's square laid back on the tile's diamond, over `base` (the HD tile)"""
    out = base.copy()
    ys, xs = np.mgrid[0:H * S, 0:W * S]
    fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
    inside = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.15  # up to the stepped edge of its mask
    u = np.clip(((fy + (fx - 32) / 2) * S).astype(int), 0, 32 * S - 1)
    v = np.clip(((fy - (fx - 32) / 2) * S).astype(int), 0, 32 * S - 1)
    out[inside] = top[v[inside], u[inside]]
    return out


def sdxl(img, seed, prompt, denoise):
    if prompt not in conds:
        conds[prompt] = pipeline.clip_encode(texts=[prompt], pool_top_k=1)
    positive = conds[prompt]
    h, w = img.shape[:2]
    f = 1024.0 / max(h, w)
    big = np.array(Image.fromarray(img).resize((int(round(w * f / 8) * 8), int(round(h * f / 8) * 8)), Image.LANCZOS))
    latent = core.encode_vae(vae=pipeline.final_vae, pixels=core.numpy_to_pytorch(big), tiled=True)
    _, _, lh, lw = latent['samples'].shape
    steps = 30
    return pipeline.process_diffusion(positive_cond=positive, negative_cond=negative, steps=steps, switch=steps,
                                      width=lw * 8, height=lh * 8, image_seed=seed, callback=None,
                                      sampler_name=config.default_sampler, scheduler_name=config.default_scheduler,
                                      latent=latent, denoise=denoise, tiled=True, cfg_scale=config.default_cfg_scale,
                                      disable_preview=True)[0]


def normals(rgb, alpha):
    """a normal map from the brightness (bright raised): x right, y down, z out of the picture"""
    l = np.asarray(Image.fromarray(rgb).convert('L').filter(ImageFilter.GaussianBlur(1.2)), dtype=np.float32) / 255
    gx = np.zeros_like(l)
    gy = np.zeros_like(l)
    gx[:, 1:-1] = l[:, 2:] - l[:, :-2]
    gy[1:-1, :] = l[2:, :] - l[:-2, :]
    k = 2.5
    nx, ny, nz = -gx * k, -gy * k, np.ones_like(l)
    ln = np.sqrt(nx * nx + ny * ny + nz * nz)
    out = np.stack([(nx / ln + 1) * 127.5, (ny / ln + 1) * 127.5, (nz / ln + 1) * 127.5], -1)
    out[alpha == 0] = (128, 128, 255)
    return out.astype(np.uint8)


done = 0
only = [int(v) for v in os.environ.get('HD_TILES', '').split(',') if v]
for tid in range(256):
    if only and tid not in only:
        continue
    if types[tid] not in GROUND:
        continue
    t = bgra_to_rgba(tiles[tid])
    if t[..., 3].max() == 0:
        continue
    ctx, (ox, oy) = context(t)
    sr = perform_upscale(ctx)  # 4x
    sr = np.array(Image.fromarray(sr).resize((ctx.shape[1] * S, ctx.shape[0] * S), Image.LANCZOS))
    out = sdxl(sr, 1000 + tid, PROMPTS.get(types[tid], ROAD), DENOISE if types[tid] == 5 else ROAD_DENOISE)
    out = np.array(Image.fromarray(out).resize((ctx.shape[1] * S, ctx.shape[0] * S), Image.LANCZOS))
    crop = out[oy * S:(oy + H) * S, ox * S:(ox + W) * S]
    if types[tid] == 5:  # flat ground: its top repeats without a seam
        crop = fold(seamless_top(t, 2000 + tid, PROMPTS[5]), crop)
    alpha = np.array(Image.fromarray(t[..., 3]).resize((W * S, H * S), Image.NEAREST))
    Image.fromarray(np.dstack([crop, alpha]), 'RGBA').save(os.path.join(OUT, 'tile_%03d.png' % tid))
    Image.fromarray(normals(crop, alpha)).save(os.path.join(OUT, 'tile_%03d_n.png' % tid))
    Image.fromarray(t, 'RGBA').save(os.path.join(OUT, 'tile_%03d_orig.png' % tid))
    done += 1
    print('tile', tid, flush=True)

print('done', done)
