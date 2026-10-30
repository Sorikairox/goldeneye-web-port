#!/bin/bash
# G7: native Linux reference build + run on this Mac without emulation (arm64 container, image goldeneye-linux from
# scripts/web/Dockerfile.linux built for the host arch). Game TUs get __x86_64__ (CMake GE_LP64_GAME_DEFINE) like the
# web build. Rendering: Mesa llvmpipe under Xvfb. Frames: GE_PCDUMP PPMs.
#   scripts/web/native_ref.sh build
#   scripts/web/native_ref.sh run OUTDIR SECS 'ENV=VAL ...' [game args...]   (OUTDIR under the repo; PPMs land in OUTDIR/ppm)
# Needs data/ge007.pal-final.z64 and the Python sidecars in data/ (docs/web/HANDOFF.md, "one-time").
# Env: GE_NATIVE (container, default goldeneye-g7-native), GE_CPUS (4). The build takes /tmp/goldeneye-build.lock.
set -euo pipefail
cd "$(dirname "$0")/../.."
export PATH=/Applications/Docker.app/Contents/Resources/bin:$PATH
ROOT=$(pwd)
[ -f "$ROOT/CMakeLists.txt" ] && [ -d "$ROOT/scripts/web" ] || { echo "not the repo root" >&2; exit 1; }
C=${GE_NATIVE:-goldeneye-g7-native}
if [ "$(docker inspect -f '{{.State.Running}}' "$C" 2>/dev/null)" != true ]; then
  docker rm -f "$C" >/dev/null 2>&1 || true
  docker run -d --name "$C" --cpus="${GE_CPUS:-4}" -v "$ROOT":/src -v "$C-build":/build -w /src goldeneye-linux sleep infinity >/dev/null
fi
case "${1:-}" in
  build)
    scripts/web/lockrun.sh /tmp/goldeneye-build.lock docker exec -w /src "$C" sh -c \
      "cmake -S . -B /build/native -G Ninja -DROMID=pal-final -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGE_LP64_GAME_DEFINE=ON > /build/cmake.log 2>&1 && ninja -C /build/native -j${GE_CPUS:-4} > /build/build.log 2>&1; rc=\$?; tail -3 /build/build.log; ls -la /build/native/ge007* 2>/dev/null; exit \$rc"
    ;;
  run)
    OUT=$2; SECS=$3; ENVS=$4; shift 4
    mkdir -p "$OUT/ppm"
    BIN=$(docker exec "$C" sh -c 'ls /build/native/ge007.* | head -1')
    # GE_PCDUMP writes ./ppm/frame_N.ppm (cwd = repo, which also holds data/): moved into OUT afterwards
    docker exec -w /src "$C" sh -c "rm -rf /src/ppm; env $ENVS LIBGL_ALWAYS_SOFTWARE=1 timeout ${SECS}s xvfb-run -a -s '-screen 0 1280x960x24' $BIN $* > /src/$OUT/native.log 2>&1; echo exit \$?; mv /src/ppm/* /src/$OUT/ppm/ 2>/dev/null; rmdir /src/ppm 2>/dev/null; true"
    python3 scripts/web/ppm2png.py "$OUT/ppm"
    ;;
  *) sed -n 2,8p "$0"; exit 1 ;;
esac
