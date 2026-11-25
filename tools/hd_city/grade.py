"""materials graded for the city: mean brightness, saturation, a cool tint -> HDDIR/mat/graded_KK.png
TWO_TONE: graded by brightness between colours instead (the dirt: olive-brown soil, slate-grey stones, as the
game's dirt and gravel)"""
import sys, numpy as np
from PIL import Image
HD = sys.argv[1]
# (mean brightness 0..255, saturation, tint multiplier r,g,b)
GRADE = {1: (44, 0.4, (0.95, 1.0, 1.08)), 2: (66, 0.4, (0.95, 1.0, 1.06)),
         3: (54, 0.5, (0.95, 1.0, 1.1)), 4: (30, 0.4, (1.0, 1.0, 1.05)), 5: (56, 0.4, (0.95, 1.0, 1.08)),
         6: (58, 0.85, (1.0, 1.0, 0.85))}   # 6: grass (grass.py)
# (colours from dark to light, the brightness percentiles they are at)
TWO_TONE = {0: (((26, 21, 8), (62, 49, 15), (70, 55, 18), (72, 74, 76)), (3, 45, 65, 97))}
for k, (lum, sat, tint) in GRADE.items():
    a = np.asarray(Image.open(f'{HD}/mat/material_{k:02d}.png').convert('RGB')).astype(np.float32)
    l = a.mean(-1, keepdims=True)
    a = l + (a - l) * sat
    a = a * (lum / max(1, a.mean())) * np.array(tint, np.float32)
    Image.fromarray(a.clip(0, 255).astype(np.uint8)).save(f'{HD}/mat/graded_{k:02d}.png')
for k, (cols, pct) in TWO_TONE.items():
    a = np.asarray(Image.open(f'{HD}/mat/material_{k:02d}.png').convert('RGB')).astype(np.float32)
    l = a.mean(-1)
    at = np.percentile(l, pct)
    out = np.stack([np.interp(l, at, [c[ch] for c in cols]) for ch in range(3)], -1)
    Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(f'{HD}/mat/graded_{k:02d}.png')
print('graded')
