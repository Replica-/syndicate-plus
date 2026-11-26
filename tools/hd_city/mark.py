"""the HD tiles' ground marked with their material (the game lays the material there in world space):
   mark.py HDDIR -> HDDIR/out/NNN.png, the top's pixels of the material alpha 240 + its number
Roads and pavements have no material: their tops are faithful.py's (the game's markings, kerbs and paving where it
draws them).  SAME: tiles drawn as another (the tower bases' lit pads, the game's own lighting: ours replaces it).
WATER: the black tops of the trench (the game's water) marked as material 11, which the game draws as water (black,
rippling, reflecting the sky and the sun; no picture of its own)."""
import os, shutil, sys, numpy as np
from PIL import Image
HD = sys.argv[1]
W, H, S = 64, 48, 4
MATERIAL = {13: 0, 14: 0, 76: 3, 79: 3, 168: 3, 169: 3, 170: 3, 159: 5}
MATERIAL.update({t: 7 for t in (35, 36, 41, 207, 42, 43, 251, 252, 203, 45)})   # 45: with floor lights (kept)   # the pavements: pavemat.py's slabs
SAME = {207: 36, 42: 36, 43: 36, 251: 36, 252: 36, 203: 36}
WATER, WATER_TILES = 11, [156, 158, 165, 166, 167]
KEEP = [(255, 190, 0), (223, 255, 255)]          # markings kept on the other ground: yellow, white
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 1.5 / 16   # up to its stepped edge
WTOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 4.0 / 16  # and over its sides' top rows (their earth
                                                                     # showed as lines between the water's tiles)


def load(t):
    out = np.asarray(Image.open(f'{HD}/out/{t:03d}.png').convert('RGBA')).copy()
    a = out[..., 3]
    a[(a >= 240) & (a < 255)] = 255                 # an earlier marking undone
    return out


def big(sel):
    return np.asarray(Image.fromarray(sel.astype(np.uint8) * 255).resize((W * S, H * S), Image.NEAREST)) > 0


for t, s in SAME.items():
    shutil.copy(f'{HD}/out/{s:03d}.png', f'{HD}/out/{t:03d}.png')
for f in sorted(os.listdir(f'{HD}/out')):
    if len(f) == 7 and f[:3].isdigit() and f.endswith('.png') and int(f[:3]) not in MATERIAL:
        Image.fromarray(load(int(f[:3])), 'RGBA').save(f'{HD}/out/{f}')
for t, m in MATERIAL.items():
    base = np.asarray(Image.open(f'{HD}/base/{t:03d}.png').convert('RGBA'))
    sel = base[..., 3] > 0
    for c in KEEP: sel &= np.abs(base[..., :3].astype(int) - c).sum(-1) >= 12
    out = load(t)
    mk = TOP & big(sel) & (out[..., 3] > 0)
    out[..., 3][mk] = 240 + m
    Image.fromarray(out, 'RGBA').save(f'{HD}/out/{t:03d}.png')
for t in WATER_TILES:
    out = load(t)
    mk = WTOP & (out[..., 3] > 0)                    # all of its top, to its edges: no seam between them
    out[..., 3][mk] = 240 + WATER
    Image.fromarray(out, 'RGBA').save(f'{HD}/out/{t:03d}.png')
# the roads' rims: faithful.py replaced their tops up to the diamond's edge, the texels out to the stepped edge kept
# the redraw's rust (a dotted line where tiles meet): their colour taken from just inside the top
ROADS = [100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 119, 120, 121, 122, 123]
d = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16
RIM = (d > 0.985) & (d <= 1.0 + 2.5 / 16) & (fy < 16 + 1e-6 + 16 * 0)
for t in ROADS:
    out = load(t)
    rim = RIM & (out[..., 3] > 0) | ((d > 0.985) & (d <= 1.0 + 2.5 / 16) & (out[..., 3] > 0) & (fy >= 16) & (d <= 1.0 + 1.0 / 16))
    k = 0.975 / np.maximum(d, 1e-3)
    sx = np.clip(((32 + (fx - 32) * k) * S).astype(int), 0, W * S - 1)
    sy = np.clip(((16 + (fy - 16) * k) * S).astype(int), 0, H * S - 1)
    out[..., :3][rim] = out[..., :3][sy[rim], sx[rim]]
    Image.fromarray(out, 'RGBA').save(f'{HD}/out/{t:03d}.png')
# the other cities' pavements (upscale.py's tiles): only their paving's colours (the game's checkerboard) laid in the
# pavement material, their stripes, vents, lights and stains kept
PAVE_ONLY = [24, 26, 28, 29, 30, 31, 32, 33, 37, 38, 39, 40, 44, 46, 145, 149, 153, 204, 206, 208]
PAVE_COLOURS = [(203, 134, 81), (101, 101, 101), (174, 93, 60)]
for t in PAVE_ONLY:
    base = np.asarray(Image.open(f'{HD}/base/{t:03d}.png').convert('RGBA'))
    sel = np.zeros(base.shape[:2], bool)
    for c in PAVE_COLOURS: sel |= (base[..., :3] == c).all(-1) & (base[..., 3] > 0)
    out = load(t)
    mk = TOP & big(sel) & (out[..., 3] > 0)
    out[..., 3][mk] = 247
    Image.fromarray(out, 'RGBA').save(f'{HD}/out/{t:03d}.png')
print('marked', len(MATERIAL), 'water', len(WATER_TILES), 'same', len(SAME))
