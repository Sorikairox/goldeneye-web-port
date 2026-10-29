#!/bin/bash
# Gate for the C sidecar converters (port/src/convert/geconv.c): build the native-style CLI with
# Emscripten (same zlib as the game), run it and the Python converters on the dev ROM copy
# (data/ge007.<region>.z64), and require byte-identical pcmodels/pccg images + manifests.
#   scripts/web/geconv_check.sh [pal-final]
# Holds /tmp/goldeneye-build.lock. Python output goes to data/ (gitignored), C output to the
# container's build volume.
set -euo pipefail
cd "$(dirname "$0")/../.."
. scripts/web/env.sh
R=${1:-pal-final}
ge_ensure
scripts/web/lockrun.sh /tmp/goldeneye-build.lock docker exec -w /src "$GE_DEV" bash -c "set -e
  mkdir -p /build/geconv && cd /build/geconv
  emcc -O2 -sUSE_ZLIB=1 -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -DGECONV_HOST -I/src/port/src/convert \
    /src/tools_web/geconv_cli.c /src/port/src/convert/geconv.c -o geconv_cli.js
  cd /src
  python3 tools_pc/d43_emit.py $R | tail -1
  python3 tools_pc/d69_emit.py $R | tail -1
  python3 tools_pc/d88_emit.py $R --regen | tail -1
  rm -rf /build/geconv/out
  node /build/geconv/geconv_cli.js $R /build/geconv/out
  bad=0
  for f in pcmodels-$R/pcmodels.bin pcmodels-$R/manifest.csv pccg-$R/pccg.bin pccg-$R/manifest.csv; do
    if cmp -s data/\$f /build/geconv/out/\$f; then echo \"identical: \$f (\$(stat -c%s data/\$f) bytes, sha1 \$(sha1sum data/\$f | cut -c1-12))\"
    else echo \"DIFFERENT: \$f\"; bad=1; fi
  done
  [ \$bad = 0 ] && echo 'geconv_check: PASS (C == Python)' || { echo 'geconv_check: FAIL'; exit 1; }"
