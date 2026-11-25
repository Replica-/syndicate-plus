"""the roads' tops without the palette's rust: the pixels drawn from its rust, ochre and brown colours (the grooves,
the kerbs' tops) in dark neutral grey with their light; the white and yellow markings as they are
   roadgrey.py HDDIR"""
import sys, numpy as np
from PIL import Image
HD = sys.argv[1]
W, H, S = 64, 48, 4
ROADS = [100, 102, 103, 104, 106, 107, 108, 109, 110, 111, 112, 113, 119, 120, 121, 122, 123]
RUST = [(174, 93, 60), (203, 134, 81), (113, 85, 4), (150, 121, 24), (255, 166, 97)]
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 1.5 / 16
for t in ROADS:
    base = np.asarray(Image.open(f'{HD}/base/{t:03d}.png').convert('RGBA'))
    rust = np.zeros(base.shape[:2], bool)
    for c in RUST: rust |= (base[..., :3] == c).all(-1)
    rust = np.asarray(Image.fromarray(rust.astype(np.uint8) * 255).resize((W * S, H * S), Image.NEAREST)) > 0
    out = np.asarray(Image.open(f'{HD}/out/{t:03d}.png').convert('RGBA')).copy()
    sel = TOP & rust & (out[..., 3] == 255)
    l = out[..., :3].astype(np.float32).mean(-1)
    g = np.stack([l * 0.5, l * 0.52, l * 0.56], -1)
    out[..., :3][sel] = g.clip(0, 255).astype(np.uint8)[sel]
    Image.fromarray(out, 'RGBA').save(f'{HD}/out/{t:03d}.png')
print('roads greyed', len(ROADS))
