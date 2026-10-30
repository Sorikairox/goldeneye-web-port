#!/bin/bash
# Build the web (wasm) game into build-web/ inside the goldeneye-dev container.
#   scripts/web/build.sh [pal-final] [Release|RelWithDebInfo|Debug]   (default pal-final RelWithDebInfo)
# Output: build-web/ge007.{js,wasm} + the page from port/web/ copied next to it (build-web/site/).
# Compact output: only errors, the last lines, and the artifact sizes.
set -euo pipefail
. "$(dirname "$0")/env.sh"
ROMID=${1:-pal-final}; BT=${2:-RelWithDebInfo}
# ONE container build/link at a time across sessions (two parallel emscripten links overloaded the Docker
# VM and crashed Docker Desktop, 2026-10-29): /tmp/goldeneye-build.lock holds the builder's PID.
BLOCK=/tmp/goldeneye-build.lock
while ! ( set -o noclobber; echo $$ > "$BLOCK" ) 2>/dev/null; do
  H=$(cat "$BLOCK" 2>/dev/null || true)
  if [ -n "$H" ] && ! kill -0 "$H" 2>/dev/null; then rm -f "$BLOCK"; continue; fi
  echo "build: waiting for the build lock (PID $H)"; sleep 10
done
trap '[ "$(cat "$BLOCK" 2>/dev/null)" = $$ ] && rm -f "$BLOCK"' EXIT
B=/build/web     # in the $GE_BUILDVOL named volume (env.sh); outputs are copied to build-web/
ge_exec bash -c "mkdir -p $B build-web && \
  { emcmake cmake -S . -B $B -G Ninja -DROMID=$ROMID -DCMAKE_BUILD_TYPE=$BT >$B/cmake.log 2>&1 || { tail -30 $B/cmake.log; cp $B/cmake.log build-web/; exit 1; }; } && \
  ninja -C $B -j$GE_CPUS -k 0 >$B/build.log 2>&1; rc=\$?; cp $B/cmake.log $B/build.log build-web/; \
  grep -E 'error:|undefined symbol' $B/build.log | sort | uniq -c | sort -rn | head -40; \
  [ \$rc = 0 ] || { tail -5 $B/build.log; exit \$rc; }; \
  sm=\$(grep -c 'function signature mismatch' $B/build.log || true); \
  bi=\$(strings -n 8 $B/ge007.wasm | grep -c '_bitcast_invalid' || true); \
  echo \"signature mismatches: \$sm (fix: scripts/web/sigadapt.py scan $B/build.log $B), bitcast_invalid traps: \$bi\"; \
  mkdir -p build-web/site && cp $B/ge007.js $B/ge007.wasm build-web/ && cp port/web/* build-web/site/ && \
  cp $B/ge007.js $B/ge007.wasm build-web/site/ && \
  if [ \"${GE_NOSTRIP:-0}\" = 1 ]; then rm -f build-web/site/ge007.romfill; \
    echo 'unstripped dev build: contains ROM-derived data, never deploy' > build-web/site/DEV-UNSTRIPPED; \
    echo 'GE_NOSTRIP=1: site is UNSTRIPPED (build-web/site/DEV-UNSTRIPPED), dev only'; \
  else bash scripts/web/romfill.sh $ROMID; fi && \
  python3 scripts/web/netregions.py $B/ge007.map build-web/site/ge007.netregions && \
  ls -la build-web/site/ge007.wasm build-web/site/ge007.js | awk '{print \$5, \$9}'"
