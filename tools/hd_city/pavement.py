"""the pavement material from pavemat.py's versions (after it, before install.py):   pavement.py HDDIR [version]
The chosen version (the user's pick: 4) -> HDDIR/mat/graded_07.png, its grooves lighter: the pixels darker than the
slabs around them brought LIFT of the way to that colour (the slabs' faces as they are).  Then matnormals.py."""
import sys, numpy as np
from PIL import Image, ImageFilter
HD = sys.argv[1]
VERSION = int(sys.argv[2]) if len(sys.argv) > 2 else 4
LIFT = 0.5
a = np.asarray(Image.open(f'{HD}/mat/pave/{VERSION:02d}.png').convert('RGB')).astype(np.float32)
n = a.shape[0]
big = Image.fromarray(np.tile(a, (3, 3, 1)).clip(0, 255).astype(np.uint8))       # repeating: blurred as it repeats
around = np.asarray(big.filter(ImageFilter.GaussianBlur(10))).astype(np.float32)[n:2 * n, n:2 * n]
l, la = a.mean(-1), around.mean(-1)
dark = np.clip((la * 0.9 - l) / np.maximum(la * 0.3, 1), 0, 1)[..., None]          # 0 on a face, 1 deep in a groove
out = a + (around - a) * dark * LIFT
Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(f'{HD}/mat/graded_07.png')
print('pavement', VERSION)
