# GoldenEye 007 (EU) web port: hand-off

Plan and decisions: `docs/web/PLAN.md`. Rules: `orchestrator.md`. Playbook: `WEB_PORT_ORCHESTRATOR.md`.
Session log: `docs/web/sessions.tsv`.

## Current state (after G2, 2026-10-29)

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

Debug / test parameters of the page (dev URL): `&env=NAME=VALUE` (repeatable) sets a C `getenv`
variable, `&args=-level_33` passes command-line args (warp to Dam). Useful variables: `GE_WKTRACE=<browser
frame>` (log every fiber switch/wait/spin/gfx task from that frame; a 256-entry ring `ge_web_wk_ring`
always), `GE_INPUTSCRIPT=...` (+ `GE_INPUTSCRIPT_MS=1`) scripted pad (syntax in `port/src/input.c`).
Probe env: `PROBE_DEBUG=1` (debugger on before navigation: a stuck main thread then gives its wasm stack
— this found bug 2), `PROBE_PROFILE=<s>`, `PROBE_KEYS="24:KeyD:1,28:KeyW:3"` (real key events),
`PROBE_EVAL="$(cat scripts/web/audiocheck.js)"`, `PROBE_ANGLE=metal` (real GPU), `PROBE_FULL=1`
(whole page instead of the canvas), `PROBE_SCALE`. Every run writes `console.log` (streamed, timestamps)
and `sheet.png` (contact sheet) into its outdir.

```sh
scripts/web/route_dam.sh [secs=75] [outdir=port_work/route-dam]   # real route to Dam + walk/fire; look at sheet.png
```
Native Linux reference build (amd64, emulated; compiles/links, does not run under emulation):
`docker build --platform linux/amd64 -t goldeneye-linux-amd64 -f scripts/web/Dockerfile.linux scripts/web`,
then in container `goldeneye-linux`: `cmake -S . -B build-linux -G Ninja -DROMID=pal-final && ninja -C build-linux`.

Release build: `ge_exec bash -c "emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && ninja -C build-web"`
(2.7 MB wasm vs 12 MB with DWARF), then copy `ge007.{js,wasm}` into `build-web/site/`.
`build.sh` re-runs CMake every time (new files are globbed).

After adding/changing code, if the build reports signature mismatches:
`ge_exec python3 scripts/web/sigadapt.py scan build-web/build.log build-web`, review the new rows in
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

## Open problems
- `beforeunload` BigInt exception at page close (above): SDL then never sees `SDL_APP_TERMINATING`;
  matters once saves/config are written on quit (W4). Fix: override the callback or patch the glue.
- glad logs ~150 "could not find function" lines (desktop-only GL entry points) at start: silence.
- `ScriptProcessorNode` deprecated: AudioWorklet in W4; audio by ear unverified.
- Gamepad, mouse (pointer lock) untested; no hidden-tab pause yet.
- Det mode (`GE_DETERM`) not tried on the web; the route test uses wall-clock scripted pad timing
  (`GE_INPUTSCRIPT_MS=1`), generous gaps; a read-count script would be deterministic once det works.
- 2.06 GiB initial memory (desktop only).

## Next session (G3) — recommended
1. W2 is gated on the user's decision (recorded in PLAN 3.2, 2026-10-29); the user also still decides
   hosting. Before W2: a broader runtime sweep with `-level_XX` warps over all 20 solo levels (screenshot
   + `pacing:` + no EXCEPTION each; a `scripts/web/level_sweep.sh`), fix traps found (expect more
   indirect-call signature mismatches and busy-waits; tools above).
2. Page polish needed for a real player: beforeunload fix, glad log silence, pointer lock on click,
   hidden-tab pause, gamepad check.
3. Then W2 (C sidecar emitters in wasm, IndexedDB cache) per PLAN.

## After G4 (ROM data) — recommended
1. Struct-aware fill for the residual small fields (file_resource_table, PitemZ_entries,
   c_item_entries, levelinfotable, MP weapon sets, model headers; ~5.7 KB) and game-TU strings in the
   merged string pool, or a user decision that they count as "small constants" (ROMDATA.md section 6).
2. Fill table keyed by (symbol, offset) and committed, so a stripped build does not need the ROM.
3. Any deploy script must call `scripts/web/deploy_check.sh` first.
