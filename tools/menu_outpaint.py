# menu_outpaint.py - the menus' desk photo outpainted to 576 x 200 (21:9 at the game's pixel shape) with Fooocus's
# inpaint engine, for tools/menu_sides.py.  Run with Fooocus's python_embeded, the photo (the last frame of
# MSCRENUP.DAT in true colour, 320 x 200) as MSCRENUP_rgb.png in MENU_DIR, where they are written:
#
#     FOOOCUS_DIR=... MENU_DIR=... python_embeded/python.exe -s menu_outpaint.py [SEEDS]
#
# The frame's lines are taken out first (their pixels those beside them), then the left side and the right side are
# outpainted in turn from the picture beside them, as Fooocus's own outpaint does.  out_SEED.png: 1728 x 720.
import os
import sys

FOO = os.environ['FOOOCUS_DIR']
HERE = os.environ['MENU_DIR']
SEEDS = [int(s) for s in sys.argv[1:]] or [1, 2, 3]
os.chdir(FOO)
sys.path.insert(0, FOO)
sys.argv = [sys.argv[0], '--disable-in-browser']

import args_manager  # noqa: F401
from modules.patch import patch_all

patch_all()
import numpy as np
from PIL import Image

import modules.config as config
import modules.core as core
import modules.default_pipeline as pipeline
import modules.inpaint_worker as inpaint_worker
import modules.patch as patch

W, H, CW = 576, 200, 320          # the wide picture, the game's
SX, SY = 3, 3.6                   # to square pixels, 3 times (320 x 200 is shown 4:3)
OFF = (W - CW) // 2
PROMPT = ('very blurry out of focus dim green monochrome photograph, night vision green tint, dark office desk seen '
          'from above at an angle, shapes of a desk telephone, papers and potted plant leaves, soft, hazy, low detail, '
          'low contrast, dark shadows')
NEGATIVE = ('text, letters, frame, border, lines, user interface, buttons, sharp, detailed, in focus, bright, white, grey, '
            'colours other than green, people, faces, cartoon')

src = np.array(Image.open(os.path.join(HERE, 'MSCRENUP_rgb.png')).convert('RGB')).astype(np.float32)
# the frame's lines out: each of their pixels the mean of the nearest ones on either side, outside the lines
clean = src.copy()
for y in range(H):
    for x in (1, 2, 3, 4, 5):
        clean[y, x] = clean[y, 6]
    for x in (314, 315, 316, 317, 318):
        clean[y, x] = clean[y, 313]
    clean[y, 0] = clean[y, 6]
    clean[y, 319] = clean[y, 313]
for x in range(CW):
    for y in (0, 1, 2, 3, 4):
        clean[y, x] = clean[5, x]
    for y in (195, 196, 197, 198, 199):
        clean[y, x] = clean[194, x]
clean = clean.astype(np.uint8)
Image.fromarray(clean).save(os.path.join(HERE, 'clean.png'))
big_w, big_h = int(W * SX), int(H * SY)
CB = int(CW * SX)
SIDE = (big_w - CB) // 2
patch.patch_settings[os.getpid()] = patch.PatchSettings(sharpness=config.default_sample_sharpness)
head_path, patch_path = config.downloading_inpaint_models('v2.6')
pipeline.refresh_everything(refiner_model_name='None', base_model_name=config.default_base_model_name,
                            loras=[], base_model_additional_loras=[(patch_path, 1.0)], use_synthetic_refiner=True)
positive = pipeline.clip_encode(texts=[PROMPT], pool_top_k=1)
negative = pipeline.clip_encode(texts=[NEGATIVE], pool_top_k=1)
base_unet = pipeline.final_unet



def outpaint(image, mask, seed):
    inpaint_worker.current_task = inpaint_worker.InpaintWorker(image=image, mask=mask, use_fill=True, k=1.0)
    task = inpaint_worker.current_task
    vae = pipeline.final_vae
    latent_inpaint, latent_mask = core.encode_vae_inpaint(mask=core.numpy_to_pytorch(task.interested_mask), vae=vae,
                                                          pixels=core.numpy_to_pytorch(task.interested_image))
    latent_fill = core.encode_vae(vae=vae, pixels=core.numpy_to_pytorch(task.interested_fill))['samples']
    task.load_latent(latent_fill=latent_fill, latent_mask=latent_mask, latent_swap=None)
    pipeline.final_unet = task.patch(inpaint_head_model_path=head_path, inpaint_latent=latent_inpaint,
                                     inpaint_latent_mask=latent_mask, model=base_unet)
    _, _, lh, lw = latent_fill.shape
    steps = 30
    out = pipeline.process_diffusion(positive_cond=positive, negative_cond=negative, steps=steps,
                                     switch=int(steps * 0.8), width=lw * 8, height=lh * 8, image_seed=seed,
                                     callback=None, sampler_name=config.default_sampler,
                                     scheduler_name=config.default_scheduler, latent={'samples': latent_fill},
                                     denoise=1.0, tiled=False, cfg_scale=config.default_cfg_scale,
                                     disable_preview=True)[0]
    return task.post_process(out)


centre = np.array(Image.fromarray(clean).resize((CB, big_h), Image.BICUBIC))
for seed in SEEDS:
    # the left side, then the right one, each from the picture beside it (edge-filled, as Fooocus's outpaint)
    img = np.concatenate([np.repeat(centre[:, :1], SIDE, axis=1), centre], axis=1)
    m = np.zeros(img.shape[:2], np.uint8)
    m[:, :SIDE] = 255
    left = outpaint(np.ascontiguousarray(img), m, seed)
    img = np.concatenate([left, np.repeat(left[:, -1:], SIDE, axis=1)], axis=1)
    m = np.zeros(img.shape[:2], np.uint8)
    m[:, -SIDE:] = 255
    full = outpaint(np.ascontiguousarray(img), m, seed + 100)
    Image.fromarray(full).save(os.path.join(HERE, 'out_%d.png' % seed))
    print('seed', seed, 'done')
inpaint_worker.current_task = None
