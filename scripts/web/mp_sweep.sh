#!/bin/bash
# Local split-screen sweep: starts a multiplayer match straight from boot with the port's D416 harness
# (GE_STARTMP=<players>,<mp stage index>; GE_MPVIRT=<players-1> virtual pads) and drives every seat with
# its own scripted pad (GE_INPUTSCRIPT, _P1.._P3): each walks, fires and turns its own way.
#   scripts/web/mp_sweep.sh [players,index:name ...]   (default: 2 players on all 12 arenas + 4 on Temple)
#   SECS=36 EVERY=3 OUT=port_work/mpsweep
# MP stage indices: front.c multi_stage_setups[] (1 Temple 2 Complex 3 Caves 4 Library 5 Basement
# 6 Stack 7 Facility 8 Bunker 9 Archives 10 Caverns 11 Egypt; 0 = random). Same summary.tsv as
# level_sweep.sh. Screenshots are game imagery: gitignored port_work/ only.
set -uo pipefail
cd "$(dirname "$0")/../.."
SECS=${SECS:-36}; EVERY=${EVERY:-3}; OUT=${OUT:-port_work/mpsweep}
ALL="2,1:temple 2,2:complex 2,3:caves 2,4:library 2,5:basement 2,6:stack 2,7:facility 2,8:bunker
2,9:archives 2,10:caverns 2,11:egypt 4,1:temple4 3,8:bunker3 4,2:complex4"
SEATS=${*:-$ALL}
PADS=('15000:CUPHOLD;18000:CNONE;18500:Z;19000:SLEFT;21000:SNONE;22000:Z'
      '15000:CUPHOLD;17000:CNONE;17500:Z;18000:SRIGHT;20000:SNONE;21000:Z'
      '15000:SLEFT;16500:SNONE;17000:CUPHOLD;19000:CNONE;19500:Z'
      '15000:CDOWNHOLD;16000:CNONE;16500:SRIGHT;18000:SNONE;18500:Z')
# sidecars converted in the page from the ROM (the real path); SWEEP_DEV_ASSETS=1: the Python dev ones
ASSETS=""; [ "${SWEEP_DEV_ASSETS:-0}" = 1 ] && ASSETS="&assets=../dev"
mkdir -p "$OUT"; SUM="$OUT/summary.tsv"
[ -f "$SUM" ] || printf 'level\tname\texceptions\tstalls\tanswers\tpacing\tfirst_error\n' > "$SUM"
for S in $SEATS; do
  NP=${S%%,*}; R=${S#*,}; IDX=${R%%:*}; NAME=${R#*:}
  D="$OUT/$NP-$IDX-$NAME"; rm -rf "$D"
  URL="http://localhost:${GE_PORT:-8770}/app/index.html?romurl=../dev/ge007.pal-final.z64$ASSETS&env=GE_MPVIRT=$((NP-1))&env=GE_STARTMP=$NP,$IDX&env=GE_INPUTSCRIPT_MS=1&env=GE_INPUTSCRIPT=${PADS[0]}"
  for ((p = 1; p < NP; p++)); do URL="$URL&env=GE_INPUTSCRIPT_P$p=${PADS[$p]}"; done
  node scripts/web/probe.mjs "$URL${GE_EXTRA_PARAMS:-}" "$SECS" "$D" "$EVERY" > "$D.probe.txt" 2>&1
  C="$D/console.log"
  EX=$(grep -E 'EXCEPTION|ABORT|RuntimeError|CRASHED' "$C" 2>/dev/null | grep -vc 'Cannot mix BigInt'); ST=$(grep -c 'STALL' "$C" 2>/dev/null)
  ANS=$(grep -q 'main thread answers' "$D.probe.txt" && echo yes || echo NO)
  PAC=$(grep 'pacing:' "$C" | tail -1 | sed 's/^[0-9.]* //' | cut -c1-110)
  ERR=$(grep -E 'EXCEPTION|ABORT|RuntimeError|CRASHED|STALL|refused|\[ERROR' "$C" | grep -v 'Cannot mix BigInt' | head -1 | cut -c1-140)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$NP,$IDX" "$NAME" "$EX" "$ST" "$ANS" "$PAC" "$ERR" | tee -a "$SUM"
done
