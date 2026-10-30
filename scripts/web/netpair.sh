#!/bin/bash
# G6 online multiplayer test (docs/web/ONLINE.md): an online session of PLAYERS (2..4) pages in ONE muted headless
# Chrome, connected over WebRTC on this machine through the page's own code flow (scripts/web/netgroup.mjs drives
# window.__online). Every page plays its own channel's scripted pad (GE_NETPADn, &env=GE_NETSCRIPT=1); player 1 drives
# the menus into a multiplayer match through the real menu route (scripts/web/netmp_pads.sh).
#   scripts/web/netpair.sh NAME SECS
#   env: PLAYERS=2..4 (default 2)      pages host, guest (player 2), guest3, guest4
#        NETSIM_HOST / NETSIM_GUEST / NETSIM_GUEST3 / NETSIM_GUEST4   e.g. lat:40,jit:20,loss:5
#        NETFAIL=A-B   players A and B (2..4) never get a direct link (?netfailto): the host relays that pair
#        NETD=D|auto   input delay (default auto)          HASHEVERY=K (default 1: every retrace is compared)
#        LEAVE=NAME    that page leaves at the end, the others run on 15 s
#        ROOM=1        connect with a room code through a LOCAL room server (scripts/web/roomserver.mjs on 127.0.0.1:8773)
#        UI=1          through the Online panel's buttons      SHOTS="60 150" screenshots of every window
#        DESYNC_AT=N   the guest flips one byte of N64 DRAM at retrace N (desync report test)
#        HIDE=AT:FOR   the guest's page is hidden from AT s for FOR s
#        GE_PORT (8772) the page server (scripts/web/serve.sh 8772)
# Logs: port_work/g6/net/NAME/{host,guest,guest3,guest4}.log; every guest's [nethash] lines compared with the host's.
set -uo pipefail
cd "$(dirname "$0")/../.."
D=port_work/g6/net/$1; rm -rf "$D"; mkdir -p "$D"
T=${2:-120}; P=${PLAYERS:-2}
source scripts/web/netmp_pads.sh   # PAD0..PAD3 (+ MP_SETUP: the menu route for $P players)
RPID=
trap '[ -n "$RPID" ] && kill $RPID 2>/dev/null' EXIT
ROOMQ=
if [ -n "${ROOM:-}" ]; then
  node scripts/web/roomserver.mjs 8773 > "$D/roomserver.log" 2>&1 &
  RPID=$!; ROOMQ="&roomapi=http://127.0.0.1:8773/api/room"; sleep 0.5
fi
B="http://localhost:${GE_PORT:-8772}/app/index.html?romurl=../dev/ge007.pal-final.z64&online=1&nethashevery=${HASHEVERY:-1}&netd=${NETD:-auto}&env=GE_NETSCRIPT=1&env=GE_NETHASHLOG=1&env=GE_NETPAD0=$PAD0&env=GE_NETPAD1=$PAD1&env=GE_NETPAD2=$PAD2&env=GE_NETPAD3=$PAD3$ROOMQ"
FA=${NETFAIL%-*}; FB=${NETFAIL#*-}
fail() { [ -n "${NETFAIL:-}" ] && { [ "$1" = "$FA" ] && echo "&netfailto=$FB"; [ "$1" = "$FB" ] && echo "&netfailto=$FA"; }; }
URLS=("$B${NETSIM_HOST:+&netsim=$NETSIM_HOST}"
      "$B${NETSIM_GUEST:+&netsim=$NETSIM_GUEST}${DESYNC_AT:+&env=GE_NETDESYNC=$DESYNC_AT}$(fail 2)"
      "$B${NETSIM_GUEST3:+&netsim=$NETSIM_GUEST3}$(fail 3)"
      "$B${NETSIM_GUEST4:+&netsim=$NETSIM_GUEST4}$(fail 4)")
NAMES=(host guest guest3 guest4)
s=$(date +%s)
node scripts/web/netgroup.mjs "$T" "$D" "${URLS[@]:0:$P}" | tee "$D/driver.out" | grep -v '^netgroup [0-9]* s:' | tail -$((P + 6))
grep '^netgroup [0-9]* s:' "$D/driver.out" | tail -1 | cut -c1-600
echo "== $1 ($P players, ${T}s, $(( $(date +%s) - s ))s wall)"
for p in "${NAMES[@]:0:$P}"; do
  L=$D/$p.log
  grep -o '\[nethash\] [0-9]* [0-9a-f]* [0-9a-f]*' "$L" | awk '{print $2, $3, $4}' > "$D/$p.hashes"
  echo "$p: hashed retraces $(wc -l < "$D/$p.hashes"), traps $(grep -ciE 'Aborted|RuntimeError|EXCEPTION' "$L"), DESYNC $(grep -c 'DESYNC' "$L"); $(grep -E '\[net\] (session:|mesh:)' "$L" | head -2 | cut -c1-160 | tr '\n' ' ')"
done
for p in "${NAMES[@]:1:$((P - 1))}"; do
  python3 - "$D/host.hashes" "$D/$p.hashes" "$p" <<'EOF'
import sys
a = dict((l.split()[0], l.split()[1:]) for l in open(sys.argv[1]) if l.strip())
b = dict((l.split()[0], l.split()[1:]) for l in open(sys.argv[2]) if l.strip())
common = sorted(set(a) & set(b), key=int)
diff = [k for k in common if a[k] != b[k]]
print(f"-- host vs {sys.argv[3]}: {len(common)} retraces compared, {len(common) - len(diff)} identical (H and S)"
      + (f" = {100.0 * (len(common) - len(diff)) / len(common):.2f} %" if common else "")
      + (f"; first difference at retrace {diff[0]}: {a[diff[0]]} vs {b[diff[0]]}" if diff else "")
      + (f"; last common {common[-1]}" if common else ""))
EOF
done
