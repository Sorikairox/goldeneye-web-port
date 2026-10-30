#!/bin/bash
# Save persistence + export / import test (G5), headless Chrome with kept profiles (probe.mjs PROBE_USERDIR).
#   scripts/web/save_test.sh [outdir=port_work/save-test]
# Needs scripts/web/serve.sh running (GE_PORT, default 8770). Three runs by the real route (power-on -> file 1 ->
# Dam Agent; scripted pad as in route_dam.sh), each ending on the watch's game-options page:
#   1. fresh profile A: in Dam, open the watch, game options, Auto-Aim ON -> OFF, close the watch: the game
#      writes its EEPROM (the folder's options); the page stores it in IndexedDB;
#   2. profile A again (page reload = new browser start): the watch must show Auto-Aim OFF; the save is
#      exported through the page (GESaves.exportBlob, the blob the Export button downloads) to export.eep;
#   3. fresh profile B (= cleared storage): import a 100-byte file and a 2048-byte file of 0xFF through the
#      real file input (both must be refused), then export.eep (accepted, the page reloads); the watch must
#      show Auto-Aim OFF again.
# Checks the logs and prints PASS/FAIL per step; the screenshots to look at: <out>/run{1,2,3}/shot-*.png
# (run1 ~70 s: OFF after the toggle; run2 ~66 s and run3 ~76 s: OFF read back from the save).
set -uo pipefail
cd "$(dirname "$0")/../.."
OUT=${1:-port_work/save-test}
BASE="http://localhost:${GE_PORT:-8770}/app/index.html?romurl=../dev/ge007.pal-final.z64&env=GE_INPUTSCRIPT_MS=1"
ROUTE='20000:START;24000:A;28000:A;32000:A;34000:SUP;34200:SNONE;35000:SUP;35200:SNONE;36000:A;40000:START;55000:B;60000:START;63000:R;64000:R;65000:R'
TOGGLE=';66500:DOWN;67000:DOWN;67500:DOWN;68000:A;69000:LEFT;71000:START'
rm -rf "$OUT"; mkdir -p "$OUT"
fail=0
ok() { if grep -qE "$2" "$OUT/$1/console.log"; then echo "PASS $1: $3"; else echo "FAIL $1: $3 (no /$2/)"; fail=1; fi; }
probe() {  # probe <run> <profile> <secs> <script> [evals]
  PROBE_USERDIR="$OUT/$2" PROBE_SCALE=1 PROBE_EVALS="${5:-}" node scripts/web/probe.mjs "$BASE&env=GE_INPUTSCRIPT=$4" "$3" "$OUT/$1" 2 |
    grep -E '^probe: (main|timeout)' || true
}

probe run1 profA 76 "$ROUTE$TOGGLE"
ok run1 'restored from IndexedDB: no EEPROM' 'fresh profile starts without a save'
ok run1 '^7[0-9]\.[0-9]+ \[save\] stored ge007\.eep 2048' 'watch closed -> EEPROM stored in IndexedDB'

EXPORT='10:(async()=>{const b=await GESaves.exportBlob();const u=new Uint8Array(await b.arrayBuffer());let s="";for(const x of u)s+=String.fromCharCode(x);return "EXPORT "+btoa(s);})()'
probe run2 profA 68 "$ROUTE" "$EXPORT"
ok run2 '\[save\] restored from .*EEPROM 2048 bytes' 'reload restores the EEPROM before main()'
ok run2 'eeprom: loaded data/ge007.eep' 'the game reads the restored EEPROM'
sed -n 's/.*probe-eval 10: "EXPORT \([A-Za-z0-9+\/=]*\)".*/\1/p' "$OUT/run2/console.log" | base64 -d > "$OUT/export.eep" 2>/dev/null
SZ=$(wc -c < "$OUT/export.eep" | tr -d ' ')
[ "$SZ" = 2048 ] && echo "PASS run2: exported $OUT/export.eep (2048 bytes)" || { echo "FAIL run2: export is $SZ bytes"; fail=1; }

B64=$(base64 < "$OUT/export.eep" | tr -d '\n')
imp() { echo "(async()=>{const i=document.getElementById('save-import');const dt=new DataTransfer();dt.items.add(new File([$1],'$2'));i.files=dt.files;i.dispatchEvent(new Event('change'));await new Promise(r=>setTimeout(r,500));return document.getElementById('save-status').textContent;})()"; }
EV="6:$(imp 'new Uint8Array(100)' short.eep)|7:$(imp 'new Uint8Array(2048).fill(255)' ff.eep)|8:$(imp "Uint8Array.from(atob('$B64'),c=>c.charCodeAt(0))" export.eep)"
probe run3 profB 78 "$ROUTE" "$EV"
ok run3 'import of short.eep refused: not a GoldenEye save: 100 bytes' 'wrong-size file refused'
ok run3 'import of ff.eep refused: not a GoldenEye save: no part' '2048 bytes of garbage refused'
ok run3 'imported export.eep \(2048 bytes\)' 'exported save imported'
ok run3 '\[save\] restored from IndexedDB: EEPROM 2048 bytes' 'after the reload the imported save is restored'
# 4. online seam: profile A with GESaves.sandbox(null) set before the game starts (as an online session will,
#    with the host's image): the game boots with a blank EEPROM (watch: Auto-Aim ON), writes it at boot, and
#    nothing reaches IndexedDB (the stored save keeps export.eep's bytes)
SB='const t=setInterval(()=>{if(window.GESaves){GESaves.sandbox(null);clearInterval(t);}},0);'
IDBSUM='20:(async()=>{const db=await new Promise(r=>{const q=indexedDB.open("ge007-web",1);q.onsuccess=()=>r(q.result);});const v=await new Promise(r=>{const q=db.transaction("files").objectStore("files").get("save-167c3c433dec1f1eb921736f7d53fac8cb45ee31/ge007.eep");q.onsuccess=()=>r(q.result);});db.close();const h=new Uint8Array(await crypto.subtle.digest("SHA-1",v));return "IDBSUM "+[...h].map(x=>x.toString(16).padStart(2,"0")).join("")+" "+JSON.stringify(GESaves.state);})()'
PROBE_PRELOAD="$SB" probe run4 profA 68 "$ROUTE" "$IDBSUM"
ok run4 'sandbox: a blank EEPROM image' 'sandbox boots with the given (blank) image'
if grep -q '\[save\] stored' "$OUT/run4/console.log"; then echo "FAIL run4: something was stored in sandbox mode"; fail=1; else echo "PASS run4: nothing stored in sandbox mode"; fi
WANT=$(shasum "$OUT/export.eep" | cut -c1-40)
ok run4 "IDBSUM $WANT" 'the stored save is unchanged after a sandboxed session'
echo "look at: $OUT/run1/shot-70s.png (OFF after the toggle), $OUT/run2/shot-66s.png and $OUT/run3/shot-76s.png (OFF from the save), $OUT/run4/shot-66s.png (ON: blank sandbox image)"
exit $fail
