# GoldenEye 007 (EU) web port: hand-off

Plan and decisions: `docs/web/PLAN.md`. Rules: `orchestrator.md`. Playbook: `WEB_PORT_ORCHESTRATOR.md`.
Session log: `docs/web/sessions.tsv`.

## Current state (after G1, 2026-10-29)

- The native PC port builds for the browser (EU, pal-final) with Emscripten in Docker and **boots to its
  first frames in headless Chrome: the 3D Nintendo logo, then the Rareware logo** (screenshots in the
  gitignored `port_work/g1/`: `nintendo-logo.png`, `rareware-logo.png`).
- ROM supplied through the page: file picker (EU SHA-1 check, IndexedDB) or the dev URL `?romurl=`.
- Sidecars: **dev-generated** by the Python converters from the EU ROM alone (`data/pcmodels-pal-final/`,
  `data/pccg-pal-final/`, gitignored), fetched by the page from the local server with `?assets=`
  (temporary, PLAN 3.2).
- **Open bug (next session): ~17 s in, right after the Rareware logo, the page's main thread stops
  answering** (no more frames, no logs, no STALL report, `Debugger.pause` does not take effect,
  screenshots keep the last frame). Last log lines are ordinary fiber switches (sched thread 2 /
  audio thread 4 / main thread 3). Suspects, in order: (1) a busy-wait in a fiber on a flag/variable
  another thread sets, with no OS call inside (cooperative scheduling hangs; on native the other thread
  runs) — e.g. audio manager vs game thread, or a loop around `osGetTime`/`osGetCount`-free polling;
  (2) a non-interruptible bulk memory op with a wrapped huge length (MEMORY64=2 lowering wraps lengths
  too); (3) something in the Rare→intro transition (stage load, decompression, framebuffer copy).
  Tools: `PROBE_PROFILE=12 node scripts/web/probe.mjs ...` (CPU profile from 12 s), a Release build
  (smaller, debugger attaches faster), temporary `sysLogPrintf` in `webkernel.c` `wkRunReady` (log
  switches after frame ~850; the log showed the last switches, see G1 notes below).

## How to build, run, test

All tools run in Docker (`export PATH=/Applications/Docker.app/Contents/Resources/bin:$PATH`, done by
`scripts/web/env.sh`). Image `goldeneye-emsdk` (built from `scripts/web/Dockerfile.emsdk`), dev container
`goldeneye-dev` (repo at `/src`), server container `goldeneye-web-serve`.

```sh
# one-time: dev ROM copy + EU sidecars (gitignored data/)
mkdir -p data && cp <roms>/GoldenEye_007_Europe.z64 data/ge007.pal-final.z64
. scripts/web/env.sh && ge_exec sh -c 'python3 tools_pc/d43_emit.py pal-final && python3 tools_pc/d69_emit.py pal-final && python3 tools_pc/d88_emit.py pal-final --regen'

scripts/web/build.sh            # -> build-web/site/{index.html,main.js,ge007.js,ge007.wasm}
                                #    prints errors, "signature mismatches: 0 ..., bitcast_invalid traps: 0"
scripts/web/serve.sh            # 127.0.0.1:8770 only; prints the dev URL
node scripts/web/probe.mjs 'http://localhost:8770/app/index.html?romurl=../dev/ge007.pal-final.z64&assets=../dev' 20 port_work/shots 4
                                # headless Chrome (host), muted, own profile; PNGs + console.log in port_work/shots
```

Release build: `ge_exec bash -c "emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && ninja -C build-web"`
(2.7 MB wasm vs 12 MB with DWARF), then copy `ge007.{js,wasm}` into `build-web/site/`.
`build.sh` re-runs CMake every time (new files are globbed).

After adding/changing code, if the build reports signature mismatches:
`ge_exec python3 scripts/web/sigadapt.py scan build-web/build.log build-web`, review the new rows in
`scripts/web/sigadapt.tsv` (note column), rebuild.

## Where things are

| What | Where |
|---|---|
| CMake web branch | `CMakeLists.txt` (`if(EMSCRIPTEN)` blocks: flags, memory map link options, srcfix copies, sigadapt, `__x86_64__` for game TUs) |
| fiber kernel | `port/src/web/webkernel.{c,h}`; hooks in `port/src/libultra.c` (`GE_WEB` blocks), main loop in `port/src/main.c` |
| DRAM single view | `port/src/dram.c` (`GE_WEB`), `port/src/web/webdram.{h,c}` |
| cart window | `scripts/web/gen_cart_wasm.py` → `build-web/romassets_e_wasm.s`; check in `port/src/romdata.c` |
| game-file build copies | `scripts/web/web_srcfix.py` → `build-web/srcfix/*.c` |
| call adapters | `scripts/web/sigadapt.py`, `scripts/web/sigadapt.tsv` → `build-web/sigadapt.c` |
| page | `port/web/index.html`, `port/web/main.js` |
| stubs | `port/src/web/webstubs.c` (crash handler, update check) |

## Traps found in G1
- clang crashes on `.set sym, CONST` in wasm assembly and wasm-ld has no `--defsym`: no absolute
  symbols in wasm. Labels in a section linked first at `GLOBAL_BASE` give exact addresses instead;
  wasm-ld drops unreferenced custom sections (`--gc-sections`), keep pad + labels in one section.
- `-sMEMORY64=2` JS glue indexes `HEAPU64[p>>3]`: any pointer >= 2 GiB that JS touches (stack, heap,
  strings) breaks. Keep stack/heap/libc data below 2 GiB; the DRAM arena above is fine as long as JS
  never reads it directly (copy first, e.g. audio buffers).
- clang: `extern T A; #pragma weak A = B` makes later uses of A "ambiguous" (GCC accepts).
- CMake drops include dirs that equal an implicit dir (libc++'s), even via genex; use
  `CMAKE_INCLUDE_FLAG_CXX -idirafter` instead of fighting the order.
- wasm traps on every call-signature mismatch; wasm-ld only *warns* at link time (and makes the call
  `unreachable`), clang makes in-TU mismatches `<fn>_bitcast_invalid`. The build gate counts both.
- Upstream's `__libm_qnan_f` stub is a function while libm declares it as data (on native cosf(NaN)
  returns code bytes as a float); the web build defines the qNaN value.
- Upstream gates many game-side 64-bit fixes on `__x86_64__`; aarch64 or wasm64 builds silently lose
  them unless the macro is provided (the web build defines it for game TUs).
- headless Chrome screenshots keep working while the page's main thread is stuck (the compositor
  serves the last frame): check `Runtime.evaluate` answers before trusting "it runs".

## Next session (G2) — recommended
1. Fix the post-Rareware-logo hang (above) and get to the intro / title / main menu, then Dam.
2. Verify 50 Hz pacing in real time (frame counter vs wall clock), input in the page, then commit.
3. Optional reference: a Linux amd64 container building the native PAL port (`goldeneye-linux`,
   headless, Xvfb + `GE_PCDUMP`) to compare frames with the web build (not built in G1).
Then W2 (C emitters for the sidecars) per PLAN.
