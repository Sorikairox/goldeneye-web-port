# GoldenEye on retrowser.app (v1 module)

retrowser.app (standard: the retrowser repo, `spec/retrowser-v1.md`) loads a game build from a GitHub repo at a
pinned commit, checks every file by SHA-256 and runs it in a dedicated Web Worker inside a sandboxed iframe, with
the player's ROM. This is a **separate build target** of the web port: `-DGE_RTW=ON`, its own CMake tree, its own
output dir. The normal web site build (`scripts/web/build.sh`) is byte-identical with or without this work.

```sh
GE_DEV=<container> bash scripts/rtw/build_rtw.sh      # -> build/rtw/{ge007.js,ge007.wasm,ge007.romfill,retrowser.json}
# needs data/ge007.pal-final.z64 (the dev ROM copy, for the template strip and the leak scans) and a
# retrowser checkout (RTW_REPO, default $HOME/Code/retrowser) for rtw-hash / rtw-check
node <retrowser>/tools/rtw-check.ts build/rtw --rom <roms>/GoldenEye_007_Europe.z64   # leak scan (local only)
scripts/web/lockrun.sh /tmp/goldeneye-chrome.lock node scripts/rtw/e2e_rtw.mjs dev build/rtw port_work/rtw/dev
scripts/web/lockrun.sh /tmp/goldeneye-chrome.lock node scripts/rtw/e2e_rtw.mjs offline build/rtw port_work/rtw/off
```

## Published output (G13)

The built files are committed on the default branch `web` itself, so the plain link `Sorikairox/goldeneye-web-port`
works on retrowser.app: `retrowser.json` at the repo root (file paths `retrowser/ge007.*`) and the files in
`retrowser/`. The orphan branch `rtw-dist` carries the same files with the manifest beside them (link form
`owner/repo@rtw-dist`). Both are refreshed by

```sh
RTW_DIST_TRAILER='...' bash scripts/rtw/publish_rtw.sh --dist   # build_rtw.sh, copy, hash, check --rom, romscan, rtw-dist commit
git add retrowser.json retrowser && git commit                   # one commit on web
```

Never with a ROM, sidecars, saves or game screenshots. `ge007.romfill` is offsets only (the script checks its exact layout).
The one picture is the cartridge label (G17, below): `retrowser/cover.png` (and `cover.png` on `rtw-dist`), copied from
the committed source art `art/cover.png` (outside `retrowser/`, which the script wipes).
The game icon (G18) is `retrowser/icon.png` (and `icon.png` on `rtw-dist`), copied from the site's own icon
`port/web/icon-512.png`.

## Manifest (`scripts/rtw/retrowser.template.json`, hashed by rtw-hash)

| Field | Value | Why |
|---|---|---|
| `id` | `goldeneye-eu` | saves are keyed by it; never change it |
| `rom` | `.z64/.n64/.v64`, SHA-1 `167c3c43…` (EU), byteswap auto | the module also refuses any other ROM with a plain error |
| `files` | `ge007.wasm` (template), `ge007.js` (glue), `data: ge007.romfill, ge007.netregions` | the fill table is offsets only (rtw-check `--rom`: 0 ROM blocks); netregions = the online state-hash ranges of this build's link map (`scripts/web/netregions.py`) |
| `module` | factory `createGoldenEye`, loop `mainloop` | `main()` installs `emscripten_set_main_loop` (webkernel.c) and returns |
| `video` | `webgl` | WebGL2 on the host's OffscreenCanvas |
| `hz` | 50 | PAL; informative (the module paces itself) |
| `memory` | 384 / 384 MiB, `memory64: true` | the `-sMEMORY64=2` (lowered) shape; module-defined memory; 2112 MiB before G12 |
| `save` | `eeprom`, 512 bytes | the cart's real 4 Kbit EEPROM, native byte order (below) |
| `controls.extra` | 16 Reload, 17 Crouch, 18 Screenshot, 19 Options | port actions without an N64 button; screenshot -> PNG via `rtw_export_file`; Options opens the port's options overlay (F10 on the site) |
| `options` | 4 shown + 8 `inGame` (section "Options (standard 1.2)") | the host's menu shows only start-up options; the others are set in the overlay; picture / feel settings are not in the manifest (kept by the game per player, G16) |
| `net` | `{ "players": 4, "model": "lockstep" }` | online play (section "Online play (standard 1.2)") |
| `picture` | `cover.png` (root manifest: `retrowser/cover.png`), credit `Box art © Nintendo / Rare; James Bond © Danjaq / MGM · image from libretro-thumbnails` | standard 1.2 cartridge label (G17): the official EU box art from libretro-thumbnails (Nintendo_-_Nintendo_64, Named_Boxarts, "GoldenEye 007 (Europe).png", 512x374 PNG, 230608 B, sha256 `926e59db…`), source `art/cover.png`; `build_rtw.sh` copies it into build/rtw and `publish_rtw.sh` adds it to build/rtw (also for `--no-build`), `retrowser/` and `rtw-dist`; not passed to the module, not in the download size |
| `icon` | `icon.png` (root manifest: `retrowser/icon.png`) | standard 1.2 game icon (G18, spec 2.2: PNG, square, 192-1024 px, <= 256 KiB): the web site's own icon `port/web/icon-512.png` (512x512, 3575 B, sha256 `6708f69f…`); copied by `build_rtw.sh` / `publish_rtw.sh` exactly like the picture; tab, recent games and home-screen app icon on retrowser.app |

## Build (CMake `GE_RTW`)

- Link: `-sENVIRONMENT=worker -sMODULARIZE=1 -sEXPORT_NAME=createGoldenEye -sDYNAMIC_EXECUTION=0`
  `--js-library=port/rtw/rtw_lib.js` (in place of `-sENVIRONMENT=web`; everything else as the web build, MEMORY64=2,
  ASYNCIFY, 384 MiB fixed since G12, was 2112). The glue takes the wasm from `Module.wasmBinary` (the host passes it; no fetch).
  RelWithDebInfo like the site, DWARF stripped after the template strip (`llvm-objcopy --strip-debug`: 12.9 MB -> 3.2 MB).
- Sources: `port/rtw/rtw.c` and `port/rtw/gfx_rtw.cpp` replace `port/src/web/{webaudio,webpage,websave}.c` and
  `port/fast3d/gfx_sdl2.cpp`. **No shared source file is changed**: the hooks into the port are link wraps
  (`-Wl,--wrap`, only in this build), so the web build stays byte-identical (checked: same SHA-256 for every site
  file before and after; the site wasm carries DWARF line tables, so even a guarded `#if` block that shifts lines
  changes it). Port wraps: `inputRumbleSupported` / `inputRumble` (Rumble Pak per host pad), `osContInit`,
  `osContGetQuery`, `osContGetReadData`, `osContReset` (pads 1-3), `configRegister{Int,UInt,Float,String}`,
  `configSetOptionMeta` (deferred, below).
- SDL2 stays linked for its plain C parts; nothing initialises SDL video / audio / joystick (they need a document).
  `-Wl,--wrap` for the SDL calls that would reach them: `SDL_GL_GetProcAddress` (-> `emscripten_webgl_get_proc_address`),
  `SDL_GL_GetAttribute` (ES profile, as SDL's Emscripten context reported), `SDL_InitSubSystem` (refuses pads, fakes
  audio), `SDL_{Set,Get}RelativeMouseMode`, `SDL_GetRelativeMouseState`, `SDL_GetMouseState`, `SDL_GetKeyboardState`.
- `scripts/web/romfill.sh` and `scripts/web/romscan.sh` take their paths from env (`ROMFILL_BUILD/OUT/REPORTS`,
  `ROMSCAN_MAP`); defaults are the web build's.

## Module interface (`port/rtw/rtw.c`)

- **Start** (§4.3): `rtw_rom_buffer(size)` mallocs; `rtw_start()` hands the ROM to `gertw_boot` (rtw_lib.js), which
  checks the z64 magic and the EU SHA-1, writes `/data/ge007.pal-final.z64` (MEMFS, where romdata.c loads it), applies
  `files.data` `ge007.romfill` (`ge_web_romfill`), takes the four sidecars from **this sandbox origin's IndexedDB**
  (`ge007-rtw`/`files`, key `sidecars-v<ver>-<sha1>`; the host never reads it) or converts them in wasm (~620 steps,
  ~260 ms, yielding every 30 ms) and stores them, frees the ROM buffer, then `ge_rtw_before_main` (save, options),
  `callMain([])`, `ge_rtw_after_main`. No URL parameters, no `ENV`, no fetches.
- **Loop**: `mainloop`. webkernel.c's rAF loop runs in the worker (Chromium has rAF in dedicated workers; Emscripten
  falls back to timers elsewhere). `rtw_pause(1/0)` = the web build's hidden-tab pause (`webkernelSetPaused`: main loop
  and console clock stop; audio clock stops).
- **Video**: `gfx_rtw.cpp` makes a WebGL2 context on `Module.canvas` with `GL.createContext` (attributes as SDL's:
  depth, stencil, no AA, extensions on) and keeps it current. Buffer size = `rtw_resize` CSS size, by option 16:
  480 lines / screen (default) / screen x devicePixelRatio, capped at 2560 on the long side and 2560x1440 pixels;
  applied between frames in `handle_events`. No swap: the worker commits at the end of the rAF task.
- **Input**: pad 0 (host keyboard + touch + first gamepad) goes through the G8 touch-pad seam (`ge_web_touch` ->
  `input.c geWebTouchMerge`), so it reaches controller 0 exactly like the page's touch pad (and seat 0 gets the game's
  1.1 roles: the stick walks). Stick -127..127 scaled to the port's 80. Pads 1-3 are plain N64 pads in
  `contSnapshotFromKeyboard`, present while the host keeps sending them (2 s). Extra bits: Reload / Crouch are fed as
  the keys the port binds to those actions by default (R, Left Ctrl) through the wrapped `SDL_GetKeyboardState`;
  Screenshot -> gfx_rtw.cpp dumps the composited frame before the commit (`gfx_opengl_dump_bound_fbo`, PPM in MEMFS)
  -> PNG (zlib) -> `rtw_export_file`. The first 60 pad-0
  changes are logged (`rtw: pad0 buttons …`, test hook).
- **Mouse**: `rtw_mouse` deltas/buttons feed the port's mouse look through the wrapped SDL mouse calls. The port's
  click-to-lock grab is armed at start, so in a stage it turns relative mode on -> `rtw_pointer_lock(1)` (host shows
  "Click to use the mouse"); menus -> `rtw_pointer_lock(0)`. No absolute pointer exists in the sandbox: the front-end
  mouse cursor stays at 0,0 (an untouched mouse); menus are driven with the pad.
- **Audio** (§4.8): audio.c's web routing (`webAudioOpen/Queue/QueuedBytes/Pause`) now lands in a 32768-frame ring of
  22050 Hz s16 stereo; `rtw_audio_drain` copies it out. The depth the game's AI_LEN regulator sees is a virtual DAC
  (frames queued minus wall time x 22050, stopped while paused), i.e. real-time playback as on the N64. `rtw_audio_rate`
  is the constant 22050 (the host asks right after `rtw_start`, before audio.c opens). The Blob-URL AudioWorklet is gone.
- **Rumble**: every present pad has a Rumble Pak (option 10); motor on/off -> `rtw_rumble(port, on)`.
- **Options** (§4.9; ids and shown / inGame: section "Options (standard 1.2)" below): id -> ini key, `configSetValue` + `videoRequestLiveConfigForKey` live (All unlocked also runs
  `portAllUnlockedApply`). Values sent before the start are applied after the fill and again after `main()` (the boot's
  D546 / low-end defaults would otherwise win). 1 Aspect (`Video.AspectMode`, default Original 4:3), 2 Skip intro,
  3 Mouse sensitivity, 4 Invert mouse Y, 5 FOV %, 6 Draw distance %, 7 Texture filter, 8 All levels unlocked,
  9 Control style (`Input.ControlScheme`), 19 Vibration % (`Input.RumbleScale`; was the bool 10 Rumble before G14), 11 Screen shake %, 12 No hit
  flash, 13 HUD scale, 14 Hide crosshair, 15 Show FPS, 16 Resolution (module-side). The ini itself is not kept
  (MEMFS only); not mapped: window/fullscreen/vsync/MSAA (host's), key bindings (host's), audio device/volume (host's).
- **Imports** (`env.rtw_*`, rtw_lib.js -> `Module.rtw`): `rtw_log` (start / pad lines), `rtw_build_id` (logged at start),
  `rtw_rumble`, `rtw_export_file` (screenshots), `rtw_pointer_lock`, since G14 `rtw_option_changed` (1.1) and
  `rtw_net_send` / `rtw_net_stat` (1.2). Helper imports are named `gertw_*` (rtw-check
  rejects unknown `env.rtw_*`), helper exports `ge_rtw_*`.

## Saves: the cart's native EEPROM

GoldenEye's cartridge has a **4 Kbit EEPROM (512 bytes, 64 blocks of 8)**, not 16 Kbit: the game's own libultra
`osEepromProbe` only reports the 4K type, `osEepromWrite` refuses blocks above `EEPROM_MAXBLOCKS` (64), and file2.c
uses exactly 512 bytes (block 0: `smallSave`, 32 B; blocks 4-63: five 96-byte `save_data`). The PC port answers
`EEPROM_TYPE_16K` and keeps a 2048-byte image whose checksum words and `options` u16 are little-endian (D492/D514).

`rtw_save_ptr/size` = 512 bytes in N64 byte order. rtw.c `eepConvert`: native -> port = eepimport.c's D514 rules
(regions valid only big-endian are converted, re-stamped); port -> native = for each port-valid region swap `options`
and re-stamp both checksums with the game's `fileGenerateCRC` over the native bytes, big-endian; other regions are
copied as they are (invalid on both sides; the game wipes them, as the N64 does). Every `geEepromStore` (via the
websave.c replacement `webSaveWritten(1)`) converts the live image and bumps `rtw_save_generation` when the 512 bytes
changed. On start the host's image goes to `ge_web_eeprom_load` (the G5 seam) before `main()`. A 2048-byte emulator
`.eep` imported in the host arrives cut to 512 bytes, which is all of its content.

## Traps found (GE_RTW)

- **Template strings before the fill** (also affects the deployed web site): the template zeroes bytes of the merged
  string pool that also occur in the ROM (`Video.AspectMode`, `Input.MouseSensitivity`, `Game.SkipIntro`, … are 0 in
  the shipped wasm). C constructors run before the page / start-up fills the template, and the port's options register
  from constructors (`configRegister*` copy the key): those keys are registered broken. The web site logs it as
  `optionsoverlay: option 'Video.AspectMode' not registered` and its ini can't set those options; the same goes for
  constructor-time default strings (`Input.Bind.Back: unknown binding 'Do'`). GE_RTW fix: the `configRegister*` /
  `configSetOptionMeta` calls are wrapped and queued until the fill, then replayed in order (`rtwConfigReplay`).
  Web fix (G11, branch `web`): the strip itself no longer zeroes those pieces; the deferral here is now redundant
  but harmless (kept).
  Not fixed in either build: default *values* copied at constructor time (the bind strings) stay broken. Any log line printed before the fill can be
  cut short the same way (rtw.c logs the build id only after the fill).
- `romscan.sh` (via `deploy_check.sh`) takes the build lock itself: don't call it while holding the lock.
- The host's AudioWorklet does not load under `npm run dev` (Vite dev: "node name 'rtw-audio' is not defined"); the
  e2e dev phase sets the host's ScriptProcessor fallback (`rtw:audio` `{"fallback":true}`). The built host (dist) is fine.

## Memory (384 MiB since G12; was 2112 MiB)

Same map as the web build (docs/web/PLAN.md 2.2, one CMake): cart window 0x10000000 + 32 MiB, N64 DRAM 0x12000000 +
8 MiB, static data from 0x12800000, `-sINITIAL_MEMORY=384mb`; manifest `memory.initialMB = maximumMB = 384`. Before
G12 the DRAM sat at 0x80000000, which forced 2112 MiB. Heap high-water in the web build: address 0x155D6000 (342
MiB) in every level and MP setup. Phones: still untested on a real device (iOS Safari's limit is ~1-1.5 GiB on most
devices, so 384 MiB is now well inside it).

## Online play (standard 1.2, G14)

The host (retrowser 1.2, spec §4.12) owns the room, the WebRTC mesh and relay, the build / ROM check, the round trips
measured before the start and the **lockstep boot**: every peer, room host included, restarts from power-on with the
room host's option values and native save. The game side is the site's: `port/src/web/webnet.c` (unchanged, shared)
keeps the input ring, frame barrier, input delay, det mode and state hashes; `port/rtw/rtw_net.js` (`--pre-js`, object
`rtwNet`) is net.js's packet logic over the host's transport (as the Conker port's rtw_net.js).

- **Calls**: `rtw_net_session(players, me)` (after `rtw_option`, before `rtw_start`): `rtwNet.begin` reads options 17
  (Online play; off = every peer plays alone) and 18 (input delay, 0 = auto), D = clamp(ceil(`rtw_net_stat(0, 3)` / 40)
  + 1, 2, 8) = net.js autoDelay with the session's worst p90 (the same number on every peer, no message). Right before
  `main()` (after the template fill) `rtwNet.beforeMain` adds the hashed ranges (files.data `ge007.netregions` + the live
  EEPROM image) and calls `_ge_net_session(mask, 1 << me, D, 10)`; the transport (5 ms pump, 250 ms time sync) starts
  after `main()`. `rtw_net_peer(i, 0)` starts the lost-player agreement. `rtw_net_buffer` / `rtw_net_recv`: a static
  64 KiB buffer, copied out at once; a packet that brings the awaited input kicks the det loop (`_ge_net_kick`) in a
  **microtask**, never inside `rtw_net_recv` (Asyncify). Sends: `_ge_rtw_net_send(peer, ch, len)` from a static
  64 KiB buffer -> `env.rtw_net_send`; webnet.c's send point (EM_ASM `Module.netAfterTick`) reaches `rtwNet`.
- **Packets**: channel 0 = net.js's input packet without type / sender bytes (ack, first frame, unacked inputs up to 64,
  newest hash, last retrace, time-sync margin), sent at each local sample, repeated after 8 ms, keep-alive 30 ms;
  channel 1 = binary lost / held / drop / desync / pages (per-page hashes raw; the report names the first differing page
  with its game symbols from the netregions S lines, or the N64 DRAM address).
- **Gone versus net.js**: rooms, codes, RTCPeerConnection, pings, the `config` message, `_ge_net_eeprom_buf` (the host
  hands over the room host's native save: `rtw_save_load` -> `ge_web_eeprom_load`), `GENet.hold`, the hidden-tab ticker.
- **Session rules in rtw.c**: options 8 / 9 are not applied (webnet.c `geNetApplySessionConfig` pins all-unlocked off and
  OG controls, and runs again after the start-up option pass); `rtw_option` after the start is ignored except for 16
  (the 1.2 host locks options in a session anyway); the host's pads 1-3 never override controllers or presence (the
  session's input ring does); no option changes are reported to the host; each player's own settings (section
  "Whose setting it is (G16)") come from its own settings record; the overlay opens with only those editable; no
  Rumble Pak (`inputRumbleSupported` 0).
- **State hash without the display lists (G16)**: the game's display-list output lives in the hashed N64 DRAM (gfx pool
  `g_GfxBuffers` in the stage bank, vtx pool `g_VtxBuffers` at DRAM + 0x700000) and a few static records describe it
  (`g_GfxMemPos`, `g_gfxTaskSettings`: the RSP task with the list's length). Per-player HUD scale changes them, so in
  a session `__wrap_geNetRetraceDone` blanks those ranges for the hash and restores them (the record ranges come from
  rtw_net.js `HASH_MASK` via the netregions symbols, `ge_rtw_hash_mask_add`); `__wrap_geNetHashBytes` (webkernel.c's
  stack hash S only; webnet.c's own calls are in its file, not wrapped) reads saved fiber words that point into the
  two pools as 0 (a waiting frame holds a list position); `__wrap_mempResetBank` zeroes the old gfx pool when the stage
  bank is reset (a new stage), else each peer's last lists leaked into unmasked memory for ~2 s (seen once in a test:
  transient desync at N64 DRAM 80103000, inside the old pool). A real divergence elsewhere is still found (DESYNC test:
  DRAM 80200000).
- **Tests**: `scripts/rtw/rtw_net_e2e.mjs <build dir> <out> [rom]` (env PLAYERS 2-4, FRAMES, DROP, DESYNC, LEAVE, SOLO,
  RTW_HOST_DIST): pages in one muted headless Chrome against the host's built dist with its in-process room server.

## Options (standard 1.2: start-up options only in the host's menu)

retrowser 1.2 (commit 39060f5) shows only settings needed before the game starts; an option with `"inGame": true` is
hidden there, set in the game's own menu, reported with `env.rtw_option_changed`, kept by the host and handed back at
the next start (and to every player of a lockstep session). This build keeps no ini (MEMFS), so `inGame` is also what
keeps those settings across starts.

- **Shown**: 16 Resolution (module-side buffer size; no overlay row), 2 Skip intro (read once, on the first main menu,
  lv.c), 17 Online play, 18 Online input delay (0 = auto).
- **inGame** (the room host's in a session): 3 Mouse sensitivity (1..500), 4 Invert mouse Y, 5 Field of view (game-side
  FOV, frustum culling), 6 Draw distance (game-side far clip), 8 All levels unlocked, 9 Control style, 11 Screen shake
  (fr.c, 0..1000 % = ini 0..10), 12 No hit flash.
- **Not in the manifest since G16** (each player's own, kept by the game): 1 Aspect ratio, 7 Texture filter, 13 HUD scale,
  14 Hide crosshair, 15 Show FPS, 19 Vibration % (Input.RumbleScale). Players who had set them in G14 / G15 get the
  defaults again until they set them in the overlay (the host no longer sends them).
- **Dropped**: 10 Rumble (a bool), replaced by 19.

### Whose setting it is (G16, user decision 2026-11-05)

In a lockstep session everything that changes how much a player sees follows the room host, so nobody gets an edge;
every other picture or feel setting stays each player's own. Offline every setting is the player's. What the code does:

| Setting | What it does | Online |
|---|---|---|
| 1 Aspect ratio | Offline Hor+: Native widescreen (on by default) projects the world at the output aspect (Fill = window, 16:9, 21:9; Original = 4:3 with bars). A session pins Native widescreen and Widescreen auto off (`geNetApplySessionConfig`): the world is always projected at 4:3 and the aspect only stretches the picture (and HUD) to the rectangle or adds bars. No wider view online. | own |
| 5 Field of view | `portScaleFovY` (projection), frustum culling, room pool (`portRoomPoolScale`): a wider view, and game state | room host's |
| 6 Draw distance | far clip and room culling (`drawdistgameplay.c`, hashed): farther view, game state | room host's |
| 12 No hit flash | drops the white / red overlay that hides the view while hit; writes the player's fade state (`currentPlayerSetFadeColour`), which the game reads (bondview2.c `colourscreenfrac` tests) | room host's |
| 11 Screen shake | `viShake` amplitude (fr.c, game state); shake hides the view | room host's |
| 16 Resolution | buffer size only (same projection, same view area) | own (the record's copy) |
| 7 Texture filter, 15 Show FPS | renderer / overlay only | own |
| 13 HUD scale | the game's HUD display lists only (left out of the hash, above) | own |
| 14 Hide crosshair | offline the game's switch; online the crosshair's opacity 0 (the switch would also skip the crosshair's texture load into the game's texture cache, which is game state) | own |
| 19 Vibration % | motor strength; no Rumble Pak online | own |
| 3 / 4 mouse | the mouse never reaches the game online (pure pad) | room host's (no effect) |

- **Settings record**: rtw_lib.js `$gertwSettings`, one object `{ id: value }` in this sandbox origin's IndexedDB
  (`ge007-rtw` / `files`, key `settings-v1`, beside the sidecar cache; the host never reads it). Read at every start
  before `ge_rtw_before_main` (`ge_rtw_own_option` per entry), sessions included; written on every change
  (`geRtwPump` -> `gertw_settings_put`). 16 Resolution: outside a session the host's value is copied in; a session uses
  the copy instead of the room host's.
- **A session never overwrites the stored choice**: the host boots a session with the room host's values without storing
  them, and the module reports nothing during a session; after leaving, the player's own values are back (test).
- **Overlay in a session**: the rtw build compiles a copy of optionsoverlay.c (`scripts/rtw/rtw_overlay_src.py`, exact
  replacements, fails on any upstream drift; the shared file is untouched). It opens during a session; the root list
  shows Video, Mouse, Controller, Game; rows: own settings (editable), the room host's mapped settings (locked, tip "Set
  by the room host during online play."), Back, the Player 1 link; everything else is hidden (Bond-file values,
  presets, resets, bindings, Restart / Quit). `rowSetCommit` / `rowAdjust` refuse locked and hidden rows; `geRtwPump`
  puts back any room-host value that changed by another path. frontoptions.c (hashed) sees no overlay during a session
  (CMake compile definitions: `optionsOverlayIsOpen` / `optionsOverlayToggle` -> `geRtwOverlayOpenForGame` /
  `geRtwOverlayToggleForGame`), else file select would reset its idle timer on one machine only.
- **Tests**: `rtw_net_e2e.mjs` with `RTW_OPTIONS_HOST` / `RTW_OPTIONS_GUEST` (view options), `RTW_OWN_HOST` /
  `RTW_OWN_GUEST` (own settings), `LEVEL=1` (into Dam, aim and fire), `OVERLAY=1`, `GUEST_LEAVE=1`;
  `rtw_options_e2e.mjs` (record vs host store, both after a reload).
- **The game's menu**: the port's options overlay (F10 on the site) opens with extra button 19 Options (host key 4 by
  default; during a session too, since G16). While it is open host pad 0 drives it through the wrapped `SDL_GetKeyboardState` (stick / D-pad = arrows, A =
  Enter, B = Backspace: back / close from the top page, Start closes); buttons still held when it closes are held back
  from the game until released. `geRtwPump` (once per frame, gfx_rtw.cpp `handle_events`) polls every mapped key and calls
  `rtw_option_changed` on a change of a room-host option outside a session (rounded, clamped to the manifest range; host
  values are not echoed), or stores an own setting in the settings record.
- **Fixed on the way**: "N64 3-point" texture filter (option 7 = 2) stopped the game: fast3d's fragment shader called
  `mix(vec4, vec4, int)`, which GLSL ES 3.00 lacks. G14 worked around it in the rtw build only (a wrapped
  `glShaderSource` rewrote the call); since G15 the source itself uses `float(three_point_filter)` (gfx_opengl.cpp, web,
  rtw and native alike) and the wrapper is gone (`__wrap_SDL_GL_GetProcAddress` returns the WebGL entry point as is).
- **Test**: `scripts/rtw/rtw_options_e2e.mjs <build dir> <out> [rom]`.

## What does not fit v1 (host would need)

- Mouse in menus: the sandbox only has relative motion while captured; the port's front-end mouse pointer is unused.
- Settings files: the port's ini (bindings, PC options beyond the mapped ones) is not persisted (the mapped ones are kept by the host since G14: `inGame` options); the host would need an
  options blob or a second save kind ("config") to keep it.
- Recordings (`.gerec`, G6 recorder) would go through `rtw_export_file`, but replay needs an import path into the
  module (the host has none for non-save files).
- `rtw_audio_rate` is read once right after `rtw_start`: a module that learns its rate only during an asynchronous
  start must hard-code it (fine here: 22050).
