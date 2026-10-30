# GoldenEye 007 (EU) web port: level sweep

Session G3, 2026-10-30, branch `web` (builds from `c2727145` + highp, re-checked on the merge `cfedcc88`). Headless Chrome (SwiftShader
WebGL2 unless noted), muted, dev sidecars (`?assets=`). Screenshots, contact sheets and console logs are
in the gitignored `port_work/` (game imagery, never committed).

How it was run:
- **Solo:** `scripts/web/level_sweep.sh` (warp `-level_XX`, Agent, fresh save; B pulses skip the intro
  camera; walk 3 s with C-up held, fire, turn left 2 s, strafe right 1 s, fire; a shot every 3 s for
  34 s). Runs: `port_work/sweep/` (G3 start build, A/B pad), `port_work/sweep2/` (after the page-polish
  commits, B-only pad), `port_work/sweep3/` (after the WebGL-extension fix, first 13 s only).
- **Fidelity:** the 12 s shot (start position, intro skipped, before walking) next to upstream's
  native Linux golden frame (`tools_pc/golden/<level>/linux/`, last stem): sheet
  `port_work/g3/compare12b.png` (left native, right web), made by `port_work/g3/mkcompare.sh` +
  `port_work/g3/pageshot.mjs`. `compare12.png` is the same before the WebGL-extension fix.
- **Multiplayer:** `scripts/web/mp_sweep.sh` (port harness `GE_STARTMP=<players>,<stage>` +
  `GE_MPVIRT`, one scripted pad per seat), `port_work/mpsweep/` and `port_work/mpsweep2/`.
- Pacing column: the `pacing:` line of the last two 10 s windows (console retraces/s, game frames/s,
  retraces dropped by the catch-up cap). The first window always contains the stage load (heavy
  frames: 46-49 retraces/s, 9-41 dropped), not repeated in the table.

## Solo missions (all 20) and the ending

| # | Level | `-level_` | Loads | Renders right | Pacing (steady) | Errors | Notes |
|---|---|---|---|---|---|---|---|
| 1 | Dam | 33 | yes | yes (= native golden) | 50.0 / 50.0 / 0 | none | intro camera, HUD, walk/fire/turn; also by the real route (`route_dam.sh`) |
| 2 | Facility | 34 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | starts in the vent as on N64 |
| 3 | Runway | 35 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | guards shoot Bond (health/armour arcs) |
| 4 | Surface 1 | 36 | yes | yes (= golden after the fix) | 50.0 / 50.0 / 0 | none | sky was pinker/brighter before the noperspective fix |
| 5 | Bunker 1 | 09 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | |
| 6 | Silo | 20 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | |
| 7 | Frigate | 26 | yes | yes (= golden after the fix) | 50.0 / 50.1 / 0 | none | water was teal with a bright horizon (native: dark navy) before the noperspective fix |
| 8 | Surface 2 | 43 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | |
| 9 | Bunker 2 | 27 | yes | yes (= golden) | 49.4-50.0 / 0-5 | none | starts in the cell |
| 10 | Statue | 22 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | |
| 11 | Archives | 24 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | interrogation room, guard lines, ammo pickup |
| 12 | Streets | 29 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | mission timer on the HUD (as native) |
| 13 | Depot | 30 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | |
| 14 | Train | 25 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | smoke, guards |
| 15 | Jungle | 37 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | Natalya line |
| 16 | Control | 23 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | lift, Natalya |
| 17 | Caverns | 39 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | (sweep 1: the A pulse selected grenades and Bond blew himself up: test artefact) |
| 18 | Cradle | 41 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | "Trevelyan has activated the antenna control console" |
| 19 | Aztec | 28 | yes | yes (= golden) | 50.0 / 50.0 / 0 | none | |
| 20 | Egypt | 32 | yes | yes (= native, G7) | 50.0 / 50.0 / 0 | none | start view = golden; in several runs a pillar close to the camera (and once two mid-distance pillars) renders almost black (dark gradient, decals still drawn) for many seconds, on SwiftShader and Metal, with and without `GE_NEARCLIP=0` / `GE_FOGPIXEL=1`; not reproducible run to run (random AI/intro). G7: the native port renders such pillars the same way (vertex colours near 0 from the room data); not a web bug |
| - | Cuba (ending) | 54 | yes | yes (= golden) | 50.0 / 49.1 / 0 | none | ending cutscene, then back to the boot screens (as native, see `tools_pc/golden/README.md`) |

No wasm trap, no `STALL`, no refused `emscripten_sleep`, main thread answering at the end, in every solo
run (3 sweeps). The page-unload exception is gone since `9732b5b0` (it fired when the probe navigated
away; sweep 1 still shows it).

## Multiplayer (split-screen, real path: `GE_STARTMP`)

`scripts/web/mp_sweep.sh`: each seat walks / fires / turns with its own scripted pad; MP default
options (`init_mp_options_for_scenario`), random spawns. Two runs (`port_work/mpsweep/`,
`port_work/mpsweep2/`, the second after the WebGL-extension fix).

| Players | Arena (index) | Loads | Renders right | Pacing (steady) | Errors | Notes |
|---|---|---|---|---|---|---|
| 2 | Temple (1) | yes | yes (= native, G7) | 50.0 / 0 | none | split views, radar, independent seats; in some views a big area is flat light grey or black; G7: a near model with bright vertex colours / dark walls and overhangs, the same on native at the same spawn |
| 2 | Complex (2) | yes | yes | 50.0 / 0 | none | |
| 2 | Caves (3) | yes | yes | 49.9-50.0 / 0 | none | dark level |
| 2 | Library (4) | yes | probably (same family as Temple; Library not compared on native, G7) | 48.5-50.0 / 0-13 | none | a large black area over the balcony in one view |
| 2 | Basement (5) | yes | yes | 50.0 / 0 | none | |
| 2 | Stack (6) | yes | yes | 50.0 / 0 | none | |
| 2 | Facility (7) | yes | yes | 50.0 / 0 | none | one seat in the vent |
| 2 | Bunker (8) | yes | yes | 50.0 / 0 | none | |
| 2 | Archives (9) | yes | yes | 50.0 / 0 | none | |
| 2 | Caverns (10) | yes | yes | 50.0 / 0 | none | |
| 2 | Egypt (11) | yes | yes (G7) | 50.0 / 0 | none | black/grey close surfaces like solo Egypt |
| 3 | Bunker (8) | yes | yes | 50.0 / 0 | none | 3 views + the empty fourth quarter (as N64) |
| 4 | Temple (1) | yes | yes | 50.0 / 0 | none | 4 views, all seats move |
| 4 | Complex (2) | yes | yes | 50.0 / 0 | none | |

No trap, stall or busy page in any run; the console clock stays at 50 retraces/s with four views.
What the headless runs do not cover: human pads on seats 2-4 (real Gamepad API devices: one fake pad
was tested on seat 1 only), the MP menus (the harness skips them), match end / scoring screens.

## Solo warps into multiplayer-only stages (not a game path)

`-level_38` Temple, `31` Complex, `50` Caves, `48` Library, `45` Basement, `46` Stack: wasm trap
"memory access out of bounds" 3.5 s in, in `modelPromoteNodeOffsetsToPointers` <- `fileLoad` <-
`modelLoad` <- `domakedefaultobj` <- `proplvreset2` <- `lvlStageLoad` (a default object of the solo
setup the stage does not have). `-level_40` Citadel (cut from the MP menu, "old format setup"):
the main thread loops (page busy, no screenshot after 3 s). The front end never starts these stages in
solo; they load fine as multiplayer arenas (above). Native behaviour for these warps is unknown
(upstream's sweeps cover the 21 solo stages only); not investigated further.

## What was ruled out for the dark / grey areas (Egypt, Temple, Library)
Not room-model pool exhaustion (the D294 class: `GE_D104=1` logs 0 `LOAD ... FAIL` and 0 `SKIP` in
Egypt and 2P Temple), not the CPU near clip (`GE_NEARCLIP=0` same), not the fog mode
(`GE_FOGPIXEL=1` same), not the GL backend (SwiftShader and ANGLE Metal both), not the
missing WebGL depth clamp / noperspective (present after `c2727145`, still seen). Bullet-hole decals
stay visible on the dark surface. A native run at the same spot decides whether it is faithful.

## G7 verdict on the dark / grey areas (2026-10-30)
Native reference built and run on the same Mac (`scripts/web/native_ref.sh`, aarch64 Linux, llvmpipe):
the native port shows the near-black Egypt pillars, the large black areas in 2P Temple at the same spawn
and the flat light-grey surfaces too (`port_work/g7/compare/`). On the web side, at a det-stopped retrace
(`scripts/web/darkdet.sh`, `GE_DETSTOP`), the dark pixels come from geometry drawn with its own dark vertex
colours and fog (room walls, large near models), not from culling, the sky, clipping, the shade
interpolation or changed vertex data. Details and confidence: `docs/web/HANDOFF.md`, "Dark areas (G7)".
2P Temple through `mp_sweep.sh` is the same every run (flat grey in the second view from 21 s).

## What to look at next (G3 notes, before G7)
- Egypt dark pillars: compare with a native build on the same spot (an arm64 native Linux build in
  Docker with Xvfb/llvmpipe would give a same-machine reference; the amd64 container crashes under
  emulation), then decide web-specific or faithful.
- The sweeps use wall-clock scripted pads, AI and intros are random (`GE_RSEED` pins the PRNG but not
  the timing): a det-mode (`GE_DETERM`) web run would make shots comparable frame by frame.
