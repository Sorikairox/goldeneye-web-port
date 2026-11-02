# GoldenEye 007 (EU) web port: hand-off

Plan and decisions: `docs/web/PLAN.md`. Rules: `orchestrator.md`. Playbook: `WEB_PORT_ORCHESTRATOR.md`.
Session log: `docs/web/sessions.tsv`.

## Current state (after G3 .. G9, 2026-11-02)

G9: **offline play**: after one visit the site works with no network (section "Offline play (G9)" below). Page option
"Offline play", default on.

G8: **touch controls**: the site is playable on a phone (section "Touch controls (G8)" below). Full N64 pad on
screen, Auto / On / Off setting, portrait and landscape, rotation mid-game, fed into controller 0 like a keyboard
or gamepad (online lockstep and recorder unchanged).


G7: the "dark areas" (Egypt pillars, MP black / flat-grey walls) are **not a web-port bug**: the native
PC port, built and run on this Mac (aarch64 Linux in Docker, llvmpipe), renders the same surfaces the
same way (section "Dark areas (G7)" below). No rendering change was made. New tools: a native reference
build (`scripts/web/native_ref.sh`) and det runs that stop at a chosen retrace (`GE_DETSTOP`,
`scripts/web/darkdet.sh`).

G6: optional online multiplayer (deterministic input lockstep over WebRTC, 2-4 players), branch `web-net`
(section "Online multiplayer" below, `docs/web/ONLINE.md`).

G5: saves persist in IndexedDB with export / import and an online sandbox seam (section "Saves in the
browser" below).


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

## Offline play (G9, 2026-11-02)

User request: once someone has loaded the website, the game must work with no network.
- **Service worker** `port/web/sw.js` + generated `sw-build.js` (`scripts/web/swbuild.mjs <site>`, run by `build.sh` on
  build-web/site and by `deploy_web.sh` on the publish dir after BUILD.txt). `sw-build.js` holds `GE_FILES` (every
  file of the site except sw.js / sw-build.js) and `GE_BUILD` = SHA-256 over names + contents. Cache name
  `ge007-offline-<GE_BUILD>`. Install precaches all files (`cache: 'no-cache'`: revalidated, never a stale HTTP-cache
  copy of an older build); activate deletes every other `ge007-offline-*` cache and claims the page (the first visit is
  already offline-capable, no reload). Fetch: same-origin GET under the scope, cache-first (`ignoreSearch`, navigations
  get index.html); never touched: non-GET, other origins, `/api/*` (the room function) and `/.netlify/*`.
  **Precache: 14 files, 13.23 MiB** (ge007.wasm 12.3 MiB, ge007.js 0.45 MiB, romfill 0.28 MiB, the rest small).
- **Updates:** a deploy changes sw-build.js; the browser's update check (each load, and every 30 min of a long session)
  installs the new worker, which waits. The page shows "Update ready [Reload] [×]" (fixed, above the touch layer). The
  next page load takes the new version before the game starts: `main.js` awaits `GEOffline.ready` (max 6 s), which
  tells a waiting worker to skipWaiting and reloads once on controllerchange. So the game never runs on mixed versions.
- **Page** `port/web/offline.js`: option checkbox "Offline play" in the Options box (default ON because the user
  asked; an explicit Off is stored as localStorage `ge007-offline=off`; `?offline=0|1` for tests, not stored); status
  line under it (MB cached, build, persistent or not). On: register, `navigator.storage.persist()` (headless Chrome
  answers false; real Chrome grants it to engaged / installed sites). Off: unregister + delete the caches; the page
  then loads from the network exactly as before G9 (tested: 0 registrations, 0 caches, after reload too).
- **Manifest** `manifest.webmanifest` + plain generic icons (`icon.svg`, `icon-192.png`, `icon-512.png`: grey ring and
  play triangle on dark grey, drawn by a few lines of Python, no game art). Installable as an app.
- **Online panel** (`net.js`): with `navigator.onLine === false` it says "No connection: online multiplayer needs the
  internet. Solo and split-screen play work offline." and disables Create / Join / Host / Join (Play offline stays);
  follows the online / offline events.
- **Unchanged:** the ROM (`ge007-web` / `files` / `rom`), the converted sidecars and the saves stay in IndexedDB.
- **Memory:** the cache lives in Cache Storage on disk and is streamed by the browser; nothing of it is in the wasm
  heap or the page's JS heap. The extra cost is a one-time second download of ~13 MiB (the precache runs next to the
  page's own fetch on the very first visit) and ~13 MiB of disk. It adds no pressure to the ~2 GiB wasm memory risk on
  phones (that risk is unchanged, still open: real phones).
- **Tests** (muted headless Chrome, one at a time; probe.mjs got steps `net:offline|online` = Network.emulate-
  NetworkConditions + overrideNetworkState on the page and on every service worker, and `reload`):
  - `scripts/web/offline_test.sh OUTDIR [SITE_URL]`: Android phone emulation 412x915, fresh profile; first visit,
    ROM chosen with the file picker (CDP setFileInputFiles), game boots; at 80 s network off (locally the dev server
    is also killed), reload: page + ROM picker present, stored ROM boots, sidecars from IndexedDB, touch layer up and
    playing, intro renders, Online note shown and buttons disabled.
  - `scripts/web/offline_update_test.sh OUTDIR`: builds A -> B -> C in one kept profile: notice appears, a plain reload
    switches to B and leaves only cache B; the notice's Reload button switches to C and leaves only cache C.

- **Deployed** 2026-11-02, commit 09812668 (live sw-build build id `14b2513d6e10ae1a`, 15 files incl. BUILD.txt); the
  offline test passed against https://goldeneye-web-port.netlify.app/ as well as locally.
- **Next:** check on real phones (Android Chrome: install to home screen, airplane mode; iOS Safari: offline after a
  visit, home-screen app storage is separate from Safari's, so the ROM must be chosen again there).

### Traps found in G9
- Chrome 156: after a reload served by the service worker, `navigator.onLine` came back true although the page's
  offline emulation was set; probe.mjs re-applies the network state after `reload`. Requests still failed (no file
  came from the network: the local server was dead), only the flag was wrong.
- probe.mjs steps are split on `;`: an `eval:` with a `;` in its JS breaks ("Uncaught").
- A probe that died on an unhandled rejection left its Chrome running (orphan, killed by PID); probe.mjs now kills
  Chrome on any unhandled rejection and reads the browser endpoint once at start.

## Touch controls (G8, 2026-11-01)

User rule (orchestrator.md): touch controls are always part of the web port; every session that changes the page
re-checks phone emulation before deploying (`scripts/web/phone_test.sh`, below).

- **Page** (`port/web/touch.js`, markup + CSS in `index.html`, `GETouch.started()` from `main.js` right before
  `callMain`): floating analogue stick (touch anywhere in the lower-left zone; 50 px x scale = full tilt, 12 % dead
  zone, -80..80), **Z** (fire, the big button) and **R** (aim) side by side under the right thumb, **B** (action) and
  **A** (weapon) above them, the four **C** buttons as a diamond above those, **L** and **START**, a ☰ page-menu
  button and a ⛶ full-screen button (hidden where the browser has no page full screen, i.e. iPhone Safari). Each
  control follows its own pointer (pointer events + capture): stick and buttons together work (tested: walk + fire).
  - Portrait: the 4:3 picture at the top (under the safe-area inset), controls in the space below.
    Landscape: the picture centred at full height, controls over the side margins (R / B slightly over the picture
    edge on 844x390).
  - Positions are computed from the window size in `layout()` on every resize / orientationchange /
    visualViewport resize; the stick's resting ring only from a laid-out zone box (ResizeObserver); the faces are DOM
    (no canvases, so nothing is drawn while hidden). Conker P369 lessons applied from the start.
  - ☰ shows the normal page (Save box, Options, Online panel, log) with a green "Back to the game" button; the pad
    is idle meanwhile. Turning the phone with the menu open: the layout is redone on close (tested).
  - While the layer is up: no pinch / double-tap zoom, no scroll, no long-press callout or selection.
  - Touch devices (detected below): no `accept` filter on the ROM and save pickers (iOS greys out `.z64`; Android
    may hide unknown types; SHA-1 / CRC checks stay), bigger tap targets.
- **Setting** (Options box under the picture): Touch controls **Auto** (default: on when `(pointer: coarse)`, or touch
  points on an iOS / Android / mobile browser) / **On** / **Off**. Only an explicit On / Off is stored
  (`localStorage ge007-touch`); Auto stores nothing (one wrong detection never sticks). `?touch=1` / `?touch=0` for
  tests (not stored). A help list explains the 1.1 controls. Desktop default: off, page unchanged (canvas 640x480).
- **Input** (`port/src/web/webtouch.c`): the page calls `Module._ge_web_touch(on, buttons, x, y)` on every change
  (N64 bit layout); `input.c inputComputePadSlot` merges it into controller 0 after keyboard / gamepad / adapters and
  before the menu release hold-off (buttons OR'd, a moved touch stick replaces the stick). So the offline read, the
  online lockstep sample (`geNetRetraceStart -> inputComputePad(0)`) and the recorder see touch exactly like a pad.
  While on, offline Modern controls force the game's **1.1 HONEY** style for seat 0 (stick walks + turns) instead of
  the 1.2 the port forces for keyboard + mouse; online (OG controls) the game's own style stands (1.1 by default).
- **Proof** (port_work/g8, gitignored):
  - `phone_test.sh` iPhone 390x844 -> 844x390 (`ios/`), Android 412x915 -> 915x412 (`and/`), Android 844x390 ->
    412x915 (`land/`): controls complete and in place in every orientation, also after turning with the menu open;
    controller 0 (GE_INPUTLOG) shows `btn=2000 stick=(3,80)` = Z + stick at once; Bond walks; the HUD ammo goes
    7 -> 4 -> 2 -> 1 and bullet holes appear in the Dam barrier (`and/shot-d_fired.png`).
  - ROM picker on an emulated iPhone, fresh profile (`picker/`): no accept filter, the file input starts the game.
  - Boot by touch only (`boot/`): START / A taps go from power-on to the Dam briefing.
  - Offline det (`netdet.sh`, 4P menu route, `&touch=1`): a1 vs a2 5883 / 5883 identical; touch-on vs touch-off
    det runs 2890 / 2890 identical.
  - Online 2P (`netpair.sh`, guest with `GUEST_TOUCH=1`, presses via `touchpress.js`): host vs guest 3575 / 3575
    identical (H and S); against a run without touch the host's hashes are equal up to retrace 2211 and differ
    from 2212 (the first touch): the guest's touch input reached the shared game.
  - `deploy_check.sh` OK.

```sh
GE_PORT=8775 bash scripts/web/phone_test.sh port_work/g8/ios ios 390,844 844,390      # OUT ios|android W,H [TURN_W,H]
PROBE_MOBILE=android PROBE_VIEWPORT=412,915 PROBE_STEPS='20:down:#tk-Z,1;20.3:up:#tk-Z,1;22:orient:915,412;23:shot:x' node scripts/web/probe.mjs URL 25 OUT
GE_PORT=8775 GUEST_TOUCH=1 GUEST_EVALS="30:$(grep -v '^ *//' scripts/web/touchpress.js | tr '\n' ' ')|64:__tp('tk-Z',300)" bash scripts/web/netpair.sh NAME 110
```

### Traps found in G8
- CDP `Input.dispatchTouchEvent` `touchEnd`: Chrome ends the points listed. Listing the fingers still down (as the
  Conker probe's comment says) lifted the wrong finger; probe.mjs sends the lifting finger.
- A whole-page screenshot of a long page at DPR 3 takes seconds; a tap sent meanwhile was lost (phone_test.sh waits).
- `tr '\n' ' '` on a JS file with `//` comments turns the rest into a comment; strip comment lines first.

### Next (touch)
- A human on a real phone: iOS Safari (bottom toolbar, safe areas, no full screen: Add to Home Screen), Android
  Chrome full screen, haptics, feel of the stick size / travel and the button places; whether the 2 GiB wasm memory
  is granted on the phone at all (desktop-only so far, open problem below) - the first thing to check.
- Tapping menu items on the picture (SDL turns touches on the canvas into mouse events; untested).
- Optional: R as a latch on touch (the game's own watch option Aim: toggle exists).

## Dark areas (G7, 2026-10-30)

**Verdict: the web port draws what the native port draws.** Confidence: high that it is not web-specific
(same-spawn and same-situation native frames show all three symptoms); medium that the native port is
itself faithful to the N64 (both share fast3d and the data; upstream compared near-camera shading and fog
against LLE mupen64plus / angrylion for D543/D553, but nobody has put these exact views next to an N64).

What the surfaces are (probes at a det-stopped retrace, `port_work/g7/`):
- Egypt pillar near the camera, almost black: room geometry, combiner `TEXEL x SHADE`; the triangles carry
  vertex colours at or near 0,0,0 straight from the room vertex table. The colour bytes in memory never
  change after the room load (checksum at load vs every draw, 0 changes), no light-fixture code runs in
  Egypt. Native frame with the same kind of pillar: `port_work/g7/compare/egypt-native-f750.png`
  (web: `egypt-web-22s.png`).
- MP Temple black areas (and G6's dark 4-player views): either the cloud sky's region covered by large
  near models (overhangs / blocks, w 300-600, vertex colour ~68, blender fog towards black 40 %) or room
  walls with dark vertex colours. Not portal culling (a no-cull switch left the frame unchanged), not the
  sky (dropping every sky triangle changed 210 pixels). Same spawn on native (fresh save, same
  `GE_STARTMP=2,1`): `compare/temple2p-spawn-native.png` vs `temple2p-spawn-web.png`, the same black area.
- MP flat light grey (130,130,130): a near model with bright vertex colours (129-188) and partial fog,
  drawn last at that pixel; native shows the same kind of flat grey surface in the same situation
  (`compare/temple2p-grey-native-f1000.png` vs `temple2p-grey-web-24s.png`).

Ruled out on the way (all A/B at the same det retrace, pixel-compared): GPU side-plane clipping of
`noperspective` shade/fog (a CPU clip at the RSP's x/y guard band, FRUSTRATIO_2, gave a pixel-identical
frame; reverted), perspective vs affine shade (`GE_SHADEPERSP=1` identical), portal culling, the sky,
the room vertex data (unchanged), light fixtures, room pool (G3), fog mode and near clip (G3). The cloud
sky's environment record, vertex values, ENV colour and the sky textures match native byte for byte.

Reproduce:
```sh
GE_SERVE=goldeneye-g7-serve bash -c '. scripts/web/env.sh && scripts/web/serve.sh 8774'
EVERY=50 bash scripts/web/darkdet.sh port_work/g7/s2120 50 '&env=GE_DETSTOP=2120'    # 4P Temple, dark in all views
GE_PORT=8774 OUT=port_work/g7/mp bash scripts/web/mp_sweep.sh 2,1:temple              # 2P: flat grey at 21-36 s (same every run)
GE_PORT=8774 OUT=port_work/g7/eg bash scripts/web/level_sweep.sh 32:egypt             # dark pillar in most runs after ~20 s
bash scripts/web/native_ref.sh build
rm -f data/ge007.ini data/ge007.eep    # fresh native config and save
bash scripts/web/native_ref.sh run port_work/g7tmp/nat 45 "GE_FAKE_LOWEND=0 GE_PCDUMP=0-4000:25 GE_MPVIRT=1 GE_STARTMP=2,1" # + pads like mp_sweep.sh
```
`GE_DETSTOP=<retrace>` holds a det run (`?netdet`) at that retrace; offline det runs also take the
render-only switches `GE_SHADEPERSP`, `GE_FOGPIXEL`, `GE_NEARCLIP`, `GE_NEARCLIPFOG`, `GE_D104`,
`GE_D176` (online sessions keep the whitelist). Native runs: `GE_FAKE_LOWEND=0` (llvmpipe is otherwise
classed low-end and the draw distance halved; that shows the sky through distant floors), ms-timed pads
work (movement scales with frame time, llvmpipe runs ~20 fps), frames land in `OUTDIR/ppm/*.png`.

Native aarch64 build fixes (port-side or layout-only, all PC builds benefit): `GE_LP64_GAME_DEFINE`
CMake option (game TUs get `__x86_64__` as on the web), low game-thread stacks without `MAP_32BIT`
(`portAllocLowStack` hint loop; G2's amd64-emulated `texReadBits` crash was this), and
`bgMarkSpecialPortals` (was `sub_GAME_7F0B37EC`) bounded by its own array instead of the address of
the next global (porting-notes D5; the web layout was already adjacent).

## Online multiplayer (G6, 2026-10-30)

Design, determinism table, results and what is left: `docs/web/ONLINE.md`. Branch `web-net` (worktree
`~/goldeneye-wt/net`).

- **Det mode** (`geNetDetOn`, `port/src/web/webnet.c`; hooks in `webkernel.c`, `libultra.c`, `audio.c`, `input.c`,
  `config.c`, `optionsoverlay.c`, `main.c`): det clock for `osGetCount` / `osGetTime` / timers, fibers run only
  inside retraces (spinners park until the next retrace), virtual AI queue, controller reads from the input ring,
  pure N64 pad (port input adapters that write game state are off), fixed session options, sandboxed EEPROM (G5
  seam), 4 ms timer loop, frame barrier, time sync, kick, hidden-page worker. Off (no session) every path is the
  original one.
- **State hash:** H = game data from the link map (`scripts/web/netregions.py` -> `ge007.netregions`, built by
  `build.sh`) + 8 MiB DRAM + the live EEPROM image, in 4 KB pages; S = the game fibers' asyncify buffers.
- **Page:** `port/web/net.js` (from Conker's net.js): Online panel behind the checkbox "Online multiplayer
  (experimental)", copy-paste codes, room codes, mesh + host relay, desync report, lost player, recorder.
- **Room server:** `port/netlify/functions/room/` (not deployed); local copy `scripts/web/roomserver.mjs` on
  127.0.0.1:8773 for tests.
- **Proofs (numbers in ONLINE.md section 6):** offline det, the real menu route into a 4-player Temple match:
  two runs identical at 6870 / 6870 retraces (H and S), one changed input diverges at the next retrace. Online
  (one headless Chrome, 2-4 windows): 2P 7441 / 7441 identical (99.3 % speed); 2P with 40+0..20 ms and 5 % loss
  7437 / 7437 (99.5 %, auto D 4); 4P with netsim, one relayed pair and a player leaving 9776 / 9776 (99.9 %);
  4P by room code through the panel (local room server) 5450 / 5450; hidden guest page 30 s 5534 / 5534; a
  flipped DRAM byte is reported as a desync at that retrace with the page named. Recorder: a 70 s recording replays with 65 / 65 checkpoints identical; one changed input is
  found at the next checkpoint.

How to run the tests: `docs/web/ONLINE.md` section 5b. Own containers: `GE_DEV=goldeneye-g6-dev`,
`GE_SERVE=goldeneye-g6-serve` (port 8772).

### Traps found in G6
- Fibers on browser frames / spinners resumed per browser frame are not deterministic: det runs fibers only inside
  retraces (playbook trap 43).
- The port's input layer writes game state directly (mouse-aim turn, crouch, style, gadget, sway): pure pad online
  (trap 44).
- Machine-dependent options (window aspect, GPU low-end check, Deck preset) must be pinned in a session (trap 45).
- C (shadow) stacks of fibers carry uninitialised slack with log text: hash the asyncify buffers (trap 46).
- Editing a running bash test script kills the run (trap 47); copy it first.
- The MP menu needs >= 2 controllers present at `osContInit`: the session mask must be set before main().
- In the MP options menu START starts the match; the real route takes ~1900 retraces (38 s) from power-on.

## Saves in the browser (G5, 2026-10-30)

- **Persistence (on by default):** the game keeps `ge007.eep` (2048-byte EEPROM image, native format)
  and `ge007.ini` in MEMFS `/data` as before. After every write the C side calls
  `webSaveWritten()` (`port/src/web/websave.c`; hooks in `libultra.c geEepromStore`, `config.c
  configSave`, and after an emulator save was converted at load) -> `Module.geSaveWritten` ->
  `port/web/saves.js` marks the file dirty and copies it to IndexedDB (`ge007-web`/`files`, keys
  `save-<ROM SHA-1>/ge007.eep` and `.../ge007.ini`) 400 ms after the last write of a burst; at once
  when the tab is hidden; on `pagehide` (after beforeunload's configSave) dirty files also go
  synchronously to localStorage `ge007-pending-<sha1>`, which wins at the next start. `restore()` runs
  in `onRuntimeInitialized` before `callMain`, so the game's first EEPROM read and `configLoad` see
  the stored copies. Storage unreadable (4 s timeout) -> saving off for that page load (a blank save
  never overwrites a real one). `?nosave`: nothing restored or stored (Conker had no such flag).
- **Export / import / delete** (Save box under the picture; it says the save lives only in this
  browser): export = the EEPROM image as `goldeneye-eu.eep` (= the PC port's `data/ge007.eep`).
  Import = exactly 2048 bytes and at least one of the six checksummed regions valid with the game's own
  CRC in port or emulator byte order (`ge_web_eep_check`, `port/src/eepimport.c`; emulator saves are
  converted at boot by the existing D514 code); then persistence is switched off for the running game,
  the file is stored and the page reloads. Config is not exported.
- **Proof** (`scripts/web/save_test.sh`, 4 runs by the real route to Dam, kept Chrome profiles): Auto-Aim
  ON -> OFF in the watch, watch closed -> EEPROM stored (`fileClearSavefileForFolder` writes the
  folder's options); reload -> the watch shows OFF; export (2048 B) -> fresh profile: a 100-byte file
  and 2048 bytes of 0xFF are refused, the export is accepted, page reloads, the watch shows OFF; a
  sandboxed run boots with a blank image (watch: ON) and stores nothing (IndexedDB SHA-1 unchanged).
  The file-select screen shows no progress for options-only saves (no mission was finished: Dam's
  objectives are too long to script).
- **Online seam for G6** (playbook 10.2 item 8): `GESaves.sandbox(bytes | null)` — call it before the
  page calls `main()` (e.g. right when the session config arrives, before `start()`): the game boots
  with exactly those 2048 bytes (null = blank) through `ge_web_eeprom_load`, `ge_web_save_sandbox(1)`
  keeps every EEPROM write in wasm RAM (no `/data` file, no notification), the page stores nothing and
  does not restore the player's ini either (default config) for the rest of the page load; end the
  session with a reload. `await GESaves.image()` = the player's stored image (what a host sends);
  `Module._ge_web_eeprom_image()` = the live 2048-byte image (for a state hash). Called after `main()`
  it still sandboxes but logs that the game may already have read its EEPROM.
- Note: on a fresh profile the port's `watchsettings` builds a blank file 1 at boot, so a save is
  stored within seconds of the first start.

```sh
GE_PORT=8771 scripts/web/save_test.sh [port_work/save-test]   # PASS/FAIL per step; look at the watch shots it names
PROBE_USERDIR=<dir> node scripts/web/probe.mjs ...            # keep a Chrome profile across runs (IndexedDB survives)
```

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
| saves (G5) | C: `port/src/web/websave.c`, `GE_WEB` blocks in `port/src/libultra.c` (EEPROM), `config.c`, `eepimport.c` (`ge_web_eep_check`); page: `port/web/saves.js` (`GESaves`), Save box in `index.html`; test `scripts/web/save_test.sh` |
| native reference (G7) | `scripts/web/native_ref.sh` (aarch64 Linux container, Xvfb/llvmpipe, `GE_PCDUMP` -> PNG via `scripts/web/ppm2png.py`), CMake `GE_LP64_GAME_DEFINE` |
| det stop / dark repro (G7) | `GE_DETSTOP` in `port/src/web/webnet.c`, `ENV_DET_OK` in `port/web/net.js`, `scripts/web/darkdet.sh` |
| touch controls (G8) | page `port/web/touch.js` (+ `index.html` markup/CSS, `main.js` GETouch.started); C `port/src/web/webtouch.c`, merge in `port/src/input.c` (`geWebTouchMerge`, `stickSolitare`); tests `scripts/web/phone_test.sh`, probe.mjs `PROBE_MOBILE`/`PROBE_STEPS`, `touchpress.js` |
| deploy gate | `tools_web/romscan.c`, `tools_web/romscan_attr.py`, `scripts/web/romscan.{sh,allow}`, `scripts/web/deploy_check.sh` |

## Traps found in G7
- An "unreproducible" web rendering bug may be the scripted pad, not the renderer: logging that slows the
  game moves Bond and the symptom vanishes. Use det mode and stop at a retrace (`GE_DETSTOP`) instead of
  printing during wall-clock runs, then A/B render-only switches on the same frame.
- A same-machine native reference needs more than `-D__x86_64__`: on aarch64 Linux there is no
  `MAP_32BIT` (game stacks above 4 GiB break `(u32)` pointer idioms) and GCC's data order differs from the
  N64 linker's (a loop bounded by the next global's address ran away). Fix those port-side and the
  native build runs under Xvfb/llvmpipe on an Apple-silicon Mac without emulation.
- llvmpipe is classed "low end" by the native PC port and halves the draw
  distance on first start; set `GE_FAKE_LOWEND=0` and delete the saved ini, or native frames show sky
  through distant floors that the web build draws.
- The decomp's own libc (atol, ...) is linked into the web build with other signatures; port code that
  calls such names traps (`signature_mismatch:atol`). Parse by hand or use port helpers.
- Offline det sessions filter `&env=` through the online whitelist: a dev switch silently does nothing
  unless it is in `net.js` `ENV_DET_OK`.

## Traps found in G5
- An IndexedDB write started while the page closes may never complete; `beforeunload` listeners run
  in registration order, and the game's own (config write) is registered after the page's. Hook
  `pagehide` (after every beforeunload) and keep a synchronous localStorage copy of what is still dirty.
- An import must switch the running game's persistence off before storing: otherwise its next save,
  or the unload flush, writes the old save over the imported one before the reload.
- Persistence tests need a kept Chrome profile and a clean `Browser.close` (probe.mjs
  `PROBE_USERDIR`): the default probe deletes its profile and SIGKILLs Chrome.
- `indexedDB.open` connections that are never closed block `deleteDatabase` and version upgrades;
  saves.js closes each connection after its transaction (main.js's ROM / sidecar helpers still do not).

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
- Egypt dark pillar / MP black or grey wall areas: same on the native port (G7, "Dark areas" above);
  N64 fidelity of those views not checked against an N64 / LLE emulator. Identify the flat-grey Temple
  model (model + texture) before calling it faithful.
- Pointer lock (mouse aim) and real gamepads/rumble: need a human in a real browser. SDL requests the
  lock in its mousedown handler once the game has turned relative mode on (port click-to-lock), so the
  first click arms and the lock may only come with the second click; check that.
- Audio not listened to by ear (headless, muted); AudioWorklet resampler is linear.
- Det mode (`GE_DETERM`) not tried on the web; sweeps are wall-clock with random AI/intros.
- Solo warps into MP-only stages trap (not a game path, LEVELS.md); Citadel warp loops.
- 2.06 GiB initial memory (desktop only).
- swiftshader: in route_dam.sh, after Bond walks forward (C-up since G5) and faces the Dam,
  retraces drop to ~25/s with ~9 browser frames/s (heavy view in software GL; before G5 the walk looked
  at the ground). With `PROBE_ANGLE=metal` the same route holds 50.0 retraces/s throughout
  (`port_work/g5/route-dam-metal`): a software-GL cost, not a game or save problem.
- Saves: real-browser checks by a human (download dialog of Export, the file picker, closing the tab
  right after a save, private windows / blocked storage); a mission-completion save shown on the
  file-select screen was not scripted.

## Next session — recommended (after G7)
1. If the dark / grey surfaces still need an answer: identify the flat-grey Temple model and the Egypt
   pillar vertex colours in the ROM data, and compare one view with an LLE N64 emulator (angrylion) in
   Docker; the native reference (`native_ref.sh`) and `GE_DETSTOP` make the web side reproducible.
2. Use `native_ref.sh` for other fidelity questions (native frames beyond the golden start views).

## Next session — recommended (after G5)
1. G6 (online): use `GESaves.sandbox(hostImage)` before `main()` (seam above); hash
   `_ge_web_eeprom_image()` at session start on every peer.
2. Human pass in a real browser: export download, import file picker, close the tab right after
   a save then reopen, private window.
3. Optional: a "Settings" export (ge007.ini) next to the save, and save slots like Conker's `?slot=`.

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
