#!/bin/bash
# Serve the web build locally (127.0.0.1 only; never expose this server: it serves your ROM copy).
#   scripts/web/serve.sh [port]     (default 8770) -> prints the dev URL; `serve.sh stop` stops it.
# Container goldeneye-web-serve mounts read-only: build-web/site -> /app, data/ (the gitignored dev ROM
# copy + dev sidecars) -> /dev. The page fetches them with ?romurl=../dev/ge007.pal-final.z64&assets=../dev
set -euo pipefail
. "$(dirname "$0")/env.sh"
NAME=${GE_SERVE:-goldeneye-web-serve}   # G4 uses GE_SERVE=goldeneye-g4-serve + port 8771
if [ "${1:-}" = stop ]; then docker rm -f $NAME >/dev/null 2>&1 || true; exit 0; fi
PORT=${1:-8770}
if [ "$(docker inspect -f '{{.State.Running}}' $NAME 2>/dev/null)" != true ]; then
  docker rm -f $NAME >/dev/null 2>&1 || true
  docker run -d --name $NAME -p 127.0.0.1:$PORT:8000 \
    -v "$GE_ROOT/build-web/site":/www/app:ro -v "$GE_ROOT/data":/www/dev:ro -w /www \
    goldeneye-emsdk python3 -m http.server 8000 --bind 0.0.0.0 >/dev/null
  sleep 1
fi
P=$(docker port $NAME 8000 | head -1 | sed 's/.*://')
echo "http://localhost:$P/app/index.html?romurl=../dev/ge007.pal-final.z64&assets=../dev"
