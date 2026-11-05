#!/bin/bash
# Refresh the retrowser.app build that lives on this branch (docs/web/RTW.md, "Published output"):
#   retrowser.json          repo root: the manifest retrowser.app reads from the default branch, so the plain link
#                           Sorikairox/goldeneye-web-port works; its file paths point into retrowser/
#   retrowser/ge007.{js,wasm,romfill,netregions}   the built files (same bytes as build/rtw)
#   retrowser/cover.png     the cartridge picture (manifest "picture", standard 1.2), copied from the source art
#                           art/cover.png (box art, credit in the template; never a screenshot or ROM-derived)
# and, with --dist, the local orphan branch rtw-dist (manifest + files at its root, link form owner/repo@rtw-dist).
#
#   scripts/rtw/publish_rtw.sh [--no-build] [--dist]
#     --no-build   reuse build/rtw (default: run scripts/rtw/build_rtw.sh first)
#     --dist       also commit build/rtw onto branch rtw-dist (plumbing only: no checkout, the work tree is untouched)
# Then commit retrowser.json + retrowser/ yourself (one commit; the script prints the command).
# Env: RTW_REPO (retrowser checkout, default $HOME/Code/retrowser; uses its built dist/tools/rtw.mjs, else
# tools/rtw-{hash,check}.ts), RTW_ROM (ROM for the leak scan, default the dev copy data/ge007.pal-final.z64),
# RTW_DIST_TRAILER (extra lines appended to the rtw-dist commit message).
# Checks (any failure stops): rtw check of the root manifest with --rom, deploy_check (romscan) of retrowser/, and
# the romfill layout (header + symbol extents + fill entries, nothing else: no ROM bytes, see RTW.md).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
RTW_REPO=${RTW_REPO:-$HOME/Code/retrowser}
RTW_ROM=${RTW_ROM:-$ROOT/data/ge007.pal-final.z64}
BUILD=1; DIST=0
for a in "$@"; do
  case $a in --no-build) BUILD=0 ;; --dist) DIST=1 ;; *) echo "publish_rtw: unknown argument $a" >&2; exit 2 ;; esac
done
rtw() {   # rtw hash|check ARGS...
  if [ -f "$RTW_REPO/dist/tools/rtw.mjs" ]; then node "$RTW_REPO/dist/tools/rtw.mjs" "$@"
  else local c=$1; shift; node "$RTW_REPO/tools/rtw-$c.ts" "$@"; fi
}
[ $BUILD = 1 ] && bash scripts/rtw/build_rtw.sh
B=build/rtw; O=retrowser; FILES="ge007.js ge007.wasm ge007.romfill ge007.netregions"
for f in $FILES retrowser.json; do [ -s "$B/$f" ] || { echo "publish_rtw: $B/$f missing (build first)" >&2; exit 1; }; done
# the cartridge picture: art/cover.png + the template's "picture" entry, put into build/rtw here too, so a build made
# before the picture existed (--no-build) gets it; rtw hash rewrites only the manifest (the game files are unchanged)
cp art/cover.png "$B/cover.png"
node -e '
const fs = require("fs"), t = JSON.parse(fs.readFileSync(process.argv[1], "utf8")), m = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const p = { ...t.picture }, o = m.picture || {};
if (o.path === p.path && o.sha256) { p.sha256 = o.sha256; p.size = o.size; }
m.picture = p;
const out = {}; for (const k of Object.keys(t)) if (k in m) out[k] = m[k]; for (const k of Object.keys(m)) if (!(k in out)) out[k] = m[k];
fs.writeFileSync(process.argv[2], JSON.stringify(out, null, 2) + "\n");
' scripts/rtw/retrowser.template.json "$B/retrowser.json"
rtw hash "$B" | sed 's/^/rtw hash build\/rtw: /'

# romfill = offsets only: u32 magic 'GERF', version, count, crc_fill, crc_syms, nsyms; nsyms x 8 B; count x 20 B
node -e '
const b = require("fs").readFileSync(process.argv[1]);
const u = (o) => b.readUInt32LE(o), n = u(8), ns = u(20), want = 24 + ns * 8 + n * 20;
if (u(0) !== 0x46524547 || u(4) !== 1 || b.length !== want) { console.error("publish_rtw: romfill layout wrong", b.length, want); process.exit(1); }
for (let i = 0, o = 24 + ns * 8; i < n; i++, o += 20)
  if (b[o + 16] > 1 || b[o + 17] > 2 || b[o + 18] || b[o + 19]) { console.error("publish_rtw: romfill entry", i, "bad"); process.exit(1); }
console.log(`romfill: offsets only (${ns} symbol extents, ${n} fill entries, ${b.length} B = 24 + ${ns}x8 + ${n}x20)`);
' "$B/ge007.romfill"

mkdir -p "$O"
rm -f "$O"/*
for f in $FILES cover.png; do cp "$B/$f" "$O/$f"; done
# root manifest = build/rtw's with every file path moved into retrowser/
node -e '
const fs = require("fs"), m = JSON.parse(fs.readFileSync(process.argv[1], "utf8")), d = process.argv[2];
const mv = (e) => { e.path = d + "/" + e.path; };
mv(m.files.wasm); if (m.files.js) mv(m.files.js); (m.files.data || []).forEach(mv); if (m.picture) mv(m.picture);
fs.writeFileSync("retrowser.json", JSON.stringify(m, null, 2) + "\n");
' "$B/retrowser.json" "$O"
rtw hash . | sed 's/^/rtw hash: /'
rtw check . --rom "$RTW_ROM" | sed 's/^/rtw check: /'
ROMSCAN_MAP=/build/rtw/ge007.map bash scripts/web/deploy_check.sh "$O" | tail -2
for f in $FILES cover.png; do cmp -s "$B/$f" "$O/$f" || { echo "publish_rtw: $O/$f differs from $B/$f" >&2; exit 1; }; done

if [ $DIST = 1 ]; then
  IDX=$(mktemp "${TMPDIR:-/tmp}/rtwdist-index.XXXXXX"); trap 'rm -f "$IDX"' EXIT
  GIT_INDEX_FILE=$IDX git read-tree --empty
  for f in retrowser.json $FILES cover.png; do
    GIT_INDEX_FILE=$IDX git update-index --add --cacheinfo "100644,$(git hash-object -w --no-filters "$B/$f"),$f"
  done
  TREE=$(GIT_INDEX_FILE=$IDX git write-tree)
  OLD=$(git rev-parse -q --verify refs/heads/rtw-dist || true)
  if [ -n "$OLD" ] && [ "$(git rev-parse "$OLD^{tree}")" = "$TREE" ]; then
    echo "rtw-dist: unchanged ($(git rev-parse --short "$OLD"))"
  else
    BID=$(shasum -a 256 "$B/retrowser.json" | cut -c1-16)
    MSG="rtw-dist: GoldenEye 007 (EU) retrowser.app v1 build (build id $BID; source $(git rev-parse --abbrev-ref HEAD) $(git rev-parse --short HEAD))"
    [ -n "${RTW_DIST_TRAILER:-}" ] && MSG=$(printf '%s\n\n%s' "$MSG" "$RTW_DIST_TRAILER")
    NEW=$(git commit-tree "$TREE" ${OLD:+-p "$OLD"} -m "$MSG")
    git update-ref refs/heads/rtw-dist "$NEW" ${OLD:+"$OLD"}
    echo "rtw-dist: $(git log --oneline -1 "$NEW" | cut -c1-120)"
  fi
fi
echo "publish_rtw: OK; commit with: git add retrowser.json retrowser && git commit"
