#!/usr/bin/env python3
"""Build-dir copies of game files for the web build, for the two things clang/wasm
cannot take as written. The game source file itself is never edited; a #line directive
keeps diagnostics pointing at the original. Semantics are identical.

1. clang bug: a `#pragma weak A = B` that follows an `extern T A;` declaration makes every
   later use of A "ambiguous" (GCC accepts it). The declaration gets the same meaning as an
   attribute (`extern T A __attribute__((weak, alias("B")));`) and the pragma line is dropped.

2. --abs NAME=ADDR: wasm objects have no absolute data symbols (native: port/src/dram_syms.S).
   For a symbol used in a static initializer (so the pointer trick of port/src/web/webdram.h
   cannot work), the TU's own `extern T NAME[N];` becomes
   `#define NAME (*(T (*)[N])(ADDR))`: same object, same address, still a constant.

3. --fn NAME=PROTO: a function the TU declares as data (`extern s32 NAME;`) only to take
   its address. wasm links functions and data separately (and a function's address is a
   table index), so the declaration becomes the given function prototype; `&NAME` then
   is the real function pointer, as it is on native.

usage: web_srcfix.py SRC.c OUT.c [--abs NAME=ADDR | --fn NAME=PROTO ...]
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
abss, fns = {}, {}
for opt, val in zip(sys.argv[3::2], sys.argv[4::2]):
    k, v = val.split("=", 1)
    {"--abs": abss, "--fn": fns}[opt][k] = v
lines = open(src, encoding="utf-8", errors="surrogateescape").read().split("\n")

pragmas = {}
for i, l in enumerate(lines):
    m = re.match(r"\s*#pragma\s+weak\s+(\w+)\s*=\s*(\w+)\s*$", l)
    if m:
        pragmas[m.group(1)] = (i, m.group(2))
for alias, (pi, target) in pragmas.items():
    decl = None
    for j in range(pi):
        if re.match(r"\s*extern\b.*\b%s\s*;" % re.escape(alias), lines[j]):
            decl = j
    if decl is None:
        continue                      # no prior extern: clang handles the pragma fine
    lines[decl] = re.sub(r"\b(%s)\s*;" % re.escape(alias),
                         r'\1 __attribute__((weak, alias("%s")));' % target, lines[decl])
    lines[pi] = "/* web: #pragma weak %s = %s -> attribute above */" % (alias, target)

for name, addr in abss.items():
    done = False
    for j, l in enumerate(lines):
        m = re.match(r"\s*extern\s+([\w\s]+?)\s+%s\s*(\[[^\]]*\])\s*;" % re.escape(name), l)
        if m:
            lines[j] = "#define %s (*(%s (*)%s)(%s))" % (name, m.group(1), m.group(2), addr)
            done = True
    if not done:
        sys.exit("web_srcfix: no `extern T %s[N];` in %s" % (name, src))

for name, proto in fns.items():
    done = False
    for j, l in enumerate(lines):
        if re.match(r"\s*extern\s+\w+\s+%s\s*;" % re.escape(name), l):
            lines[j] = "extern %s;" % proto
            done = True
    if not done:
        sys.exit("web_srcfix: no `extern T %s;` in %s" % (name, src))

open(dst, "w", encoding="utf-8", errors="surrogateescape").write(
    '#line 1 "%s"\n' % src + "\n".join(lines))
