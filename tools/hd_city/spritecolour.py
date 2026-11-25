"""the HD sprites in the game's colours (after sprites.py and the picks):   spritecolour.py HDDIR SPRITEDIR id...
HDDIR/sprites/NNNN.png: its light and detail kept, its colour (and part of its light) taken from the game's own
sprite (SPRITEDIR/NNNN.png, 4x, softened): the trees in the original's yellow-olive.  The redraw itself is kept as
HDDIR/sprites/NNNN_redraw.png (run again from it)."""
import os, sys, shutil
import numpy as np
from PIL import Image, ImageFilter
HD, SRC = sys.argv[1], sys.argv[2]
ids = [int(a) for a in sys.argv[3:]]
LIGHT = 0.25            # of the brightness from the game's sprite (the rest the redraw's)
CONTRAST, SATURATION = 1.6, 1.5   # the original's strong lights and shadows, its vivid yellow


def ycc(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    return y, (b - y) * 0.564, (r - y) * 0.713


def rgb(y, cb, cr):
    r = y + 1.403 * cr
    b = y + 1.773 * cb
    g = (y - 0.299 * r - 0.114 * b) / 0.587
    return np.stack([r, g, b], -1)


for s in ids:
    p = os.path.join(HD, 'sprites', '%04d.png' % s)
    keep = p[:-4] + '_redraw.png'
    if not os.path.exists(keep):
        shutil.copy(p, keep)
    hd = np.asarray(Image.open(keep).convert('RGBA')).astype(np.float32)
    w, h = hd.shape[1], hd.shape[0]
    orig = Image.open(os.path.join(SRC, '%04d.png' % s)).convert('RGBA')
    oa = np.asarray(orig).astype(np.float32)
    mean = (oa[..., :3] * (oa[..., 3:] / 255)).sum((0, 1)) / max(1, (oa[..., 3] / 255).sum())
    flat = Image.fromarray((oa[..., :3] * (oa[..., 3:] / 255) + mean * (1 - oa[..., 3:] / 255)).astype(np.uint8))
    ref = np.asarray(flat.resize((w, h), Image.BICUBIC).filter(ImageFilter.GaussianBlur(5))).astype(np.float32)
    hy, _, _ = ycc(hd[..., :3])
    ry, rcb, rcr = ycc(ref)
    # the redraw's light brought to the game's mean brightness, its detail kept
    hy = hy * (ry.mean() / max(1.0, hy[hd[..., 3] > 128].mean()))
    y = ry * LIGHT + hy * (1 - LIGHT)
    m = y[hd[..., 3] > 128].mean()
    y = m + (y - m) * CONTRAST
    out = rgb(y, rcb * SATURATION, rcr * SATURATION)
    Image.fromarray(np.dstack([out.clip(0, 255), hd[..., 3]]).astype(np.uint8), 'RGBA').save(p)
    print('coloured', s)
