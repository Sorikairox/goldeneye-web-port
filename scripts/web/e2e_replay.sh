#!/bin/bash
# G6 input recorder test (docs/web/ONLINE.md "Recorder"): record a scripted solo route from power-on as a .gerec,
# replay it headless at full speed and compare every checkpoint; then replay a copy with ONE input changed, which
# must report a first difference.
#   scripts/web/e2e_replay.sh [SECS=60] [OUT=port_work/g6/e2e]
# The route (GE_NETPAD0, retraces): intro skip -> file 1 -> select mission -> Dam -> Agent -> briefing -> Dam, walk
# and fire. Recordings go to data/g6-*.gerec (gitignored dev dir the server exposes as ../dev/).
# Env: GE_PORT (8772), REPLAY_SECS (time given to each replay, default SECS + 20).
set -euo pipefail
cd "$(dirname "$0")/../.."
SECS=${1:-60}; OUT=${2:-port_work/g6/e2e}; mkdir -p "$OUT"
RS=${REPLAY_SECS:-$((SECS + 20))}   # a fast replay runs ~60-70 retraces/s on SwiftShader (rendering bound)
PAD='1100:A;1300:A;1400:A;1600:A;1700:A;1800:SUP;1806:SNONE;1850:SUP;1856:SNONE;1900:A;2100:START;2600:CUPHOLD;2800:CUPREL,SLEFT;2850:SNONE;2900:Z;2950:Z;3000:CUPHOLD,SX30;3300:CUPREL,SNONE'
U="http://localhost:${GE_PORT:-8772}/app/index.html?romurl=../dev/ge007.pal-final.z64"
echo "e2e: recording $SECS s"
PROBE_EVALS="$((SECS - 1)):__rec.dump()" node scripts/web/probe.mjs "$U&rec=1&recscript=1&env=GE_NETPAD0=$PAD" "$SECS" "$OUT/rec" 10 | grep -E '^probe: (contact|timeout)|EXCEPTION' || true
python3 - "$OUT/rec/console.log" data/g6-rec.gerec data/g6-mut.gerec <<'EOF'
import json, sys
line = [l for l in open(sys.argv[1]) if 'probe-eval' in l][-1]
rec = json.loads(json.loads(line.split(': ', 1)[1]))
open(sys.argv[2], 'w').write(json.dumps(rec))
# one changed input: retrace 2900's (Z, fire) -> neutral, in a copy
ins = [v for v, n in rec['inputs'] for _ in range(n)]
f = 2900
print(f"e2e: recorded {rec['n']} retraces, {len(rec['checks'])} checkpoints, {len(rec['inputs'])} runs; input {f} = {ins[f - 1]:#x}")
ins[f - 1] = 0 if ins[f - 1] else 0x80000000
packed = []
for v in ins:
    if packed and packed[-1][0] == v: packed[-1][1] += 1
    else: packed.append([v, 1])
rec['inputs'] = packed
open(sys.argv[3], 'w').write(json.dumps(rec))
EOF
for k in rec mut; do
  PROBE_EVALS="$((RS - 1)):JSON.stringify(__rec.result())" node scripts/web/probe.mjs "$U&replay=../dev/g6-$k.gerec&recfast=1" "$RS" "$OUT/replay-$k" 10 | grep -E '^probe: (contact|timeout)|EXCEPTION' || true
  echo "replay $k: $(grep -E '\[rec\] (replay:|FIRST)|probe-eval' "$OUT/replay-$k/console.log" | sed 's/.*\[rec\]/[rec]/' | tr '\n' ' ')"
done
