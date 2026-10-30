#!/bin/bash
# G7 dark-surface repro: G6's scripted 4-player Temple match (real menu route, scripts/web/netmp_pads.sh) in det mode,
# one page, screenshots every EVERY s. Det mode gives the same frames every run, so render-only switches can be A/B'd
# at the same retrace.
#   scripts/web/darkdet.sh OUT [SECS=110] ['&env=EXTRA=1...']
# Env: GE_PORT (8774), EVERY (5). Needs scripts/web/serve.sh running.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$1; SECS=${2:-110}; EXTRA=${3:-}
. scripts/web/netmp_pads.sh
URL="http://localhost:${GE_PORT:-8774}/app/index.html?romurl=../dev/ge007.pal-final.z64&netdet=1&netmask=F&nethashevery=50&env=GE_NETHASHLOG=1"
URL="$URL&env=GE_NETPAD0=$PAD0&env=GE_NETPAD1=$PAD1&env=GE_NETPAD2=$PAD2&env=GE_NETPAD3=$PAD3$EXTRA"
mkdir -p "$OUT"
node scripts/web/probe.mjs "$URL" "$SECS" "$OUT" "${EVERY:-5}" > "$OUT.probe.txt" 2>&1 || true
grep -o '\[nethash\] [0-9]* [0-9a-f]* [0-9a-f]*' "$OUT/console.log" | awk '{print $2, $3, $4}' > "$OUT/hashes.txt" || true
echo "$OUT: $(wc -l < "$OUT/hashes.txt") hashes, last $(tail -1 "$OUT/hashes.txt")"
