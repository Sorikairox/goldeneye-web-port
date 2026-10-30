#!/usr/bin/env python3
"""G6 online state hash: the game-state ranges of the wasm static data, from the wasm-ld map.

  python3 scripts/web/netregions.py <ge007.map> <out: ge007.netregions>

The online desync check / determinism proof (port/src/web/webnet.c) hashes the 8 MiB N64 DRAM arena plus the
.data/.bss of every object that holds game state: the decompiled game and libultra (src/**), the build copies of
game files (srcfix/), the asset data (assets/**), and the port files that keep game state (PORT_GAME_STATE).
Port-only state (renderer, audio ring, input devices, fiber stacks, logging, timers in wall time, the net ring)
is left out: it may differ between peers without changing the game.

Output lines (text, loaded by the page in a session and passed to ge_net_region_add):
  R <hex addr> <hex len>            a hashed range (adjacent symbols merged across gaps <= 64 bytes)
  S <hex addr> <hex len> <name>     one symbol inside the ranges (names the memory of a desync report)
"""
import re
import sys

PORT_GAME_STATE = ('random.c', 'pcmodels.c', 'pccg.c', 'gimgfixup.c', 'drawdistgameplay.c', 'frontoptions.c',
                   'watchsettings.c', 'webdram.c')


def wanted(obj):
    p = obj.replace('CMakeFiles/ge007.dir/', '')
    if p.startswith('src/') or p.startswith('assets/') or '/srcfix/' in '/' + p or p.startswith('srcfix'):
        return True
    if p.startswith('port/src/') and p.split('/')[-1].rsplit('.o', 1)[0] in PORT_GAME_STATE:
        return True
    return False


def main():
    mp, out = sys.argv[1], sys.argv[2]
    cur = None
    syms = []
    rx_sec = re.compile(r'^\s*([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+(\S+):\((\S+)\)\s*$')
    for line in open(mp):
        f = line.split()
        if len(f) >= 4 and re.match(r'^[0-9a-f]+$', f[0]) and f[3].startswith('.') and len(line) - len(line.lstrip()) < 12:
            cur = f[3]
            continue
        if cur not in ('.data', '.bss'):
            continue
        m = rx_sec.match(line)
        if not m:
            continue
        addr, size, obj, sec = int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)
        if size and wanted(obj):
            name = re.sub(r'^\.(data|bss)\.', '', sec)
            syms.append((addr, size, name))
    syms.sort()
    regions = []
    for a, n, _ in syms:
        if regions and a <= regions[-1][0] + regions[-1][1] + 64:
            regions[-1][1] = max(regions[-1][1], a + n - regions[-1][0])
        else:
            regions.append([a, n])
    with open(out, 'w') as fo:
        for a, n in regions:
            fo.write('R %x %x\n' % (a, n))
        for a, n, name in syms:
            fo.write('S %x %x %s\n' % (a, n, name))
    total = sum(n for _, n in regions)
    print('netregions: %d symbols, %d ranges, %d bytes hashed (+ 8 MiB DRAM) -> %s' % (len(syms), len(regions), total, out))


if __name__ == '__main__':
    main()
