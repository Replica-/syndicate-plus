"""the water's material (11): HDDIR/mat/graded_11.png (near black: the game draws the water itself) and
graded_11_n.png, its ripples' normals - a height field of band-limited noise (made by FFT, so it repeats without a
seam), x along the rows (U), y along the columns (V) down, z up out of the ground, as matnormals.py's.
   water.py HDDIR"""
import sys, numpy as np
from PIL import Image
HD = sys.argv[1]
N = 1024                      # 8 x 8 tiles: a tile is 128 texels
rng = np.random.default_rng(11)
f = np.fft.fftfreq(N) * N     # cycles across the square
fu, fv = np.meshgrid(f, f)
r = np.hypot(fu, fv)
h = np.zeros((N, N))
# slow swells about a tile long, ripples on them, a little chop
for centre, width, amp in ((9, 3, 1.0), (20, 6, 0.3), (44, 12, 0.06)):
    band = np.exp(-((r - centre) / width) ** 2) * amp
    ph = np.exp(2j * np.pi * rng.random((N, N)))
    h += np.real(np.fft.ifft2(band * ph))
h /= np.abs(h).max()
dv, du = np.gradient(h)                       # rows are V, columns U
k = 0.18 / np.hypot(du, dv).mean()           # the ripples' mean slope
n = np.stack([-du * k, -dv * k, np.ones_like(h)], -1)
n /= np.linalg.norm(n, axis=-1, keepdims=True)
Image.fromarray(((n * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)).save(f'{HD}/mat/graded_11_n.png')
Image.fromarray(np.full((N, N, 3), 4, np.uint8)).save(f'{HD}/mat/graded_11.png')
print('water: slope mean', float(np.hypot(du, dv).mean() * k))
