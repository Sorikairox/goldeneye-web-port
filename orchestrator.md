# GoldenEye 007 (EU) web port: orchestrator notes

Playbook: `WEB_PORT_ORCHESTRATOR.md` (lessons from the Conker web port). This file holds the status snapshot (overwrite
it, don't append history) and the rules specific to this repo. Session history: `docs/web/HANDOFF.md` and
`docs/web/sessions.tsv`.

## 1. Status snapshot (2026-10-29, before G1)

- **Goal:** GoldenEye 007 running in a web browser (WebAssembly), from readable C, for a player who owns the
  **EU (PAL) ROM**. Later: features (each optional) and online multiplayer, as in the Conker port.
- **Base:** this repo is a clone of `jkdansereau/goldeneye-pc-port` (MIT; a native PC port of the n64decomp/007
  decompilation, SDL2 + OpenGL 3, NTSC-U in releases, PAL/JP "convert, build and boot from source" per its
  ROADMAP D258/D487/D488). Remote `upstream` is fetch-only (push URL disabled). Our work is on branch `web`.
- **Reference:** the decompilation itself is cloned at `~/goldeneye-ref/007` (read-only reference).
- **ROM:** `~/roms/GoldenEye_007_Europe.z64`, read-only, SHA-1 `167c3c433dec1f1eb921736f7d53fac8cb45ee31`
  (= the decomp's `ge007.e.sha1`). Copy it where tools expect it (e.g. `baserom.e.z64`, `data/ge007.pal-final.z64`,
  all gitignored); never modify the original.
- **Remote / hosting:** no private GitHub remote and no Netlify site yet: both need the user's go-ahead.

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
- Every new feature is optional (an Options toggle, default off = original behaviour); off must be provably the
  original behaviour.
- Rename functions and variables you touch to readable names where the decomp left `func_`/`D_` names, with a
  `/* was ... */` comment, repo-wide via a script.
- Browser tests: headless Chrome, muted, one Chrome run at a time; kill only PIDs you started; never `pkill`/`pgrep`.
- No `git stash`. No system installs on the host without the user's approval: use Docker containers
  (`export PATH=/Applications/Docker.app/Contents/Resources/bin:$PATH`; the Conker containers on this machine are
  not ours to touch, but its `conker-emsdk` image may be reused).
- If a permission is denied: stop and report the exact command.
- At most 2–3 agent sessions at once, each with its own worktree and container, disjoint files.
