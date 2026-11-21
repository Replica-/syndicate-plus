"""Find the functions of one recompiled program in the others (for the hooks of src/game/additions.c).

    python tools/find_equiv.py syn_main 427f80 417e00 ...      every other game program
    python tools/find_equiv.py syn_main 427f80 --in ar_main

The programs of the other languages and American Revolt are built from nearly the same source, so a function
keeps its instructions while its addresses move.  Each function of gen/<module> is reduced to its instructions
without the numbers in them (addresses and constants); the best match of the same length, or of the nearest
similarity, is printed with how alike the two are (1.0: the same instructions).
"""
import difflib
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = ['syn_main', 'syn_smain', 'syn_gmain', 'ar_main', 'ar_smain', 'ar_gmain']
INSN = re.compile(r'/\* ([0-9a-f]{6}): (.*?) \*/')
NUM = re.compile(r'0x[0-9a-f]+|\b\d+\b')


def functions(module):
    """{entry: [instruction shapes]} of gen/<module>"""
    funcs = {}
    cur = None
    for path in sorted(glob.glob(os.path.join(ROOT, 'gen', module, module + '_*.c'))):
        with open(path) as f:
            for line in f:
                if line.startswith('void %s_' % module) and line.rstrip().endswith('(void)'):
                    cur = int(line[len(module) + 6:].split('(')[0], 16)
                    funcs[cur] = []
                    continue
                m = INSN.search(line)
                if m and cur is not None:
                    funcs[cur].append(NUM.sub('#', m.group(2)))
    return funcs


def best(shape, funcs):
    exact = [a for a, s in funcs.items() if s == shape]
    if exact:
        return [(1.0, a) for a in exact]
    scored = []
    for a, s in funcs.items():
        if abs(len(s) - len(shape)) > max(8, len(shape) // 5):
            continue
        r = difflib.SequenceMatcher(None, shape, s, autojunk=False).quick_ratio()
        if r > 0.8:
            r = difflib.SequenceMatcher(None, shape, s, autojunk=False).ratio()
        scored.append((r, a))
    scored.sort(reverse=True)
    return scored[:2]


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    targets = GAMES
    if '--in' in args:
        i = args.index('--in')
        targets = args[i + 1:]
        args = args[:i]
    src, addrs = args[0], [int(a, 16) for a in args[1:]]
    sf = functions(src)
    for t in targets:
        if t == src:
            continue
        tf = functions(t)
        print('%s:' % t)
        for a in addrs:
            found = best(sf[a], tf)
            print('  %06x -> %s' % (a, ', '.join('%06x (%.3f)' % (x, r) for r, x in found) or '-'))


if __name__ == '__main__':
    main()
