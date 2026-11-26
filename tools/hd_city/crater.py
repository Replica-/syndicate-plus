"""the crater (tile 209), drawn (after upscale.py 209, normals.py, mark.py):   crater.py HDDIR
Its top the pavement (material 7, laid by the game) with a blast crater in the middle: a height field (a pit, a
broken raised rim of slab chunks, rubble round it, rough walls), lit from the top left, charred earth at its
bottom, scorch marks spreading over the pavement (darkening the material's pixels is not possible: the scorch is
drawn in the tile's own pixels, the pavement under it in the material's colour); its normals from the same heights.
-> HDDIR/out/209.png, 209_n.png"""
import sys, numpy as np
from PIL import Image, ImageFilter
HD = sys.argv[1]
T, W, H, S = 209, 64, 48, 4
R_PIT, R_RIM, R_SCORCH = 11.5, 14.0, 17.5      # game pixels on the top (its side 32)
rng = np.random.default_rng(209)
ys, xs = np.mgrid[0:H * S, 0:W * S]
fx, fy = (xs + 0.5) / S, (ys + 0.5) / S
u, v = fy + (fx - 32) / 2, fy - (fx - 32) / 2        # along the ground
TOP = np.abs(fx - 32) / 32 + np.abs(fy - 16) / 16 <= 1.0 + 1.5 / 16


def noise(sigma, amp, seed):
    n = np.random.default_rng(seed).normal(0, 1, (H * S, W * S))
    n = np.asarray(Image.fromarray(((n * 40) + 128).clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(sigma))).astype(np.float32)
    n = (n - n.mean()) / max(1e-3, n.std())
    return n * amp


du, dv = u - 16, v - 16
ang = np.arctan2(dv, du)
r = np.hypot(du, dv) * (1 + 0.10 * np.sin(ang * 5 + 1.3) + 0.06 * np.sin(ang * 9 + 0.4))   # ragged outline
# heights (game pixels): the pit (a bowl), the rim (raised, broken), rubble scattered round it
h = np.zeros_like(r)
pit = r < R_PIT
h[pit] = -6.0 * (1 - (r[pit] / R_PIT) ** 2) ** 0.8
ring = np.exp(-((r - R_PIT - 1.0) / 1.8) ** 2) * 2.6
h += ring * (0.6 + 0.6 * (noise(1.2 * S, 1, 3) > -0.3))
chunks = (noise(0.7 * S, 1, 5) > 0.9) & (r > R_PIT - 0.5) & (r < R_RIM + 4.0)
h += chunks * 1.6
h += noise(0.5 * S, 0.35, 7) * (r < R_RIM)
# light: the slope from the heights (on the ground, u and v), the sun from the top left
gy, gx = np.gradient(h * S)
hu = (gx + gy) / 2 * 2                                   # d/du, d/dv from the picture's x, y (x = u - v, y = (u + v) / 2)
hv = (gy * 2 - gx * 2) / 2 / 2
n = np.stack([-hu, -hv, np.ones_like(h) * 1.2], -1)
n /= np.linalg.norm(n, axis=-1, keepdims=True)
sun = np.array([-0.55, -0.55, 0.62]); sun /= np.linalg.norm(sun)
lit = np.clip(n @ sun, 0, 1) / sun[2]
# colours: charred earth in the pit, broken concrete on the rim and in the chunks, scorch spreading out
earth = np.array([34, 27, 20], np.float32)
concrete = np.array([132, 104, 80], np.float32)    # the pavement's, broken (its lit faces)
depth = np.clip(-h / 6.0, 0, 1)[..., None]
col = np.where(pit[..., None], earth * (1 - 0.35 * depth) + 18 * (1 - depth), concrete)
col = col * (1 + noise(0.4 * S, 0.12, 11)[..., None])
col = col * (0.35 + 0.65 * lit)[..., None]
scorch = np.clip(1 - (r - R_PIT) / (R_SCORCH - R_PIT), 0, 1) * (0.65 + 0.35 * (noise(1.5 * S, 1, 13) > -0.2))
drawn = TOP & ((r < R_RIM + 0.5) | chunks | (scorch * (1 + noise(2.0 * S, 0.5, 17)) > 0.12))
soot = (np.clip(scorch, 0, 1) * (r >= R_PIT) * np.where(chunks, 0.35, 0.85))[..., None]
col = col * (1 - soot) + np.array([22, 18, 14], np.float32) * soot   # soot, black towards the pit

out = np.asarray(Image.open(f'{HD}/out/{T:03d}.png').convert('RGBA')).copy()
top = TOP & (out[..., 3] > 0)
out[..., 3][top] = 247                                   # the pavement (material 7) everywhere on the top
sel = top & drawn
out[..., :3][sel] = col.clip(0, 255).astype(np.uint8)[sel]
out[..., 3][sel] = 255
Image.fromarray(out, 'RGBA').save(f'{HD}/out/{T:03d}.png')
# its normals in the picture's axes (x right, y down, z out): the ground's u right-down, v left-down
nm = np.asarray(Image.open(f'{HD}/out/{T:03d}_n.png').convert('RGB')).copy()
px = (n[..., 0] - n[..., 1]) * 0.7071
py = (n[..., 0] + n[..., 1]) * 0.3536
pn = np.stack([px, py, n[..., 2]], -1)
pn /= np.linalg.norm(pn, axis=-1, keepdims=True)
nm[sel] = ((pn * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)[sel]
nm[top & ~sel] = (128, 128, 255)
Image.fromarray(nm).save(f'{HD}/out/{T:03d}_n.png')
print('crater')
