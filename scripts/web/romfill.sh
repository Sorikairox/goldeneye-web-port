#!/bin/bash
# Template + fill, build half (docs/web/ROMDATA.md). Runs INSIDE the dev container after the link
# (called by scripts/web/build.sh): makes build-web/site/ge007.wasm a template (ROM-derived data
# zeroed) + build-web/site/ge007.romfill (offsets-only fill table, applied by the page at boot),
# then runs the deploy-gate scan on the site.
#   scripts/web/romfill.sh [region]      (needs the dev ROM copy data/ge007.<region>.z64)
set -euo pipefail
R=${1:-pal-final}
B=/build/web; T=/build/tools
ROM=data/ge007.$R.z64
mkdir -p $T
for t in romfill romscan; do
  if [ ! -f $T/$t.js ] || [ tools_web/$t.c -nt $T/$t.js ]; then
    emcc -O2 -sUSE_ZLIB=1 -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4gb tools_web/$t.c -o $T/$t.js
  fi
done
if [ ! -f "$ROM" ]; then
  echo "romfill: $ROM missing: cannot strip the ROM-derived data. Put the dev ROM copy there, or build"
  echo "         with GE_NOSTRIP=1 for an unstripped dev-only site (marked DEV-UNSTRIPPED, refused by"
  echo "         scripts/web/deploy_check.sh)."
  rm -f build-web/site/ge007.romfill build-web/site/ge007.wasm
  exit 1
fi
rm -f build-web/site/DEV-UNSTRIPPED
node $T/romfill.js "$ROM" $B/ge007.wasm $B/ge007.map build-web/site/ge007.wasm build-web/site/ge007.romfill \
  build-web/romfill-report.tsv
node $T/romscan.js -t build-web/romscan.tsv "$ROM" build-web/site/* | tail -1 | sed 's/^/romscan (site): /' || true
