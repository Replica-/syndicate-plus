"""the pack where the game looks for it:   install.py HDDIR/out SAVE/hd
The tiles, their normals, the materials (HDDIR/mat/graded_KK.png) and tile_lights.ini.  On the way the redraw's orange
(the buildings' walls, the towers) is muted towards the game's intro (dusky brown, less saturated): ORANGE_SAT of its
saturation kept, ORANGE_VALUE of its brightness; the pavements' tops (paving.py's slabs) as they are."""
import sys, os, glob, shutil
import numpy as np
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
ORANGE_SAT, ORANGE_VALUE = 0.5, 0.9
PAVED = [35, 36, 41, 207, 42, 43, 251, 252, 203]
KEEP = [210]                     # grass.py's tiles: as they are
W, H, S = 64, 48, 4
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 3.0 / 16


def muted(path, t):
    a = np.asarray(Image.open(path).convert('RGBA')).copy()
    c = a[..., :3].astype(np.float32)
    mx, mn = c.max(-1), c.min(-1)
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    hue = np.degrees(np.arctan2(np.sqrt(3) * (g - b), 2 * r - g - b)) % 360
    sat = (mx - mn) / np.maximum(mx, 1)
    # how orange: hues 5..55 (red-orange to amber), saturated
    w = np.clip(1 - np.abs(hue - 30) / 25, 0, 1) * np.clip((sat - 0.25) / 0.2, 0, 1)
    w[(a[..., 3] >= 240) & (a[..., 3] < 255)] = 0            # materials' pixels (the game draws them)
    if t in PAVED:
        w[TOP] = 0
    if t in KEEP:
        w[:] = 0
    l = c.mean(-1, keepdims=True)
    k = (1 - w * (1 - ORANGE_SAT))[..., None]
    out = (l + (c - l) * k) * (1 - w * (1 - ORANGE_VALUE))[..., None]
    a[..., :3] = out.clip(0, 255).astype(np.uint8)
    return Image.fromarray(a, 'RGBA')


os.makedirs(dst, exist_ok=True)
n = 0
for f in glob.glob(os.path.join(src, '[0-9][0-9][0-9].png')):
    t = os.path.basename(f)[:3]
    muted(f, int(t)).save(os.path.join(dst, 'tile_%s.png' % t)); n += 1
    nf = f[:-4] + '_n.png'
    if os.path.exists(nf): shutil.copy(nf, os.path.join(dst, 'tile_%s_n.png' % t))
print(n, 'tiles')
mat = os.path.join(os.path.dirname(os.path.abspath(src)), 'mat')
for f in glob.glob(os.path.join(mat, 'graded_[0-9][0-9].png')):
    k = os.path.basename(f)[7:9]
    shutil.copy(f, os.path.join(dst, 'material_%s.png' % k))
    if os.path.exists(f[:-4] + '_n.png'): shutil.copy(f[:-4] + '_n.png', os.path.join(dst, 'material_%s_n.png' % k))
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tile_lights.ini'), dst)
print('materials', len(glob.glob(os.path.join(dst, 'material_[0-9][0-9].png'))))
