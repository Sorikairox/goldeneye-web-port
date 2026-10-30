#!/bin/bash
# Deploy the tested web build to the public site https://goldeneye-web-port.netlify.app (Netlify project
# goldeneye-web-port, created 2026-10-30 with the user's go-ahead). Ships build-web/site as built, plus the
# room-code function (port/netlify/functions/room, Netlify Blobs store goldeneye-rooms) at /api/room.
#   scripts/web/build.sh && scripts/web/deploy_web.sh
# Hard gate first: scripts/web/deploy_check.sh refuses an unstripped site, a site without the fill table, any
# ROM/sidecar file, or ROM byte runs (romscan). Nothing ROM-derived may ever be published.
set -euo pipefail
cd "$(dirname "$0")/../.."
R=$PWD
SITE_ID=d7b40659-2991-4be9-99fa-a32d7478abfe
B=build-web/site
[ -f $B/ge007.wasm ] || { echo "no build at $B: run scripts/web/build.sh"; exit 1; }
for f in port/web/*; do cmp -s "$f" "$B/$(basename "$f")" || { echo "$B differs from port/web ($f): rebuild first"; exit 1; }; done
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "uncommitted changes: commit first so BUILD.txt names the deployed code"; exit 1; }
scripts/web/deploy_check.sh
# publish only what deploy_check passed (a fresh copy, so stale files never linger)
S=~/goldeneye-site/publish; rm -rf "$S"; mkdir -p "$S"
cp $B/* "$S"/
git log -1 --format='%h %ad %s' --date=format:'%Y-%m-%d %H:%M' > "$S/BUILD.txt"
# the function is bundled from a directory outside the published one (node_modules must never be published), where
# npm installs @netlify/blobs from port/netlify/package.json (Conker trap: the bundler only takes packages from
# under the directory it runs in)
F=~/goldeneye-site/fn; mkdir -p $F/functions
rm -rf $F/functions/room; cp -R port/netlify/functions/room $F/functions/
if ! cmp -s port/netlify/package.json $F/package.json || [ ! -d $F/node_modules/@netlify/blobs ]; then
  cp port/netlify/package.json $F/ && (cd $F && npm install --no-audit --no-fund --silent)
fi
cd $F && npx -y netlify-cli@latest deploy --prod --site $SITE_ID --dir "$S" --functions functions --no-build 2>&1 |
  grep -E "Deploy is live|Error|error|function" || true
cd "$R"
echo "live: $(curl -s https://goldeneye-web-port.netlify.app/BUILD.txt)"
# the room server answers (a bad request gives a JSON error; nothing is stored)
curl -s -X POST -H 'content-type: application/json' -d '{"op":"host","code":"QQQQQ","secret":"x"}' https://goldeneye-web-port.netlify.app/api/room; echo
