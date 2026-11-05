#!/bin/bash
# Build the retrowser.app v1 module (docs/web/RTW.md) into build/rtw/:
#   ge007.js (MODULARIZE factory createGoldenEye, worker), ge007.wasm (template, ROM data zeroed),
#   ge007.romfill (offsets-only fill table, files.data), ge007.netregions (online state-hash ranges of this build's
#   link map, files.data; scripts/web/netregions.py), cover.png (the cartridge picture, art/cover.png),
#   retrowser.json (hashed with rtw-hash, checked with rtw-check).
#   scripts/rtw/build_rtw.sh [Release|RelWithDebInfo]       (default RelWithDebInfo, like the web site; DWARF stripped)
# Env: RTW_REPO (a retrowser checkout, default $HOME/Code/retrowser), RTW_ROM (ROM for the leak scan, default the
# dev copy data/ge007.pal-final.z64). Separate CMake tree /build/rtw (-DGE_RTW=ON); the web build is untouched.
set -euo pipefail
. "$(dirname "$0")/../web/env.sh"
BT=${1:-RelWithDebInfo}
RTW_REPO=${RTW_REPO:-$HOME/Code/retrowser}
RTW_ROM=${RTW_ROM:-$GE_ROOT/data/ge007.pal-final.z64}
BLOCK=/tmp/goldeneye-build.lock
while ! ( set -o noclobber; echo $$ > "$BLOCK" ) 2>/dev/null; do
  H=$(cat "$BLOCK" 2>/dev/null || true)
  if [ -n "$H" ] && ! kill -0 "$H" 2>/dev/null; then rm -f "$BLOCK"; continue; fi
  echo "build_rtw: waiting for the build lock (PID $H)"; sleep 10
done
trap '[ "$(cat "$BLOCK" 2>/dev/null)" = $$ ] && rm -f "$BLOCK"' EXIT
B=/build/rtw; O=build/rtw; R=build/rtw-reports
ge_exec bash -c "mkdir -p $B $O $R && rm -f $O/* && \
  { emcmake cmake -S . -B $B -G Ninja -DROMID=pal-final -DGE_RTW=ON -DCMAKE_BUILD_TYPE=$BT >$B/cmake.log 2>&1 || { tail -30 $B/cmake.log; exit 1; }; } && \
  ninja -C $B -j$GE_CPUS -k 0 >$B/build.log 2>&1; rc=\$?; cp $B/cmake.log $B/build.log $R/; \
  grep -E 'error:|undefined symbol' $B/build.log | sort | uniq -c | sort -rn | head -40; \
  [ \$rc = 0 ] || { tail -5 $B/build.log; exit \$rc; }; \
  sm=\$(grep -c 'function signature mismatch' $B/build.log || true); \
  bi=\$(strings -n 8 $B/ge007.wasm | grep -c '_bitcast_invalid' || true); \
  echo \"signature mismatches: \$sm, bitcast_invalid traps: \$bi\"; \
  cp $B/ge007.js $O/ && \
  ROMFILL_BUILD=$B ROMFILL_OUT=$O ROMFILL_REPORTS=$R bash scripts/web/romfill.sh pal-final && \
  python3 scripts/web/netregions.py $B/ge007.map $O/ge007.netregions && \
  /emsdk/upstream/bin/llvm-objcopy --strip-debug $O/ge007.wasm && \
  ls -la $O | awk 'NR>1 {print \$5, \$9}'"
rm -f "$BLOCK"; trap - EXIT     # romscan (deploy_check) takes the build lock itself
cp scripts/rtw/retrowser.template.json $O/retrowser.json
cp art/cover.png $O/cover.png     # the cartridge picture (manifest "picture", standard 1.2; not passed to the module)
node "$RTW_REPO/tools/rtw-hash.ts" "$O" | sed 's/^/rtw-hash: /'
ROMSCAN_MAP=$B/ge007.map bash scripts/web/deploy_check.sh "$O" | tail -2
node "$RTW_REPO/tools/rtw-check.ts" "$O" --rom "$RTW_ROM" | sed 's/^/rtw-check: /'
