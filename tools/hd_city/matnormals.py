"""normals of the materials (DeepBump, seamless: seen 3 x 3, the middle kept): HDDIR/mat/graded_KK_n.png
x along the texture's rows (U), y along its columns (V) DOWN, z up out of the ground"""
import sys, glob, numpy as np
from PIL import Image
sys.path.insert(0, 'C:/development/deepbump')
import module_color_to_normals as c2n
HD = sys.argv[1]
for f in sorted(glob.glob(f'{HD}/mat/graded_[0-9][0-9].png')):
    a = np.asarray(Image.open(f).convert('RGB').resize((512, 512), Image.LANCZOS)).astype(np.float32) / 255
    big = np.tile(a, (3, 3, 1))
    n = np.transpose(c2n.apply(np.transpose(big, (2, 0, 1)), 'SMALL', None), (1, 2, 0))[512:1024, 512:1024]
    n[..., 1] = 1 - n[..., 1]
    Image.fromarray((n.clip(0, 1) * 255).astype(np.uint8)).resize((1024, 1024), Image.LANCZOS).save(f[:-4] + '_n.png')
    print('normals', f[-6:-4], flush=True)
