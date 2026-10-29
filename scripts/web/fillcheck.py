#!/usr/bin/env python3
"""Gate for template + fill: independent check that the filled memory equals the original build.

    fillcheck.py ORIGINAL.wasm SITE/ge007.romfill [TEMPLATE.wasm]

Prints the SHA-256 over every symbol extent listed in the fill table, read from the ORIGINAL
(unstripped) binary's data segments, i.e. what memory must hold after the fill. Load the stripped
site with &fillcheck in the URL: the page logs '[page] fillcheck: ... sha256 <h>' right after the fill
and before main(); the two hashes must be equal. With TEMPLATE.wasm, also reports how many bytes of
those extents differ between the template and the original (= bytes the fill must restore).
"""
import hashlib
import struct
import sys

sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '../../tools_web'))
from romscan_attr import data_segments  # noqa: E402


def image(path):
    w = open(path, 'rb').read()
    mem = {}
    for foff, addr, n in data_segments(w):
        mem[addr] = w[foff:foff + n]
    starts = sorted(mem)

    def read(a, n):
        out = bytearray(n)
        for s in starts:
            seg = mem[s]
            lo, hi = max(a, s), min(a + n, s + len(seg))
            if lo < hi:
                out[lo - a:hi - a] = seg[lo - s:hi - s]
        return bytes(out)
    return read


def main():
    orig, table = sys.argv[1], sys.argv[2]
    t = open(table, 'rb').read()
    magic, ver, count, crcf, crcs, nsyms = struct.unpack_from('<6I', t, 0)
    syms = [struct.unpack_from('<2I', t, 24 + 8 * i) for i in range(nsyms)]
    read = image(orig)
    data = b''.join(read(a, n) for a, n in syms)
    print('fillcheck: %d symbols, %d bytes, sha256 %s (original build)' % (nsyms, len(data), hashlib.sha256(data).hexdigest()))
    if len(sys.argv) > 3:
        tread = image(sys.argv[3])
        tdata = b''.join(tread(a, n) for a, n in syms)
        diff = sum(1 for x, y in zip(data, tdata) if x != y)
        print('fillcheck: template differs from the original in %d bytes of those extents' % diff)


if __name__ == '__main__':
    main()
