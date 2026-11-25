"""sprites made 4x as the game draws them (ESRGAN, no redraw: the doors and their opening frames)
(run with Fooocus's Python):   upscale_sprites.py HDDIR SPRITEDIR id...
SPRITEDIR/NNNN.png on its own mean colour, a little margin, its 1-pixel dithering blended, 4x by ESRGAN, darker
as the redrawn tiles, cut to the sprite's own outline -> HDDIR/sprites/NNNN.png"""
import os, sys
HD, SRC = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
ids = [int(a) for a in sys.argv[3:]]
FOOOCUS = 'C:/development/fooocus/Fooocus_win64_2-5-0/Fooocus'
sys.argv = [sys.argv[0]]
sys.path.insert(0, FOOOCUS)
os.chdir(FOOOCUS)
import numpy as np
from PIL import Image, ImageFilter
from modules import config
config.update_files()
from modules.upscaler import perform_upscale

S, M, DARKER = 4, 4, 0.85
os.makedirs(os.path.join(HD, 'sprites'), exist_ok=True)
for sid in ids:
    spr = Image.open(os.path.join(SRC, '%04d.png' % sid)).convert('RGBA')
    w, h = spr.size
    a = np.asarray(spr)
    if (a[..., 3] > 0).sum() == 0:
        continue
    mean = tuple(int(v) for v in a[..., :3][a[..., 3] > 0].mean(0)) + (255,)
    canvas = Image.new('RGBA', (w + 2 * M, h + 2 * M), mean)
    canvas.alpha_composite(spr, (M, M))
    up = perform_upscale(np.asarray(canvas.convert('RGB').filter(ImageFilter.GaussianBlur(0.4))))
    up = np.asarray(Image.fromarray(up).resize(((w + 2 * M) * S, (h + 2 * M) * S), Image.LANCZOS)).astype(np.float32)
    rgb = (up[M * S:(M + h) * S, M * S:(M + w) * S] * DARKER).clip(0, 255)
    alpha = np.asarray(spr.split()[3].resize((w * S, h * S), Image.NEAREST))[..., None]
    Image.fromarray(np.concatenate([rgb, alpha], -1).astype(np.uint8), 'RGBA').save(
        os.path.join(HD, 'sprites', '%04d.png' % sid))
    print('sprite', sid, flush=True)
