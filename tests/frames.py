"""Helpers for the automated tests: run synd_headless(.exe) and look at the frames it dumps.

The headless build writes 32-bit BMP frames in true colour (tests read them with read_bmp; 8-bit ones too) and
can save them as PNG for a person to look at (write_png).  No third-party modules are needed.
"""
import array
import os
import struct
import subprocess
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# SYND_HEADLESS names another build of it (the development build: build-dev/RelWithDebInfo/synd_headless.exe)
HEADLESS = os.environ.get('SYND_HEADLESS') or os.path.join(ROOT, 'build', 'synd_headless.exe' if os.name == 'nt'
                                                           else 'synd_headless')
GAME_DIR = os.path.join(ROOT, '_references', 'dos')
ARCH = os.environ.get('SYND_ARCH', '')  # macOS: arm64 or x86_64 to pick one half of the universal build


class Frame:
    def __init__(self, w, h, pix):
        self.w, self.h, self.pix = w, h, pix  # pix: 0xRRGGBB values, row-major, top row first

    def rgb(self, x, y):
        p = self.pix[y * self.w + x]
        return (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF

    def region_signature(self, x0, y0, x1, y1):
        """a hash of a rectangle (fractions 0..1 of the picture), to tell screens apart"""
        a, b = int(x0 * self.w), int(x1 * self.w)
        c, d = int(y0 * self.h), int(y1 * self.h)
        rows = b''.join(self.pix[y * self.w + a:y * self.w + b].tobytes() for y in range(c, d))
        return zlib.crc32(rows)

    def colour_count(self):
        return len(set(self.pix))

    def mean_brightness(self):
        tot = 0
        for p in self.pix:
            tot += ((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF)
        return tot / (3 * len(self.pix))


def read_bmp(path):
    with open(path, 'rb') as f:
        d = f.read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bits = struct.unpack_from('<H', d, 28)[0]
    pix = array.array('I')
    if bits == 32:
        rows = [d[off + y * w * 4: off + (y + 1) * w * 4] for y in range(h)]
        for r in reversed(rows):
            line = array.array('I')
            line.frombytes(r)
            pix.extend(line)
        for i in range(len(pix)):
            pix[i] &= 0xFFFFFF
    else:
        pal = [struct.unpack_from('<I', d, 54 + 4 * i)[0] & 0xFFFFFF for i in range(256)]
        row = (w + 3) & ~3
        for y in reversed(range(h)):
            pix.extend(pal[c] for c in d[off + y * row: off + y * row + w])
    return Frame(w, h, pix)


def write_png(frame, path, scale=1):
    raw = bytearray()
    for y in range(frame.h):
        line = bytearray()
        for x in range(frame.w):
            line += bytes(frame.rgb(x, y)) * scale
        for _ in range(scale):
            raw += b'\0' + line

    def chunk(kind, body):
        c = kind + body
        return struct.pack('>I', len(body)) + c + struct.pack('>I', zlib.crc32(c))

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', frame.w * scale, frame.h * scale, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def run(start=None, seconds=10, inputs=(), dump_dir=None, dump_every=1000, user_dir=None, extra=(), log=None,
        virtual_step=10):
    """run the headless game; `inputs` is a list of "ms:event" strings (see headless_main.c); returns
    (exit code, sorted list of (ms, frame path), log text).  By default on the virtual clock (virtual_step
    microseconds per clock reading), so a run repeats exactly; virtual_step=0 uses the real clock."""
    args = [HEADLESS, '--game-dir', GAME_DIR, '--seconds', str(seconds)]
    if virtual_step:
        args += ['--virtual-time', str(virtual_step)]
    if start:
        args += ['--start', start]
    if user_dir:
        args += ['--user-dir', user_dir]
    if inputs:
        args += ['--input', ','.join(inputs)]
    if dump_dir:
        os.makedirs(dump_dir, exist_ok=True)
        for n in os.listdir(dump_dir):
            if n.endswith('.bmp'):
                os.remove(os.path.join(dump_dir, n))
        args += ['--dump', dump_dir, '--dump-every', str(dump_every)]
    args += list(extra)
    if ARCH:  # a universal macOS build: run this architecture of it (x86_64 goes through Rosetta)
        args = ['arch', '-' + ARCH] + args
    p = subprocess.run(args, capture_output=True, text=True, errors='replace')
    frames = []
    if dump_dir:
        for n in sorted(os.listdir(dump_dir)):
            if n.startswith('frame_') and n.endswith('.bmp'):
                frames.append((int(n[6:11]), os.path.join(dump_dir, n)))
    if log:
        with open(log, 'w', encoding='utf-8') as f:
            f.write(p.stdout + p.stderr)
    return p.returncode, frames, p.stdout + p.stderr
