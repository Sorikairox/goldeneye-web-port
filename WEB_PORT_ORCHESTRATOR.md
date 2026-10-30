# Web port orchestrator: from a legal ROM to a browser source port with online multiplayer

What this is: the playbook distilled from porting **Conker's Bad Fur Day (EU, N64)** to readable C compiled to
WebAssembly, playable at https://conker-web-port.netlify.app with 2–4 player online play. Use it to start the same kind
of project for another game you own. It is written for the person running the project (you) and for the orchestrating
agent that drives worker sessions. Conker-specific paths are given as worked examples (`conker:` prefix = this repo).

Most of the knowledge is N64-shaped (MIPS, libultra, RSP microcode). Section 12 says what changes for other consoles.

---

## 0. Ground rules (legal and project hygiene)

- **You must own the ROM.** The site never ships game data: the page asks the player for their own ROM file, checks
  its hash, and keeps it in the browser (IndexedDB). Every asset is read from that ROM at run time.
- **Never commit ROM-derived bytes**: no extracted assets, data blobs, textures, screenshots, audio clips, fonts, or
  `.bin` files. What you may commit: your C, tooling, symbol/name tables, hashes, offsets, documentation.
  Screenshots and captures go to a gitignored `port_work/`.
- **Never modify the user's ROM file.** Read it only.
- **Keep the repo private** even if the code is yours: history tends to collect ROM-derived evidence by accident.
  Back up to a private remote only; never push to an upstream decomp project.
- **SDK-derived code** (libultra C from a decomp) is fine privately; it matters if you ever distribute source.
- **Fan-project framing** on the site and in any trailer: "fan-made, not affiliated with or endorsed by the rights
  holders, bring your own ROM".
- **No third-party downloads without the user's approval** (fonts, music, libraries). Prefer what the game or the
  toolchain already has.

---

## 1. Decide the approach first: source port, not recompilation or emulation

| Option | What it is | Why we did/didn't |
|---|---|---|
| Emulator in the browser | run the ROM in an N64 emulator compiled to wasm | already exists; no readable code, no real features, timing-hungry |
| Static recompilation (N64Recomp style) | translate MIPS to C mechanically | fast to boot, but the output is not readable and features stay hard |
| **Source port (chosen)** | every game function as readable C, compiled natively to wasm; the console's OS and hardware replaced by a PC layer | slowest start, but you get names, a debugger, real features (widescreen, high frame rate, photo mode, online) and a path to native builds |

A source port needs a **decompilation** first (every function in C). If a matching decomp project exists for your
game (decomp.me, GitHub "xxx-decomp"), start from it; if it only covers another region, port it to your region's ROM
(Conker started from a US-centric decomp and was retargeted to EU).

**Target wasm32 first, native later.** wasm32 pointers are 32-bit like the N64, so offset-style decomp code
(`*(s32 *)(p + 0x1C)`, pointers in `s32` temporaries) keeps working. 64-bit native needs a pointer-width cleanup; do
it later from the same C.

---

## 2. Phases and milestones (what worked, in order)

1. **Toolchain in containers.** Splat/IDO (or the game's original compiler) + Emscripten in Docker images. Install
   nothing on the host without asking. One container per worktree.
2. **Every function in C, verified.** Non-matching C is fine for a port, but every function must be *proven
   equivalent* (section 4). Small functions first; producers before consumers.
3. **Boot the browser build early with stubs.** A wasm build where unconverted functions are logged stubs. Get to the
   first frame, then the title, then gameplay. The browser run is your integration test from day one.
4. **Memory model, endianness, calls** (section 3): solve these before mass conversion lands, they decide how all C is
   written.
5. **Platform layer**: OS threads → fibers, renderer (display-list interpreter on WebGL2), audio HLE, input, saves.
6. **Real route without test flags**: power-on → logos → menu → new game → first level, with sound.
7. **Clean-up**: drop the matching-build scaffolding from the integration branch, keep it on an archive branch;
   rename everything to readable names (section 7).
8. **Features**, each optional (section 9), then **online** (section 10), then **end-to-end recordings** (section 11).

Conker timeline for scale: ~5900 functions; boot-to-title in the first ~10 sessions on stubs; all functions C by about
P317; ~350 sessions total including features and online.

---

## 3. The core technical decisions (the ones that hurt if you get them wrong)

### 3.1 Memory: mirror the console RAM, relocate pointers at load

The game's data segments are raw ROM blobs full of absolute console addresses (data pointers, function tables, jump
tables). Chosen design (conker: `docs/port/MEMORY.md`):

- A static `n64_rdram[8 MB]` mirror in wasm memory holds the console's RAM at the same offsets. Every data symbol
  without a C definition is a **label inside the mirror** at its console offset, so `&a - &b`, struct overruns and
  bss clears keep working byte for byte.
- At load, the data segments are copied from the ROM into the mirror; a **build-time relocation list** rewrites each
  word that is a pointer: exact function start → wasm table index; address in a C-defined object → its wasm address;
  otherwise → mirror address.
- Function pointers: a generated address→function table with **exact prototypes** (wasm traps on type mismatch).

**Traps:**
- **False-positive relocations corrupt data silently.** A pair of `u16`s like `0x8000, 0x7EBA` (32768, 32442) looks
  like a KSEG0 pointer. Conker's acos table had exactly that: its first entries became halves of a wasm pointer,
  which skewed lighting angles, Conker's tail pose and MP cameras for months. Start the relocation window *above*
  the region where such tables live and review the report of relocated words.
- **Game behaviour must not depend on where the mirror lands.** Three real bugs: the heap aligned blocks relative to
  the wasm address instead of console RAM 0; `osVirtualToPhysical`/`K0_TO_PHYS` returned wasm addresses that the
  audio library then used as numbers; relocated data (above). Fixes: align offsets inside the mirror, make
  virtual→physical return console physical addresses, and **link the mirror first at a fixed, size-aligned address**
  (conker: 0x10800000, 8 MB aligned) so no unrelated change can move it. Keep a regression that builds with the
  mirror shifted (conker: `make RDRAM_SHIFT=N`, `tools/eu_port/check.sh --layout`) and requires pixel-identical
  shots and identical memory dumps.
- **Pointer masking** (`p & 0x1FFFFFFF`, `| 0xA0000000`, range tests like `(p & 0xFF000003) == 0x80000000`) is wrong
  on the PC. Find each site and rewrite it (conker did it in the IR pass, scoped by function, with an expected count
  so a mismatch warns).
- **Leave a null zone**: link with `-sGLOBAL_BASE=256MB`. Stubs return 0 and the game writes through null; low
  addresses then stay garbage-only, and display-list values split into disjoint classes (≥0x80000000 console pointer,
  0x10000000.. wasm pointer, 0x01000000.. RSP segment address, below = physical/id). Keep the mirror below
  0x11000000 if any game code reads `(addr >> 24) & 0xF` as a segment number.
- Renderer read-backs (GPU pixels written into game RAM for effects) must not be read by game logic; exclude them
  from state hashes.

### 3.2 Endianness: virtual big-endian memory via an LLVM IR pass

Do **not** byte-swap assets per format (hundreds of unknown formats, and every mixed-width access breaks). Instead
store game memory "word-swapped" like N64 emulators: each aligned 32-bit word holds its big-endian value as a native
i32. Then 32-bit access is free, 16-bit is at `A ^ 2`, 8-bit at `A ^ 3`, 64-bit swaps halves, misaligned goes through
helpers. ROM bytes become this layout with one bswap32 per word, whatever they are.

Apply it with an **IR pass on every game TU** (conker: `port/tools/vbe.py`), at `-O0` with optnone disabled, *before*
optimization, so clang's later load narrowing stays correct. A binary rewrite of the finished wasm is wrong.

**Traps:**
- **Bitfields**: IDO allocates from the MSB, clang (LE) from the LSB. Reverse the declaration under `#ifdef PORT` or
  use masks.
- **PC-layer code is native**: any sub-word field of game memory it touches needs `vbe_ld8/16` / `vbe_st8/16`.
- **libc**: the decomp's own `memcpy`/`strlen`/`sinf`… must be renamed inside game TUs, or they replace Emscripten's
  libc for the PC layer (musl `printf` calling a vbe `strlen` garbles all output). PC code must never hand game memory
  to byte-wise libc functions.
- **Varargs doubles** come out with swapped halves unless `va_arg` loads are handled.
- **In a debugger**, byte and halfword fields look lane-swapped: read `HEAPU8[a ^ 3]`.

### 3.3 Calls: one canonical signature per function

The original compiler linked anything (implicit `int f()`, prototypes that disagree, f32/s32 mix-ups). In wasm a
direct call with the wrong type links as a trap. Collect every definition and call site from the IR, pick a canonical
signature per function (the definition's; for unconverted ones the most common call site), and rewrite mismatching
call sites the way an o32 register would pass them: sign-extend/truncate ints, bitcast f32↔i32, missing args = 0,
extra args dropped (conker: `port/tools/sigs.py`, `sigdb.json`).

**Trap (cost a week): keep `zeroext`/`signext` on narrow integer parameters.** Dropping them meant a callee declared
`u8 id` received `0x300B` instead of `0x0B` — the weapon pickups in multiplayer silently failed. After any change to
call rewriting, diff the number of rewritten sites and run gameplay tests.

**Indirect calls**: handler tables mix `void f(void)`, `void f(s32)`, `f32 f(s32)` called through one pointer type.
On MIPS that works, in wasm `call_indirect` traps. Use **site-typed trampolines with a runtime signature check**: fast
path = signature ids equal → plain call; slow path = convert args/result like o32 (conker: `port/tools/icall.py`,
`pc_icall.c`). Don't use `-sEMULATE_FUNCTION_POINTER_CASTS` (slow everywhere, wrong for floats, breaks pointer
identity). A function in a handler table must return what the dispatcher expects (check the ROM asm for `v0` use).

### 3.4 Threads: cooperative fibers, the browser drives the retrace

libultra threads block on message queues fed by interrupts (VI retrace, SP/DP task done, AI, PI DMA). Implement
`OSThread` as Emscripten fibers (ASYNCIFY), schedule cooperatively by priority, and when every thread waits for the
VI message, return to the browser. Each animation frame posts retraces at the console rate (PAL 50 Hz / NTSC 60 Hz,
fixed timestep, catch-up capped). `osSpTaskStart` runs the renderer or audio HLE synchronously and posts "done".

**Traps:**
- Busy-waits on hardware registers or `osGetCount` never yield: convert them to yields.
- Port-side OS replacements must do *everything* libultra's version does, including side effects (creating queues).
  A hang with no trap is usually a blocking `osRecvMesg` on a queue the port should feed.
- **Fiber stacks must sit at fixed addresses** (static slots, not heap): their addresses end up in game memory, and
  two browsers then disagree from retrace 1 (found by online determinism tests).
- Hidden tab: browsers throttle `requestAnimationFrame`; pause cleanly (and online, switch to a timer loop).

### 3.5 Renderer: your own display-list interpreter on WebGL2

Write (or port) an F3DEX2-style interpreter (Fast3D/libultraship style): geometry, matrices, lighting on the CPU;
combiner and textures in shaders. Log unknown opcodes first — Rare and others add custom ones (Conker: TRI4 and
microcode variants). RT64 is not an option in the browser.

- Segment addresses resolve through the game's `gsSPSegment` bases.
- Frame-buffer effects (motion blur, pause snapshot, shadows) need read-backs or emulation; keep them out of game
  logic.
- Render-only features (hi-res 2D, widescreen, high frame rate, smooth font) live entirely here and are safe online.

### 3.6 Audio: HLE the audio microcode on the CPU

Interpret the game's audio command lists (ADPCM, resample, envelope mixer, reverb) in C, write to a Web Audio ring
(start on a user gesture). Write it from documentation; GPL HLE code is a license question.

**Traps:**
- The audio library keeps *physical* addresses as numbers in its command lists. Map values below RAM size into the
  mirror; never use a fixed top byte (conker had `0x10` hard-coded: codebooks read zeros for weeks).
- `AI_LEN` must not come from the real Web Audio ring fill under determinism (use a virtual queue drained by retrace).

### 3.7 ROM, DMA, saves, input

- Page: file picker → detect/normalise byte order (.z64/.n64/.v64) → SHA-1/256 check against the known region →
  IndexedDB. Never upload it anywhere.
- `osPiStartDma` copies ROM bytes into game memory in the vbe layout. Compressed assets keep going through the game's
  own decompressor (it works unchanged on vbe memory).
- EEPROM / controller pak → RAM images persisted to IndexedDB, with save import/export in the page.
- Controllers → `osContGetReadData` from keyboard + browser Gamepad API, rebinding UI, touch controls, rumble via
  `vibrationActuator`.

---

## 4. Verifying converted C (the decomp gate)

- **Differential test per function** (conker: `tools/port/difftest.sh`, Unicorn MIPS): original bytes vs your
  compiled C, random inputs, lazily randomised memory, stubbed callees; compare call trace with arguments, memory at
  each call, final memory, return value, callee-saved registers. Accept only EQUIV.
- **Mutants that must DIFFER**: deliberately break your C (swap a constant, drop a store); if difftest still says
  EQUIV, the *harness* is blind there. A surviving obvious mutant is a harness bug until proven otherwise.
- **Test the harness itself** with a selftest suite and add a case for every harness fix. Twice in Conker a harness
  bug made every result meaningless while looking fine.
- Callees that feed a branch need defined return values (`RET`/`RETF`), or random returns let mutants survive.
- Stack-built request structs passed to callees need explicit memory checks; class-only comparison hides dropped
  stores.
- Keys/knobs use the region's real addresses; take call targets from the decoded `jal`, not from names.
- Pin the emulator library version per worktree (Unicorn 2.1.x faulted next to FPU instructions; 2.0.1 didn't).
- **Producers before consumers**: a correct C function fed by an asm stub (NULL buffers, garbage out-params) corrupts
  memory that looks like someone else's bug. When the browser shows heap corruption, bisect merges first.
- Standing static checks in the commit gate: implicitly declared calls (an implicit call passing/returning a float
  is always a real bug), absolute-value link symbols, census build of every TU (a TU that fails to compile must not
  be silently replaced by stubs).

**m2c-era traps** in decompiled C: typed pointer + byte offset (`f32 *p; p + 0xC`), word reads where the asm uses a
byte load, float bit patterns passed as ints (`0xC61C4000` to an `f32`), `0.0f + x` folded away (loses -0.0), IDO
leaving a value in a0..a3 that the callee reads though the C passes nothing, small stack arrays assumed 4-byte
aligned (give every alloca IDO's layout: ≥4 aligned, padded slots).

---

## 5. Testing the running game

| Test | What | Conker tool |
|---|---|---|
| Commit gate | static checks + harness selftest + census + web build, GREEN/RED in one command | `tools/eu_port/check.sh` |
| Program unchanged | for tooling/rename commits: the wasm md5 must not change | in check.sh / apply_names.sh |
| Warp test | boot straight into a level with a fresh save (`?warp=<level>`), scripted pads, screenshots | `warp.sh` |
| Real route | power-on → menus → new game → level, retrace numbers checked (opening ends at 5789, level at 5795) | `holdl.sh`, `newgame.sh` |
| Layout independence | mirror shifted vs not: pixel-identical + memory dumps equal | `check.sh --layout` |
| Online | two/four pages in one headless Chrome, per-retrace state hashes compared | `netpair.sh`, `netgroup.js` |
| End-to-end recordings | the user plays, the recorder stores inputs + checkpoints, headless replay finds the first difference | `e2e_replay.sh` (section 11) |

**Determinism modes** are the foundation of all of it: a det clock (time = retrace × period + a fixed tick per read),
no wall-time frame budget, a virtual audio queue, frame-indexed scripted pads (`?padf`). With them a route replays
identically, so a test is a number, not a judgement.

**Browser test rules:** headless Chrome **muted** (`--mute-audio`), **one Chrome run at a time** (a second window hides
the first tab and the game pauses), kill only PIDs you started, never `pkill`. Look at the screenshots yourself —
agents have claimed "it works" from logs while the picture was wrong (a radar was mistaken for a weapon wheel).

---

## 6. Debugging

- **Debug build with DWARF** served locally (conker: `tools/port/scripts/debug_build.sh`, `docs/port/DEBUGGING.md`),
  plus Chrome's "C/C++ DevTools Support (DWARF)" extension: breakpoints and C source in Chrome DevTools. The deployed
  build is `-g0`; keep the two apart.
- `compile_commands.json` for the IDE (CLion/VS Code) so navigation works; the IDE can't run wasm natively, debug in
  Chrome.
- **Write watchpoint**: instrument every game store in a debug build and log the JS stack for writes into an address
  range (`?watch=ADDR`). It finds stale-pointer bytes and who corrupts a word.
- **State hashing per retrace** (`?nethash`) + a dump at a chosen retrace: diff two runs word by word to find the
  first diverging object; map addresses to names with the symbol table.
- Game-side logging needs a vbe-aware logger (the normal one prints nothing from rewritten TUs).
- Stubs log their name on first hit: a list of stubs hit on a route is your next work list.

---

## 7. Names: make the code readable

- Rename every function and variable you touch to an explicit name, and keep `/* was func_XXXXXXXX */`. Do it
  repo-wide with a script and a names table (conker: `tools/eu_port/names.tsv`, `rename.py`, `apply_names.sh`),
  never with an IDE rename. A global rename batch must leave the wasm byte-identical.
- Mark guessed names ("(low)", "(med)") so a human can review.
- Tools that derive addresses from `func_<ADDR>` names break after renaming; resolve through the symbol table.
- Symbols/addresses live in frozen tables (`port/symbols/*.tsv`); game code and data come from the ROM at run time.

---

## 8. Running agent sessions (orchestration)

- **One feature or one bounded slice per agent session**, numbered (P1, P2, …). Each session ends with: commit(s),
  a HANDOFF.md entry, a row in `docs/progress/sessions.tsv`, deploy if it changed the game, backup, and a short report.
- **Brief = paths to read + the goal in the user's words + concrete steps + how to prove it + the rules + what to
  report.** Pass concrete findings forward (function names, addresses, commits), not summaries of summaries.
  Template: conker `AGENT_NEW_FEATURE.md`.
- **2–3 agents at once at most**, each in **its own git worktree and container**, with **disjoint files**. If you
  reassign files mid-flight, tell both agents.
- Long contexts degrade quality: rotate to a fresh agent rather than letting one run forever (Conker later dropped
  explicit tool budgets for feature agents because one feature per agent rotated naturally).
- **Scripts that do `cd ~/fixed/worktree`** silently test the wrong tree from another worktree — make scripts resolve
  their own repo root.
- **Never `git stash`** (the stack is shared across worktrees; stashing during a merge reset an index once). Use WIP
  commits.
- **Permissions**: if an agent is denied something, don't run it yourself on its behalf; show the user the exact
  command. Approval only counts from the user's own message.
- **Verify agent claims** before relaying: re-run the key test, look at the screenshot, check the file exists (an
  agent claimed a GIF and a sheet existed that didn't).
- Merge discipline: one integration branch that always holds the latest of everything (conker: `port-web`); merge
  worker branches the same day; doc conflicts with a union-merge helper, code conflicts by hand; run the gate after.
- Keep the user's play copy frozen and separate from agent work.
- Host hygiene: Docker Desktop crashes take all containers down — restart and tell running agents to redo in-flight
  work; bind mounts can serve stale files after a crash.

---

## 9. Features (what players liked, and how to do them safely)

Rules: **every feature is an option, off by default** (unless asked), **one commit per feature** with a message that
explains how it works. Presets (Classic / Modern / Custom) bundle them.

Classify each feature before building it:

| Class | Examples | Online |
|---|---|---|
| **Render-only** (renderer/page, game state untouched) | hi-res 2D, widescreen, high frame rate, smooth font, full-resolution canvas | each player chooses their own |
| **Game-logic** (changes C behaviour) | longer tail spin, ledge grab everywhere, HUD always on, air meter, inverted swimming, skip intro | the host dictates; the option vector is part of the session |
| **Tools** (freeze/restore state) | photo mode, free camera, input recorder | off online |

Lessons per feature:
- **Find the original mechanism first.** Conker already had a ledge grab on flagged triangles; the option extended it.
  Note the original values; "off" must be exactly the original behaviour (prove it: wasm md5 or identical hashes).
- **Widescreen**: scale clip-space x in the renderer and widen viewports; keep **game-logic visibility at 4:3**
  (visibility gates object updates — changing it changes the game and desyncs online). Re-anchor corner HUD
  elements. Objects just outside 4:3 appear late at the edges; accept or widen culling render-side only.
- **High frame rate**: replay the newest game frame's display list with matrices interpolated between the last two
  game frames (match matrices by a stable key, slerp/lerp), snap on camera cuts, teleports and scene changes. Game
  logic stays at its own rate.
- **Hi-res 2D / smooth font**: centre texture rectangles on the console pixel, clamp to what the console samples;
  for glyphs, bicubic-sample the intensity and threshold with a narrow smoothstep (distance-field look). Identify
  glyph draws by texture source address range, not by guesswork.
- **Photo mode**: snapshot the hashed game state on entry, restore it before every photo frame, let the game build
  the frame with the photo camera, restore on exit. Pose/outfit edits are applied only inside photo frames; prove
  the state after exit equals a run without photo mode.
- **Free camera**: drive the game's own follow camera (so its wall collision applies); leave cutscene/scripted
  cameras alone; off online (the camera stick isn't in the lockstep input).

---

## 10. Online multiplayer (deterministic lockstep)

Conker: `docs/design/ONLINE_MULTIPLAYER.md`, `port/ultra/pc_net.c`, `port/web/net.js`,
`port/web/netlify/functions/room/`.

### 10.1 Model
- **Input lockstep with a small input delay** (D = 2–5 retraces). Every peer runs the same wasm on its own ROM from
  power-on; only inputs travel (4 bytes per player per retrace). Retrace N runs only when every player's input for N
  is present (the frame barrier). The local input for N + D is sampled at the start of retrace N.
- Rollback is possible later (snapshot ~11 MB, ~0.4 ms each way, ~14 KB dirty per retrace in Conker) but not needed
  for an N64 game whose MP already runs at 15–25 fps.
- Video streaming is ruled out: guests would play without owning the ROM.

### 10.2 Determinism checklist (pin every source)
1. Wall clock reads (`osGetTime`/`osGetCount`, often mixed into RNG) → det clock.
2. Wall-time frame budget in the scheduler → off.
3. Audio-driven timing (`AI_LEN`) → virtual queue drained by retrace.
4. Input sampling time → inputs indexed by retrace, never written asynchronously by the page.
5. Controller presence → a mask fixed by the host before boot.
6. Options that change game code → the host's option vector; volume becomes a page-side gain.
7. URL test flags → ignored except a whitelist.
8. Save data → the host's EEPROM image (user data), sandboxed: never persisted during the session.
9. GPU read-backs into game RAM → excluded from the hash, never read by logic.
10. Heap/stack layout → fixed fiber stack slots, no page-side `malloc` during a session.
11. Same build and ROM → refuse to start unless both hashes match.
12. Floats → IEEE-deterministic in wasm; NaN bit patterns are the only theoretical risk (Chrome-only in practice).

Prove it offline first: the same 4-pad script run twice gives identical per-retrace hashes; then two tabs online.

### 10.3 Transport and signaling
- **WebRTC DataChannels**: one reliable ordered channel for control, one unordered `maxRetransmits: 0` channel for
  inputs; every input packet repeats all unacknowledged inputs plus an ack and the newest state hash.
- **Full mesh** for 2–4 players; the host relays any pair that fails to connect.
- **Signaling**: copy-paste codes first (no server; SDP deflated + base64url after ICE gathering, ~770 chars), then
  **room codes via a serverless function + key-value store** (conker: a 17-line Netlify Function adapter + a core
  module on Netlify Blobs; 5-character codes from an unambiguous alphabet, 10-minute TTL, compare-and-swap updates,
  host secret and per-slot tokens, rate limit). Netlify Functions can't hold WebSockets: poll over HTTP.
- **STUN** is free (Google/Cloudflare). Without **TURN**, roughly 10–20 % of pairs (symmetric NAT, mobile CGNAT)
  cannot connect; decide with the user whether to accept that.
- Time sync: the peer that is ahead slows its period ~1 %; send right after each tick; choose D from the measured ping.
- Lost peer → neutral input from an agreed frame; hidden tab → timer loop, never a silent pause.
- Desync → compare hashes every K retraces, report the retrace and both hashes; per-block hashes to locate the object.
- Test locally with an in-page network simulator (`?netsim=lat:MS,jit:MS,loss:PCT`) and a loopback peer; tests must
  use a *local* room server, never production.
- **Chrome only** was the support target for online; cross-engine determinism was never proven.

**Online traps:**
- A late joiner must press Start at the right retrace in scripted tests (the join timing is part of the input).
- Render-only options may differ per player; anything that touches game C must come from the host.
- Prove online multiplayer with a **real multiplayer session**, not a warp into the level: the warp skipped the code
  path that had the bug.

---

## 11. End-to-end recordings (your own TAS)

Published TAS/speedrun inputs won't sync: the game's variable time step and the port's own timing differ from any
emulator's lag frames, and speedruns are only videos. So record your own:

- A **recorder in the page** runs a one-player lockstep session (all the determinism of section 10) at full speed with
  sound, stores the starting save, the option vector, the input of every retrace (run-length packed) and state
  checkpoints (full hashes for the same build, a build-independent digest — RNG, level, mode, object count, character
  records — for other builds). ~3–13 KB per minute.
- One recording per chapter, each starting from a save, so a broken chapter is re-recorded alone.
- A **headless replay** at ~10× real time reports the first differing retrace, level/mode, the player's position and
  expected-vs-actual screenshots.
- Prove the recorder catches a deliberate one-constant change, survives a hidden tab, and ignores render-only options.

---

## 12. Adapting to another game or console

**Another N64 game:** most of this carries over directly: the mirror + relocation, the vbe IR pass, signature
canonicalisation, icall trampolines, fibers for libultra threads, the F3DEX2 interpreter (check the game's microcode:
F3D, F3DEX, F3DEX2, S2DEX, custom), naudio/aspMain HLE (or the game's own audio ucode), the online design.
Per game, check: compressed code segments and overlays (Conker: rzip code decompressed and TLB-mapped at boot —
on the PC code is native, so skip the decompression/TLB and load data segments directly), custom microcode commands,
frame-buffer tricks, the region's refresh rate (PAL 50 / NTSC 60), and the save type (EEPROM 4K/16K, SRAM,
flash, controller pak).

**Other consoles:**
- **Big-endian CPUs** (GameCube/Wii PowerPC, Saturn SH-2, PS3): the vbe idea applies as is.
- **Little-endian** (PS1 MIPS LE, PS2, GBA/DS ARM): drop the vbe pass; keep the mirror, relocation, signature and
  indirect-call work.
- **64-bit pointers or CPUs with wider registers**: wasm32 no longer matches; plan the pointer-width cleanup up front.
- The OS layer (threads, DMA, interrupts) and the GPU interpreter are per console; the browser constraints (one main
  thread, user gesture for audio, rAF pacing, hidden-tab throttling, IndexedDB, no installs) are the same.
- Check that the decomp toolchain exists (splat, m2c, the original compiler for difftest, decomp.me presets).

---

## 13. Deploy and publish

- Static hosting (Netlify) + serverless functions for room codes. One deploy script: copy the build, deploy, then
  fetch `BUILD.txt` to prove the live version (conker: `tools/eu_port/deploy_web.sh`).
- **Always deploy the latest tested build** from the integration branch; never deploy a build that skipped its browser
  test. Back up right after (conker: `backup_push.sh`).
- Page essentials: ROM picker with hash check, Options (with presets), Controls (rebinding, touch), Saves (import/
  export), Online panel, pause on hidden tab, a `PLAYING.md`-style player guide kept current.
- `WebAssembly.Memory` of ~352 MB: fine on desktop and 64-bit Android; untested on iOS Safari and risky on 32-bit
  browsers.
- **Trailer** (optional): capture in headless Chrome with the det mode (one retrace per frame, 60 fps pictures with
  interpolation), the game's PCM drained per retrace, compose on a canvas, encode H.264 + AAC with WebCodecs (no
  ffmpeg needed). Use the game's own music, font (smoothed) and voice clips; credit any external music.

---

## 14. Top 25 traps (quick list)

1. Relocating data words that only look like pointers (u16 pairs, masks) — silent, months-long bugs.
2. Game behaviour depending on the mirror's wasm address (alignment, physical addresses, masks) — fix the address.
3. Dropping `zeroext`/`signext` on narrow parameters in call rewriting.
4. `call_indirect` type mismatches through handler tables; void functions whose result is tested.
5. Bitfield order (MSB vs LSB) in game headers.
6. Decomp libc functions leaking into the PC layer.
7. Audio using physical addresses as numbers; a hard-coded top byte for audio RAM.
8. Fiber stacks or anything addressable allocated from a heap whose layout differs between browsers.
9. Wall-clock reads mixed into RNG (kills determinism).
10. Changing game-logic visibility for widescreen (changes the game, desyncs online).
11. A correct C function fed by an asm stub's garbage.
12. Harness bugs that make every test pass; surviving obvious mutants.
13. A TU that fails to compile silently replaced by stubs.
14. Implicit declarations passing or returning floats.
15. Two Chrome test runs at once (the hidden tab pauses the game).
16. Scripts hard-coding one worktree's path.
17. `git stash` across worktrees.
18. Trusting an agent's "it works" without looking at the picture or re-running the test.
19. Testing a feature through a shortcut (warp) that skips the broken path.
20. Port OS replacements missing a side effect of the original (queues, messages).
21. Busy-wait loops that never yield to the browser.
22. GPU read-backs reaching game logic or the state hash.
23. URL test flags changing hashed state (keep test flags out of recordings and sessions).
24. Committing ROM-derived files (screenshots, extracted audio, fonts) — keep them in a gitignored work folder.
25. Deploying a build that skipped its browser test.
26. Starting from an existing **64-bit** PC port and targeting wasm32: its layout fixes and converters assume
    LP64. Compile as wasm64 and lower with `-sMEMORY64=2` (runs on any engine, wraps addresses to 32 bits);
    keep everything JS touches below 2 GiB (that glue mis-indexes higher addresses). (GoldenEye G1)
27. wasm has no absolute symbols (no `.set sym, CONST`, no `--defsym`): place labels in a section linked first
    at `GLOBAL_BASE`, or turn the name into a dereferenced pointer macro. (GoldenEye G1)
28. Trusting a headless screenshot as proof the page runs: the compositor keeps serving the last frame while
    the main thread is stuck; also check that `Runtime.evaluate` answers. (GoldenEye G1)
29. Libraries that yield to the browser by themselves inside a fiber: SDL2 linked with ASYNCIFY calls
    `emscripten_sleep(0)` in `SDL_GL_SwapWindow` and `SDL_Delay`. It unwinds the fiber as if it were the
    main loop; it works for hundreds of frames, then the main loop silently dies. Set
    `SDL_HINT_EMSCRIPTEN_ASYNCIFY=0` and link `--wrap=emscripten_sleep` with a guard that refuses it in a
    fiber. (GoldenEye G2)
30. Busy-waits through a plain getter on state another thread changes (`while (alCSPGetState(p));` until
    the audio thread processes a stop): no clock, no queue, so no yield. `-Wl,--wrap=<getter>` that counts
    polls and parks the fiber; the game code stays as is. (GoldenEye G2)
31. A page stuck in wasm never processes a late `Debugger.enable`: enable the debugger before navigating,
    then `Debugger.pause` returns the wasm stack of the stuck loop. (GoldenEye G2)
32. "No ROM data in the binary" from a decomp: don't hand-write templates. Post-link, match every
    run of the game's data symbols (wasm-ld map + data section) against the ROM and its decompressed
    streams in both byte orders, zero the matches in the shipped wasm and fill them at boot from an
    offsets-only table; pointers are never touched, so relocation stays as linked. Prove it with an
    independent hash of the filled memory vs the original build, and scan the site (>= 64-byte runs)
    as a deploy gate. (GoldenEye G4)
33. Data the runtime touches before your boot-time fill (C constructors, library init) must not be
    zeroed or CRC-checked: restrict the fill to the decomp's data and read-only port tables. (GoldenEye G4)
34. Docker Desktop on macOS: never bind-mount more than the repo (a zsh-sourced bash script resolved
    its root to `$HOME` and mounted all of it), keep build trees in named volumes, and serialise heavy
    builds across sessions with a lock file; the VM's file sharing stalls and Docker dies otherwise.
    (GoldenEye G3/G4)
35. Converters ported from Python must reproduce Python's incidental orderings (dict insertion order,
    stable sorts, last-write-wins maps, set iteration) to be byte-identical; port line by line, then
    diff the outputs before trusting a single file. (GoldenEye G4)
36. A renderer taken from a desktop port silently takes its fallbacks on WebGL2 (GLSL ES): no
    `noperspective` (sky/water gradients wrong), no depth clamp, no mirror clamp, `mediump` floats.
    WebGL2 has the extensions, but each must be enabled per context before use. Put the start view of
    every level next to the native port's reference frames early. (GoldenEye G3)
37. `-sMEMORY64=2`: a C callback that returns a pointer to the glue (SDL's beforeunload returns `""`)
    arrives as a BigInt and `UTF8ToString` throws while the page closes; replace such callbacks
    (return NULL) and run the clean-exit work synchronously there. (GoldenEye G3)
38. Cross-session lock files: a wrapper that takes the lock around a tool that takes the same lock
    deadlocks (the tool waits on its own parent); let the tool accept a lock held by an ancestor.
    (GoldenEye G3)
39. Scripted pads in sweeps: know what each button does once gameplay starts (an "intro skip" A
    cycled to grenades and Bond blew himself up in a lift; the stick's Y axis looked instead of
    walking). Read the screenshots before filing a bug. (GoldenEye G3)
40. Browser saves lost at page close: an IndexedDB write begun in `beforeunload` may not finish, and the
    game's own beforeunload (config write) runs after the page's listeners. Debounce writes while
    running, flush when the tab is hidden, and on `pagehide` keep a synchronous localStorage copy of
    anything still dirty that wins at the next start. (GoldenEye G5)
41. Save import: switch the running game's persistence off before storing the imported image, or its
    next write / the unload flush puts the old save back before the reload. Validate with the game's own
    checksum code (in wasm), not just the size. (GoldenEye G5)
42. Persistence tests with a probe that deletes its profile and SIGKILLs Chrome prove nothing: keep the
    profile and close Chrome cleanly (CDP `Browser.close`). (GoldenEye G5)

---

## 15. Starting checklist for a new game

1. Confirm you own the ROM; note its exact hash and region.
2. Find or start a decomp; set up containers (original compiler, splat, m2c, Emscripten). Private repo, `port_work/`
   gitignored.
3. Write `docs/port/PLAN.md` (memory map, segments, compression, threads, microcode, audio) and `orchestrator.md`
   (rules, branches, gate) before the first worker session.
4. Build the difftest harness + its selftest; build the stub-based wasm boot; get the first frame.
5. Decide memory, endianness, calls, threads (section 3) and write them down in `docs/port/MEMORY.md`.
6. Convert in disjoint slices with 2–3 agents; gate every commit; boot-test continuously.
7. Reach the real route with sound; add the determinism modes and route tests.
8. Rename everything; drop matching scaffolding from the integration branch.
9. Deploy; then features (each optional), then online, then e2e recordings.
