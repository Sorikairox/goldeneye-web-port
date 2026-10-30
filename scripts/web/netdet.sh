#!/bin/bash
# G6 determinism proof (docs/web/ONLINE.md): the same scripted session run twice in det mode must give the same
# state hash at every retrace; a run with one changed input must diverge.
#   scripts/web/netdet.sh OUT SECS 'PADS_A' ['PADS_B']
# PADS = '&env=GE_NETPAD0=...&env=GE_NETPAD1=...' (webnet.c syntax: <retrace>:<tok>,...;...). Run A is played twice
# (OUT/a1, OUT/a2); with PADS_B, a third run OUT/b plays those pads instead (the divergence check).
# Env: NETMASK (default F), HASHEVERY (default 1 = every retrace), GE_PORT (8772), EVERY (screenshot period, 10 s),
#      NETDET_EXTRA (more URL parameters for every run).
# Prints the number of retraces compared, identical, and the first difference. Needs scripts/web/serve.sh running.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$1; SECS=$2; PA=$3; PB=${4:-}
BASE="http://localhost:${GE_PORT:-8772}/app/index.html?romurl=../dev/ge007.pal-final.z64&netdet=1&netmask=${NETMASK:-F}&nethashevery=${HASHEVERY:-1}&env=GE_NETHASHLOG=1${NETDET_EXTRA:-}"
run() {   # name pads
  mkdir -p "$OUT/$1"
  node scripts/web/probe.mjs "$BASE$2" "$SECS" "$OUT/$1" "${EVERY:-10}" | grep -E '^probe: (contact|timeout)|EXCEPTION|STALL' || true
  grep -o '\[nethash\] [0-9]* [0-9a-f]* [0-9a-f]*' "$OUT/$1/console.log" | awk '{print $2, $3, $4}' > "$OUT/$1/hashes.txt" || true
  echo "$1: $(wc -l < "$OUT/$1/hashes.txt") hashed retraces, last $(tail -1 "$OUT/$1/hashes.txt")"
}
cmp_runs() {   # a b
  python3 - "$OUT/$1/hashes.txt" "$OUT/$2/hashes.txt" "$1" "$2" <<'EOF'
import sys
a = dict((l.split()[0], l.split()[1:]) for l in open(sys.argv[1]) if l.strip())
b = dict((l.split()[0], l.split()[1:]) for l in open(sys.argv[2]) if l.strip())
common = sorted(set(a) & set(b), key=int)
same = [k for k in common if a[k] == b[k]]
diff = [k for k in common if a[k] != b[k]]
hs = [k for k in common if a[k][0] == b[k][0]]
hd = [k for k in common if a[k][0] != b[k][0]]
print(f"{sys.argv[3]} vs {sys.argv[4]}: {len(common)} retraces compared, {len(same)} identical (H and S) = "
      f"{100.0 * len(same) / max(1, len(common)):.2f} %, H identical {len(hs)}"
      + (f"; first difference at retrace {diff[0]}: {a[diff[0]]} vs {b[diff[0]]}" if diff else "")
      + (f"; first H difference at retrace {hd[0]}" if hd else ""))
EOF
}
run a1 "$PA"
run a2 "$PA"
cmp_runs a1 a2
if [ -n "$PB" ]; then run b "$PB"; cmp_runs a1 b; fi
