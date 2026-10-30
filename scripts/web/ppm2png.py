#!/usr/bin/env python3
"""G7: convert binary PPM (P6) frame dumps (GE_PCDUMP) to PNG next to them, stdlib only.
   python3 scripts/web/ppm2png.py DIR      (every *.ppm in DIR -> *.png)"""
import os, struct, sys, zlib


def ppm_to_png(src, dst):
    data = open(src, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:   # magic, width, height, maxval (comments not used by the dumper)
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    if fields[0] != b'P6':
        raise ValueError(f'{src}: not a P6 PPM')
    w, h = int(fields[1]), int(fields[2])
    px = data[pos:pos + w * h * 3]
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda t, c: struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)


if __name__ == '__main__':
    d = sys.argv[1]
    n = 0
    for f in sorted(os.listdir(d)):
        if f.endswith('.ppm'):
            ppm_to_png(os.path.join(d, f), os.path.join(d, f[:-4] + '.png'))
            n += 1
    print(f'ppm2png: {n} frames in {d}')
