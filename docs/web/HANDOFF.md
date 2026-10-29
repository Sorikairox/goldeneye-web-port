# GoldenEye 007 (EU) web port: hand-off

Plan and decisions: `docs/web/PLAN.md`. Rules: `orchestrator.md`. Playbook: `WEB_PORT_ORCHESTRATOR.md`.
Session log: `docs/web/sessions.tsv`.

## Current state (after G3, 2026-10-30)

G3 (level sweep, page polish, split-screen; details `docs/web/LEVELS.md`):
- **All 20 solo missions and the ending (Cuba) load, render and play** by warp (`-level_XX`, Agent):
  no trap, no stall, 50.0 retraces/s and 50 game frames/s after the load, in three sweeps
  (`scripts/web/level_sweep.sh`). The start view of every level matches upstream's native Linux golden
  frames by eye (`port_work/g3/compare12b.png`, left native, right web) after the WebGL-extension fix
  below. Open: Egypt, a pillar near the camera sometimes renders almost black (not reproducible run to
  run; room pool, near clip, fog mode and GL backend ruled out, see LEVELS.md).
- **Split-screen works** (2, 3, 4 players, all 11 arenas, scripted pad per seat through the port's
  `GE_STARTMP` harness, `scripts/web/mp_sweep.sh`): no trap, 50 retraces/s. Open: in some views large
  wall areas render uniformly black or flat light grey (the 2P sky grey) in Temple / Library; same
  family as the Egypt item, unconfirmed against native.
- **Page:** unload without the MEMORY64=2 BigInt exception (config written on unload), hidden-tab pause
  (scheduler, console clock and audio stop; resume continues from the same frame), one GL log line
  instead of ~90, audio through an AudioWorklet (non-silent, 48 kHz context), Gamepad API pad works
  (fake pad in headless Chrome: SDL opens it, A/stick/trigger drive Bond). Pointer lock and real
  gamepads need a human (headless Chrome refuses pointer lock: "WrongDocumentError").
- **Fidelity fix:** WebGL2 extensions for the native renderer's features (noperspective shade for the
  sky/water horizon fades, depth clamp, mirror clamp) + highp shaders. Frigate's water and Surface's sky
  were visibly wrong before.
- **Docker:** Desktop crashed four times on 2026-10-29 (file-sharing event channel); causes and the
  rules now in the scripts: see "Traps found in G3".

## State after G2 (2026-10-29)

- **The game runs in headless Chrome from power-on into Dam by the real route**: Nintendo/Rareware
  logos, gun-barrel intro + attract demos, GoldenEye title, file select, mission select, difficulty,
  briefing, Dam intro camera, gameplay (walk, turn, fire, guards, HUD). Contact sheets in the gitignored
  `port_work/route-dam/sheet.png` (swiftshader) and `port_work/g2/route-metal/sheet.png` (Metal);
  keyboard test (warp `-level_33`, W/D keys) in `port_work/g2/keys2/sheet.png`.
- **Pacing:** retraces 50.0/s and game frames 50/s in Dam, real time (not det), both GL backends;
  ~30 retraces dropped by the catch-up cap during the mission load (expected, heavy load frames).
  The `pacing:` log line every 10 s reports it.
- **Sound:** SDL2 Web Audio (ScriptProcessor) runs and is non-silent (peak 0.74 at the intro music,
  `scripts/web/audiocheck.js`); context 48 kHz, SDL converts from 22050 Hz. SDL resumes a suspended
  context once the page has user activation (ROM picker click / key). Not listened to by ear (headless,
  muted): pitch/tempo unverified.
- **Input:** keyboard through SDL works in the page (CDP key events: W walks, D strafes); scripted pad
  `GE_INPUTSCRIPT` works (route test). Gamepad (Gamepad API) and mouse/pointer lock untested.
- Native build: compiles and links on Linux amd64 (container `goldeneye-linux`, emulated on this Mac);
  running it there crashes in `texReadBits` at boot **also for unmodified upstream `main`**, so that is
  the emulated environment, not our changes; native runtime is unverified here. All G2 code changes are
  under `GE_WEB` / `__EMSCRIPTEN__` / `if(EMSCRIPTEN)`.

### The G1 "hang" was two bugs (G2)
1. SDL2's Emscripten backend calls `emscripten_sleep(0)` in `SDL_GL_SwapWindow` (and `SDL_Delay`)
   whenever ASYNCIFY is linked. The swap runs in the scheduler fiber, so the sleep unwound the fiber as if
   it were the main loop, paused the Emscripten main loop and later rewound into the wrong context; after
   ~870 frames the main loop died. Fix: `SDL_HINT_EMSCRIPTEN_ASYNCIFY=0` (gfx_sdl2.cpp) + a
   `--wrap=emscripten_sleep` guard that refuses (and logs once) a sleep from a fiber.
2. `music.c musicTrack{1,2,3}Play`: `while (alCSPGetState(seqp));` waits for the audio thread to process
   the stop event. Cooperative fibers never run the audio thread there. Fix: `--wrap=alCSPGetState`, the
   wrapper calls `webkernelSpinCheck()` (like `osGetCount`), so the fiber parks after 64 polls.
Then a call-signature trap: the vertex allocator (`void get_ptr_allocated_block_for_vertices(int)`
called as `struct Vertex *(*)(s32)`, the register ABI carried dynAllocate's return); fixed with the
port-side `ge_web_vertex_allocator` installed via the build copy of initweaponanigroups.c.

## How to build, run, test

All tools run in Docker (`export PATH=/Applications/Docker.app/Contents/Resources/bin:$PATH`, done by
`scripts/web/env.sh`, bash only). Image `goldeneye-emsdk` (built from `scripts/web/Dockerfile.emsdk`), dev
container `goldeneye-dev` (repo at `/src`, `--cpus=4`, CMake build tree in the named volume
`goldeneye-dev-build` at `/build/web`; build.sh copies `ge007.{js,wasm}`, `cmake.log`, `build.log` to
`build-web/`), server container `goldeneye-web-serve` (or `node scripts/web/serve_node.mjs` without
Docker). Locks shared by all sessions: `/tmp/goldeneye-build.lock` (build.sh), `/tmp/goldeneye-chrome.lock`
(probe.mjs).

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

Debug / test parameters of the page (dev URL): `&env=NAME=VALUE` (repeatable) sets a C `getenv`
variable, `&args=-level_33` passes command-line args (warp to Dam). Useful variables: `GE_WKTRACE=<browser
frame>` (log every fiber switch/wait/spin/gfx task from that frame; a 256-entry ring `ge_web_wk_ring`
always), `GE_INPUTSCRIPT=...` (+ `GE_INPUTSCRIPT_MS=1`) scripted pad (syntax in `port/src/input.c`).
Probe env: `PROBE_DEBUG=1` (debugger on before navigation: a stuck main thread then gives its wasm stack
— this found bug 2), `PROBE_PROFILE=<s>`, `PROBE_KEYS="24:KeyD:1,28:KeyW:3"` (real key events),
`PROBE_EVAL="$(cat scripts/web/audiocheck.js)"`, `PROBE_ANGLE=metal` (real GPU), `PROBE_FULL=1`
(whole page instead of the canvas), `PROBE_SCALE`, `PROBE_CLICKS="20:150"` (trusted canvas click held
150 ms), `PROBE_EVALS="14:Module._webPageSetHidden(1)|17:..."` (timed evaluations, results in
console.log), `PROBE_PRELOAD="$(cat scripts/web/fakepad.js)"` (fake Gamepad API pad, drive it with
`__pad.press(0,200)` / `__pad.axis(1,-1,2000)` from PROBE_EVALS). Every run writes `console.log` (streamed, timestamps)
and `sheet.png` (contact sheet) into its outdir.

```sh
scripts/web/route_dam.sh [secs=75] [outdir=port_work/route-dam]   # real route to Dam + walk/fire; look at sheet.png
scripts/web/level_sweep.sh [33:dam ...]   # warp sweep, default 20 missions + Cuba + MP stages; port_work/sweep/summary.tsv
scripts/web/mp_sweep.sh [2,1:temple ...]  # split-screen sweep (GE_STARTMP), default 11 arenas 2P + 3P/4P
#   both: SECS, EVERY, OUT, GE_EXTRA_PARAMS='&env=GE_RSEED=0x0123456789abcdef', PROBE_ANGLE=metal,
#   SWEEP_DEV_ASSETS=1 (Python sidecars instead of the in-page conversion)
```
Fidelity check: put the 12 s sweep shots next to `tools_pc/golden/<level>/linux/` (G3 did it with a
throw-away page in `port_work/g3/`; the start view after the B intro-skip matches the native stems).
`&env=GE_SDLAUDIO=1` = SDL's ScriptProcessor instead of the AudioWorklet; `Module._webPageSetHidden(1/0)`
= hidden-tab pause test hook.
Native Linux reference build (amd64, emulated; compiles/links, does not run under emulation):
`docker build --platform linux/amd64 -t goldeneye-linux-amd64 -f scripts/web/Dockerfile.linux scripts/web`,
then in container `goldeneye-linux`: `cmake -S . -B build-linux -G Ninja -DROMID=pal-final && ninja -C build-linux`.

Release build: `ge_exec bash -c "emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && ninja -C build-web"`
(2.7 MB wasm vs 12 MB with DWARF), then copy `ge007.{js,wasm}` into `build-web/site/`.
`build.sh` re-runs CMake every time (new files are globbed).

After adding/changing code, if the build reports signature mismatches:
`ge_exec python3 scripts/web/sigadapt.py scan /build/web/build.log /build/web`, review the new rows in
`scripts/web/sigadapt.tsv` (note column), rebuild.

## ROM data from the player's ROM (G4, 2026-10-30)

Details, numbers and what is left: `docs/web/ROMDATA.md`.

- **Sidecars are made in the browser** (W2): before `main()` the page runs the C port of the Python
  converters (`port/src/convert/geconv.c`) inside the game wasm on the player's ROM (~620 steps,
  ~225 ms, progress bar), caches the four files in IndexedDB (`sidecars-v<GECONV_VERSION>-<sha1>`)
  and writes them to MEMFS `/data/`. Output byte-identical to the Python converters
  (`scripts/web/geconv_check.sh`, and `&fillcheck` hashes in the page). `?assets=` is dev-only now.
- **Template + fill** (W3): `build.sh` runs `scripts/web/romfill.sh` after the link:
  `tools_web/romfill.c` zeroes every run of the game's / assets' data that also exists in the ROM
  (raw, RZ streams, 16/32-bit swapped; display lists and mixed-width fields included) in
  `build-web/site/ge007.wasm` and writes the offsets-only table `build-web/site/ge007.romfill`; the
  page applies it with `ge_web_romfill()` before `main()` and checks two CRC32s against the
  original build. EU: 13,462 ranges, 155,056 bytes; `fillcheck` SHA-256 equal.
- **Deploy gate:** `scripts/web/deploy_check.sh` (romscan >= 64 B outside `romscan.allow`, stripped
  site, no ROM/sidecar files). romscan: 178 runs / 58,097 B before G4 -> 4 runs / 280 B after, all
  allowlisted third-party tables.
- A stripped build needs `data/ge007.pal-final.z64` at build time (else the build fails and removes
  the site wasm); `GE_NOSTRIP=1` makes an unstripped dev site marked `DEV-UNSTRIPPED`.

```sh
scripts/web/geconv_check.sh                       # C converters == Python converters (EU)
scripts/web/romscan.sh [site] [minrun=64]          # deploy gate scan (also run by deploy_check.sh)
scripts/web/deploy_check.sh                        # must exit 0 before any deploy
python3 scripts/web/fillcheck.py build-web/ge007.wasm build-web/site/ge007.romfill   # in the container;
#   compare with the page's "[page] fillcheck: ... sha256" line (dev URL parameter &fillcheck)
GE_NOSTRIP=1 scripts/web/build.sh                  # dev: unstripped site (DEV-UNSTRIPPED)
ROUTE_DEV_ASSETS=1 GE_PORT=8771 scripts/web/route_dam.sh   # route with the Python dev sidecars
```
Page dev parameters added: `&fillcheck` (hashes after the fill and of the sidecars),
`&reloadonce` (reload once after converting, to exercise the IndexedDB path in one profile).
Second session's own containers: `GE_DEV=goldeneye-g4-dev` (build volume `goldeneye-g4-dev-build`),
`GE_SERVE=goldeneye-g4-serve scripts/web/serve.sh 8771`. Source `env.sh` from bash only.

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
| page integration (C) | `port/src/web/webpage.c` (beforeunload, hidden-tab pause), `port/src/web/webaudio.c` (AudioWorklet output; routed from `port/src/audio.c`) |
| WebGL specifics | `port/fast3d/gfx_opengl.cpp` (`__EMSCRIPTEN__`: extensions, highp, GL log summary) |
| test scripts | `scripts/web/{level_sweep,mp_sweep,route_dam}.sh`, `probe.mjs`, `fakepad.js`, `audiocheck.js` |
| sidecar converters (C) | `port/src/convert/geconv.{c,h}`; page glue `port/src/web/geconvweb.c`; inputs `scripts/web/gen_geconv_inputs.py` → `build-web/geconv_inputs.c`; native-style check tool `tools_web/geconv_cli.c` |
| template + fill | build: `tools_web/romfill.c` via `scripts/web/romfill.sh`; boot: `port/src/web/romfill.c`; checks `scripts/web/fillcheck.py` |
| deploy gate | `tools_web/romscan.c`, `tools_web/romscan_attr.py`, `scripts/web/romscan.{sh,allow}`, `scripts/web/deploy_check.sh` |

## Traps found in G4
- Sourcing `env.sh` from zsh: `BASH_SOURCE` is empty, GE_ROOT became `$HOME` and the whole home
  directory was bind-mounted into the dev container (suspected cause of repeated Docker Desktop
  crashes). env.sh now refuses non-bash shells and non-repo roots; call scripts with `bash`.
- `MEMORY64=2`: exported functions with pointer parameters get BigInts in JS; give page-facing
  exports `uint32_t` address parameters and a `ge_web_alloc`/`ge_web_free` pair instead of
  `_malloc`; `Module.HEAPU8` must be in `EXPORTED_RUNTIME_METHODS`.
- Emscripten under node does not pass the host environment to `getenv` (use argv in tools).
- The final wasm's DWARF is unusable after wasm-opt/asyncify ("unsupported version"); use the wasm-ld
  map (`-Wl,-Map`) for symbol addresses and the data section for initial bytes.
- wasm-opt splits data segments around zero gaps: a symbol can span several segments; work on a
  reconstructed memory image, not per segment.
- Do not fill (or CRC-check) data that runs before the fill: port `.data` is changed by C
  constructors before `onRuntimeInitialized`; the fill only touches decomp/asset data and port
  read-only tables.
- The Dam intro camera is chosen by `randomGetNext()` after the RNG has advanced every frame
  (seeded from the clock), so screenshots of the intro differ between runs even with `GE_RSEED`;
  compare gameplay frames, not intro cuts.

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

## Traps found in G2
- Libraries that yield to the browser on their own break a fiber scheduler: SDL2 + ASYNCIFY does
  `emscripten_sleep(0)` in `SDL_GL_SwapWindow`/`SDL_Delay`. It "works" for hundreds of frames, then the
  main loop stops (no exception, page idle). Hint `SDL_HINT_EMSCRIPTEN_ASYNCIFY=0`; wrap `emscripten_sleep`.
- Busy-waits on state another thread changes need not touch the clock or a queue: a plain getter
  (`alCSPGetState`) in a `while` loop. Find them with `PROBE_DEBUG=1`; fix with `-Wl,--wrap=<getter>` +
  `webkernelSpinCheck()`, game code untouched.
- A page stuck in wasm does not process a `Debugger.enable` sent after the hang; enable before navigating.
- Function pointers installed with a decompiled `void f()` and called as returning a value (the MIPS/x86
  return register carried a callee's result): wasm traps, an adapter must return that value.
- Emscripten glue bug with `-sMEMORY64=2`: the `beforeunload` handler mixes BigInt and Number
  (`UTF8ToString` of SDL's returned pointer), an exception at page unload. Harmless so far (open).

## Traps found in G3
- fast3d picks features from desktop-GL capabilities; on WebGL2 it silently took every fallback
  (perspective shade instead of the RDP's affine shade, no depth clamp, mirrored-repeat instead of
  mirror clamp): the sky/water horizon fades looked wrong. WebGL extensions must also be *enabled*
  per context (`emscripten_webgl_enable_extension`) before glad can use them. Compare against native
  frames (`tools_pc/golden/*/linux`) early: the start view of every level is a cheap fidelity test.
- `-sMEMORY64=2` glue: a C callback returning a pointer to JS (SDL's beforeunload returns `""`) arrives
  as a BigInt; `UTF8ToString(BigInt)` throws. Replace such callbacks or return NULL.
- `<emscripten/html5.h>` includes `pthread.h`, which reaches the port's `sched.h` shim and the game
  headers (`typedef s32 bool` vs `<stdbool.h>`): declare the one html5 call locally in port files.
- In the port's default control style the stick's Y axis looks up/down; scripted walking needs
  `CUPHOLD`/`CNONE`. A pulses during an intro skip also cycle the weapon once gameplay starts (B is
  safe). Look at the screenshots: a "bug" in sweep 1 was Bond throwing a grenade into his own lift.
- Headless Chrome cannot take pointer lock ("WrongDocumentError"); a fake Gamepad API pad
  (`scripts/web/fakepad.js` via `PROBE_PRELOAD`) does reach SDL.
- Docker Desktop (macOS) died four times in one evening ("service fs failed: injecting event blocked
  for 60s"): a dev container had all of `$HOME` bind-mounted (env.sh sourced from zsh, found by G4).
  Now: env.sh refuses zsh and a non-repo root and recreates a container whose /src is not the repo;
  build trees in a named volume (`$GE_DEV-build` at /build); `--cpus=4` + `ninja -j4`; one build at a
  time (`/tmp/goldeneye-build.lock`) and one probe Chrome at a time (`/tmp/goldeneye-chrome.lock`,
  probe.mjs takes it). `scripts/web/serve_node.mjs` serves the site without Docker.

## Open problems
- Egypt dark pillar / MP black or grey wall areas (LEVELS.md): needs a native reference at the same
  spot. Ruled out on the web side: room-model pool (`GE_D104`: 0 FAIL / SKIP), CPU near clip
  (`GE_NEARCLIP=0`), fog mode (`GE_FOGPIXEL=1`), GL backend (SwiftShader and Metal).
- Pointer lock (mouse aim) and real gamepads/rumble: need a human in a real browser. SDL requests the
  lock in its mousedown handler once the game has turned relative mode on (port click-to-lock), so the
  first click arms and the lock may only come with the second click; check that.
- Audio not listened to by ear (headless, muted); AudioWorklet resampler is linear.
- Det mode (`GE_DETERM`) not tried on the web; sweeps are wall-clock with random AI/intros.
- Solo warps into MP-only stages trap (not a game path, LEVELS.md); Citadel warp loops.
- 2.06 GiB initial memory (desktop only).
- `route_dam.sh`'s in-level `SUP` looks down instead of walking (stick Y looks in the port's default
  control style); `CUPHOLD`/`CNONE` walks (level_sweep.sh uses it).

## Next session — recommended (after G3)
1. A same-machine native reference: arm64 native Linux build in Docker (no emulation; game TUs need the
   LP64 `__x86_64__` fixes the way the web build gets them) under Xvfb/llvmpipe with `GE_PCDUMP`, to
   settle the Egypt / MP dark-area question and for golden comparisons beyond the start view.
2. A human pass in a real browser: pointer lock (first click?), a real gamepad + rumble, audio by ear
   (AudioWorklet), hidden-tab pause/resume, the unload path.
3. Saves/config persistence (IDBFS or IndexedDB sync of `ge007.eep`/`ge007.ini`; webpage.c already
   writes the config on unload), export/import (W4).
4. Det mode on the web (`GE_DETERM`) so sweeps compare frame by frame.

## After G4 (ROM data) — recommended
1. Struct-aware fill for the residual small fields (file_resource_table, PitemZ_entries,
   c_item_entries, levelinfotable, MP weapon sets, model headers; ~5.7 KB) and game-TU strings in the
   merged string pool, or a user decision that they count as "small constants" (ROMDATA.md section 6).
2. Fill table keyed by (symbol, offset) and committed, so a stripped build does not need the ROM.
3. Any deploy script must call `scripts/web/deploy_check.sh` first.
