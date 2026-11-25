import sys, os, glob, shutil
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
n = 0
for f in glob.glob(os.path.join(src, '[0-9][0-9][0-9].png')):
    t = os.path.basename(f)[:3]
    shutil.copy(f, os.path.join(dst, 'tile_%s.png' % t)); n += 1
    nf = f[:-4] + '_n.png'
    if os.path.exists(nf): shutil.copy(nf, os.path.join(dst, 'tile_%s_n.png' % t))
print(n, 'tiles')
mat = os.path.join(os.path.dirname(os.path.abspath(src)), 'mat')
for f in glob.glob(os.path.join(mat, 'graded_[0-9][0-9].png')):
    k = os.path.basename(f)[7:9]
    shutil.copy(f, os.path.join(dst, 'material_%s.png' % k))
    if os.path.exists(f[:-4] + '_n.png'): shutil.copy(f[:-4] + '_n.png', os.path.join(dst, 'material_%s_n.png' % k))
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tile_lights.ini'), dst)
print('materials', len(glob.glob(os.path.join(dst, 'material_[0-9][0-9].png'))))
