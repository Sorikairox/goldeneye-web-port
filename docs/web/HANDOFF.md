# GoldenEye 007 (EU) web port: hand-off

Plan and decisions: `docs/web/PLAN.md`. Rules: `orchestrator.md`. Playbook: `WEB_PORT_ORCHESTRATOR.md`.
Session log: `docs/web/sessions.tsv`.

## Current state (after G3 .. G18, 2026-11-06)

G18: **retrowser game icon** (section "Game icon (G18)" below): the manifest's standard 1.2 `icon` is the web site's
own 512x512 icon. rtw output only; the site build is byte-identical. Not deployed.

G17: **retrowser cartridge picture** (section "Cartridge picture (G17)" below): the manifest's standard 1.2 `picture`
is the official EU box art. rtw output only; the site build is byte-identical. Not deployed.

G16: **retrowser online play: view settings follow the room host, picture / feel settings are each player's own**
(section "Whose setting it is online (G16)" below; RTW.md "Whose setting it is (G16 ...)"). rtw build only; the site
build is byte-identical. Not deployed (the retrowser build is published on `web` / `rtw-dist`).

G15: **"N64 3-point" texture filter no longer stops the site** (section "N64 3-point filter (G15)" below): the fragment
shader's two `mix(..., three_point_filter)` calls take `float(three_point_filter)`; the rtw-only shader rewrite is gone.
Built and tested, **not deployed** (the user deploys).

G14: **retrowser build plays online and follows the 1.2 options rule** (section "retrowser online play and start-up
options (G14)" below): standard 1.2 lockstep for 2-4 players through the host (rtw build only), and the host's menu
keeps only start-up options; the rest are `inGame` options set in the port's options overlay (extra button Options).
The site build is byte-identical (proof below). Nothing deployed.

G13: **retrowser build on `web`** (section "retrowser build on the default branch (G13)" below): `web-rtw` fast-forwarded
into `web`; the built module is committed on `web` (root `retrowser.json`, files in `retrowser/`), so retrowser.app
loads the plain link `Sorikairox/goldeneye-web-port` (default branch `web`). `scripts/rtw/publish_rtw.sh [--dist]`
rebuilds and refreshes it (and `rtw-dist`). The site build is byte-identical (proof below).

G12: **memory 2112 -> 384 MiB** (section "Memory map for phones (G12)" below): N64 DRAM moved from 0x80000000 to
0x12000000 and the static data from 0x20000000 to 0x12800000; fast3d's 256 MiB texture upload buffer now grows on
demand. Same frames at the same det retraces as before, saves byte-identical, sidecars and their cache unchanged.

G11: **option keys fixed on the site** (section "Option keys and the template strip (G11)" below): the template strip
no longer zeroes short coincidental pieces of the string pool / port tables, so every option registers, the ini
loads and saves all keys, and the key bindings have their real defaults.

G10: **retrowser.app v1 build** (was branch `web-rtw`; on `web` since G13): a separate build target `-DGE_RTW=ON` (`scripts/rtw/build_rtw.sh` -> `build/rtw/`), the game as a worker
module for the retrowser host. Details, decisions and what v1 lacks: `docs/web/RTW.md`; section "retrowser build
(G10)" below.

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

## Cartridge picture (G17, 2026-11-05)

retrowser standard 1.2 (retrowser 82b624b) shows a manifest `picture` as the label of a drawn N64 cartridge. User
decision: the official box art. File: libretro-thumbnails, Nintendo_-_Nintendo_64 / Named_Boxarts /
"GoldenEye 007 (Europe).png" (PNG 512x374, 230608 B, sha256 926e59db5883cc5c…), committed as source art
`art/cover.png` (outside `retrowser/`, which `publish_rtw.sh` wipes).
- `scripts/rtw/retrowser.template.json` has `picture` { path `cover.png`, credit `Box art © Nintendo / Rare; James
  Bond © Danjaq / MGM · image from libretro-thumbnails` }. `build_rtw.sh` copies the art into build/rtw before hashing.
  `publish_rtw.sh` copies it into build/rtw too and puts the template's `picture` into build/rtw/retrowser.json, then
  re-hashes it (so `--no-build` works on a build made before G17; the game files are unchanged), copies it to
  `retrowser/cover.png` (root manifest path `retrowser/cover.png`) and adds `cover.png` to `rtw-dist`.
- README retrowser section: the credit plus "the box art belongs to its owners and is only used here to identify the game".
- Run: retrowser `npm run build` (dist/tools/rtw.mjs), then `publish_rtw.sh --no-build --dist` (build/rtw had the
  same bytes as the committed `retrowser/` files of 51bedc95; no wasm rebuild). `rtw.mjs check . --rom
  ~/roms/GoldenEye_007_Europe.z64` OK (build id 25403c4d…), build/rtw and the rtw-dist tree OK (407555b5…); romscan
  PASS; rtw-dist e2b5d4b1. build-web/site untouched (16 files, same SHA-256 before and after).
- Changing the picture: replace `art/cover.png` (PNG/JPEG/WebP, <= 512 KiB, <= 4096 px; N64 label crops to 4:3),
  update the credit in the template + README, run `publish_rtw.sh --no-build --dist`.

## Game icon (G18, 2026-11-06)

retrowser standard 1.2 (retrowser 21a5fab, spec 2.2) adds `"icon": { path, sha256, size }`: PNG only, square, 192-1024 px,
<= 256 KiB (512 recommended), path relative to the manifest and ending in `.png`; shown in the browser tab, the home
page's recent games and as the home-screen app icon. User decision: the site's own icon `port/web/icon-512.png`
(512x512 RGB PNG, 3575 B, sha256 6708f69fac5b3121…), used as is (no copy under `art/`).
- Same way as the picture: template `icon` { path `icon.png` }; `build_rtw.sh` copies `port/web/icon-512.png` to
  build/rtw/icon.png; `publish_rtw.sh` copies it into build/rtw too, puts the template's `picture` and `icon` into
  build/rtw/retrowser.json, re-hashes, copies it to `retrowser/icon.png` (root path `retrowser/icon.png`) and adds
  `icon.png` to `rtw-dist`.
- Run: retrowser `npm run build`, then `publish_rtw.sh --no-build --dist` (build/rtw had the same bytes as the
  committed `retrowser/` files and no code changed since 2fcd839a; no wasm rebuild). rtw check --rom OK, build id
  853d02c6…, 0 errors/warnings, icon 512x512, picture png 512x374; romscan PASS; rtw-dist 89025c2d.
- Note: the bundled `dist/tools/rtw.mjs check` (tools/rtw-cli.ts) prints only `OK <manifest>  build id …`, without the
  picture / icon suffix; `node tools/rtw-check.ts` prints `… picture png 512×374  icon 512×512`, and
  `rtw.mjs check --json` reports icon {width 512, height 512}. A retrowser CLI gap, not a GoldenEye one.
- Changing the icon: replace `port/web/icon-512.png` (it is also the site's icon) or point the scripts at another
  square PNG, run `publish_rtw.sh --no-build --dist`.

## Whose setting it is online (G16, 2026-11-05)

User decision: in a lockstep session everything that changes how much a player sees follows the room host; every other
picture or feel setting stays each player's own; offline every setting is the player's. Details, the per-setting table
and the mechanisms: `docs/web/RTW.md` "Whose setting it is (G16 ...)" and "State hash without the display lists".
- **What the code does**: Aspect ratio is Hor+ offline (Native widescreen projects at the output aspect), but a session
  pins Native widescreen off, so online the world is always 4:3 and the aspect only stretches / adds bars: per player.
  FOV and draw distance widen / deepen the view and are game state: room host's. No hit flash hides nothing while hit
  and writes the player's fade state the game reads: room host's (the user's list had it as cosmetic; it is an edge and
  game state). Resolution is the buffer size only: per player (the record's copy in a session). Mouse settings have no
  effect online (pure pad). Screen shake stays the room host's (game state).
- **Final lists**: room host's online (manifest inGame): 5 FOV, 6 draw distance, 12 no hit flash, 11 screen shake, 3 / 4 /
  8 / 9 (and the start-up 2 / 17 / 18). Each player's own (not in the manifest, the game's settings record): 1 aspect,
  7 texture filter, 13 HUD scale, 14 hide crosshair, 15 show FPS, 19 vibration; 16 resolution (start-up option, own copy).
- **2fcd839a feature**: rtw.c (settings record API, apply order host -> record, pump: own -> record, room host's -> host
  outside a session / put back inside; crosshair as opacity online; no Rumble Pak online; overlay row rules; hash wraps
  `geNetRetraceDone` / `geNetHashBytes` / `mempResetBank`), rtw_lib.js `$gertwSettings` (IndexedDB `ge007-rtw` / `files`
  / `settings-v1`), rtw_net.js (hash-mask symbols, test aids optionNow / settings / setOwn), CMake GE_RTW (overlay copy
  from scripts/rtw/rtw_overlay_src.py, frontoptions.c compile definitions, three wraps), manifest without 1/7/13/14/15/19.
- **53842a85 tests**, **51bedc95 publish**: rtw-dist 00f7ffb4 (build id eeb9af93...), root build id 0df87453...,
  `rtw.mjs check . --rom` OK.
- **Found on the way** (each fixed in the rtw build; first hash runs failed): the gfx task records `g_gfxTaskSettings`
  (list length) and the fibers' saved list pointers (S) differ with HUD scale; Hide crosshair skips the crosshair's
  texture load (texture cache = game state); a new stage leaves each peer's old lists in freed stage memory (transient
  desync at DRAM 80103000 inside the old gfx pool, ~2 s); the overlay's first page row was a separator.
- **Tests** (muted headless Chrome; net runs through the chrome lock; retrowser dist as in ~/Code/retrowser/dist):
  - `rtw_net_e2e.mjs` 2 players, host FOV 120 / draw 400 vs guest 80 / 150, own aspect 1 vs 0, filter 1 vs 0, HUD 100 vs
    150, crosshair shown vs hidden, FPS off vs on, vibration 50 vs 0, `LEVEL=1 FRAMES=1500 OVERLAY=1 GUEST_LEAVE=1`, on
    build/rtw and on the repo-layout copy (git archive): both peers FOV 120 / draw 400, own values kept, into Dam (aim +
    fire), 64/64 hash points equal (1630..2260), overlay in the session: FOV / draw / no hit flash locked (tip shown),
    HUD 150 -> 135 stored in the record, 64/64 equal after; guest leaves: 80 / 150 back, its stored host options
    unchanged. ALL PASS. Two more 2500-retrace runs: desync none to 4020.
  - one own setting at a time vs the host (13 / 15 / 1 / 14 / 7 / 19) in Dam: all equal after the fixes; 4 players
    (own aspect 21:9 / trilinear / HUD 75 / no crosshair / FPS / no vibration on the guests, LEVEL, DROP): 64/64, drop
    agreed, 64/64 after; 3 players DROP + DESYNC: report at N64 DRAM 80200000 (real divergence still found); 2 players
    LEAVE + SOLO: PASS; resolution 2 vs 0 kept per peer in a session.
  - `rtw_options_e2e.mjs` ALL PASS (default steps; and Display mode -> Original N64: 1 = 1, 7 = 2 in the record, 6 = 100 at
    the host, all back after a reload). `e2e_rtw.mjs dev` 12/12 PASS, `offline` (repo-layout copy) PASS.
  - Site byte-identical: HEAD pinned at 51e8bbd2 (the live build's VERSION_HASH) + `git checkout web -- .`,
    `scripts/web/build.sh`: all 16 files of build-web/site equal G15's (wasm 61718646..., romfill d09588da..., js
    e45e2488..., sw-build 25f5feeb4238841a).
- **Open**: players who set 1 / 7 / 13 / 14 / 15 / 19 in G14 / G15 get the defaults once (the host no longer sends
  them). The test's overlay key route depends on the overlay's page layout (OVERLAY_KEYS env overrides it).

## N64 3-point filter (G15, 2026-11-05)

- **Bug**: `Video.TextureFilter` = 2 ("3-point" in F10 > Video > Texture filter, and also what the Display mode preset
  "Original N64" sets) makes fast3d generate `hookTexture2D(..., in int three_point_filter)` returning
  `mix(SAMPLE_TEX(...), filter3point(...), three_point_filter)` (plain and blur variants, gfx_opengl.cpp ~450 / ~461).
  GLSL ES 3.00 has no `mix(vec4, vec4, int)` and no implicit int -> float, so WebGL refused the shader and
  `gfx_opengl_create_and_load_new_shader` called sysFatalError: the game aborted.
- **Repro on the deployed build** (build-web/site = live: wasm 18e4d479, sw-build d903878e5228234a): Dam warp, F10 at 9 s,
  Enter (Video), Right on Display mode at 13 s: `video: applied Original N64 preset` at 13.13 s, then `ERROR: 0:28: 'mix' :
  no matching overloaded function found`, `[FATAL] Fragment shader compilation failed`, `Aborted()` from
  sysFatalError (port_work/g15/repro_console_before.log).
- **Fix (51e8bbd2)**: both calls use `float(three_point_filter)` (the uniform is 0 or 1; desktop GLSL accepted the int only
  by implicit conversion, `float()` is valid in every version). The rtw build's `rtwShaderSource` wrapper (71875c00) no
  longer matched anything and is removed; `__wrap_SDL_GL_GetProcAddress` is back to its pre-G14 form.
- **Tests** (port_work/g15/): the same F10 route on the new build: preset applied at 15.1 s, no shader error, Dam plays
  on in 4:3 with 3-point. All four filters (0 Nearest, 1 Bilinear, 2 3-point, 3 Trilinear), config seeded through the
  page's pending-save copy (localStorage `ge007-pending-<sha1>` = `{ini: "[Video]\nTextureFilter = N"}`, checked by reading
  /data/ge007.ini in the page): Dam warp 24 s with walk/fire, and the real menu route with GE_MPVIRT=3 (title, file
  select, mode select -> Multiplayer, MP options, Temple 4P match, the in-match MP menu) 64 s: 0 shader failures, 0 FATAL,
  0 exceptions, 0 WebGL warnings, `gl.getError()` empty at the end; screenshots checked. (The ms-timed pad route runs ~4 s
  early with Nearest, which boots faster on SwiftShader: there the route needs `20000:START;24000:A` before the stick.)
  save_test all PASS; e2e_replay (75 s recording: 70/70 identical, mutation first differs at 2950); netpair 2P 120 s
  through the chrome lock: 3236/3236 retraces identical, 0 DESYNC; offline_test PASS (boot from caches, sidecars from
  IndexedDB); deploy_check OK, romscan 4 runs / 280 B (allowlisted). Native aarch64 reference build (native_ref.sh)
  compiles, GLSL 130, runs Dam with TextureFilter = 2 (25/25 shaders warmed, no error).
- **rtw**: publish_rtw.sh --dist: rtw check --rom OK (root build id b9a1d426..., build/rtw 8bbc85ca...), rtw-dist 85a22e9c;
  rtw_options_e2e ALL PASS (its default steps choose Original N64 = option 7 = 2, kept after a reload), e2e_rtw offline
  PASS (repo-layout copy).
- **Site build** (built at 51e8bbd2): wasm 61718646..., js e45e2488... (unchanged), romfill d09588da...; build-web/site
  sw-build 25f5feeb4238841a (deploy_web.sh re-hashes with its BUILD.txt, so the live id differs; see the session report).

## retrowser online play and start-up options (G14, 2026-11-05)

Details: `docs/web/RTW.md` sections "Online play (standard 1.2, G14)" and "Options (standard 1.2 ...)".
- **f4461d77 online play** (rtw target only): manifest `"net": { "players": 4, "model": "lockstep" }`, options 17 Online
  play (on) and 18 Online input delay (0 = auto), files.data `ge007.netregions` (build_rtw.sh runs netregions.py on the rtw
  map; publish_rtw.sh copies it). rtw.c exports `rtw_net_session/peer/buffer/recv`, imports `rtw_net_send/stat` (via
  `ge_rtw_net_send` / `_stat`, static 64 KiB buffers). webnet.c unchanged (ring, barrier, D, det, hashes). New
  `port/rtw/rtw_net.js` (`--pre-js`): net.js's packet logic on the host's transport (Conker's rtw_net.js adapted). D =
  clamp(ceil(rtw_net_stat(0,3)/40)+1, 2, 8). Session started right before main() (after the fill); kick in a microtask.
  Session rules: options 8/9 pinned by geNetApplySessionConfig (re-run after the option pass), later rtw_option ignored
  (except picture-only 1/7/15/16), host pads 1-3 never override the ring. `_ge_net_eeprom_buf` / `GENet.hold` unused
  (the host hands over the room host's native save and boots the session).
- **71875c00 options** (retrowser 39060f5, user request via the coordinator): shown 16 Resolution, 2 Skip intro, 17
  Online play, 18 Online input delay; inGame 1, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 19 Vibration %; dropped 10
  Rumble (bool, replaced by 19). Overlay opened by extra button 19 Options, driven by host pad 0 through the wrapped
  keyboard state; `geRtwPump` reports every change with `rtw_option_changed`. Found + fixed (rtw only): "N64 3-point"
  texture filter aborted the game (GLSL ES `mix(vec4, vec4, int)`); a wrapped `glShaderSource` casts to float. The site
  had that latent bug too; G15 fixed it at the source and removed the wrapper (section "N64 3-point filter (G15)").
- 3dbf47cd root build (publish_rtw.sh --dist; rtw-dist 8848c8ff), `rtw.mjs check . --rom ~/roms/...` OK (build id
  7c06fc68).
- **Tests** (muted headless Chrome through the shared chrome lock; retrowser dist d7782274 copied to the scratchpad;
  `scripts/rtw/rtw_net_e2e.mjs`, ~1 min each):

  | Players | Hash points equal (retraces) | Drop | Other |
  |---|---|---|---|
  | 2 | 64/64 (40..670); 64/64 (1950..2580) in a 2500-retrace run | - | P1 leaves -> alone; solo 49.9 retraces/s, no session; repo-layout copy 64/64 |
  | 3 | 64/64 (50..680) | P3 closed: neutral from 686 on both, 64/64 after (440..1070) | desync report: page 721 N64 DRAM 80200000 on both |
  | 4 | 64/64 (40..670) | P4 closed: neutral from 682 on all 3, 63/63 after (450..1070) | desync report as above; solo OK |

  Every run: same D everywhere (2, loopback p90 1-3 ms), session set at retrace 0, room host's options on every peer,
  0 bad / refused packets, 0 glue errors. `rtw_options_e2e.mjs`: host menu shows only the 4, overlay by key 4, changes
  of options 1/6/7/9 reported + stored by the host, back in the game after a reload, ALL PASS. e2e_rtw dev 12/12 PASS,
  offline (repo-layout copy) PASS.
- **Site byte-identical**: HEAD pinned at 8137fa19 + `git checkout web -- .`, `scripts/web/build.sh`: wasm 18e4d479,
  js e45e2488, romfill d90acc60, netregions, sw.js and all page files equal the live site's (downloaded; live
  index.html differs only by the two snippets Netlify injects), sw-build id d903878e5228234a; deploy_check / romscan OK.
- **Host gaps**: none blocking (see the session report). The drop still waits for the host's 15 s "gone" when a page
  closes without a goodbye (Conker's gap 1).

## Memory map for phones (G12, 2026-11-05)

- **Before:** `INITIAL_MEMORY` 2112 MiB because the DRAM arena sat at 0x80000000 (+8 MiB) and the static data at
  0x20000000 (after a 256 MiB cart window). Phones / 32-bit browsers refuse a 2 GiB+ ArrayBuffer.
- **Now (web and rtw, same CMake):** cart window 0x10000000 + 32 MiB, DRAM `ge_dram_window` 0x12000000 + 8 MiB (tail
  of the cart section, `scripts/web/gen_cart_wasm.py ... DRAM_BASE`), static data from 0x12800000, 4 MiB stack, heap
  from ~0x13FA0000; **`-sINITIAL_MEMORY=384mb`** (fixed). Table: docs/web/PLAN.md 2.2.
  - Why 0x12000000: 16 MiB aligned (so `ptr & 0xFFFFFF` = physical offset and `phys | base` = pointer, as with
    0x80000000 and native 0x70000000); >= 0x10000000 (fast3d never takes it for a segmented address or a physical
    offset; a base below 0x01000000 would have broken `& 0x00FFFFFF` users like objecthandler_2.c, one in
    0x01..0x0F000000 would read as a segment); right after the ROM + sidecars (~17 MiB used of the 32).
  - Code: CMake `GE_WEB_DRAM_BASE` -> `PORT_DRAM_V1_BASE = PORT_DRAM_K0_BASE = PORT_CART_END = 0x12000000` (portaddr.h
    gets `PORT_CART_END`, default 0x20000000 native); the only live game site is title.c's gun-barrel vertex pointer
    (`| (u32)PORT_DRAM_K0_BASE`, native value unchanged; the four `| 0x80000000` texnum reads in bg.c / propobj.c are
    N64-only `#else` branches); fast3d `gfx_tex_source_is_c_array` cart test up to `PORT_CART_END`; `dram.c` checks
    `ge_dram_window` (the heap is above the arena now); `romdata.c` window size check; `webnet.c` DRAM hash region and
    desync test byte; `net.js` DRAM label; srcfix `animations_frame_buffer`; `tools_web/romfill.c` image start
    0x12800000 and pointer-like range 0x10000000..0x20000000.
  - **fast3d texture upload buffer** (`gfx_pc.cpp`): was `malloc(min(8192, GL max)^2 * 4)` = 256 MiB at init on
    every GPU with an 8k limit (the largest allocation of the web build); now 1 MiB, grown before each import to
    (size_bytes + line sizes) x 8 (4-bit -> RGBA32 worst case), new bytes zeroed. Native gets the same (less memory).
- **Measured** (`port_work/g12/`): highest touched heap address 0x155D6000 (342 MiB) in all 21 solo levels, all 14 MP
  setups (2-4 players) and at boot (sidecar conversion), so 384 MiB leaves ~42 MiB. Chrome (probe.mjs `PROBE_MEM=1`,
  ps RSS of the probe's own Chrome tree), Dam 40 s / 4P Temple 40 s: renderer process swiftshader 410 / 399 MiB ->
  366 / 367 MiB, ANGLE Metal 415 / 399 -> 367 / 369 MiB; GPU process unchanged (~400 swiftshader, ~180 Metal).
- **Same behaviour:** det runs (4P menu route, `darkdet.sh` with `GE_DETSTOP` 1800 / 2600 / 3400) give byte-identical
  screenshots on the G11 build and this one; state hashes differ by design (they include pointer values). netdet a1
  vs a2 5864/5864 identical; recorder record/replay 68/68, mutated replay diverges at 2950; save_test 12/12 PASS and
  its exported EEPROM is byte-identical to the G11 build's; sidecars byte-identical (same SHA-256), so the IndexedDB
  sidecar cache (`sidecars-v<GECONV_VERSION>-<sha1>`) stays valid, no version bump; fillcheck equal; romscan 4 runs /
  280 B, deploy_check OK; route_dam, level + MP sweeps (no trap), phone_test (Android), offline_test PASS; native
  aarch64 reference build compiles and reaches Dam gameplay.
- **rtw** (branch `web-rtw` rebased onto this): manifest memory 384 / 384 MiB (`scripts/rtw/retrowser.template.json`),
  rtw-dist rebuilt; rtw-check `--rom` EU ROM OK, deploy_check OK; e2e on the retrowser host + GitHub mock: dev phase
  12/12 PASS (boot, pad input, pointer lock, screenshot, options, audio, native save valid + handed back, sidecars
  from IndexedDB), offline phase 6/6 PASS.
- **Note:** `.gerec` recordings made with an older build report a difference on replay (checkpoint hashes include
  pointers); recordings are per build already (the file carries the build id).
- Fill table now 7,820 ranges / 135,958 B (pointer values no longer coincide with the ROM's 0x80xxxxxx words).

## Option keys and the template strip (G11, 2026-11-05)

- **Bug** (found in G10): the site's template zeroed bytes of the merged string pool and of port C tables that
  happened to match the ROM (pass 2 of `tools_web/romfill.c`: 1-15-byte pieces near an anchor, e.g. 4 bytes of
  `Input.MouseSensitivity`, `Debug.` in config.c's padded key copies). The port registers its options from C
  constructors, which run before the page fills the template, and `configRegister*` copy the key: 110
  `optionsoverlay: option '…' not registered` per boot, those keys could not be loaded / saved through the ini,
  `Input.Bind.Back: unknown binding 'Do'` (default `S,Down`).
- **Fix: the strip, not a deferral.** Pass 2 (and the display-list pass) now run only on the decomp's and the
  assets' symbols (`pass2_selected`: `src/`, `srcfix/`, `assets/`). The string pool and port tables keep their
  pass-1 runs (>= 16 bytes: the game's names and paths, mixer.c's resample table). Why not the G10 deferral:
  queueing `configRegister*` fixes the keys only; default values copied at constructor time (the bind strings in
  `inputConfigInit`, any other constructor-time string) stay broken, and every future pre-fill reader would need its
  own hook. The pieces dropped were coincidences, not ROM data; all >= 16-byte runs are still removed (romscan
  unchanged: 4 runs / 280 B, allowlisted; deploy_check OK). Numbers: docs/web/ROMDATA.md section 3.
- **Tests** (`port_work/g11/`): boot: 1 warning left, `Game.CheckUpdates` (updatecheck.c is not compiled into the
  web build; the unstripped build logs the same); no "unknown binding". The fresh `ge007.ini` of the stripped
  build is byte-identical to a `GE_NOSTRIP=1` build's (147 keys; `[Input.Bind]` Back = `S,Down`, Fire = `Mouse 1`,
  ...). F10 overlay (`GE_OPTIONSOVERLAY=1`, real key events): Field of view 60 -> 70 deg and No hit flash on (both
  were broken keys), reload in the same profile: both shown and in the ini. fillcheck.py == page `&fillcheck`
  (sha256 fbedeaf9…); route_dam.sh to Dam gameplay (walk, fire, 50 retraces/s); level_sweep dam/surface/egypt and
  mp_sweep 2P temple: no trap; save_test.sh all PASS; offline_test.sh (Android emulation) PASS.
- probe.mjs `PROBE_KEYS` now sends key codes for Escape, Enter, F10 and the arrows (SDL maps by keyCode).
- Site build id (sw-build) `7d9d3bcb5d2a697d`; ge007.js unchanged (9eff2711…), ge007.wasm 3c302b2b…, ge007.romfill
  704abf7e….
## retrowser build on the default branch (G13, 2026-11-05)

- **Layout**: retrowser.app reads `retrowser.json` from the root of the default branch (`web` on GitHub); manifest
  paths are relative to it, no `..`. So `web` carries `retrowser.json` (paths `retrowser/ge007.{wasm,js,romfill}`) and
  `retrowser/` (the same bytes as `build/rtw`). `.gitattributes`: both `-text` (the repo has `* text=auto eol=lf`;
  the manifest pins bytes). Branch `rtw-dist` (manifest + files at its root, link `owner/repo@rtw-dist`) is kept and
  refreshed by the same script; RTW.md and earlier hand-offs name it.
- **Refresh** after any change that reaches the rtw build: `scripts/rtw/publish_rtw.sh --dist` (build_rtw.sh, copy,
  root manifest, `rtw hash` + `rtw check . --rom`, deploy_check of `retrowser/`, romfill layout check, rtw-dist commit
  by plumbing), then commit `retrowser.json retrowser` on `web`. `--no-build` reuses `build/rtw`. Every build embeds
  `git rev-parse --short HEAD` (CMake `VERSION_HASH`, printed at start-up), so the wasm hash changes with every
  rebuild at a new commit even when nothing else changed; that is expected.
- **Site byte-identical**: the deployed site was built with HEAD at 8137fa19 (its wasm carries that string; the G12
  work was committed afterwards as 85c96932). Building the merged tree's content with HEAD pinned at 8137fa19
  (`git checkout 8137fa19 && git checkout ee2c873a -- .`, clean `/build/web`) gives the deployed hashes exactly:
  wasm 18e4d479…, js e45e2488…, romfill d90acc60…, and all 14 `build-web/site` files equal, sw-build id
  d903878e5228234a; deploy_check OK (romscan 4 runs / 280 B, allowlisted zlib tables). A normal build at a later commit
  differs only by that version string (and the romfill's offsets/CRCs that follow from it).
- **rtw-dist's old wasm** (025eedf5) had `VERSION_HASH` "unknown": it was built in the `web-rtw` worktree, whose `.git`
  file points outside the container mount, so git failed in CMake. A build from `~/goldeneye` at ee2c873a gives the
  same ge007.js and a wasm 9 bytes longer (the "ee2c873a" string); rtw-dist is now c62ff000 = that build.
- **romfill holds no ROM bytes**: format (tools_web/romfill.c) = 24-byte header (magic, version, count, two CRC32s,
  nsyms) + nsyms x {addr, size} + count x {dst, len, rom_off, src_off, kind, order, pad}; publish_rtw.sh checks the file
  length equals exactly that (2336 extents, 7819 entries, 175,092 B). romscan (raw, RZ-inflated, 16/32-bit swapped)
  finds no run in it at 64 bytes (deploy_check) nor at 16 bytes; `rtw check --rom`: OK.
- **Checks**: `rtw.mjs check . --rom ~/roms/GoldenEye_007_Europe.z64` OK, build id 13a06bb2…; e2e offline phase on a
  `git archive HEAD retrowser.json retrowser` copy served by the GitHub mock as the default branch, built host
  (`dist/`, serve-worker): online boot -> Start x3 -> mission select, servers stopped, offline reload with the
  remembered ROM -> boot from caches, sidecars from IndexedDB, Start x3 -> mission select, AudioWorklet playing, no
  request to the mock, no page errors (e2e_rtw.mjs now goes to the title in both halves).
- **Pushed**: `web` and `rtw-dist` to `origin` (private). Nothing deployed; the Netlify site is unchanged.

## retrowser build (G10, 2026-11-05)

- **What**: `GE_RTW` CMake option, own tree `/build/rtw`; `port/rtw/{rtw.c,gfx_rtw.cpp,rtw_lib.js}` replace the
  page-facing web files (`webaudio.c`, `webpage.c`, `websave.c`, `gfx_sdl2.cpp`). Classic-script MODULARIZE factory
  `createGoldenEye`, `-sENVIRONMENT=worker`, `-sDYNAMIC_EXECUTION=0`, wasm from `Module.wasmBinary`, MEMORY64=2 kept
  (manifest `memory.memory64: true`, 2112/2112 MiB in G10, 384/384 since G12, pointer exports/imports are i64 = BigInt), loop `mainloop`,
  video `webgl` (WebGL2 on the host's OffscreenCanvas, no SDL video / page listeners), pull audio, native save, pads,
  mouse + `rtw_pointer_lock`, rumble, 16 options, 3 extra buttons (Reload, Crouch, Screenshot -> PNG ->
  `rtw_export_file`). `ge007.romfill` is `files.data` (hashed). Sidecar cache in the sandbox origin's IndexedDB
  (`ge007-rtw`). No URL parameters / ENV / fetches.
- **Web build untouched**: no shared source changed (port hooks are `-Wl,--wrap` in the rtw link only). Every
  `build-web/site` file has the same SHA-256 before and after (checked in this worktree; wasm
  5536c556…, js 9eff2711…). Guarded `#if GE_RTW` blocks in shared files were tried first and changed the site
  wasm: its DWARF line tables move with any inserted line.
- **True save size: 4 Kbit (512 B)**, not 16 Kbit: the game's libultra probes only the 4K type and refuses blocks
  > 64; file2.c uses exactly 512 B. The rtw save is the native big-endian image; conversion from / to the port's
  little-endian 2 KB image in rtw.c (D514 rules one way, re-stamp with the game CRC the other).
- **Found (web site bug, not fixed there)**: the template zeroes string-pool bytes that occur in the ROM
  (`Video.AspectMode`, `Input.MouseSensitivity`, `Game.SkipIntro`, …); the option registrations run from C
  constructors before the fill and copy those keys broken, so the deployed site cannot load those keys from its ini
  (`optionsoverlay: option '…' not registered` in every web log; bind default `'Do'`). The rtw build queues the
  registrations until the fill. A web fix needs the same deferral (or key pointers) and a site rebuild.
- **Checks** (all in `port_work/rtw/`, local only): `rtw-check build/rtw --rom <roms>/GoldenEye_007_Europe.z64` OK
  (no ROM block in any file), `deploy_check.sh build/rtw` OK (romscan 4 runs / 280 B, allowlisted zlib tables),
  signature mismatches 0. e2e (`scripts/rtw/e2e_rtw.mjs`, retrowser host + its GitHub mock serving `build/rtw`, one
  muted headless Chrome): dev host: boot (logos, gun barrel, title, Start/A through the menus into Dam; pacing
  50.0 retraces/s, ~60 browser frames/s), pad input (Start, A, Z, stick: Bond walks, ammo 7 -> 6 + bullet hole),
  "Click to use the mouse" shown in a stage, screenshot in Saved files, options applied, audio drained and played
  (ScriptProcessor in Vite dev), save stored 512 B with all six regions valid in N64 order (independent JS CRC,
  `scripts/rtw/eepcheck.mjs`), handed back after a reload and read as the EEPROM, sidecars from IndexedDB on the
  second start; built host: online boot, servers stopped, offline reload, boot from the caches with the remembered
  ROM, audio through the host's AudioWorklet, no request to the mock.
- **Not tested**: mouse motion (headless Chrome refuses pointer lock), real gamepads / rumble, phones (384 MiB since G12),
  pads 1-3 in a split-screen match.
- **Host notes** (retrowser repo, not changed): its AudioWorklet does not load under `npm run dev`; v1 has no netplay
  yet, no absolute pointer, no config persistence, no import path for recordings (`docs/web/RTW.md`).

```sh
GE_DEV=goldeneye-rtw-dev bash scripts/rtw/build_rtw.sh       # build/rtw/ + retrowser.json, rtw-hash, deploy_check, rtw-check
scripts/web/lockrun.sh /tmp/goldeneye-chrome.lock node scripts/rtw/e2e_rtw.mjs dev build/rtw port_work/rtw/dev
scripts/web/lockrun.sh /tmp/goldeneye-chrome.lock node scripts/rtw/e2e_rtw.mjs offline build/rtw port_work/rtw/off
```

### Traps found in G10
- Any line inserted in a shared source (even inside `#if GE_RTW`) changes the site wasm (DWARF line tables): use
  link wraps for build-variant hooks when the other build must stay byte-identical.
- Strings printed or copied before the template fill can be cut short (rtw.c logs its build id after the fill).
- `deploy_check.sh` / `romscan.sh` take the build lock themselves: a script holding the lock deadlocks on them.
- The retrowser host shows "Remember this ROM" only after the download; check it after `[data-field=rom]` appears.

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
  page's own fetch on the very first visit) and ~13 MiB of disk. It adds no pressure to the wasm memory (2 GiB then, 384 MiB since G12) on
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
  Chrome full screen, haptics, feel of the stick size / travel and the button places; whether the wasm memory (384 MiB since G12, was 2 GiB)
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
| retrowser build (G10) | `port/rtw/{rtw.c,gfx_rtw.cpp,rtw_lib.js}`, CMake `GE_RTW`, `scripts/rtw/{build_rtw.sh,retrowser.template.json,e2e_rtw.mjs,eepcheck.mjs}`, `docs/web/RTW.md` |
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
- 384 MiB fixed memory since G12 (was 2112 MiB): not yet tried on a real phone.
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

## Simplified retrowser standard (G19, 2026-11-09)

retrowser's standard dropped its versions and some fields (retrowser 66418df, deployed 2026-11-09 as Worker version
9b474bc0). User: "Then update Conker's and GoldenEye". `scripts/rtw/retrowser.template.json` and the root
`retrowser.json`: no `module` (the loop is read from the module: no `rtw_frame` export = self-running; ge007.wasm
exports none), `factory` moved into `files.js`, `rom.accept` (the N64 default), `rom.byteswap`, `rom.maxSize` and
`memory.memory64` (read from the wasm) removed. The wasm and data files are unchanged; new build id c9211701768676de.
`scripts/rtw/e2e_rtw.mjs` follows the game page of 2026-11-07 (the cartridge alone: `.cart` instead of the Download
button, a remembered ROM starts at once, reduced motion so the cartridge stands still). Tests: `rtw.mjs check --rom`
OK; `e2e_rtw.mjs offline` on a copy of the repo layout in the new built host: PASS (boot, Start, audio, offline from
the caches, no page errors); level select looked at. Not pushed (the user asked only for the update). The rtw-dist
branch is not refreshed (`publish_rtw.sh --no-build --dist` would rebuild it from the template).

