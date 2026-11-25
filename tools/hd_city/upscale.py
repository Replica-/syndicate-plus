"""tiles made 4x as the game draws them (ESRGAN, no redraw), for tiles no mission redraw covered (run with Fooocus's
Python):   upscale.py HDDIR id...
HDDIR/base/NNN.png among 3 x 3 copies of itself as the map lays them (its edges as its neighbours make them), its
1-pixel dithering blended, 4x by ESRGAN -> HDDIR/out/NNN.png (mark.py then lays a material on its top)."""
import os, sys
HD = os.path.abspath(sys.argv[1])
ids = [int(a) for a in sys.argv[2:]]
FOOOCUS = 'C:/development/fooocus/Fooocus_win64_2-5-0/Fooocus'
sys.argv = [sys.argv[0]]
sys.path.insert(0, FOOOCUS)
os.chdir(FOOOCUS)
import numpy as np
from PIL import Image, ImageFilter
from modules import config
config.update_files()
from modules.upscaler import perform_upscale

W, H, S = 64, 48, 4
for t in ids:
    base = Image.open(os.path.join(HD, 'base', '%03d.png' % t)).convert('RGBA')
    a = np.asarray(base)
    mean = tuple(int(v) for v in a[..., :3][a[..., 3] > 0].mean(0)) + (255,)
    canvas = Image.new('RGBA', (W * 3, H + 64), mean)
    for j in range(-1, 2):
        for i in range(-1, 2):
            canvas.alpha_composite(base, (W + (i - j) * 32, 32 + (i + j) * 16))
    canvas.alpha_composite(base, (W, 32))
    up = perform_upscale(np.asarray(canvas.convert('RGB').filter(ImageFilter.GaussianBlur(0.5))))
    up = np.asarray(Image.fromarray(up).resize((W * 3 * S, (H + 64) * S), Image.LANCZOS))
    rgb = up[32 * S:(32 + H) * S, W * S:2 * W * S]
    alpha = np.asarray(base.resize((W * S, H * S), Image.NEAREST))[..., 3:]
    Image.fromarray(np.concatenate([rgb, alpha], -1).astype(np.uint8), 'RGBA').save(os.path.join(HD, 'out', '%03d.png' % t))
    print('upscaled', t, flush=True)
