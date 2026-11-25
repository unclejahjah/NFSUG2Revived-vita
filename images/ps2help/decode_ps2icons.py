"""Decodes the PS2_* button icons from a decompressed PS2 GLOBALB.BUN into images/ps2help/ps2/ as 32x32 PNGs."""
import os
import struct
import sys

from PIL import Image

PIXEL_BITS = [6, 3, 0, 1, 2, 4, 5, 7, 8, 9]  # source bit i -> pixel bit (x 0-4, y 5-9)
XORS = [(2, 6), (2, 7)]  # GS column swap: x2 ^= y1 ^ y2


def pixel(k):
    t = 0
    for i, b in enumerate(PIXEL_BITS):
        if (k >> i) & 1:
            t |= 1 << b
    u = t
    for dst, src in XORS:
        if (t >> src) & 1:
            u ^= 1 << dst
    return u


def chunks(d, o, end, out):
    while o + 8 <= end:
        cid, size = struct.unpack_from('<II', d, o)
        if o + 8 + size > end:
            break
        out.append((cid, o + 8, size))
        if cid & 0x80000000:
            chunks(d, o + 8, o + 8 + size, out)
        o += 8 + size


def main(path, outdir):
    d = open(path, 'rb').read()
    found = []
    chunks(d, 0, len(d), found)
    infos = [c for c in found if c[0] == 0x33310004]
    datas = [c for c in found if c[0] == 0x33320002]
    pos = [pixel(k) for k in range(1024)]
    for (_, io, isz), (_, do, _) in zip(infos, datas):
        base = (do + 0x7f) & ~0x7f
        for n in range(isz // 0x7c):
            e = d[io + n * 0x7c:io + (n + 1) * 0x7c]
            name = e[12:36].split(b'\0')[0].decode('latin1')
            if not name.startswith('PS2_'):
                continue
            data, pal = struct.unpack_from('<II', e, 48)
            colours = []
            for j in range(16):
                r, g, b, a = d[base + pal + j * 4:base + pal + j * 4 + 4]
                colours.append((r, g, b, min(255, a * 2)))
            raw = d[base + data:base + data + 512]
            pixels = [None] * 1024
            for k in range(1024):
                pixels[pos[k]] = colours[(raw[k // 2] >> (4 * (k % 2))) & 15]
            img = Image.new('RGBA', (32, 32))
            img.putdata(pixels)
            img.save('%s/%s.png' % (outdir, name))
            print(name)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "ps2"))
