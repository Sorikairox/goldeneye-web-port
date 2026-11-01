#!/bin/bash
# G8: the touch controls on an emulated phone (probe.mjs PROBE_MOBILE: mobile metrics at DPR 3, touch emulation, the
# phone's user agent; the first load already sees a phone, so the controls come up by themselves: no ?touch=1).
# Every game input is a real CDP touch event on the on-screen controls, located by element id at that moment.
#   scripts/web/phone_test.sh OUTDIR ios|android W,H [TURN_W,TURN_H]      (GE_PORT: dev server port, default 8770)
# Flow: warp to Dam (-level_33); a_intro; B skips the intro; b_start; walk forward with the stick and fire Z at the
# same time (two fingers) -> c_walkfire; two more Z taps -> d_fired (the HUD ammo count goes down). With TURN: turn the
# phone mid-game -> e_turned, walk + fire -> f_turned_walk; open the page menu (☰) -> g_menu, turn back with the menu
# open -> h_menu_turned, close it -> i_closed (controls complete and in place), walk + fire -> j_walk.
# "step-eval" lines: GETouch.state() (layer, pad bits / stick) and the canvas box; GE_INPUTLOG lines: controller 0 as the
# game reads it. Muted headless Chrome, one at a time (probe.mjs takes /tmp/goldeneye-chrome.lock).
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)" || exit 1
OUT=${1:-port_work/phone}; UA=${2:-ios}; WH=${3:-390,844}; TURN=${4:-}
URL="http://localhost:${GE_PORT:-8770}/app/index.html?romurl=../dev/ge007.pal-final.z64&assets=../dev&args=-level_33&env=GE_INPUTLOG=1${GE_EXTRA_PARAMS:-}"
MARK="JSON.stringify(Object.assign(GETouch.state(), { canvas: (r => [r.x, r.y, r.width, r.height].map(Math.round))(document.getElementById('canvas').getBoundingClientRect()), body: document.body.className }))"
S=(); t=12
at() { t=$(awk "BEGIN{print $t + $1}"); }
st() { S+=("$t:$1"); }
shot() { st "shot:$1"; st "eval:$MARK"; at 0.6; }
tap() { st "down:#$1,${2:-1}"; at 0.15; st "up:#$1,${2:-1}"; at "${3:-0.8}"; }
walkfire() {   # stick forward 2.4 s; Z held for 0.3 s while walking (second finger)
  st "down:#tk-base,2"; at 0.1; st "move:#tk-base,2,0,-25"; at 0.1; st "move:#tk-base,2,0,-55"; at 0.9
  st "down:#tk-Z,3"; at 0.3; st "eval:$MARK"; st "up:#tk-Z,3"; at 0.9
  shot "$1"; st "up:#tk-base,2"; at 0.6
}
shot a_intro; at 1.4
tap tk-B 1 5            # B skips the intro camera
shot b_start
walkfire c_walkfire
tap tk-Z 1 0.6; tap tk-Z 1 0.8; shot d_fired
if [ -n "$TURN" ]; then
  st "orient:$TURN"; at 2.5; shot e_turned
  walkfire f_turned_walk
  tap tk-menu 1 1.5; shot g_menu
  st "orient:$WH"; at 2.5; shot h_menu_turned; at 4   # a whole-page shot of the long page takes seconds: a tap sent meanwhile is lost
  tap tk-back 1 2; shot i_closed
  walkfire j_walk
  tap tk-Z 1 0.8; shot k_fired
fi
IFS=';'; STEPS="${S[*]}"; unset IFS
PROBE_MOBILE=$UA PROBE_VIEWPORT=$WH PROBE_STEPS="$STEPS" node scripts/web/probe.mjs "$URL" "$(awk "BEGIN{print int($t + 2)}")" "$OUT" 1000 >"$OUT.probe.txt" 2>&1
grep -E '^[0-9.]+ (step|EXCEPTION|\[page\] (ABORT|error))' "$OUT/console.log" | cut -c1-260
echo "controller 0 (GE_INPUTLOG, polls per button/stick value):"
grep GE_INPUTLOG "$OUT/console.log" | sed 's/.*btn=/btn=/' | sort | uniq -c | sort -rn | head -12
