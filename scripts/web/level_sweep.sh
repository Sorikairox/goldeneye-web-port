#!/bin/bash
# Level sweep: warp (-level_XX, Agent, per-stage memory pools auto-injected as on native, D121) into each
# level, skip the intro camera, walk / fire / turn with the scripted pad, screenshot every few seconds.
#   scripts/web/level_sweep.sh [level ...]       (default: the 20 solo missions + Cuba + the MP arenas)
#   SECS=34 EVERY=3 OUT=port_work/sweep GE_SWEEP_PAD='...' GE_EXTRA_PARAMS='&env=...'
# Level = number (33) or number:name. Needs a server on :8770 (serve.sh, or serve_node.mjs without Docker).
# Per level: <OUT>/<nn>-<name>/{shot-*.png,sheet.png,console.log}; one summary row per level in
# <OUT>/summary.tsv (exceptions, stalls, last pacing line, whether the main thread still answers).
# Screenshots are game imagery: keep them in the gitignored port_work/. LOOK at every sheet.png.
set -uo pipefail
cd "$(dirname "$0")/../.."
SECS=${SECS:-34}; EVERY=${EVERY:-3}; OUT=${OUT:-port_work/sweep}
ALL="33:dam 34:facility 35:runway 36:surface1 09:bunker1 20:silo 26:frigate 43:surface2 27:bunker2 22:statue
24:archives 29:streets 30:depot 25:train 37:jungle 23:control 39:caverns 41:cradle 28:aztec 32:egypt 54:cuba
38:temple 31:complex 50:caves 48:library 45:basement 46:stack 40:citadel"
LEVELS=${*:-$ALL}
# ms since SDL init: B pulses skip the intro camera (bondview2.c: A/B/Z/START/L/R skip; B in gameplay is
# "use", while A would cycle the weapon),
# then walk forward 3 s (C-up held: the port's default control style looks with the stick's Y axis),
# fire, turn left 2 s, strafe right 1 s, fire
PAD=${GE_SWEEP_PAD:-'8000:B;9000:B;10000:B;13000:CUPHOLD;16000:CNONE;16500:Z;17000:SLEFT;19000:SNONE;19500:CRIGHTHOLD;20500:CNONE;21000:Z'}
mkdir -p "$OUT"; SUM="$OUT/summary.tsv"
[ -f "$SUM" ] || printf 'level\tname\texceptions\tstalls\tanswers\tpacing\tfirst_error\n' > "$SUM"
for L in $LEVELS; do
  N=${L%%:*}; NAME=${L#*:}; [ "$NAME" = "$L" ] && NAME=level
  D="$OUT/$N-$NAME"; rm -rf "$D"
  URL="http://localhost:8770/app/index.html?romurl=../dev/ge007.pal-final.z64&assets=../dev&args=-level_$N&env=GE_INPUTSCRIPT_MS=1&env=GE_INPUTSCRIPT=$PAD${GE_EXTRA_PARAMS:-}"
  node scripts/web/probe.mjs "$URL" "$SECS" "$D" "$EVERY" > "$D.probe.txt" 2>&1
  C="$D/console.log"
  EX=$(grep -E 'EXCEPTION|ABORT|RuntimeError|CRASHED' "$C" 2>/dev/null | grep -vc 'Cannot mix BigInt'); ST=$(grep -c 'STALL' "$C" 2>/dev/null)
  ANS=$(grep -q 'main thread answers' "$D.probe.txt" && echo yes || echo NO)
  PAC=$(grep 'pacing:' "$C" | tail -1 | sed 's/^[0-9.]* //' | cut -c1-110)
  ERR=$(grep -E 'EXCEPTION|ABORT|RuntimeError|CRASHED|STALL|refused|\[ERROR' "$C" | grep -vE 'could not find function|Cannot mix BigInt' | head -1 | cut -c1-140)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$N" "$NAME" "$EX" "$ST" "$ANS" "$PAC" "$ERR" | tee -a "$SUM"
done
