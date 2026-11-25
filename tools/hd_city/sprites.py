"""HD sprites (run with Fooocus's Python):   sprites.py HDDIR SPRITEDIR [--denoise D] [--tag T] id...
SPRITEDIR: the sprites as the game draws them (SYND_DUMP_SPRITES=DIR, a mission run headless: NNNN.png).  Each
sprite enlarged on the ground's dark olive, redrawn img2img by SDXL with its description, cut out along the
sprite's own outline (smoothed at 4x: the shape stays the game's), made 4x its size: HDDIR/sprites[T]/NNNN.png; install.py puts them in the
pack as sprite_NNNN.png."""
import os, sys
args = sys.argv[1:]
denoise, tag = 0.6, ''
if '--denoise' in args:
    i = args.index('--denoise'); denoise = float(args[i + 1]); del args[i:i + 2]
if '--tag' in args:
    i = args.index('--tag'); tag = args[i + 1]; del args[i:i + 2]
HD, SRC = os.path.abspath(args[0]), os.path.abspath(args[1])
ids = [int(a) for a in args[2:]]
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

DESC = {
    1571: 'a leafy tree with dense clusters of small olive green and ochre autumn leaves, thin dark trunk and branches',
    1572: 'a small bushy tree, dense clusters of small olive green and ochre autumn leaves, short dark trunk',
    1589: 'a tall leafy tree with dense clusters of small olive green and ochre autumn leaves, thin dark trunk',
}
STYLE = ('isometric game sprite, three-quarter top-down view, dystopian city, realistic, highly detailed foliage, '
         'sharp, on a plain dark olive background')
NEG = ('ground, grass, shadow, soil, pot, multiple trees, text, watermark, frame, border, blurry, lowres, cartoon, '
       'pixel art, dithering, neon, bright green, flowers')
KEY = np.array([58, 50, 28], np.float32)        # the ground behind it
Z = 12                                   # the sprite's pixels to SDXL's (then 3 times smaller: 4x)
patch_settings[os.getpid()] = PatchSettings(sharpness=2.0, adaptive_cfg=7.0)
neg = pipeline.clip_encode(texts=[NEG], pool_top_k=1)
out_dir = os.path.join(HD, 'sprites' + tag)
os.makedirs(out_dir, exist_ok=True)
for s in ids:
    spr = Image.open(os.path.join(SRC, '%04d.png' % s)).convert('RGBA')
    w, h = spr.size
    cw, ch = ((w * Z + 191) // 64) * 64, ((h * Z + 191) // 64) * 64      # a margin, multiples of 64
    canvas = Image.new('RGBA', (cw, ch), tuple(int(v) for v in KEY) + (255,))
    big = spr.resize((w * Z, h * Z), Image.NEAREST).filter(ImageFilter.GaussianBlur(Z * 0.12))
    ox, oy = (cw - w * Z) // 2, (ch - h * Z) // 2
    canvas.alpha_composite(big, (ox, oy))
    latent = core.encode_vae(vae=pipeline.final_vae, pixels=core.numpy_to_pytorch(np.asarray(canvas.convert('RGB'))))
    pos = pipeline.clip_encode(texts=[DESC.get(s, 'a game object') + ', ' + STYLE], pool_top_k=1)
    img = pipeline.process_diffusion(positive_cond=pos, negative_cond=neg, steps=30, switch=30, width=cw, height=ch,
                                     image_seed=3000 + s, callback=None, sampler_name='dpmpp_2m_sde_gpu',
                                     scheduler_name='karras', latent=latent, denoise=denoise, tiled=False,
                                     cfg_scale=4.0, disable_preview=True)[0]
    Image.fromarray(img).save(os.path.join(out_dir, '%04d_ai.png' % s))
    a = np.asarray(img).astype(np.float32)[oy:oy + h * Z, ox:ox + w * Z]
    # the outline: the sprite's, smoothed (its pixel steps rounded)
    edge = spr.split()[3].resize((w * Z, h * Z), Image.BILINEAR).filter(ImageFilter.GaussianBlur(Z * 0.3))
    alpha = np.clip((np.asarray(edge).astype(np.float32) / 255 - 0.35) / 0.2, 0, 1)
    res = np.dstack([a.clip(0, 255), alpha * 255]).astype(np.uint8)
    Image.fromarray(res, 'RGBA').resize((w * 4, h * 4), Image.LANCZOS).save(os.path.join(out_dir, '%04d.png' % s))
    print('sprite', s, flush=True)
