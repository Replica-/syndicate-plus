"""the roads' tops made to match each other (after faithful.py, mark.py, normals.py):   roadmatch.py HDDIR
Each road's top brought to the same mean colour (TARGET, its markings as they are: only the asphalt's mean moves),
and its normals' mean lean taken away (the redraw's normals leant whole tiles one way: the sun's relief shaded
neighbours differently) -> HDDIR/out/NNN.png, NNN_n.png"""
import sys, numpy as np
from PIL import Image
HD = sys.argv[1]
W, H, S = 64, 48, 4
ROADS = [100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 119, 120, 121, 122, 123]
TARGET = np.array([50, 51, 54], np.float32)
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 2.5 / 16
for t in ROADS:
    a = np.asarray(Image.open(f'{HD}/out/{t:03d}.png').convert('RGBA')).astype(np.float32)
    top = TOP & (a[..., 3] > 0)
    c = a[..., :3]
    l = c.mean(-1)
    sat = c.max(-1) - c.min(-1)
    asphalt = top & (l < 110) & (sat < 40)                   # not the white or yellow markings
    k = TARGET / np.maximum(c[asphalt].mean(0), 1)
    c[top] = c[top] * k                                       # the whole top (its markings in step)
    a[..., :3] = c
    Image.fromarray(a.clip(0, 255).astype(np.uint8), 'RGBA').save(f'{HD}/out/{t:03d}.png')
    n = np.asarray(Image.open(f'{HD}/out/{t:03d}_n.png').convert('RGB')).astype(np.float32) / 127.5 - 1
    lean = n[top].mean(0)
    n[top] = n[top] - np.array([lean[0], lean[1], 0])        # flat on the whole, its detail kept
    n[top] /= np.linalg.norm(n[top], axis=-1, keepdims=True)
    Image.fromarray(((n + 1) * 127.5).clip(0, 255).astype(np.uint8)).save(f'{HD}/out/{t:03d}_n.png')
    print('road', t, 'colour x', k.round(2), 'lean', lean[:2].round(2))
