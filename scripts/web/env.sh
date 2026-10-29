#!/bin/bash
# Source me: common environment for the GoldenEye web build (docs/web/HANDOFF.md).
#   . scripts/web/env.sh        -> $GE_ROOT, $GE_DEV (container name), ge_exec CMD...
# The toolchain lives in Docker only (image goldeneye-emsdk, container goldeneye-dev with the repo
# mounted at /src). Nothing is installed on the host.
export PATH=/Applications/Docker.app/Contents/Resources/bin:$PATH
GE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
GE_DEV=${GE_DEV:-goldeneye-dev}
export GE_ROOT GE_DEV
ge_ensure() {
  if ! docker image inspect goldeneye-emsdk >/dev/null 2>&1; then
    docker build -q -t goldeneye-emsdk -f "$GE_ROOT/scripts/web/Dockerfile.emsdk" "$GE_ROOT/scripts/web" >/dev/null
  fi
  if [ "$(docker inspect -f '{{.State.Running}}' "$GE_DEV" 2>/dev/null)" != true ]; then
    docker rm -f "$GE_DEV" >/dev/null 2>&1 || true
    docker run -d --name "$GE_DEV" -v "$GE_ROOT":/src -w /src goldeneye-emsdk sleep infinity >/dev/null
  fi
}
ge_exec() { ge_ensure; docker exec -w /src "$GE_DEV" "$@"; }
