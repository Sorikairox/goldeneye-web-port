#!/bin/bash
# G9 offline play: update from build A to B to C in one kept Chrome profile (no ROM needed), local host server only.
#   scripts/web/offline_update_test.sh OUTDIR
# A: first visit, cache ge007-offline-<A>. Then a marker file is added to build-web/site and sw-build.js regenerated (= B).
# B visit 1: the browser finds the new sw-build.js, precaches B, the page shows "Update ready"; then a plain reload
#   (no click): the waiting B takes over before the game would start -> only cache <B> is left.
# C (marker changed again): visit, notice, the notice's "Reload" button -> only cache <C> is left.
# The marker is removed and sw-build.js regenerated at the end (build-web/site is back to the tested build).
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)" || exit 1
OUT=${1:-port_work/offline-update}; mkdir -p "$OUT"; SITE=build-web/site; P=${GE_OFFLINE_PORT:-8791}
PROF=$(mktemp -d "${TMPDIR:-/tmp}/ge-offline-prof.XXXXXX")
node scripts/web/serve_node.mjs "$P" >/dev/null 2>&1 & SRV=$!
cleanup() { kill $SRV 2>/dev/null; rm -f $SITE/zz-update-test.txt; node scripts/web/swbuild.mjs $SITE >/dev/null; rm -rf "$PROF"; }
trap cleanup EXIT
sleep 1; URL="http://localhost:$P/app/index.html"
Q="(async()=>JSON.stringify({caches:await caches.keys(),notice:!!document.getElementById('ge-update')&&!document.getElementById('ge-update').hidden,state:document.getElementById('opt-offline-state').textContent}))()"
run() { PROBE_USERDIR="$PROF" PROBE_NOSHEET=1 PROBE_STEPS="$2" node scripts/web/probe.mjs "$URL" "$3" "$OUT/$1" 1000 >"$OUT/$1.probe.txt" 2>&1
  echo "== $1"; grep -E '^[0-9.]+ (step-eval|EXCEPTION|\[offline\])' "$OUT/$1/console.log" | cut -c1-300; }
node scripts/web/swbuild.mjs $SITE
run A "12:eval:$Q" 14
echo B > $SITE/zz-update-test.txt; node scripts/web/swbuild.mjs $SITE
run B "15:eval:$Q;16:reload;30:eval:$Q" 32
echo C > $SITE/zz-update-test.txt; node scripts/web/swbuild.mjs $SITE
run C "15:eval:$Q;16:eval:document.getElementById('ge-update-go').click();30:eval:$Q" 32
