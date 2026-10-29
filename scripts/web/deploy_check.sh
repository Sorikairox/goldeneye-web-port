#!/bin/bash
# Pre-deploy gate for build-web/site (run it from any future deploy script; it must exit 0):
#  1. the site is a stripped build: no DEV-UNSTRIPPED marker, ge007.romfill present (template + fill);
#  2. romscan: no ROM-derived byte runs >= 64 bytes outside scripts/web/romscan.allow;
#  3. no dev-only files (ROM images, sidecars) in the site.
#   scripts/web/deploy_check.sh [site=build-web/site]
set -euo pipefail
cd "$(dirname "$0")/../.."
SITE=${1:-build-web/site}
fail() { echo "deploy_check: REFUSED: $*"; exit 1; }
[ -f "$SITE/DEV-UNSTRIPPED" ] && fail "$SITE is an unstripped dev build (GE_NOSTRIP=1)"
[ -f "$SITE/ge007.romfill" ] || fail "$SITE/ge007.romfill missing (not a template + fill build)"
[ -f "$SITE/ge007.wasm" ] || fail "$SITE/ge007.wasm missing"
if find "$SITE" -type f \( -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -name 'pcmodels*' -o -name 'pccg*' -o -name '*.bin' \) | grep -q .; then
  fail "ROM / sidecar files in $SITE"
fi
bash scripts/web/romscan.sh "$SITE" 64 || fail "romscan found ROM-derived data"
echo "deploy_check: OK"
