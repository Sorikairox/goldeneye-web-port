# Online multiplayer (G6): design and status

Deterministic **input lockstep** over WebRTC for 2 to 4 players, the model of the Conker web port
(`~/conker-wt/web/docs/design/ONLINE_MULTIPLAYER.md`, its sessions P336..P357; the page code and the room server are
adapted copies). Every player runs the same `ge007.wasm` from power-on on **their own ROM**; only controller inputs
travel (4 bytes per player per 50 Hz retrace). Optional and **off by default**: the page checkbox
"Online multiplayer (experimental)" (remembered in the browser) shows the Online panel; off, `net.js` does nothing
and the game boots exactly as before (no C path changes: every hook tests `geNetDetOn` / `geNetSessionOn`, 0 without
a session).

Results and numbers: section 6. What is left and what needs the user: sections 7 and 8.

## 1. Model

- Retrace N runs only when every session channel's input for N is in the input ring (the **frame barrier**,
  `webkernel.c wkFrameDet`). The local player's input for N + D is sampled at the start of retrace N (input delay D,
  2..8 retraces, automatic from the measured ping by default); frames 1..D are neutral.
- The ring (256 frames x 4 channels, packed `buttons << 16 | (u8)x << 8 | (u8)y`), the hash ring, the stats and the
  session state live in `port/src/web/webnet.c`, outside the hashed game state.
- Player 1 (the host) drives the title and multiplayer menus like on an N64; the presence mask makes the game see
  exactly the session's controllers, so the multiplayer menu offers 2..4 players.
- Rollback is not built (as in Conker; GoldenEye's split screen already runs at a low game rate).

## 2. Determinism (playbook 10.2), how each source is pinned

All of it is `det` mode (`geNetDetOn`), set by the page before `main()` for a session, an offline det run
(`?netdet=1`) or a recording (`?rec=1`). Without it each hook takes its original path.

| # | Source | Where it bites in GoldenEye | Pin |
|---|---|---|---|
| 1 | wall clock | `osGetCount` drives `waitForNextFrame` (frametiming.c: the game's frame delta, `g_ClockTimer`) and `bossMainloop`'s tick gate; `boss.c` seeds the RNG with `osGetCount()` (G4: the intro camera depended on it); `osGetTime` in audi.c; timers (`osSetTimer`, the boot's 100 ms controller probe) | det clock `geNetDetCount`: retrace x 931050 (one PAL retrace of the 46.5525 MHz counter) + 64 ticks per read, capped below half a retrace (the game's frame wait then always ends at the next retrace); `osGetTime` and timers use the same clock in microseconds |
| 2 | scheduler budget | the fiber kernel ran fibers on every browser frame and parked busy-waiting fibers until the next browser frame | in det, fibers run only inside a retrace, until every fiber blocks; a busy-waiting fiber (`webkernelSpinCheck`) parks until the next retrace |
| 3 | audio timing | `osAiGetLength` (audi.c's frame-size regulator) read the AudioWorklet ring fill | a virtual AI queue: `audioSetNextBuffer` adds the block, every retrace drains 1764 bytes (22050 Hz x 4 / 50); same setpoint arithmetic as the real path (`audio.c`); the real ring only plays the sound |
| 4 | input | `osContStartReadData` sampled SDL at read time; the port's input layer writes game state directly in places (control-style forcing D194, free crouch D377, gadget key, PD mouse-aim turn `portMouseAimPdGetTurn`, gun-sway feed `portGunSwayTheta`) | controller reads return the ring entry of the running retrace (`libultra.c contSnapshotFromKeyboard`); local input is sampled once per retrace as a pure N64 pad (`input.c geNetPurePad`: those adapters are off online, OG controls) |
| 5 | controller presence | `osContInit` / queries from SDL's pads, Rumble Pak = "this pad can rumble" | the session mask; no Rumble Pak (`osMotorInit` -> no pack) |
| 6 | options | ~100 ini options, several reach game code (draw / LOD distance, FOV, native widescreen with the window aspect, low-end GPU classification that lowers draw distance, control scheme) | the host's ini text is written to `/data/ge007.ini` on every peer before `main()`; `geNetApplySessionConfig` fixes 4:3 projection, no GPU classification, OG controls, no PD mouse aim, no all-unlocked; `configSave` does nothing in det; the F10 overlay refuses to open |
| 7 | URL test flags | `&env=` / `&args=` | a session passes only whitelisted env variables (`GE_NETPAD0..3`, `GE_NETSCRIPT`, `GE_NETHASHLOG`, `GE_NETDESYNC`, `GE_WKTRACE`) and no args |
| 8 | save data | `ge007.eep` from the page's file system | the session's image: blank, or the host's once G5's seam lands (`ge_net_eeprom_buf`, `N.hostEeprom`); never written back |
| 9 | GPU read-backs | none: fast3d draws to WebGL only; `glReadPixels` is used by a debug dump only | nothing to do |
| 10 | heap / stacks | fiber stacks were already static slots (G1); the page's ROM / fill-table allocations happen before `main()` | no page-side malloc during a session (the ring and the EEPROM buffer are static) |
| 11 | build / ROM | | the host checks every guest's build id (SHA-256 of `ge007.wasm`, 16 hex) and ROM SHA-256 (`hello`); the room server compares them at join |
| 12 | floats | wasm f32/f64 are IEEE | Chrome only (as Conker); cross-engine not proven |

Det mode keeps the 50 Hz wall pacing (a 4 ms timer loop instead of requestAnimationFrame) so the game plays at full
speed with sound; wall time only decides when a retrace runs.

**State hash.** H = the game's data: the `.data/.bss` of the decompiled game, libultra and asset objects, the build
copies of game files (`srcfix/`) and the port files holding game state (`random.c`, `pcmodels.c`, `pccg.c`,
`gimgfixup.c`, `drawdistgameplay.c`, `frontoptions.c`, `watchsettings.c`, `webdram.c`), listed from the link map by
`scripts/web/netregions.py` into `ge007.netregions` (6 ranges, ~840 KB), plus the 8 MiB N64 DRAM arena: 9.2 MB in
2257 pages of 4 KB. S = the live part of every game fiber's C stack and asyncify buffer. Per-page hashes of the last 16
hash points are kept for the desync report (it names the first differing page and the game symbols in it).

## 3. Transport and signaling (net.js, from Conker P338..P345)

- One RTCPeerConnection per pair (full mesh), negotiated DataChannels `ctl` (reliable: hello, config, mesh setup,
  pings, desync report, lost player) and `in` (unordered, `maxRetransmits: 0`: input packets repeating every
  unacknowledged input + ack + newest hash + retrace + input margin; pings).
- A guest pair without a direct link goes through the host (relay, one extra hop for that pair).
- Time sync: the peer that runs ahead slows its 50 Hz backlog by up to 8 % (`ge_net_slow`); a packet that brings the
  input a stalled retrace waits for runs the tick at once (`ge_net_kick`); a hidden page ticks from a Worker timer.
- Lost player: the coordinator collects what every remaining player holds of the lost player's input and fixes F;
  from F that controller is neutral on every peer (`ge_net_drop`). The match goes on.
- Signaling: copy-paste codes `GE1.` + base64url(deflate-raw(SDP after ICE gathering)); room codes through a
  serverless function (`port/netlify/functions/room/{core.mjs,room.mjs}`, Netlify Blobs; 5 letters, 10 minutes,
  compare-and-swap, host secret and slot tokens, rate limit), tested only against the local copy
  `scripts/web/roomserver.mjs` (127.0.0.1:8773). Public STUN only, no TURN.

## 4. Page

`port/web/net.js` (new) builds the Online panel under the ROM picker: the checkbox, Create room / code / Join room,
Play offline, manual codes (Host / Join with the two text boxes), one box per invited player, Start the game
(N players), input delay (automatic by default), status, ping per player, waiting overlay, desync box, Leave.
`main.js` has three small hooks (`GENet.hold`, `GENet.env` / `GENet.args`, `GENet.beforeMain`).

## 5. Recorder (.gerec)

`?rec=1` records a one-player det session from power-on (`ge_rec_session`): the input of every retrace (run-length
packed) and a checkpoint (H, S) every 50 retraces; `__rec.dump()` / `__rec.download()`. `?replay=URL&recfast=1`
replays it back to back and compares every checkpoint (`[rec] replay: N checkpoints compared, M identical, first
difference at ...`). `scripts/web/e2e_replay.sh` records a scripted solo route, replays it, and replays a copy with
one changed input. Checkpoints are exact within one build only (a build-independent digest as Conker's G is not
built yet).

## 5b. How to test

```sh
GE_DEV=goldeneye-g6-dev bash -c 'scripts/web/build.sh'          # also writes build-web/site/ge007.netregions
GE_DEV=goldeneye-g6-dev GE_SERVE=goldeneye-g6-serve bash -c 'scripts/web/serve.sh 8772'
source scripts/web/netmp_pads.sh                                 # PAD0..PAD3: menu route + 4 players playing
bash scripts/web/netdet.sh port_work/g6/det 150 "&env=GE_NETPAD0=$PAD0&env=GE_NETPAD1=$PAD1&env=GE_NETPAD2=$PAD2&env=GE_NETPAD3=$PAD3" ["<pads B>"]
PLAYERS=2 PROBE_ANGLE=metal bash scripts/web/netpair.sh pair2 150      # online, 2 tabs of one Chrome
PLAYERS=4 NETSIM_GUEST=lat:40,jit:20,loss:5 NETFAIL=3-4 LEAVE=guest4 bash scripts/web/netpair.sh relay4 200
ROOM=1 UI=1 PLAYERS=4 bash scripts/web/netpair.sh room4 120         # room code via the local server on 8773
node scripts/web/roomserver.mjs --selftest
bash scripts/web/e2e_replay.sh 60                                # recorder: record, replay, mutated replay
```
Dev URL parameters: see the top of `port/web/net.js` (`?netdet`, `?netloop`, `?netsim`, `?netfailto`, `?roomapi`,
`?netd`, `?nethashevery`, `?rec`, `?replay`, `?recfast`); env `GE_NETHASHLOG=1` (`=2` also logs the C-stack and
asyncify parts of S), `GE_NETDESYNC=N` (flip one DRAM byte at retrace N), `GE_NETPAD0..3`, `GE_NETSCRIPT=1`.

## 6. Results

Logs and screenshots in the gitignored `port_work/g6/` (absolute: `~/goldeneye-wt/net/port_work/g6/`).

**Phase 1, offline determinism** (`scripts/web/netdet.sh`, one page per run, 4 channels from `GE_NETPAD0..3`,
real menu route from power-on: A (intro skip) -> A (title) -> A (file 1) -> mode select, stick down, A ->
Multiplayer options (4 players, Normal, Temple, 10 min) -> START -> 4-way split-screen match from retrace ~1900,
then every player walks / turns / fires / picks up weapons for ~100 s; hash every retrace):

| Runs | Retraces compared | Identical |
|---|---|---|
| det4 a1 vs a2 (S = C stacks + asyncify) | 7376 | H 7376 (100 %); S differed from retrace 28 (C-stack slack, see trap 46) |
| detS a1 vs a2, S split | 2127 | asyncify part 2127 (100 %), C-stack part 74 |
| det5 a1 vs a2 (final S = asyncify buffers; play script with more walking) | 6870 | 6870 H and S (100 %) |
| det4 a1 vs b: player 2's stick changed at retrace 2300 only | 7375 | H identical up to 2300, different from 2301 on |
| det5 a1 vs b (same change) | 6870 | 2300 (all up to the change), first difference at 2301 (H; S equal there) |

The run speed stayed 50 retraces/s with a hash every retrace (2257 pages, 9.2 MB) on SwiftShader, and det
handled a CPU-starved stretch (42-45 retraces/s while another session built) without any difference.

**Phase 3/4, online** (`scripts/web/netpair.sh`: 2-4 windows of one headless Chrome (ANGLE Metal), WebRTC on this
machine, every page plays its own channel's script, player 1 drives the real menu route into the match; hash every
retrace, every guest compared with the host; speed = retraces/s / 50 from 20 s on):

| Run | Setup | D (auto) | Speed | Retraces identical | Notes |
|---|---|---|---|---|---|
| pair2 | 2 players, copy-paste codes (784 / 779 chars), no netsim | 2 | 99.3 % | 7441 / 7441 | 2-player Temple from the menus; 0 desyncs, 0 traps |
| sim2 | 2 players, netsim 40 + 0..20 ms, 5 % loss both ways | 4 (ping median 106 ms) | 99.5 % | 7437 / 7437 | ~0.7 s of waits per side in 130 s |
| relay4 | 4 players, netsim on P2 30+0..10 ms 3 %, P4 20+0..10 ms 2 %; P3-P4 relayed through P1 (`?netfailto`); P3 leaves at the end | 3 (60 ms) | 99.9 % | P2 9776 / 9776, P4 9776 / 9776, P3 9029 / 9029 (until it left) | 4-way split; host relayed ~52 k packets; P3 dropped from retrace 9030 on every remaining peer, which ran on identical for 750 more retraces |
| room4ui | 4 players, room code through the panel's buttons (local room server, 127.0.0.1:8773), no netsim | 2 | 100 % | 5450 / 5450 each guest | joins 3.0 / 2.2 / 2.2 s; unknown code and a 5th player (room full) refused with the server's messages |
| desync2 | 2 players; player 2 flips one byte of N64 DRAM (0x80200000) at retrace 2600 (`GE_NETDESYNC`) | 2 | | identical up to 2599 | both panels: "Desync at retrace 2600"; report on both sides: "first different page 722 (N64 DRAM 80200000..80200FFF); 1 page differs, thread stacks equal" |
| hide2 | 2 players; player 2's window minimised from 41 s to 71 s (`HIDE=40:30`) | 2 | 100 % both | 5534 / 5534 | hidden page ticked from the Worker timer (7868 ticks over 30 s, 1982 -> 3558 retraces), host never stalled |

**Phase 2, recorder** (`scripts/web/e2e_replay.sh 70`): a 70 s one-player recording from power-on through the menus
into Dam (walk, fire) = 3424 retraces, 28 input runs, 68 checkpoints. Fast replay (back to back; ~60-67 retraces/s on
SwiftShader, rendering bound): 65 / 65 checkpoints compared identical (the replay window of 54 s ended at retrace
~3250; the default window is now SECS + 20). The copy with ONE input changed (retrace 2900: fire -> nothing):
"FIRST DIFFERENCE at retrace 2950", the first checkpoint after the change.

Regression with the feature off: `scripts/web/route_dam.sh` on the final build reaches Dam gameplay by the real route
(`port_work/g6/route-dam/sheet.png`); `scripts/web/deploy_check.sh` OK (romscan PASS).

Screenshots (gitignored): `~/goldeneye-wt/net/port_work/g6/menu2/sheet.png` (menu route to the 4-player match),
`port_work/g6/det5/a1/shot-90s.png` (offline 4-player det run), `port_work/g6/net/pair2/guest_60s.png` (2P online,
panel), `port_work/g6/net/relay4/guest4_100s.png` (4P online with relay, 4-way split), `port_work/g6/net/room4ui/
host_80s.png` (4P by room code, host panel with the joined players), `port_work/g6/e2e/rec/sheet.png` (recording).

## 7. Left

- A real two-machine (and real-network) session: every run here was one machine, latency only from `?netsim`.
- Cross-browser determinism (Firefox / Safari peers): not tried; Chrome only, as Conker.
- Options from the host: `GESaves.sandbox` boots every peer with the default config, so the host's ini is not sent
  yet (`cfg.ini` is carried and written; `N.hostIni()` returns ''). When saves.js exposes the stored ini, return it
  there; the session-fixed values of `geNetApplySessionConfig` still apply on top.
- A build-independent checkpoint digest for recordings (Conker's G: RNG, stage, mode, player positions), so a
  .gerec survives a rebuild; today checkpoints compare within one build only.
- A late joiner / reconnect (deterministic fast-forward of the input log) is not possible; a guest whose link to the
  host fails keeps its room slot (reload and make a new room). No kick button for a stranger who guessed a room code.
- The page's local pad: in a session the keyboard / gamepad go through `inputComputePad(0)` as a pure N64 pad with
  OG controls (the port's modern scheme, mouse aim and free crouch are off online); a human should check the feel.
- "My view only" (crop the split screen to the local player's quarter) is not built.

## 8. Needs the user

- **TURN or not:** without TURN roughly 10-20 % of pairs (symmetric NAT, mobile CGNAT) cannot connect directly;
  the host relay only helps guest-guest pairs. Conker's decision was "no TURN"; the same is assumed here.
- **Netlify site** for the room function (`port/netlify/functions/room`, Netlify Blobs store `goldeneye-rooms`):
  no site exists; the deploy script must also run `scripts/web/deploy_check.sh` first.
- **A real two-machine test** (two people, two networks, their own EU ROMs).
