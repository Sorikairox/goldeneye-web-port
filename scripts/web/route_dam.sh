#!/bin/bash
# Real-route test: power-on -> logos -> intro -> title -> file select -> mission select (Dam) ->
# difficulty Agent -> briefing -> START -> Dam gameplay, with the port's scripted pad
# (GE_INPUTSCRIPT, port/src/input.c; GE_INPUTSCRIPT_MS=1: entry times are ms since SDL init).
# Headless Chrome, muted; screenshots + contact sheet in port_work/route-dam/ (gitignored: game imagery).
#   scripts/web/route_dam.sh [seconds=75] [outdir=port_work/route-dam]
# Needs scripts/web/serve.sh running (GE_PORT, default 8770). Sidecars: converted in the page from the
# ROM (cached in IndexedDB, the real path); ROUTE_DEV_ASSETS=1 loads the Python dev sidecars instead. Look at sheet.png: the last rows must show Dam gameplay
# (PP7 in hand, ammo counter), and console.log must have no EXCEPTION / STALL and 'pacing:' lines
# with retraces 50.00/s.
set -euo pipefail
cd "$(dirname "$0")/../.."
SECS=${1:-75}; OUT=${2:-port_work/route-dam}
# times (ms since SDL init): START skips the gun-barrel to the title, A = title -> file select,
# A = file 1 (new), A = "select mission", A = Dam, stick up x2 = Agent, A, START at the briefing; in Dam: walk forward
# 4 s (C-up held: in the default control style the stick's Y axis looks up/down), fire, turn left 2 s (input check)
PAD='20000:START;24000:A;28000:A;32000:A;34000:SUP;34200:SNONE;35000:SUP;35200:SNONE;36000:A;40000:START;60000:CUPHOLD;64000:CNONE;65000:Z;66000:SLEFT;68000:SNONE'
ASSETS=""; [ "${ROUTE_DEV_ASSETS:-0}" = 1 ] && ASSETS="&assets=../dev"
URL="http://localhost:${GE_PORT:-8770}/app/index.html?romurl=../dev/ge007.pal-final.z64$ASSETS&env=GE_INPUTSCRIPT_MS=1&env=GE_INPUTSCRIPT=$PAD${GE_EXTRA_PARAMS:-}"
node scripts/web/probe.mjs "$URL" "$SECS" "$OUT" 3 | grep -E '^probe: (main|contact|timeout)|EXCEPTION|STALL' || true
grep -E 'pacing:|EXCEPTION|STALL|refused' "$OUT/console.log" | tail -12 || true
