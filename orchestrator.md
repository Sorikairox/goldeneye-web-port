# GoldenEye 007 (EU) web port: orchestrator notes

Playbook: `WEB_PORT_ORCHESTRATOR.md` (lessons from the Conker web port). This file holds the status snapshot (overwrite
it, don't append history) and the rules specific to this repo. Session history: `docs/web/HANDOFF.md` and
`docs/web/sessions.tsv`.

## 1. Status snapshot (2026-10-30, after G3 + G4 + G5; G6 online on branch web-net)

- **Goal:** GoldenEye 007 running in a web browser (WebAssembly), from readable C, for a player who owns the
  **EU (PAL) ROM**. Later: features (each optional) and online multiplayer, as in the Conker port.
- **Base:** this repo is a clone of `jkdansereau/goldeneye-pc-port` (MIT; a native PC port of the n64decomp/007
  decompilation, SDL2 + OpenGL 3, NTSC-U in releases, PAL/JP "convert, build and boot from source" per its
  ROADMAP D258/D487/D488). Remote `upstream` is fetch-only (push URL disabled). Our work is on branch `web`.
- **Web port state:** `docs/web/PLAN.md` (decisions: C compiled as wasm64 = LP64 like the native port,
  lowered to wasm32 with `-sMEMORY64=2`; cart window at 0x10000000, single-view DRAM at 0x80000000;
  libultra threads as fibers driven by rAF at 50 Hz). In headless Chrome: the real route power-on ->
  menus -> Dam; **all 20 solo missions + the ending load and play by warp** (50 retraces/s, no traps;
  start views match the native Linux golden frames), **split-screen 2-4 players on all 11 arenas**
  (`docs/web/LEVELS.md`). Page: AudioWorklet audio, hidden-tab pause, clean unload, Gamepad API pad
  works (fake pad); pointer lock / real pads / audio by ear need a human. G4: sidecars converted in the
  browser (C port of the converters, IndexedDB cache) and ROM data stripped from the wasm (template +
  fill at boot, `deploy_check.sh` gate). G5: saves persist in IndexedDB per ROM SHA-1
  (export/import in the page, `?nosave`, online sandbox seam `GESaves.sandbox`). G7: the Egypt / MP
  dark-or-grey surfaces are the same on the native port (aarch64 native reference build,
  `scripts/web/native_ref.sh`); not a web bug, N64 parity of those views unchecked. G6 (branch `web-net`, `docs/web/ONLINE.md`): optional
  online multiplayer, deterministic input lockstep over WebRTC for 2-4 players (det mode, state hashes,
  copy-paste codes, room codes on a local server, recorder); production room function and a real
  two-machine test wait for the user. G8: touch controls (`port/web/touch.js`, `webtouch.c`; phone emulation
  tests `scripts/web/phone_test.sh`). G9: offline play (service worker precache keyed by build, page option "Offline play" default on, `scripts/web/offline_test.sh`). How to build/run/test: `docs/web/HANDOFF.md`.
- **EU-only:** confirmed — the PAL build, the sidecar converters and the game need only the EU ROM.
- **Tooling:** Docker images `goldeneye-emsdk`, `goldeneye-linux-amd64` (native reference build), containers
  `goldeneye-dev` (build, `--cpus=4`, build tree in volume `goldeneye-dev-build`), `goldeneye-web-serve`
  (127.0.0.1 only), `goldeneye-linux` (stopped when idle); a second session uses its own (`GE_DEV`,
  `GE_SERVE`). Shared locks: `/tmp/goldeneye-build.lock`, `/tmp/goldeneye-chrome.lock`. `env.sh` from
  bash only (a zsh-sourced run mounted `$HOME` and crashed Docker Desktop). Scripts in `scripts/web/`;
  screenshots in gitignored `port_work/`.
- **Reference:** the decompilation itself is cloned at `~/goldeneye-ref/007` (read-only reference).
- **ROM:** `~/roms/GoldenEye_007_Europe.z64`, read-only, SHA-1 `167c3c433dec1f1eb921736f7d53fac8cb45ee31`
  (= the decomp's `ge007.e.sha1`). Dev copy: `data/ge007.pal-final.z64` (gitignored); never modify the original.
- **Remote / hosting (user go-ahead 2026-10-30):** private repo `origin` = github.com/Sorikairox/goldeneye-web-port (back up with `scripts/web/backup_push.sh`); public site https://goldeneye-web-port.netlify.app (deploy the tested build with `scripts/web/deploy_web.sh`, which runs the `deploy_check.sh` gate first). No TURN (user: accept that ~10-20 % of pairs cannot connect).

## 2. Rules for this repo (ours override upstream's AGENTS.md / CLAUDE.md where they conflict)

- Upstream's `CLAUDE.md`, `AGENTS.md`, `docs/HANDOFF.md`, `docs/ROADMAP.md`, `docs/porting-notes.md` and
  `docs/dev/findings*.md` are valuable knowledge: read them. Their process rules (batched commits, dispatch
  budgets, upstream PR flow) do not apply here; ours do:
  - one commit per feature or per coherent step, with a message that explains how it works;
  - session entry in `docs/web/HANDOFF.md` + a row in `docs/web/sessions.tsv` at the end of every session;
  - never push to `upstream`; never open PRs or issues upstream.
- **The deployed web build must contain no ROM-derived data.** Everything game-specific (code is fine; data, models,
  textures, audio, text, setup files) comes from the player's ROM in the browser at run time. Upstream commits some
  extracted/converted data files; anything ROM-derived must not end up in the wasm or the site. Never commit new
  ROM-derived bytes, pictures or captures (keep them in a gitignored `port_work/`).
- **ROM data in the binary (user decision 2026-10-29):** bulk media (logos, textures, fonts, audio) and game
  data tables (weapon stats, model headers, skeletons, image tables, setup data) compiled into the binary are
  ROM data and must come from the player's ROM in the browser (template + fill at boot, C sidecar converters
  run in wasm, cached in IndexedDB). Small initialised constants inside the decompiled game C files count as
  code and may stay. Deploy gate: scan the shipped wasm/js/site for ROM byte runs before every deploy.
- Every new feature is optional (an Options toggle, default off = original behaviour); off must be provably the
  original behaviour.
- Rename functions and variables you touch to readable names where the decomp left `func_`/`D_` names, with a
  `/* was ... */` comment, repo-wide via a script.
- Browser tests: headless Chrome, muted, one Chrome run at a time; kill only PIDs you started; never `pkill`/`pgrep`.
- No `git stash`. No system installs on the host without the user's approval: use Docker containers
  (`export PATH=/Applications/Docker.app/Contents/Resources/bin:$PATH`; the Conker containers on this machine are
  not ours to touch, but its `conker-emsdk` image may be reused).
- **Touch controls are always part of the web port (user rule 2026-11-01).** The site must be playable on a phone:
  on-screen N64 pad (stick, A, B, Z, R, L, C buttons, Start), portrait (picture on top, controls below) and
  landscape, multi-touch, rotation mid-game, ROM picker and page menus usable by touch, fed through the same input
  path as keyboard/gamepad. Setting Auto (on for touch devices) / On / Off. Every session that changes the page
  re-checks phone emulation (CDP touch, mobile UA, portrait + landscape) before deploy. Reference: the Conker port
  (~/conker-wt/web port/web/main.js, docs/design/PLAYING.md).
- If a permission is denied: stop and report the exact command.
- At most 2–3 agent sessions at once, each with its own worktree and container, disjoint files.
