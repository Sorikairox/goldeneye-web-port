#!/usr/bin/env python3
"""Attribute romscan hits in a .wasm file to data symbols (scripts/web/romscan.sh).

    romscan_attr.py HITS.tsv WASM LINK.map [ALLOW]

For each hit row (file, offset, length, source, source_offset) inside WASM, maps the file offset
to a memory address through the wasm data section, then to the data symbol(s) of the wasm-ld map
(LINK.map) covering it. Prints one line per hit: length, address, symbol(s), source.
ALLOW (scripts/web/romscan.allow): substrings of map input names whose hits are accepted
(third-party library constants that merely coincide with ROM bytes); a hit is allowed when every
symbol it touches matches. Exit status 1 when a hit is not allowed (or not in WASM at all).
"""
import bisect
import sys


def uleb(b, o):
    v = sh = 0
    while True:
        x = b[o]; o += 1
        v |= (x & 0x7f) << sh; sh += 7
        if not x & 0x80:
            return v, o


def sleb(b, o):
    v = sh = 0
    while True:
        x = b[o]; o += 1
        v |= (x & 0x7f) << sh; sh += 7
        if not x & 0x80:
            break
    if sh < 64 and x & 0x40:
        v -= 1 << sh
    return v, o


def data_segments(w):
    segs = []  # (file_off, addr, len)
    o = 8
    while o < len(w):
        sid = w[o]; o += 1
        sz, o = uleb(w, o)
        end = o + sz
        if sid == 11:
            q = o
            cnt, q = uleb(w, q)
            for _ in range(cnt):
                flags, q = uleb(w, q)
                addr = None
                if flags == 2:
                    _, q = uleb(w, q)
                if flags in (0, 2):
                    q += 1
                    addr, q = sleb(w, q)
                    q += 1
                n, q = uleb(w, q)
                if addr is not None:
                    segs.append((q, addr, n))
                q += n
        o = end
    return segs


def main():
    hits, wasm, mapf = sys.argv[1:4]
    allow = []
    if len(sys.argv) > 4:
        for line in open(sys.argv[4]):
            line = line.split('#', 1)[0].strip()
            if line:
                allow.append(line)
    bad = 0
    segs = sorted(data_segments(open(wasm, 'rb').read()))
    fstarts = [s[0] for s in segs]
    syms = []
    for line in open(mapf, errors='replace'):
        p = line.split()
        if len(p) >= 4 and p[0] != '-' and ':(' in p[3]:
            try:
                syms.append((int(p[0], 16), int(p[2], 16), p[3]))
            except ValueError:
                pass
    syms.sort()
    sstarts = [s[0] for s in syms]
    for row in open(hits):
        f = row.rstrip('\n').split('\t')
        if f[0] == 'file':
            continue
        if not f[0].endswith('.wasm'):
            print('%6s  %s +%s  <- %s+%s  NOT ALLOWED' % (f[2], f[0], f[1], f[3], f[4]))
            bad += 1
            continue
        off, ln = int(f[1]), int(f[2])
        i = bisect.bisect_right(fstarts, off) - 1
        where = 'outside the data section'
        names = []
        if i >= 0 and segs[i][0] <= off < segs[i][0] + segs[i][2]:
            addr = segs[i][1] + off - segs[i][0]
            names = []
            j = bisect.bisect_right(sstarts, addr) - 1
            while j >= 0 and j < len(syms) and syms[j][0] < addr + ln:
                if syms[j][0] + syms[j][1] > addr:
                    names.append(syms[j][2].replace('CMakeFiles/ge007.dir/', ''))
                j += 1
            where = '%#x %s' % (addr, ' '.join(names[:3]) + (' +%d more' % (len(names) - 3) if len(names) > 3 else ''))
        ok = bool(names) and all(any(a in n for a in allow) for n in names)
        bad += not ok
        print('%6d  %s  <- %s+%s%s' % (ln, where, f[3], f[4], '  (allowed)' if ok else '  NOT ALLOWED'))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
