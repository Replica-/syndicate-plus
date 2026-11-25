"""Normal maps for the HD tiles with DeepBump: run with DeepBump's venv Python.
   normals.py HDDIR [ids...]
HDDIR/out/NNN.png (RGBA) -> HDDIR/out/NNN_n.png: x right, y DOWN (DeepBump's green flipped), z out of the picture;
flat (128,128,255) where the tile is transparent.  Each tile is seen among 3 x 3 copies of itself as the map lays
them (and over its own mean colour), so its edges are not taken for slopes: tiles side by side join without a seam."""
import os, sys, glob
import numpy as np
from PIL import Image
sys.path.insert(0, 'C:/development/deepbump')
import module_color_to_normals as c2n

HD = sys.argv[1]
ids = [int(a) for a in sys.argv[2:]]
files = [os.path.join(HD, 'out', '%03d.png' % t) for t in ids] if ids else \
    [f for f in sorted(glob.glob(os.path.join(HD, 'out', '[0-9][0-9][0-9].png')))]
W, H = 256, 192          # the HD tile
DX, DY = 128, 64         # the map's step between tiles side by side
for f in files:
    tile = Image.open(f).convert('RGBA')
    a = np.asarray(tile).astype(np.float32)[..., 3:] / 255
    mean = tuple(int(v) for v in ((np.asarray(tile)[..., :3] * a).sum((0, 1)) / max(1e-3, a.sum())))
    cw, ch = W + 2 * DX, H + 2 * DY
    canvas = Image.new('RGBA', (cw, ch), mean + (255,))
    for j in range(-1, 2):
        for i in range(-1, 2):
            canvas.alpha_composite(tile, (DX + (i - j) * DX, DY + (i + j) * DY))
    canvas.alpha_composite(tile, (DX, DY))   # the middle one on top
    img = np.transpose(np.asarray(canvas.convert('RGB')).astype(np.float32) / 255, (2, 0, 1))
    n = np.transpose(c2n.apply(img, 'MEDIUM', None), (1, 2, 0))[DY:DY + H, DX:DX + W]
    n[..., 1] = 1 - n[..., 1]                # y down
    n = n * a + np.array([0.5, 0.5, 1.0], np.float32) * (1 - a)
    Image.fromarray((np.clip(n, 0, 1) * 255).astype(np.uint8)).save(f[:-4] + '_n.png')
    print('normals', os.path.basename(f), flush=True)
