"""uniform materials made from periodic noise (seamless, no pattern): asphalt, concrete, dark steel floor
   procedural.py HDDIR -> HDDIR/mat/graded_01.png, _02, _05 (over the graded AI ones)"""
import sys, numpy as np
from PIL import Image
HD = sys.argv[1]
N = 1024
rng = np.random.default_rng(7)
fy = np.fft.fftfreq(N)[:, None]; fx = np.fft.fftfreq(N)[None, :]
F = np.sqrt(fx * fx + fy * fy)


def band(lo, hi):
    """periodic noise with features lo..hi pixels across, mean 0, deviation 1"""
    w = np.fft.fft2(rng.standard_normal((N, N)))
    m = (F >= 1.0 / hi) & (F <= 1.0 / lo)
    n = np.real(np.fft.ifft2(w * m))
    return (n - n.mean()) / max(1e-6, n.std())


def make(base, tint, grain, stains, patches, speck, speck_amt):
    l = base + grain * band(1, 3) + stains * band(8, 40) + patches * band(60, 400)
    l += speck_amt * (rng.random((N, N)) < speck)
    a = l[..., None] * np.array(tint, np.float32)
    return a.clip(0, 255).astype(np.uint8)


Image.fromarray(make(44, (0.95, 1.0, 1.08), 9, 3, 5, 0.015, 28)).save(f'{HD}/mat/graded_01.png')   # asphalt
Image.fromarray(make(66, (0.96, 1.0, 1.05), 5, 5, 6, 0.004, 18)).save(f'{HD}/mat/graded_02.png')   # concrete
Image.fromarray(make(40, (0.93, 1.0, 1.1), 4, 6, 6, 0.0, 0)).save(f'{HD}/mat/graded_05.png')       # dark floor
print('procedural')
