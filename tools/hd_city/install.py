"""the pack where the game looks for it:   install.py HDDIR/out SAVE/hd
The tiles, their normals, the materials (HDDIR/mat/graded_KK.png) and tile_lights.ini.  On the way the redraw's orange
(the buildings' walls, the towers) is muted towards the game's intro (dusky brown, less saturated): ORANGE_SAT of its
saturation kept, ORANGE_VALUE of its brightness; the pavements' tops (paving.py's slabs) as they are."""
import sys, os, glob, shutil
import numpy as np
from PIL import Image, ImageFilter
src, dst = sys.argv[1], sys.argv[2]
ORANGE_SAT, ORANGE_VALUE = 0.5, 0.9
PAVED = [35, 36, 41, 207, 42, 43, 251, 252, 203]   # (material pixels are never muted)
KEEP = [210, 18, 19, 20, 21, 22, 23, 27, 211, 212, 213, 214, 215]   # grass.py's tiles: as they are
W, H, S = 64, 48, 4
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 3.0 / 16


BLOCK = (np.abs(fx - 32) <= 32) & (fy >= np.abs(fx - 32) / 2) & (fy <= 48 - np.abs(fx - 32) / 2)   # a whole block, smooth
ys1, xs1 = np.mgrid[0:H, 0:W]
BLOCK1 = (np.abs(xs1 + 0.5 - 32) <= 32) & (ys1 + 0.5 >= np.abs(xs1 + 0.5 - 32) / 2) & (ys1 + 0.5 <= 48 - np.abs(xs1 + 0.5 - 32) / 2)


def smooth_outline(a, t, base_dir):
    """a tile that fills a whole block (its outline the block's, stepped): its outline the block's straight edges (tiles
    side by side meet along a line, not a sawtooth); the pixels it gains coloured from their neighbours"""
    p = os.path.join(base_dir, '%03d.png' % t)
    if not os.path.exists(p):
        return a
    op = np.asarray(Image.open(p).convert('RGBA'))[..., 3] > 0
    if not (op & BLOCK1).any():
        return a
    # the outline smoothed: the game's coverage scaled up smoothly, cut at half (its pixel steps become slanted lines:
    # rails, fences, edges); inside the tile's own pixels kept
    soft = np.asarray(Image.fromarray((op * 255).astype(np.uint8)).resize((W * S, H * S), Image.BILINEAR)
                      .filter(ImageFilter.GaussianBlur(S * 0.35))) / 255 > 0.5
    # only at solid parts' edges (a pixel with most of its 3 x 3 neighbours opaque): fine patterns (a fence's mesh) kept
    cnt = np.zeros(op.shape, np.int32)
    pad = np.pad(op, 1)
    for dy in range(3):
        for dx in range(3): cnt += pad[dy:dy + op.shape[0], dx:dx + op.shape[1]]
    solid = np.asarray(Image.fromarray(((cnt >= 5) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
                       .resize((W * S, H * S), Image.NEAREST)) > 0
    soft = np.where(solid, soft, np.asarray(Image.fromarray((op * 255).astype(np.uint8)).resize((W * S, H * S), Image.NEAREST)) > 0)
    a = a.copy()
    lost = (a[..., 3] > 0) & ~soft
    a[..., 3][lost] = 0
    have = a[..., 3] > 0
    gain = soft & ~have
    # along the block's outline (a game pixel either side): the straight edge where the tile reaches it
    reach = np.asarray(Image.fromarray((have * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(2 * S + 1))) > 0
    d = np.minimum(np.minimum(fx - 0, 64 - fx), np.minimum(fy - np.abs(fx - 32) / 2, 48 - np.abs(fx - 32) / 2 - fy))
    band = np.abs(d) <= 1.2
    c = Image.fromarray(np.where(have[..., None], a[..., :3], 0).astype(np.uint8))
    m = Image.fromarray((have * 255).astype(np.uint8))
    for _ in range(3):                                   # spread the colours a few pixels outwards
        c2 = np.asarray(c.filter(ImageFilter.MaxFilter(3))).astype(np.uint8)
        m2 = np.asarray(m.filter(ImageFilter.MaxFilter(3))) > 0
        cc = np.asarray(c).copy(); mm = np.asarray(m) > 0
        cc[~mm & m2] = c2[~mm & m2]
        c, m = Image.fromarray(cc), Image.fromarray((m2 * 255).astype(np.uint8))
    out = a.copy()
    grow = (band & BLOCK & reach & ~have) | gain
    out[..., :3][grow] = np.asarray(c)[grow]
    out[..., 3][grow] = 255
    out[..., 3][band & ~BLOCK] = 0
    return out


def muted(path, t):
    a = np.asarray(Image.open(path).convert('RGBA')).copy()
    c = a[..., :3].astype(np.float32)
    mx, mn = c.max(-1), c.min(-1)
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    hue = np.degrees(np.arctan2(np.sqrt(3) * (g - b), 2 * r - g - b)) % 360
    sat = (mx - mn) / np.maximum(mx, 1)
    # how orange: hues 5..55 (red-orange to amber), saturated
    w = np.clip(1 - np.abs(hue - 30) / 25, 0, 1) * np.clip((sat - 0.25) / 0.2, 0, 1)
    # pinks and magentas (hues 280-355): nearly grey, a little darker
    pink = np.clip((np.minimum(hue - 280, 355 - hue)) / 12, 0, 1) * np.clip((sat - 0.1) / 0.15, 0, 1)
    w[(a[..., 3] >= 240) & (a[..., 3] < 255)] = 0            # materials' pixels (the game draws them)
    if t in PAVED:
        w[TOP] = 0
    if t in KEEP:
        w[:] = 0
    l = c.mean(-1, keepdims=True)
    k = (1 - w * (1 - ORANGE_SAT))[..., None]
    out = (l + (c - l) * k) * (1 - w * (1 - ORANGE_VALUE))[..., None]
    pink[(a[..., 3] >= 240) & (a[..., 3] < 255)] = 0
    out = out + ((l * np.array([1.0, 0.97, 0.92]) * 0.85) - out) * (pink * 0.9)[..., None]
    a[..., :3] = out.clip(0, 255).astype(np.uint8)
    a = smooth_outline(a, t, os.path.join(os.path.dirname(os.path.abspath(src)), 'base'))
    return Image.fromarray(a, 'RGBA')


os.makedirs(dst, exist_ok=True)
n = 0
for f in glob.glob(os.path.join(src, '[0-9][0-9][0-9].png')):
    if f.endswith('_up.png'): continue
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
# the HD sprites (sprites.py; the user's picks copied to HDDIR/sprites)
spr = os.path.join(os.path.dirname(os.path.abspath(src)), 'sprites')
for f in glob.glob(os.path.join(spr, '[0-9][0-9][0-9][0-9].png')):
    shutil.copy(f, os.path.join(dst, 'sprite_%s' % os.path.basename(f)))
print('materials', len(glob.glob(os.path.join(dst, 'material_[0-9][0-9].png'))))
