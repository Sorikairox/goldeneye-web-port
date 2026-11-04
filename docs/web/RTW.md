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

The built files are committed on the local branch `rtw-dist` (manifest at the root; link form
`owner/repo@rtw-dist`). Never with a ROM, sidecars, saves or pictures.

## Manifest (`scripts/rtw/retrowser.template.json`, hashed by rtw-hash)

| Field | Value | Why |
|---|---|---|
| `id` | `goldeneye-eu` | saves are keyed by it; never change it |
| `rom` | `.z64/.n64/.v64`, SHA-1 `167c3c43…` (EU), byteswap auto | the module also refuses any other ROM with a plain error |
| `files` | `ge007.wasm` (template), `ge007.js` (glue), `data: ge007.romfill` | the fill table is offsets only (rtw-check `--rom`: 0 ROM blocks) |
| `module` | factory `createGoldenEye`, loop `mainloop` | `main()` installs `emscripten_set_main_loop` (webkernel.c) and returns |
| `video` | `webgl` | WebGL2 on the host's OffscreenCanvas |
| `hz` | 50 | PAL; informative (the module paces itself) |
| `memory` | 384 / 384 MiB, `memory64: true` | the `-sMEMORY64=2` (lowered) shape; module-defined memory; 2112 MiB before G12 |
| `save` | `eeprom`, 512 bytes | the cart's real 4 Kbit EEPROM, native byte order (below) |
| `controls.extra` | 16 Reload, 17 Crouch, 18 Screenshot | port actions without an N64 button; screenshot -> PNG via `rtw_export_file` |
| `options` | 16 options (below) | the ini keys that make sense without the F10 overlay |

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
- **Options** (§4.9): id -> ini key, `configSetValue` + `videoRequestLiveConfigForKey` live (All unlocked also runs
  `portAllUnlockedApply`). Values sent before the start are applied after the fill and again after `main()` (the boot's
  D546 / low-end defaults would otherwise win). 1 Aspect (`Video.AspectMode`, default Original 4:3), 2 Skip intro,
  3 Mouse sensitivity, 4 Invert mouse Y, 5 FOV %, 6 Draw distance %, 7 Texture filter, 8 All levels unlocked,
  9 Control style (`Input.ControlScheme`), 10 Rumble (`Input.RumbleScale` 0.5 / 0), 11 Screen shake %, 12 No hit
  flash, 13 HUD scale, 14 Hide crosshair, 15 Show FPS, 16 Resolution (module-side). The ini itself is not kept
  (MEMFS only); not mapped: window/fullscreen/vsync/MSAA (host's), key bindings (host's), audio device/volume (host's).
- **Imports** (`env.rtw_*`, rtw_lib.js -> `Module.rtw`): `rtw_log` (start / pad lines), `rtw_build_id` (logged at start),
  `rtw_rumble`, `rtw_export_file` (screenshots), `rtw_pointer_lock`. Helper imports are named `gertw_*` (rtw-check
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

## What does not fit v1 (host would need)

- Netplay (`net.js`, WebRTC lockstep, 2-4 players) waits for the v1 draft `rtw_net_*` (W11); `net.players` is not set.
- Mouse in menus: the sandbox only has relative motion while captured; the port's front-end mouse pointer is unused.
- Settings files: the port's ini (bindings, PC options beyond the 16 mapped) is not persisted; the host would need an
  options blob or a second save kind ("config") to keep it.
- Recordings (`.gerec`, G6 recorder) would go through `rtw_export_file`, but replay needs an import path into the
  module (the host has none for non-save files).
- `rtw_audio_rate` is read once right after `rtw_start`: a module that learns its rate only during an asynchronous
  start must hard-code it (fine here: 22050).
