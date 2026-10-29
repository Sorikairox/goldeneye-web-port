#!/bin/bash
# Source me: common environment for the GoldenEye web build (docs/web/HANDOFF.md).
#   . scripts/web/env.sh        -> $GE_ROOT, $GE_DEV (container name), ge_exec CMD...
# The toolchain lives in Docker only (image goldeneye-emsdk, container goldeneye-dev with the repo
# mounted at /src). Nothing is installed on the host.
export PATH=/Applications/Docker.app/Contents/Resources/bin:$PATH
GE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
GE_DEV=${GE_DEV:-goldeneye-dev}
# The CMake build tree lives in a Docker named volume mounted at /build (one per dev container), not on the
# bind-mounted repo: heavy build I/O on the macOS virtiofs share stalled Docker Desktop's file-event channel
# and crashed it ("service fs failed: injecting event blocked for 60s", 2026-10-29). build.sh copies only
# the outputs (ge007.js/.wasm, logs) back into build-web/.
GE_BUILDVOL=${GE_BUILDVOL:-$GE_DEV-build}
# CPU cap for the dev container (and ninja -j in build.sh): a full build on all VM CPUs coincided with the
# same Docker Desktop crash; keep part of the VM free.
GE_CPUS=${GE_CPUS:-4}
export GE_ROOT GE_DEV GE_BUILDVOL GE_CPUS
ge_ensure() {
  if ! docker image inspect goldeneye-emsdk >/dev/null 2>&1; then
    docker build -q -t goldeneye-emsdk -f "$GE_ROOT/scripts/web/Dockerfile.emsdk" "$GE_ROOT/scripts/web" >/dev/null
  fi
  # (re)create the container when it is not running or predates the /build volume / CPU cap
  if [ "$(docker inspect -f '{{.State.Running}}' "$GE_DEV" 2>/dev/null)" != true ] ||
     ! docker inspect -f '{{range .Mounts}}{{.Destination}} {{end}}' "$GE_DEV" | grep -q '/build' ||
     [ "$(docker inspect -f '{{.HostConfig.NanoCpus}}' "$GE_DEV")" = 0 ]; then
    docker rm -f "$GE_DEV" >/dev/null 2>&1 || true
    docker run -d --name "$GE_DEV" --cpus="$GE_CPUS" -v "$GE_ROOT":/src -v "$GE_BUILDVOL":/build -w /src goldeneye-emsdk sleep infinity >/dev/null
  fi
}
ge_exec() { ge_ensure; docker exec -w /src "$GE_DEV" "$@"; }
