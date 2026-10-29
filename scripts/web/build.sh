#!/bin/bash
# Build the web (wasm) game into build-web/ inside the goldeneye-dev container.
#   scripts/web/build.sh [pal-final] [Release|RelWithDebInfo|Debug]   (default pal-final RelWithDebInfo)
# Output: build-web/ge007.{js,wasm} + the page from port/web/ copied next to it (build-web/site/).
# Compact output: only errors, the last lines, and the artifact sizes.
set -euo pipefail
. "$(dirname "$0")/env.sh"
ROMID=${1:-pal-final}; BT=${2:-RelWithDebInfo}
LOG=build-web/build.log
ge_exec bash -c "mkdir -p build-web && \
  { emcmake cmake -S . -B build-web -G Ninja -DROMID=$ROMID -DCMAKE_BUILD_TYPE=$BT >build-web/cmake.log 2>&1 || { tail -30 build-web/cmake.log; exit 1; }; } && \
  ninja -C build-web -k 0 >$LOG 2>&1; rc=\$?; \
  grep -E 'error:|undefined symbol' $LOG | sort | uniq -c | sort -rn | head -40; \
  [ \$rc = 0 ] || { tail -5 $LOG; exit \$rc; }; \
  sm=\$(grep -c 'function signature mismatch' $LOG || true); \
  bi=\$(strings -n 8 build-web/ge007.wasm | grep -c '_bitcast_invalid' || true); \
  echo \"signature mismatches: \$sm (fix: scripts/web/sigadapt.py scan $LOG build-web), bitcast_invalid traps: \$bi\"; \
  mkdir -p build-web/site && cp port/web/* build-web/site/ && cp build-web/ge007.js build-web/ge007.wasm build-web/site/ && \
  ls -la build-web/site/ge007.wasm build-web/site/ge007.js | awk '{print \$5, \$9}'"
