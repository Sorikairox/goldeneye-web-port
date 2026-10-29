#!/bin/bash
# Deploy gate: no ROM-derived byte runs in the site (docs/web/ROMDATA.md).
#   scripts/web/romscan.sh [site dir=build-web/site] [min run=64]
# Scans every file of the site for runs of >= MINRUN bytes that also occur in the dev ROM copy
# (data/ge007.pal-final.z64): raw, in any RZ-compressed stream, 16/32-bit byte-swapped
# (tools_web/romscan.c). Hits inside ge007.wasm are attributed to data symbols through the link map;
# only the third-party constants listed in scripts/web/romscan.allow are accepted. Exit 1 = do not
# deploy. Runs in the dev container (node + emcc), holding the build lock.
set -euo pipefail
cd "$(dirname "$0")/../.."
. scripts/web/env.sh
SITE=${1:-build-web/site}; MIN=${2:-64}
ge_ensure
scripts/web/lockrun.sh /tmp/goldeneye-build.lock docker exec -w /src "$GE_DEV" bash -c "set -e
  mkdir -p /build/tools
  if [ ! -f /build/tools/romscan.js ] || [ tools_web/romscan.c -nt /build/tools/romscan.js ]; then
    emcc -O2 -sUSE_ZLIB=1 -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4gb tools_web/romscan.c -o /build/tools/romscan.js
  fi
  node /build/tools/romscan.js -m $MIN -t build-web/romscan.tsv data/ge007.pal-final.z64 \$(find $SITE -type f | sort) || true
  python3 tools_web/romscan_attr.py build-web/romscan.tsv $SITE/ge007.wasm /build/web/ge007.map scripts/web/romscan.allow \
    && echo 'romscan: PASS (no ROM-derived runs >= $MIN bytes outside the allowlist)' \
    || { echo 'romscan: FAIL (see above; full list in build-web/romscan.tsv)'; exit 1; }"
