#!/bin/bash
# G9 offline play test, on an emulated phone (probe.mjs PROBE_MOBILE), one muted headless Chrome with a fresh profile:
#   1. first visit online: the service worker installs and precaches the site; the ROM is chosen with the file picker
#      (CDP DOM.setFileInputFiles: the dev ROM copy goes into this test profile's IndexedDB, as a player's would) and the
#      game boots (sidecars converted, cached);
#   2. network off (CDP Network.emulateNetworkConditions offline, on the page and on every service worker; locally the
#      dev server is also stopped), reload: the page, the ROM picker, the stored ROM, the game and the touch controls
#      must come up from the browser's storage alone; the Online panel must say it needs a connection.
#   scripts/web/offline_test.sh OUTDIR [SITE_URL]   (default: a local host server for build-web/site, started and
#   stopped here). SITE_URL e.g. https://goldeneye-web-port.netlify.app/ for the live check.
# Read the "step" / "step-eval" lines it prints; screenshots in OUTDIR (game imagery: keep them in port_work/).
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)" || exit 1
OUT=${1:-port_work/offline}; URL=${2:-}; ROM=$PWD/data/ge007.pal-final.z64
mkdir -p "$OUT"; [ -f "$ROM" ] || { echo "no dev ROM copy at data/ge007.pal-final.z64"; exit 1; }
SRV=
if [ -z "$URL" ]; then
  P=${GE_OFFLINE_PORT:-8790}
  node scripts/web/serve_node.mjs "$P" >/dev/null 2>&1 & SRV=$!
  trap '[ -n "$SRV" ] && kill $SRV 2>/dev/null' EXIT
  sleep 1; URL="http://localhost:$P/app/index.html"
fi
T_OFF=${GE_OFFLINE_AT:-80}
SWQ="(async()=>JSON.stringify({ctl:!!navigator.serviceWorker.controller,caches:await caches.keys(),online:navigator.onLine,state:document.getElementById('opt-offline-state')?.textContent}))()"
PAGEQ="JSON.stringify({romfile:!!document.getElementById('romfile'),status:document.getElementById('status').textContent,main:!!window.geMainCalled,fromIdb:/sidecars from IndexedDB/.test(document.getElementById('log').textContent),touch:window.GETouch&&GETouch.state(),netNote:!document.getElementById('net-offline-note')?.hidden,online:navigator.onLine,ctl:!!navigator.serviceWorker.controller})"
NETQ="JSON.stringify({noteShown:!document.getElementById('net-offline-note').hidden,text:document.getElementById('net-offline-note').textContent,createDisabled:document.getElementById('net-room-create').disabled})"   # no ';' (the step separator)
S="15:eval:$SWQ;40:eval:$SWQ;$((T_OFF-4)):eval:$PAGEQ;$((T_OFF-3)):shot:a_online;$T_OFF:net:offline;$((T_OFF+2)):reload"
S="$S;$((T_OFF+8)):eval:$SWQ;$((T_OFF+30)):eval:$PAGEQ;$((T_OFF+31)):shot:b_offline;$((T_OFF+45)):eval:$PAGEQ;$((T_OFF+46)):shot:c_offline_later;$((T_OFF+48)):eval:$NETQ"
PROBE_MOBILE=${GE_OFFLINE_UA:-android} PROBE_VIEWPORT=${GE_OFFLINE_VP:-412,915} PROBE_FILE="4:#romfile:$ROM" PROBE_STEPS="$S" \
  node scripts/web/probe.mjs "$URL" $((T_OFF + 52)) "$OUT" 1000 >"$OUT.probe.txt" 2>&1 &
PR=$!
if [ -n "$SRV" ]; then sleep "$T_OFF"; kill $SRV 2>/dev/null; SRV=; echo "dev server stopped at ${T_OFF}s"; fi
wait $PR
grep -E '^[0-9.]+ (step|probe:|EXCEPTION|\[page\] (ABORT|error|rejection)|\[offline\])' "$OUT/console.log" | cut -c1-400
