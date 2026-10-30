// GoldenEye 007 web port, G6: online multiplayer, page side (docs/web/ONLINE.md). Adapted from the Conker web port's
// port/web/net.js (its sessions P338..P345: same packets, mesh, relay, time sync, lost-player rule, codes and room
// protocol; the P3xx numbers in the comments below are those Conker sessions, docs/design/ONLINE_MULTIPLAYER.md there).
// The game side is port/src/web/webnet.c: an input ring of 256 frames x 4 channels in wasm memory, a frame barrier in
// webkernel.c (retrace N runs only when every session channel's input for N is in the ring), input delay D (the local
// input for frame N + D is sampled at the start of retrace N), and a state hash every K retraces.
// This file reads the local entries out of the ring (to send them), writes the peers' entries in, and reads the hashes.
//
// Optional, default off: nothing here acts unless the player ticks "Online multiplayer (experimental)" (remembered in
// this browser) or a test URL asks for it (?online=1, ?netdet=1, ?netloop=1). Off, main.js starts the game as before.
// Hooks main.js calls (small hunks there): GENet.hold (keep the game from starting until a session is configured),
// GENet.env / GENet.args (only whitelisted test variables in a session), GENet.beforeMain (session setup before main()).
//
// Test aids (dev URL):
//   ?netdet=1[&netmask=F][&netd=1][&nethashevery=1]  offline det run: every channel of the mask is local and plays its
//       GE_NETPADn script (&env=GE_NETPAD0=... ...); with &env=GE_NETHASHLOG=1 the state hash of every hashed retrace
//       is logged ("[nethash] N H S"): two runs must give identical lines (scripts/web/netdet.sh)
//   ?netloop=1[&netmask=F][&netd=3][&netlat=MS][&netloss=PCT]  a session against a loopback peer in this page that
//       plays the GE_NETPADn scripts of the other channels
//   ?netsim=lat:MS,jit:MS,loss:PCT  delay / reorder / drop this page's input packets; ?netfailto=N (no direct link to
//       player N: the pair goes through the host); ?roomapi=URL a local room server; ?netd=D|auto; ?nethashevery=K
'use strict';
(function () {
  const Q = new URLSearchParams(location.search);
  const LS_KEY = 'ge-online';
  const lsGet = () => { try { return localStorage.getItem(LS_KEY) === '1'; } catch (e) { return false; } };
  const lsSet = (on) => { try { localStorage.setItem(LS_KEY, on ? '1' : '0'); } catch (e) { /* private mode */ } };
  const RING = 256;
  let u32 = null;   // HEAPU32 is not exported by ge007.js: a view on the same memory
  const U32 = () => { if (!u32 || u32.buffer !== N.M.HEAPU8.buffer) u32 = new Uint32Array(N.M.HEAPU8.buffer); return u32; };
  // env variables a session may take from the URL (test scripts / logging; every peer's game must see the same values)
  const ENV_OK = /^GE_(NETPAD[0-3]|NETSCRIPT|NETHASHLOG|NETDESYNC|WKTRACE)=/;
  const N = {
    M: null, cfg: null, base: {}, log: (s) => console.log(s),
    enabled: Q.has('online') || Q.has('netdet') || Q.has('netloop') || Q.has('rec') || Q.has('replay') || lsGet(),
    // ---- the game side (port/src/web/webnet.c)
    begin(M, cfg) {   // before callMain: cfg = { mask, local, delay, hashEvery }
      N.M = M; N.cfg = cfg;
      M._ge_net_session(cfg.mask, cfg.local, cfg.delay, cfg.hashEvery);
      for (const [k, i] of [['input', 0], ['frame', 1], ['hash', 2], ['pad', 3], ['stat', 4]]) N.base[k] = M._ge_net_addr(i) >>> 2;
      M.netAfterTick = () => { if (N.afterTick) N.afterTick(); };   // webnet.c, right after a local input is sampled
    },
    frame() { return N.M._ge_net_frame() >>> 0; },          // the last retrace run
    ready(f) { return N.M._ge_net_ready(f) !== 0; },        // may retrace f run (the frame barrier)
    kick() { N.M._ge_net_kick(); },                         // run a tick now (webkernel.c)
    stat(i) { return U32()[N.base.stat + i]; },          // 0 stalled ticks, 7 ms slowed (time sync), 8 waits, 9 ms waited
    has(f, ch) { return U32()[N.base.frame + (f % RING) * 4 + ch] === f; },
    get(f, ch) { return N.has(f, ch) ? U32()[N.base.input + (f % RING) * 4 + ch] : -1; },
    put(f, ch, v) {   // a peer's input for frame f (packed: buttons << 16 | (u8)x << 8 | (u8)y)
      if (f <= N.frame() || N.has(f, ch)) return false;      // already run / already there (packets repeat inputs)
      const i = (f % RING) * 4 + ch, H = U32();
      H[N.base.input + i] = v >>> 0; H[N.base.frame + i] = f;
      return true;
    },
    localTop() { return N.frame() + N.cfg.delay; },            // newest local frame sampled so far
    hashes() {   // [{f, h, s}] of the hash ring, oldest first
      const H = U32(), out = [];
      for (let i = 0; i < 64; i++) { const p = N.base.hash + i * 3; if (H[p]) out.push({ f: H[p], h: H[p + 1], s: H[p + 2] }); }
      return out.sort((a, b) => a.f - b.f);
    },
    // ---- test: a loopback peer in the same page (?netloop=1): it owns every session channel that is not local and
    // plays that channel's GE_NETPADn script, as a remote peer would (lat ms late, loss % of the sends lost and re-sent)
    loopback({ lat = 0, loss = 0 } = {}) {
      const remote = [0, 1, 2, 3].filter((ch) => (N.cfg.mask >> ch & 1) && !(N.cfg.local >> ch & 1));
      let next = N.cfg.delay + 1, sent = 0, lost = 0, lastLog = 0;
      const q = [];
      setInterval(() => {
        const now = performance.now(), top = N.localTop();
        for (; next <= top; next++) for (const ch of remote) q.push({ t: now + lat, f: next, ch, v: N.M._ge_net_script_input(ch, next) >>> 0 });
        let put = false;
        for (let i = 0; i < q.length;) {
          const e = q[i];
          if (e.t > now) { i++; continue; }
          sent++;
          if (loss && Math.random() * 100 < loss) { lost++; e.t = now + 20 + lat; i++; continue; }
          put = N.put(e.f, e.ch, e.v) || put; q.splice(i, 1);
        }
        if (put && N.ready(N.frame() + 1)) N.kick();
        const f = N.frame();
        if (f >= lastLog + 500) {
          lastLog = f - f % 500;
          const hs = N.hashes(), h = hs[hs.length - 1];
          N.log(`[net] retrace ${f}: waits ${N.stat(8)}, loopback sends ${sent} lost ${lost}` + (h ? `, hash ${h.f} ${h.h.toString(16)} ${h.s.toString(16)}` : ''));
        }
      }, 4);
      N.log(`[net] loopback peer: channels ${remote.join(',')}, latency ${lat} ms, loss ${loss} %`);
    },
  };
  // ---------------------------------------------------------------- P344: the WebRTC transport, 2 to 4 players
  // (P338 / P339 built it for 2 players; P344 generalises it, docs/design/ONLINE_MULTIPLAYER.md section 10.)
  // Players: the host is player 1 (controller channel 0) and gives every guest a fixed controller slot 2..4 (channel
  // 1..3) when it makes that guest's offer. Topology: full mesh. Every pair of players has its own RTCPeerConnection
  // (a "link") with two negotiated DataChannels: 'ctl' (reliable, ordered: setup, config, bye, desync report, drops)
  // and 'in' (unordered, maxRetransmits 0: input packets, pings). Each player sends its own input to every other
  // player. A guest-guest pair whose direct link does not open (strict NATs: no TURN server is used) sends through the
  // host instead (the host relays that pair's packets: one extra hop for that pair only).
  // Signaling: only the host <-> guest offer / answer is done by the players, through a signaler (N.signal below; the
  // copy-paste codes GE1.... wrap its payloads; a room-code signaler can carry the same payloads through a server).
  // Everything after a guest's link to the host is open goes over the host's 'ctl' channels (star): the host tells the
  // guests the roster, each guest makes an offer for every higher-numbered guest and the host forwards the offers /
  // answers between them, so a player still only exchanges codes with the host.
  // Input packet (little endian): u8 type 1 | u8 sender channel | u32 ack (the newest frame of the receiver's channel
  // this sender holds without gaps) | u32 first frame | u8 count | u32 input x count | u32 hash frame | u32 H | u32 S |
  // u32 the sender's last retrace | i16 the sender's input margin of the receiver's channel x 64 (time sync).
  // Packets are made per receiver (ack, first frame and margin differ), repeat every input the receiver has not
  // acknowledged (up to 64), so a lost packet costs nothing and only a burst longer than the input delay stalls.
  // Ping: u8 2 | u8 sender | f64 time; pong: u8 3 | u8 sender | f64 time. Relay: u8 4 | u8 receiver channel | packet
  // (sent to the host, which passes the inner packet on to that receiver).
  // P339 (kept): a packet leaves as soon as a local input is sampled (webnet.c, start of each retrace), the same packet
  // goes out a second time ~8 ms later, a keep-alive every 30 ms; a packet that brings the input a stalled game waits
  // for runs the retrace at once (webkernel.c webkernelKick).
  // Time sync (GGPO-style frame advantage), per pair: my input margin of player p = frames of p's input I hold beyond
  // the retrace I ran (time average, ~0.5 s); p sends me its margin of my input. If p's margin of my input is larger
  // than mine of its input, I run ahead of p; every 250 ms I slow down by half the largest such difference over all
  // players (webnet.c ge_net_slow). Nobody ever speeds up.
  // Lost player (decision 6: it stands still): the coordinator (the host; if the host itself is gone, the lowest
  // remaining player) asks every remaining player for the inputs of the lost player it holds, picks F = 1 + the newest
  // frame anyone holds, sends everybody the inputs up to F - 1 and F ('drop' {neutralFrom: F}); from F that controller reads neutral everywhere
  // (webnet.c ge_net_drop). So all remaining players run the same inputs and stay in lockstep.
  const STUN = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun.cloudflare.com:3478' }];
  const CODE_TAG = 'GE1.';
  async function packCode(obj) {
    const raw = new TextEncoder().encode(JSON.stringify(obj));
    const z = new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate-raw'))).arrayBuffer());
    let s = ''; for (let i = 0; i < z.length; i += 32768) s += String.fromCharCode.apply(null, z.subarray(i, i + 32768));
    return CODE_TAG + btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }
  async function unpackCode(code) {
    code = String(code).replace(/\s+/g, '');
    if (!code.startsWith(CODE_TAG)) throw new Error('not a connection code (it starts with ' + CODE_TAG + ')');
    const b = atob(code.slice(CODE_TAG.length).replace(/-/g, '+').replace(/_/g, '/'));
    const z = Uint8Array.from(b, (c) => c.charCodeAt(0));
    const raw = await new Response(new Blob([z]).stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer();
    return JSON.parse(new TextDecoder().decode(raw));
  }
  const b64 = (u8) => { let s = ''; for (let i = 0; i < u8.length; i += 32768) s += String.fromCharCode.apply(null, u8.subarray(i, i + 32768)); return btoa(s); };
  const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

  // test aids: ?netsim=lat:80,jit:20,loss:5 delays (ms, plus 0..jit), reorders and drops this page's input packets;
  // ?netfailto=4 (player numbers, comma separated): this page's direct link to that player never connects (the ICE
  // candidates are removed from both SDPs), so the pair falls back to the host relay
  const SIM = {};
  { const m = /[?&]netsim=([^&]*)/.exec(location.search); if (m) for (const kv of decodeURIComponent(m[1]).split(',')) { const [k, v] = kv.split(':'); SIM[k] = +v || 0; } }
  const FAIL_TO = new Set(((/[?&]netfailto=([^&]*)/.exec(location.search) || [])[1] || '').split(',').filter(Boolean).map((x) => +x - 1));

  const T = N.t = {
    role: null, myCh: 0, state: 'idle', started: false,
    links: {},          // ch -> link: the RTCPeerConnection to that player
    peers: {},          // ch -> lockstep state of that player (made on first use)
    slots: {},          // host: ch -> {state: invited|connecting|joined|left|failed, why}; shown by the panel
    roster: [],         // the session's channels (host: set at start; guest: from the config)
    routes: {},         // 'a-b' -> 'direct' | 'relay' (guest-guest pairs, reported to the host after the mesh setup)
    sent: 0, recv: 0, simDropped: 0, relayed: 0, left: false, quit: false, desync: null, desyncReport: null, rtt: 0, myHashSeen: 0,
    on: {},             // event handlers set by main.js: status(text, cls), config(cfg), roster(slots), desync, desyncReport, left(ch, why), tick
  };
  const emit = (ev, ...a) => { if (T.on[ev]) try { T.on[ev](...a); } catch (e) { N.log('[net] ' + ev + ' handler: ' + e, 'fail'); } };
  const status = (s, cls) => { T.state = s; emit('status', s, cls); N.log('[net] ' + s); };
  const pn = (ch) => `player ${ch + 1}`;
  function peer(ch) {
    return T.peers[ch] || (T.peers[ch] = { ch, recvTop: 0, peerAck: 0, pending: [], hash: new Map(), myMargin: 0, peerMargin: 0,
      peerFrame: 0, marginAt: 0, rtt: 0, lastRecv: 0, lastSendTop: 0, lastSendAt: 0, lastPacket: null, repeated: true,
      lost: false, suspect: false, dropping: false });
  }
  const others = () => T.roster.filter((c) => c !== T.myCh);
  const livePeers = () => others().map(peer).filter((p) => !p.lost);
  // the coordinator of a drop: the lowest channel among me and the players that are neither lost nor silent
  const amCoordinator = () => livePeers().every((p) => p.suspect || p.dropping || p.ch > T.myCh);

  // ---- links
  function stripCandidates(sdp) { return sdp.split(/\r\n/).filter((l) => !/^a=(candidate|end-of-candidates)/.test(l)).join('\r\n'); }
  function newLink(ch) {
    const old = T.links[ch]; if (old) try { old.pc.close(); } catch (e) { /* closed */ }
    const pc = new RTCPeerConnection({ iceServers: STUN });
    const L = { ch, pc, ctl: pc.createDataChannel('ctl', { negotiated: true, id: 0 }),
      inp: pc.createDataChannel('in', { negotiated: true, id: 1, ordered: false, maxRetransmits: 0 }), open: false, noDirect: FAIL_TO.has(ch) };
    L.inp.binaryType = 'arraybuffer';
    L.ctl.onopen = () => { L.open = true; linkOpened(L); };
    L.ctl.onmessage = (e) => onCtl(JSON.parse(e.data), ch);
    L.ctl.onclose = () => { if (T.links[ch] !== L) return; L.open = false; linkClosed(L, 'the connection closed'); };
    L.inp.onmessage = (e) => onPacket(e.data);
    L.inp.onopen = () => { if (T.meshWait) T.meshWait(); };   // 'in' can open after 'ctl' (the mesh waits for both)
    pc.oniceconnectionstatechange = () => {
      const s = pc.iceConnectionState;
      N.log(`[net] ICE ${s} (${pn(ch)})`);
      if (s === 'failed' && T.links[ch] === L) linkClosed(L, 'the network path failed');
    };
    T.links[ch] = L;
    return L;
  }
  const directOpen = (ch) => { const L = T.links[ch]; return !!(L && L.open && L.inp.readyState === 'open'); };
  function gathered(pc, ms = 8000) {
    return new Promise((res) => {
      if (pc.iceGatheringState === 'complete') return res();
      const t = setTimeout(res, ms);   // use what was gathered so far
      pc.addEventListener('icegatheringstatechange', () => { if (pc.iceGatheringState === 'complete') { clearTimeout(t); res(); } });
    });
  }
  async function makeOffer(L) {
    await L.pc.setLocalDescription(await L.pc.createOffer());
    await gathered(L.pc);
    return L.noDirect ? stripCandidates(L.pc.localDescription.sdp) : L.pc.localDescription.sdp;
  }
  async function takeOffer(L, sdp) {
    await L.pc.setRemoteDescription({ type: 'offer', sdp: L.noDirect ? stripCandidates(sdp) : sdp });
    await L.pc.setLocalDescription(await L.pc.createAnswer());
    await gathered(L.pc);
    return L.noDirect ? stripCandidates(L.pc.localDescription.sdp) : L.pc.localDescription.sdp;
  }
  async function takeAnswer(L, sdp) { await L.pc.setRemoteDescription({ type: 'answer', sdp: L.noDirect ? stripCandidates(sdp) : sdp }); }

  // ---- the signaler: the host <-> guest exchange. Payloads are plain objects: offer {t: 'o', ch, sdp} (made by the host
  // for the guest slot ch), answer {t: 'a', ch, sdp} (made by that guest). N.signal works on payloads; the copy-paste
  // codes below wrap them (N.host / N.invite / N.join / N.accept). A room-code signaler (next phase) posts the same
  // payloads to a room on a server instead and calls the same three functions; nothing else depends on how they travel.
  const setSlot = (ch, state, why) => { T.slots[ch] = Object.assign(T.slots[ch] || {}, { state, why: why || '' }); emit('roster', N.slots()); };
  N.signal = {
    async offer(ch) { const L = newLink(ch); setSlot(ch, 'invited'); return { t: 'o', ch, sdp: await makeOffer(L) }; },   // host
    async answer(o) {   // guest
      T.role = 'guest'; T.myCh = o.ch || 1;
      return { t: 'a', ch: T.myCh, sdp: await takeOffer(newLink(0), o.sdp) };
    },
    async accept(a) {   // host
      const ch = a.ch || 1, L = T.links[ch];
      if (!L || L.pc.signalingState !== 'have-local-offer') throw new Error(`no open invitation for ${pn(ch)} (make a new code for that player)`);
      await takeAnswer(L, a.sdp); setSlot(ch, 'connecting');
    },
  };
  // host: start hosting; returns the offer code for player 2. sessionFn({ping, mask, pairs}) -> the session config
  N.host = async function (sessionFn) {
    T.role = 'host'; T.myCh = 0; T.sessionFn = sessionFn;
    return N.invite();
  };
  // host: the offer code for the next free slot (players 2, 3, 4)
  N.invite = async function () {
    if (T.role !== 'host') throw new Error('host first');
    if (T.room && !T.room.closed) throw new Error('a room is open: the other players join with its code (reload the page to use manual codes)');
    if (T.started) throw new Error('the game has started: no more players can join');
    const ch = [1, 2, 3].find((c) => !T.slots[c] || T.slots[c].state === 'left' || T.slots[c].state === 'failed');
    if (ch === undefined) throw new Error('4 players at most');
    status(`making the code for ${pn(ch)}…`);
    const code = await packCode(await N.signal.offer(ch));
    status(`send the code to ${pn(ch)}, then paste the answer code`, 'ok');
    return { ch, code };
  };
  // guest: takes the host's offer code, returns the answer code
  N.join = async function (code) {
    const o = await unpackCode(code);
    if (o.t !== 'o') throw new Error('this is not an offer code (paste the code the host made)');
    status('gathering network addresses…');
    const a = await N.signal.answer(o);
    status(`you are ${pn(T.myCh)}: send the answer code to player 1, then wait`, 'ok');
    return packCode(a);
  };
  // host: takes a guest's answer code (it names the guest's slot)
  N.accept = async function (code) {
    const a = await unpackCode(code);
    if (a.t !== 'a') throw new Error('this is not an answer code (paste the code the guest sent back)');
    await N.signal.accept(a);
    status(`connecting to ${pn(a.ch || 1)}…`);
    return a.ch || 1;
  };
  // ---- P345 room codes (design section 11): the same payloads as the copy-paste codes, carried by the room server
  // (port/web/netlify/functions/room, POST N.roomApi). The host makes a room and polls it every second: for each guest
  // that joined (the server gives slots 2..4 in join order) it posts N.signal.offer(ch) and, when that guest's answer
  // is there, calls N.signal.accept. A guest joins with the code, polls until its offer is there, posts
  // N.signal.answer(offer). From then on everything is peer to peer as before; the host deletes the room when the game
  // starts (or when it leaves). The server compares build id / ROM SHA-256 at join (the host still checks 'hello').
  const ROOM_POLL_MS = 1000, ROOM_GUEST_WAIT_MS = 120000;
  N.roomApi = '/api/room';
  async function roomCall(body) {
    let r, j = null;
    try { r = await fetch(N.roomApi, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body), cache: 'no-store' }); }
    catch (e) { throw new Error('the room server cannot be reached (check the internet connection, or use the manual codes)'); }
    try { j = await r.json(); } catch (e) { /* not json */ }
    if (!r.ok || !j || j.error) { const e = new Error((j && j.error) || `room server error ${r.status} (try the manual codes)`); e.code = j && j.code; throw e; }
    return j;
  }
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  function roomStop(close) {   // host: stop polling; close = delete the room on the server
    const R = T.room; if (!R || R.closed) return;
    R.closed = true; if (R.timer) clearInterval(R.timer);
    if (close && R.secret) roomCall({ op: 'close', code: R.code, secret: R.secret }).then(() => N.log(`[net] room ${R.code} closed`), (e) => N.log(`[net] room close: ${e.message}`));
  }
  // host: make a room for up to max players; returns the room code. sessionFn as for N.host.
  N.roomCreate = async function (sessionFn, max = 4) {
    if (T.role) throw new Error(T.role === 'host' ? 'already hosting' : 'already joined a session');
    const j = await roomCall({ op: 'create', build: N.buildId, rom: N.romSha256, max });
    T.role = 'host'; T.myCh = 0; T.sessionFn = sessionFn;
    const R = T.room = { code: j.code, secret: j.secret, max: j.max, gens: {}, accepted: {}, fails: 0 };
    status(`room ${j.code} is open: tell the other players this code`, 'ok');
    R.timer = setInterval(roomHostPoll, ROOM_POLL_MS); roomHostPoll();
    return j.code;
  };
  async function roomHostPoll() {
    const R = T.room; if (!R || R.closed || R.busy) return;
    if (T.started) return roomStop(true);
    R.busy = true;
    try {
      const j = await roomCall({ op: 'host', code: R.code, secret: R.secret });
      for (const s of j.slots) {
        if (R.closed || T.started) break;
        if (R.gens[s.ch] !== s.gen) {   // a new guest in this slot: make its offer (ICE gathering, up to 8 s)
          R.gens[s.ch] = s.gen; delete R.accepted[s.ch];
          status(`${pn(s.ch)} joined the room: making the connection offer…`);
          const o = await N.signal.offer(s.ch);
          await roomCall({ op: 'offer', code: R.code, secret: R.secret, ch: s.ch, gen: s.gen, sdp: o.sdp });
          status(`room ${R.code}: waiting for ${pn(s.ch)}'s answer…`, 'ok');
        } else if (s.answer && !R.accepted[s.ch]) {
          R.accepted[s.ch] = true;
          await N.signal.accept({ t: 'a', ch: s.ch, sdp: s.answer });
          status(`connecting to ${pn(s.ch)}…`);
        }
      }
      R.fails = 0; R.ttl = j.ttl;
    } catch (e) {
      if (e.code === 'unknown') { roomStop(false); status(`room ${R.code} has expired (rooms last 10 minutes): reload the page and make a new room`, 'bad'); }
      else if (e.code === 'gone') N.log('[net] room: ' + e.message);
      else if (++R.fails === 5) status('room server: ' + e.message, 'bad');
      else N.log('[net] room poll: ' + e.message);
    } finally { R.busy = false; }
  }
  // guest: join a room by its code; resolves with my channel once my answer is posted (the link then opens by itself)
  N.roomJoin = async function (code) {
    if (T.role) throw new Error('already in a session (reload the page to join another room)');
    code = String(code || '').toUpperCase().replace(/[\s-]+/g, '');
    if (!code) throw new Error('type the room code player 1 gave you');
    status(`joining room ${code}…`);
    const j = await roomCall({ op: 'join', code, build: N.buildId, rom: N.romSha256 });
    const R = T.room = { code, ch: j.ch, token: j.token, guest: true };
    T.role = 'guest'; T.myCh = j.ch;
    status(`in room ${code}: you are ${pn(j.ch)}, waiting for player 1…`, 'ok');
    let offer = null;
    for (const t0 = Date.now(); !offer; await sleep(ROOM_POLL_MS)) {
      if (R.closed) throw new Error('left the room');
      if (Date.now() - t0 > ROOM_GUEST_WAIT_MS) throw new Error('player 1 did not answer (is the host page still open?)');
      try { offer = (await roomCall({ op: 'guest', code, ch: j.ch, token: j.token })).offer; }
      catch (e) { if (['unknown', 'token'].includes(e.code)) throw e; N.log('[net] room poll: ' + e.message); }
    }
    status('gathering network addresses…');
    const a = await N.signal.answer({ t: 'o', ch: j.ch, sdp: offer });
    await roomCall({ op: 'answer', code, ch: j.ch, token: j.token, sdp: a.sdp });
    R.closed = true;
    status(`you are ${pn(j.ch)}: connecting to player 1…`, 'ok');
    return j.ch;
  };
  // host: the slots for the panel: [{ch, state, rtt, why, route}]
  N.slots = () => [1, 2, 3].filter((c) => T.slots[c]).map((c) => {
    const p = T.peers[c], s = T.slots[c];
    const relayPairs = Object.entries(T.routes).filter(([k, v]) => v === 'relay' && k.split('-').map(Number).includes(c)).map(([k]) => k);
    return { ch: c, state: p && p.lost ? 'left' : s.state, why: s.why, rtt: Math.round((p && p.rtt) || s.rtt || 0), relay: relayPairs };
  });
  N.leave = function () {
    T.quit = true;
    if (T.room && !T.room.guest) roomStop(true); else if (T.room) T.room.closed = true;   // P345
    for (const c of Object.keys(T.links)) try { sendCtlTo(+c, { t: 'bye' }); } catch (e) { /* closed */ }
    setTimeout(() => { for (const L of Object.values(T.links)) try { L.pc.close(); } catch (e) { /* closed */ } }, 150);
  };

  // ---- 'ctl' messages: to a player with an open link directly, else (guest) through the host
  function ctlRaw(L, obj) { if (L && L.ctl.readyState === 'open') { L.ctl.send(JSON.stringify(obj)); return true; } return false; }
  function sendCtlTo(ch, m) {
    m.from = T.myCh;
    if (ctlRaw(T.links[ch], m)) return true;
    if (T.myCh !== 0 && ch !== 0) return ctlRaw(T.links[0], { t: 'fwd', to: ch, m });
    return false;
  }
  function sendCtlSimTo(ch, m) { const ms = (SIM.lat || 0) + Math.random() * (SIM.jit || 0); if (ms) setTimeout(() => sendCtlTo(ch, m), ms); else sendCtlTo(ch, m); }
  // P339 item 2, per pair since P344: PING_COUNT pings over 'ctl' (PING_GAP_MS apart, at most PING_WAIT_MS); under
  // ?netsim the pings and pongs get the same latency / jitter as the input packets (not the loss: 'ctl' is reliable)
  const PING_COUNT = 12, PING_GAP_MS = 40, PING_WAIT_MS = 2500;
  const pingWait = {};
  function measurePing(ch, count = PING_COUNT) {
    return new Promise((resolve) => {
      const w = pingWait[ch] = { rtts: [] };
      w.finish = () => { if (pingWait[ch] !== w) return; delete pingWait[ch]; const r = w.rtts.slice().sort((a, b) => a - b);
        resolve(r.length ? { n: r.length, min: r[0], median: r[r.length >> 1], p90: r[Math.min(r.length - 1, Math.floor(r.length * 0.9))] } : null); };
      w.got = () => { if (w.rtts.length >= count) w.finish(); };
      for (let i = 0; i < count; i++) setTimeout(() => sendCtlSimTo(ch, { t: 'ping', at: performance.now() }), i * PING_GAP_MS);
      setTimeout(w.finish, PING_WAIT_MS);
    });
  }
  // D = clamp(ceil((RTT / 2 + jitter margin) / 20 ms) + 1, 2, 8): RTT = the median round trip, jitter margin = half of
  // (90th percentile - median); the + 1 covers sending / scheduling and the loss of single packets. P344: the pair
  // with the largest D decides (every player waits for every other player's input).
  N.autoDelay = (p) => p ? Math.max(2, Math.min(8, Math.ceil((p.median / 2 + (p.p90 - p.median) / 2) / 20) + 1)) : 4;

  function linkOpened(L) {
    if (T.role === 'guest' && L.ch === 0) {
      status(`connected: you are ${pn(T.myCh)}, checking the game build…`, 'ok');
      sendCtlTo(0, { t: 'hello', build: N.buildId, rom: N.romSha256 });
    } else if (T.role === 'host') setSlot(L.ch, 'connected');
    else N.log(`[net] direct link to ${pn(L.ch)} open`);
    if (T.meshWait) T.meshWait();
  }
  function linkClosed(L, why) {
    N.log(`[net] link to ${pn(L.ch)}: ${why}`);
    if (T.quit) return;
    if (!T.started && T.role === 'host') { setSlot(L.ch, T.slots[L.ch] && T.slots[L.ch].state === 'joined' ? 'left' : 'failed', why); return; }
    if (!T.started && T.role === 'guest' && L.ch === 0) {
      status(why === 'the network path failed' ? 'Could not connect: the two networks do not allow a direct connection (no relay server is used). Try another network, e.g. a phone hotspot.' : 'the host closed the connection', 'bad');
      return;
    }
    if (!T.peers[L.ch] || !T.roster.includes(L.ch)) return;
    // in a session: a guest-guest link that closes is not a lost player (the pair goes on through the host); the
    // host's link to a guest (or, without a host, the coordinator's) is the player's only path
    const p = peer(L.ch);
    if (T.myCh === 0 || L.ch === 0) p.suspect = true;
    if (amCoordinator() && (T.myCh === 0 || p.suspect)) declareLost(L.ch, why);
  }

  async function onCtl(m, linkCh) {
    const from = m.from !== undefined ? m.from : linkCh;
    if (m.t === 'fwd') {   // a message between two guests, through the host
      if (m.to === T.myCh) return onCtl(m.m, m.m.from);
      if (T.myCh === 0) ctlRaw(T.links[m.to], m.m);   // the inner message keeps its sender
      return;
    }
    if (T.peers[from]) T.peers[from].lastRecv = performance.now();
    if (m.t === 'ping') { sendCtlSimTo(from, { t: 'pong', at: m.at }); return; }
    if (m.t === 'pong') { const w = pingWait[from]; if (w) { w.rtts.push(performance.now() - m.at); w.got(); } return; }
    if (m.t === 'hello' && T.role === 'host') {   // the guest's build and ROM must be ours
      if (m.build !== N.buildId) { sendCtlTo(from, { t: 'refuse', why: 'different game builds (reload both pages to get the latest version)' }); setSlot(from, 'failed', 'different build'); status(`refused: ${pn(from)} runs a different build`, 'bad'); return; }
      if (m.rom !== N.romSha256) { sendCtlTo(from, { t: 'refuse', why: 'different ROMs (the EU ROM is needed on every side)' }); setSlot(from, 'failed', 'different ROM'); status(`refused: ${pn(from)} has a different ROM`, 'bad'); return; }
      setSlot(from, 'joined');
      sendCtlTo(from, { t: 'welcome', ch: from });
      status(`${pn(from)} joined: invite more players or start the game`, 'ok');
      const ping = await measurePing(from, 5);   // for the panel
      if (ping && T.slots[from]) { T.slots[from].rtt = ping.median; emit('roster', N.slots()); }
    } else if (m.t === 'welcome' && T.role === 'guest') status(`joined as ${pn(T.myCh)}: waiting for player 1 to start the game`, 'ok');
    else if (m.t === 'mesh' && T.role === 'guest') buildMesh(m.roster);
    else if (m.t === 'sig' && T.role === 'guest') {   // a mesh offer / answer from another guest (through the host)
      if (m.kind === 'o') { const L = newLink(from); sendCtlTo(from, { t: 'sig', kind: 'a', sdp: await takeOffer(L, m.sdp) }); }
      else if (m.kind === 'a' && T.links[from]) await takeAnswer(T.links[from], m.sdp);
    } else if (m.t === 'meshdone' && T.role === 'host') { T.meshReports[from] = m; if (T.meshWait) T.meshWait(); }
    else if (m.t === 'config' && T.role === 'guest') {
      T.myCh = m.ch; T.roster = m.roster; T.started = true; T.routes = m.routes || {};
      if (m.ping) T.rtt = m.ping;
      emit('config', { mask: m.mask, local: 1 << m.ch, delay: m.delay, autoDelay: m.autoDelay, ping: m.ping, hashEvery: m.hashEvery, ini: m.ini || '',
        options: m.options, eeprom: m.eeprom ? unb64(m.eeprom) : null, ch: m.ch, roster: m.roster });
    } else if (m.t === 'refuse') status('the host refused: ' + m.why, 'bad');
    else if (m.t === 'bye') {
      N.log(`[net] ${pn(from)} left the session`);
      if (!T.started) { if (T.role === 'host') setSlot(from, 'left', 'left'); else if (from === 0) status('the host left', 'bad'); return; }
      if (T.peers[from]) { peer(from).suspect = true; if (amCoordinator()) declareLost(from, `${pn(from)} left`); }
    } else if (m.t === 'lost') onLost(m, from);
    else if (m.t === 'held') onHeld(m, from);
    else if (m.t === 'drop') applyDrop(m);
    else if (m.t === 'desync') { emit('desync', Object.assign({ remote: true }, m, { ch: from })); sendPages(m.frame, from); }
    else if (m.t === 'pages') comparePages(m, from);
  }

  // ---- P344 session setup (host): mesh between the guests, pings of every pair, config
  const MESH_WAIT_MS = 8000;
  N.start = async function () {
    if (T.role !== 'host') throw new Error('only player 1 starts the game');
    const guests = [1, 2, 3].filter((c) => T.slots[c] && T.slots[c].state === 'joined');
    if (!guests.length) throw new Error('no player has joined yet');
    T.started = true;
    roomStop(true);   // P345: the room is not needed any more (nobody can join a started game)
    T.roster = [0, ...guests];
    for (const c of [1, 2, 3]) if (!guests.includes(c) && T.links[c]) { try { T.links[c].pc.close(); } catch (e) { /* closed */ } delete T.links[c]; }
    T.meshReports = {};
    if (guests.length > 1) {
      status('connecting the players to each other…', 'ok');
      for (const g of guests) sendCtlTo(g, { t: 'mesh', roster: T.roster });
      await new Promise((res) => {
        const t = setTimeout(res, MESH_WAIT_MS + 8000);
        T.meshWait = () => { if (guests.every((g) => T.meshReports[g])) { clearTimeout(t); T.meshWait = null; res(); } };
      });
      T.meshWait = null;
    }
    status('measuring the ping…', 'ok');
    const pings = {};
    await Promise.all(guests.map(async (g) => { pings['0-' + g] = await measurePing(g); }));
    for (const g of guests) {
      const r = T.meshReports[g];
      for (const h of guests) if (h > g) {
        const k = g + '-' + h, a = r && r.pings && r.pings[h], b = T.meshReports[h] && T.meshReports[h].pings && T.meshReports[h].pings[g];
        pings[k] = a && b ? (a.median >= b.median ? a : b) : a || b || null;
        const ra = r && r.links && r.links[h], rb = T.meshReports[h] && T.meshReports[h].links && T.meshReports[h].links[g];
        T.routes[k] = ra === 'direct' && rb === 'direct' ? 'direct' : 'relay';
      }
    }
    // the worst pair decides the input delay
    let worst = null, worstKey = '';
    for (const [k, p] of Object.entries(pings)) {
      N.log(`[net] ping ${k.split('-').map((c) => 'P' + (+c + 1)).join('-')}${T.routes[k] ? ' (' + T.routes[k] + ')' : ''}: ` +
            (p ? `${p.n} round trips, min ${p.min.toFixed(1)} median ${p.median.toFixed(1)} p90 ${p.p90.toFixed(1)} ms -> auto input delay ${N.autoDelay(p)}` : 'no answer'));
      if (p && (!worst || N.autoDelay(p) > N.autoDelay(worst) || (N.autoDelay(p) === N.autoDelay(worst) && p.median > worst.median))) { worst = p; worstKey = k; }
    }
    for (const g of guests) if (pings['0-' + g] && T.slots[g]) T.slots[g].rtt = pings['0-' + g].median;
    if (worst) T.rtt = worst.median;
    const mask = T.roster.reduce((m, c) => m | 1 << c, 0);
    const cfg = await T.sessionFn({ ping: worst, mask, pairs: pings, worstPair: worstKey });
    cfg.mask = mask;
    cfg.ping = worst ? Math.round(worst.median) : 0;
    for (const g of guests)
      sendCtlTo(g, { t: 'config', mask, delay: cfg.delay, autoDelay: cfg.autoDelay, ping: cfg.ping, hashEvery: cfg.hashEvery, options: cfg.options, ini: cfg.ini || '',
        eeprom: cfg.eeprom ? b64(cfg.eeprom) : null, ch: g, roster: T.roster, routes: T.routes });
    emit('roster', N.slots());
    emit('config', Object.assign({}, cfg, { local: 1, ch: 0, roster: T.roster }));
  };
  // guest: make the direct links to the other guests (the lower channel offers), then report links + pings to the host
  async function buildMesh(roster) {
    T.roster = roster;
    const mates = roster.filter((c) => c !== 0 && c !== T.myCh);
    status('connecting to the other players…', 'ok');
    await Promise.all(mates.filter((c) => c > T.myCh).map(async (c) => { const L = newLink(c); sendCtlTo(c, { t: 'sig', kind: 'o', sdp: await makeOffer(L) }); }));
    await new Promise((res) => {
      const t = setTimeout(res, MESH_WAIT_MS);
      T.meshWait = () => { if (mates.every(directOpen)) { clearTimeout(t); T.meshWait = null; res(); } };
      T.meshWait();
    });
    T.meshWait = null;
    const links = {}, pings = {};
    for (const c of mates) links[c] = directOpen(c) ? 'direct' : 'relay';
    await Promise.all(mates.map(async (c) => { pings[c] = await measurePing(c); }));
    N.log(`[net] mesh: ${mates.map((c) => `${pn(c)} ${links[c]}${pings[c] ? ' ' + pings[c].median.toFixed(1) + ' ms' : ''}`).join(', ')}`);
    sendCtlTo(0, { t: 'meshdone', links, pings });
    status('waiting for player 1 to start the game', 'ok');
  }

  // ---- desync report, per pair: both peers swap the per-page hashes of the desync retrace (webnet.c keeps the last
  // 16 hash points: one hash per 4 KB page of the hashed game memory + one for the game threads' stacks) and name the
  // first page that differs, with the game symbols in it (ge007.netregions S lines, from scripts/web/netregions.py).
  const pagesSent = new Set();
  function pageVector(frame) {
    const p = N.M._ge_net_page_hashes(frame) >>> 0;
    if (!p) return null;
    const n = N.M._ge_net_page_info(0, 0) + 1;
    return new Uint32Array(N.M.HEAPU8.buffer.slice(p, p + 4 * n));
  }
  function sendPages(frame, ch) {
    if (!frame || pagesSent.has(frame + ':' + ch)) return;
    pagesSent.add(frame + ':' + ch);
    const v = pageVector(frame);
    if (!v) { N.log(`[net] desync report: the page hashes of retrace ${frame} are no longer kept`); return; }
    sendCtlTo(ch, { t: 'pages', frame, h: b64(new Uint8Array(v.buffer)) });
  }
  let symTable = null;
  async function symbols() {
    if (!symTable) {
      symTable = [];
      try {
        for (const l of (await (await fetch('ge007.netregions')).text()).split('\n')) {
          const f = l.split(' ');
          if (f[0] === 'S' && f.length >= 4) symTable.push([parseInt(f[1], 16), parseInt(f[2], 16), f[3]]);
        }
      } catch (e) { /* no table: addresses only */ }
    }
    return symTable;
  }
  const hex = (x) => (x >>> 0).toString(16).toUpperCase().padStart(8, '0');
  async function comparePages(m, from) {
    const mine = pageVector(m.frame);
    sendPages(m.frame, from);   // the other side needs mine too
    if (!mine) return;
    const theirs = new Uint32Array(unb64(m.h).buffer), pages = N.M._ge_net_page_info(0, 0);
    const diff = [];
    for (let i = 0; i < pages; i++) if (mine[i] !== theirs[i]) diff.push(i);
    const stacks = mine[pages] !== theirs[pages] ? ['game threads'] : [];
    let where = 'no memory page differs';
    if (diff.length) {
      const a = N.M._ge_net_page_info(2, diff[0]) >>> 0, size = N.M._ge_net_page_info(3, diff[0]), syms = await symbols();
      const list = syms.filter(([s, l]) => s < a + size && s + Math.max(l, 1) > a);
      const label = a >= 0x80000000 ? `N64 DRAM ${hex(a)}..${hex(a + size - 1)}` : `wasm ${hex(a)}..${hex(a + size - 1)}`;
      where = `first different page ${diff[0]} (${label}${list.length ? `, ${list.length} symbol(s) in it: ` + list.slice(0, 6).map((x) => x[2]).join(', ') + (list.length > 6 ? ', …' : '') : ''})`;
    }
    const report = { frame: m.frame, ch: from, where, pages: diff.length, firstPages: diff.slice(0, 8), stacks };
    T.desyncReport = report;
    N.log(`[net] DESYNC REPORT retrace ${m.frame} (this game vs ${pn(from)}): ${where}; ${diff.length} page(s) differ` +
          `${diff.length ? ' [' + diff.slice(0, 8).join(' ') + (diff.length > 8 ? ' …' : '') + ']' : ''}, thread stacks ${stacks.length ? 'differ' : 'equal'}`);
    emit('desyncReport', report);
  }

  // ---- P339: a hidden page's timers (the 4 ms main loop, the pump) are throttled to ~1 Hz by Chrome, even with a live
  // WebRTC connection, and the other players would wait. While hidden, a dedicated Worker posts a message every 4 ms
  // (worker timers are not throttled); each one runs a game tick (webkernel.c webkernelKick) and the pump.
  let hiddenTicker = null, hiddenTicks = 0;
  N.background = function (hidden) {
    if (hidden && !hiddenTicker) {
      hiddenTicker = new Worker(URL.createObjectURL(new Blob(['setInterval(() => postMessage(0), 4);'], { type: 'text/javascript' })));
      hiddenTicker.onmessage = () => { hiddenTicks++; N.kick(); pump(); };
    } else if (!hidden && hiddenTicker) { hiddenTicker.terminate(); hiddenTicker = null; }
    N.log(`[net] page ${hidden ? 'hidden: the game ticks from a worker timer' : 'visible again'} (retrace ${N.frame()}, ${hiddenTicks} worker ticks so far)`);
  };

  // ---- session (after N.begin): the pump sends inputs, pings, checks for lost players
  N.startTransport = function () {
    const now = performance.now();
    for (const p of livePeers()) {
      for (const [f, v] of p.pending) N.put(f, p.ch, v);
      p.pending = [];
      p.recvTop = Math.max(p.recvTop, N.cfg.delay); p.peerAck = Math.max(p.peerAck, N.cfg.delay);   // frames 1..D are neutral
      advanceRecvTop(p);
      p.lastRecv = now;
    }
    status(`waiting for the other player${others().length > 1 ? 's' : ''}`, 'ok');
    N.afterTick = () => sendInputs(performance.now());   // P339
    setInterval(pump, 5);
    setInterval(timeSync, 250);
    if (document.hidden) N.background(true);   // P339: the page was hidden before the session started
    setInterval(() => {
      for (const p of livePeers()) { const b = new DataView(new ArrayBuffer(10)); b.setUint8(0, 2); b.setUint8(1, T.myCh); b.setFloat64(2, performance.now(), true); sendIn(p.ch, b.buffer); }
    }, 1000);
  };
  function advanceRecvTop(p) { while (N.has(p.recvTop + 1, p.ch)) p.recvTop++; }
  const inSession = () => N.M && N.base.input !== undefined;
  // one input packet for player p: every local input p has not acknowledged, the ack, the newest hash, retrace, margin
  function sendInputsTo(p, now) {
    const top = N.localTop();
    const first = Math.max(p.peerAck + 1, N.cfg.delay + 1, top - 63);
    const count = Math.max(0, top - first + 1);
    const dv = new DataView(new ArrayBuffer(11 + 4 * count + 18));
    dv.setUint8(0, 1); dv.setUint8(1, T.myCh); dv.setUint32(2, p.recvTop, true); dv.setUint32(6, first, true); dv.setUint8(10, count);
    for (let i = 0; i < count; i++) dv.setUint32(11 + 4 * i, N.get(first + i, T.myCh) >>> 0, true);
    const hs = N.hashes(), h = hs[hs.length - 1] || { f: 0, h: 0, s: 0 }, o = 11 + 4 * count;
    dv.setUint32(o, h.f, true); dv.setUint32(o + 4, h.h, true); dv.setUint32(o + 8, h.s, true);
    dv.setUint32(o + 12, N.frame(), true); dv.setInt16(o + 16, Math.max(-32768, Math.min(32767, Math.round(p.myMargin * 64))), true);
    sendIn(p.ch, dv.buffer);
    p.lastSendTop = top; p.lastSendAt = now; p.lastPacket = dv.buffer; p.repeated = false;
  }
  function sendInputs(now) { if (!T.quit) for (const p of livePeers()) sendInputsTo(p, now); }
  function pump() {
    if (T.quit) return;
    const now = performance.now(), lastRun = N.frame();
    let rtt = 0;
    for (const p of livePeers()) {
      // the input margin of p, time-averaged (exponential, time constant 500 ms)
      const margin = p.recvTop - lastRun;
      if (p.marginAt) { const a = Math.min(1, (now - p.marginAt) / 500); p.myMargin += (margin - p.myMargin) * a; } else p.myMargin = margin;
      p.marginAt = now;
      if (N.localTop() !== p.lastSendTop || now - p.lastSendAt > 30) sendInputsTo(p, now);   // new local input (normally sent by
      else if (!p.repeated && now - p.lastSendAt >= 8) { sendIn(p.ch, p.lastPacket); p.repeated = true; }   // afterTick) / keep-alive; the repeat
      if (p.rtt > rtt) rtt = p.rtt;
      if (now - p.lastRecv > 5000 && !p.dropping) {
        if (!p.suspect) { p.suspect = true; N.log(`[net] no data from ${pn(p.ch)} for 5 s`); }
        if (amCoordinator()) declareLost(p.ch, `no data from ${pn(p.ch)} for 5 s`);
      }
    }
    if (rtt) T.rtt = rtt;
    checkHashes();
    const waiting = !N.ready(lastRun + 1);
    if (!T.playing && lastRun > N.cfg.delay + 1) { T.playing = true; status('playing', 'ok'); }
    const lp = livePeers();
    emit('tick', { frame: lastRun, waiting, rtt: T.rtt, stalls: N.stat(8), margin: lp.length ? Math.min(...lp.map((p) => p.myMargin)) : 0,
      slowed: N.stat(7), peers: others().map((c) => { const p = peer(c); return { ch: c, rtt: Math.round(p.rtt), lost: p.lost, waiting: !N.has(lastRun + 1, c) && !p.lost }; }) });
  }
  // P339 time sync, every 250 ms: if this peer runs ahead of any player, slow down by half the largest difference
  let lastSyncLog = 0;
  function timeSync() {
    if (T.quit) return;
    let ahead = 0;
    for (const p of livePeers()) if (p.peerFrame) ahead = Math.max(ahead, (p.peerMargin - p.myMargin) / 2);
    T.slowAsked = ahead > 0.3 ? Math.min(15, ahead * 20 * 0.6) : 0;
    N.M._ge_net_slow(T.slowAsked);
    const now = performance.now();
    if (now - lastSyncLog > 5000) {
      lastSyncLog = now;
      N.log(`[net] sync: retrace ${N.frame()}, ` + livePeers().map((p) => `${pn(p.ch)} ~${(p.peerFrame + p.rtt / 2 / 20).toFixed(1)} margin mine ${p.myMargin.toFixed(2)} theirs ${p.peerMargin.toFixed(2)} rtt ${Math.round(p.rtt)}`).join('; ') +
            `, slow ${T.slowAsked.toFixed(1)} ms, slowed ${N.stat(7)} ms total, stalled ticks ${N.stat(0)}`);
    }
  }
  // an 'in' packet to player ch: directly, or (guest, no direct link) through the host
  function sendRaw(chan, buf) {
    T.sent++;
    if (SIM.loss && Math.random() * 100 < SIM.loss) { T.simDropped++; return; }
    if (SIM.lat || SIM.jit) { setTimeout(() => { if (chan.readyState === 'open') chan.send(buf); }, (SIM.lat || 0) + Math.random() * (SIM.jit || 0)); return; }
    chan.send(buf);
  }
  function sendIn(ch, buf) {
    if (directOpen(ch)) return sendRaw(T.links[ch].inp, buf);
    if (T.myCh !== 0 && ch !== 0 && directOpen(0)) {
      const w = new Uint8Array(2 + buf.byteLength); w[0] = 4; w[1] = ch; w.set(new Uint8Array(buf), 2);
      T.relayed++; sendRaw(T.links[0].inp, w.buffer);
    }
  }
  function onPacket(buf) {
    T.recv++;
    const dv = new DataView(buf), type = dv.getUint8(0);
    if (type === 4) {   // relay: for me, or (host) pass it on
      const to = dv.getUint8(1), inner = buf.slice(2);
      if (to === T.myCh) return onPacket(inner);
      if (T.myCh === 0 && directOpen(to)) { T.relayed++; sendRaw(T.links[to].inp, inner); }
      return;
    }
    const from = dv.getUint8(1);
    if (from === T.myCh || from > 3) return;
    const p = peer(from);
    if (p.lost) return;
    p.lastRecv = performance.now();
    if (type === 2) { const r = new DataView(new ArrayBuffer(10)); r.setUint8(0, 3); r.setUint8(1, T.myCh); r.setFloat64(2, dv.getFloat64(2, true), true); sendIn(from, r.buffer); return; }
    if (type === 3) { const r = performance.now() - dv.getFloat64(2, true); p.rtt = p.rtt ? p.rtt * 0.8 + r * 0.2 : r; return; }
    if (type !== 1) return;
    const ack = dv.getUint32(2, true), first = dv.getUint32(6, true), count = dv.getUint8(10);
    if (ack > p.peerAck) p.peerAck = ack;
    const on = inSession();
    for (let i = 0; i < count; i++) {
      const f = first + i, v = dv.getUint32(11 + 4 * i, true);
      if (on) N.put(f, from, v); else p.pending.push([f, v]);
    }
    if (on && p.recvTop) {
      const lastRun = N.frame();
      advanceRecvTop(p);
      if (p.recvTop > lastRun && N.ready(lastRun + 1)) N.kick();   // P339: no need to wait for the next tick
    }
    const o = 11 + 4 * count, hf = dv.getUint32(o, true);
    if (hf) p.hash.set(hf, [dv.getUint32(o + 4, true), dv.getUint32(o + 8, true)]);
    const peerFrame = dv.getUint32(o + 12, true);   // P339 time sync: newer packets only (they arrive out of order)
    if (peerFrame >= p.peerFrame) { p.peerFrame = peerFrame; p.peerMargin = dv.getInt16(o + 16, true) / 64; }
  }
  function checkHashes() {
    if (T.desync) return;
    const mine = N.hashes();
    for (const p of livePeers()) {
      if (!p.hash.size) continue;
      for (const h of mine) {
        const q = p.hash.get(h.f);
        if (!q) continue;
        if (q[0] !== h.h || q[1] !== h.s) {
          T.desync = { frame: h.f, mine: [h.h, h.s], theirs: q, ch: p.ch };
          N.log(`[net] DESYNC at retrace ${h.f} with ${pn(p.ch)}: mine ${h.h.toString(16)} ${h.s.toString(16)}, theirs ${q[0].toString(16)} ${q[1].toString(16)}`, 'fail');
          sendCtlTo(p.ch, { t: 'desync', frame: h.f, mine: [h.h, h.s], theirs: q });
          emit('desync', T.desync);
          sendPages(h.f, p.ch);   // P339: the per-page hashes, to name where the two games differ
          return;
        }
        if (h.f > T.myHashSeen) T.myHashSeen = h.f;
        p.hash.delete(h.f);
      }
      if (p.hash.size > 256) for (const k of [...p.hash.keys()].slice(0, 128)) p.hash.delete(k);
    }
  }

  // ---- P344 lost player: every remaining player must run the same inputs, so the coordinator collects what everyone
  // holds of the lost player's input and fixes F (see the top of this section)
  function heldOf(ch) {   // the lost player's inputs this page holds (contiguous up to recvTop; the ring keeps 256 frames)
    const p = peer(ch), v = [], first = Math.max(N.cfg.delay + 1, p.recvTop - 200);
    for (let f = first; f <= p.recvTop; f++) v.push(N.get(f, ch));
    return { ch, top: p.recvTop, first, v };
  }
  function declareLost(ch, why) {
    const p = peer(ch);
    if (p.lost || p.dropping || !inSession()) return;
    p.dropping = true;
    N.log(`[net] ${pn(ch)} lost (${why}): collecting the inputs every player holds`);
    const ask = livePeers().filter((q) => q.ch !== ch && !q.suspect);
    T.drop = { ch, why, held: [heldOf(ch)], wait: new Set(ask.map((q) => q.ch)) };
    for (const q of ask) sendCtlTo(q.ch, { t: 'lost', ch });
    T.drop.timer = setTimeout(finishDrop, 2500);
    if (!ask.length) finishDrop();
  }
  function onLost(m) { const p = peer(m.ch); p.dropping = true; p.suspect = true; sendCtlTo(m.from, Object.assign({ t: 'held' }, heldOf(m.ch))); }
  function onHeld(m, from) {
    const d = T.drop;
    if (!d || d.ch !== m.ch) return;
    d.held.push(m); d.wait.delete(from);
    if (!d.wait.size) finishDrop();
  }
  function finishDrop() {
    const d = T.drop;
    if (!d || d.done) return;
    d.done = true; clearTimeout(d.timer);
    const neutralFrom = Math.max(...d.held.map((h) => h.top)) + 1, first = Math.min(...d.held.map((h) => h.first)), v = [];
    for (let f = first; f < neutralFrom; f++) {
      let x = -1;
      for (const h of d.held) if (f >= h.first && f <= h.top && h.v[f - h.first] !== -1) { x = h.v[f - h.first]; break; }
      v.push(x);
    }
    const m = { t: 'drop', ch: d.ch, neutralFrom, first, v, why: d.why };   // ('from' is the sender of every 'ctl' message)
    for (const q of livePeers()) if (q.ch !== d.ch) sendCtlTo(q.ch, Object.assign({}, m));
    applyDrop(m);
  }
  function applyDrop(m) {
    const p = peer(m.ch);
    if (p.lost) return;
    for (let i = 0; i < m.v.length; i++) if (m.v[i] !== -1) N.put(m.first + i, m.ch, m.v[i]);
    advanceRecvTop(p);
    N.M._ge_net_drop(m.ch, m.neutralFrom);
    p.lost = true;
    if (T.slots[m.ch]) setSlot(m.ch, 'left', m.why);
    T.left = livePeers().length === 0;
    status(`${m.why || pn(m.ch) + ' left'}: controller ${m.ch + 1} stands still from retrace ${m.neutralFrom}`, 'bad');
    emit('left', m.ch, m.why);
    if (N.ready(N.frame() + 1)) N.kick();
  }


  // ================================================================ GoldenEye page glue (main.js hooks)
  const sha256hex = async (buf) => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', buf)), (b) => b.toString(16).padStart(2, '0')).join('');
  let held = null;   // {rom, sum, go}: the game waiting for a session config (or "Play offline")
  N.go = null;
  // main.js start(): returns true while the game must not start yet (online on, no session configured)
  // the session's EEPROM: the host's image (or blank) through G5's seam, never persisted (saves.js)
  N.sandbox = (bytes) => { if (window.GESaves) window.GESaves.sandbox(bytes && bytes.length === 2048 ? bytes : null); };
  N.hold = function (rom, sum, go) {
    if (!N.enabled || N.cfg || N.goNow) return false;
    if (Q.has('rec') || Q.has('replay')) {   // input recorder (below): a one-player det session, starts at once
      N.cfg = { rec: Q.has('rec') ? 1 : 2, script: Q.has('recscript'), offline: true, mask: 1, local: 1, delay: 0, hashEvery: 0 };
      N.recRomSha = sha256hex(rom);
      N.sandbox(null);
      return false;
    }
    if (Q.has('netdet') || Q.has('netloop')) {   // offline test runs start at once
      const mask = parseInt(Q.get('netmask') || 'F', 16);
      N.cfg = Q.has('netdet')
        ? { mask, local: mask, delay: +(Q.get('netd') || 1), hashEvery: +(Q.get('nethashevery') || 1), script: true, offline: true }
        : { mask, local: 1, delay: +(Q.get('netd') || 3), hashEvery: +(Q.get('nethashevery') || 10), loop: { lat: +(Q.get('netlat') || 0), loss: +(Q.get('netloss') || 0) } };
      N.sandbox(null);
      return false;
    }
    held = { rom, sum, go };
    N.romSha256 = null;
    sha256hex(rom).then((h) => { N.romSha256 = h; });
    panelShow(true);
    netStatus('Online: create or join a room (or Play offline)', 'ok');
    return true;
  };
  // main.js preRun: the env variables the game sees (a session: only the whitelisted test ones)
  N.env = function (list) {
    if (!N.cfg) return list;
    const out = list.filter((kv) => ENV_OK.test(kv));
    if (N.cfg.script) out.push('GE_NETSCRIPT=1');
    if (out.length !== list.length + (N.cfg.script ? 1 : 0)) N.log(`[net] session: ${list.length - out.length + (N.cfg.script ? 1 : 0)} URL env variable(s) ignored`);
    return out;
  };
  N.args = (args) => (N.cfg ? [] : args);   // no command-line test flags in a session
  // main.js onRuntimeInitialized, right before callMain: the session's options, EEPROM, hash ranges, then the ring
  N.beforeMain = async function (M) {
    if (!N.cfg) return;
    const cfg = N.cfg;
    // options: the host's ini (written where configLoad reads it; every peer the same file; never saved back)
    try { M.FS.writeFile('/data/ge007.ini', cfg.ini || ''); } catch (e) { N.log('[net] ini: ' + e); }
    // EEPROM: G5's seam (saves.js GESaves.sandbox, set in N.sandbox before restore()) already loaded the host's image
    // or a blank one; without saves.js, the host's image goes to the det fallback buffer
    if (!window.GESaves && cfg.eeprom && cfg.eeprom.length) M.HEAPU8.set(cfg.eeprom.subarray(0, 2048), M._ge_net_eeprom_buf() >>> 0);
    try {   // hashed ranges of the game state (scripts/web/netregions.py)
      let n = 0;
      for (const l of (await (await fetch('ge007.netregions')).text()).split('\n')) {
        const f = l.split(' ');
        if (f[0] === 'R') { M._ge_net_region_add(parseInt(f[1], 16), parseInt(f[2], 16)); n++; }
      }
      if (!n) N.log('[net] ge007.netregions has no ranges: only the N64 DRAM is hashed');
      if (M._ge_web_eeprom_image) M._ge_net_region_add(M._ge_web_eeprom_image() >>> 0, 2048);   // the live EEPROM image (libultra.c)
    } catch (e) { N.log('[net] ge007.netregions not loaded (' + e + '): only the N64 DRAM is hashed'); }
    if (cfg.rec) return recBegin(M, cfg);
    N.begin(M, cfg);
    if (cfg.loop) N.loopback(cfg.loop);
    else if (!cfg.offline) N.startTransport();
    N.log(`[net] session: players ${(cfg.roster || [0]).map((c) => c + 1).join(' ')}, mine ${cfg.local}, input delay ${cfg.delay} (${cfg.delay * 20} ms${cfg.autoDelay ? ', auto' : ''}), ` +
          `hash every ${cfg.hashEvery}, EEPROM ${cfg.eeprom ? 'host image' : 'blank'}, options ${cfg.ini ? "host's ini" : 'defaults'}`);
  };
  document.addEventListener('visibilitychange', () => { if (N.cfg && !N.cfg.offline && N.M && N.background) N.background(document.hidden); });

  // the host's session config (sessionFn of N.host / N.roomCreate)
  // G5 seam (saves.js): hostEeprom() = the host's stored EEPROM image (sent to every guest, used sandboxed: never
  // written back). Options: GESaves.sandbox does not restore any ini, so every peer boots with the default config
  // (hostIni() = ''); sending the host's ini is ready (cfg.ini) once saves.js exposes it.
  N.hostEeprom = async () => (window.GESaves ? window.GESaves.image() : null);
  N.hostIni = async () => '';
  async function sessionConfig({ ping, mask }) {
    const auto = Q.get('netd') ? Q.get('netd') === 'auto' : $('net-delay-auto').checked;
    const d = auto ? N.autoDelay(ping) : +(Q.get('netd') || $('net-delay').value || 3);
    return { mask, delay: Math.max(1, Math.min(16, d | 0)), autoDelay: auto, hashEvery: +(Q.get('nethashevery') || 10),
      ini: await N.hostIni(), eeprom: Q.get('neteep') === 'blank' ? null : await N.hostEeprom() };
  }
  async function netLoad() {
    if (!held) throw new Error(N.cfg ? 'the game has started: reload the page to play online' : 'choose your ROM first, then host or join');
    for (let i = 0; i < 50 && !N.romSha256; i++) await new Promise((r) => setTimeout(r, 100));
    if (!N.buildId) N.buildId = (await sha256hex(await (await fetch('ge007.wasm')).arrayBuffer())).slice(0, 16);
    if (Q.get('roomapi')) N.roomApi = Q.get('roomapi');
    if (!T.on.config) T.on.config = (cfg) => {   // every peer: boot the game into the session
      N.cfg = cfg;
      N.sandbox(cfg.eeprom);
      netStatus('starting the game…', 'ok');
      held.go();
    };
  }

  // ================================================================ the Online panel
  const $ = (id) => document.getElementById(id);
  function panelBuild() {
    const pick = $('rompick');
    const box = document.createElement('div');
    box.id = 'ge-online';
    box.innerHTML = `<style>
#ge-online { width: 640px; max-width: 100%; font: 13px system-ui, sans-serif; }
#ge-online textarea { width: 100%; font: 11px monospace; }
#ge-online .row { display: flex; gap: 6px; align-items: center; flex-wrap: wrap; margin: 4px 0; }
#ge-online .netslot { border: 1px solid #444; border-radius: 4px; padding: 4px; margin: 4px 0; }
#ge-online [hidden] { display: none !important; }
#ge-online .ok { color: #8c8; } #ge-online .bad { color: #e88; }
#net-room-show { font: bold 22px monospace; letter-spacing: 3px; }
#netwait { position: fixed; top: 12px; left: 50%; transform: translateX(-50%); background: #000c; padding: 6px 12px; border-radius: 4px; }
</style>
<label><input type="checkbox" id="net-enable"> Online multiplayer (experimental)</label>
<div id="net-panel" hidden>
  <div class="row" id="net-start-row"><button id="net-room-create">Create room (player 1)</button> or
    <input id="net-room-code" size="7" placeholder="CODE"> <button id="net-room-join">Join room</button>
    <button id="net-offline">Play offline</button></div>
  <div id="net-room-box" class="row" hidden>Room code: <span id="net-room-show"></span> <button id="net-room-copy">Copy</button>
    <small>tell the other players this code</small></div>
  <details id="net-manual-box"><summary>manual codes (no room server)</summary>
    <div class="row"><button id="net-host">Host (player 1)</button> <button id="net-join">Join</button></div>
    <div id="net-step-join" hidden><label>1. Paste player 1's code</label><textarea id="net-offer-in" rows="2"></textarea>
      <div class="row"><button id="net-answer-make">Make my answer code</button></div>
      <label>2. Send this answer code to player 1 <button id="net-answer-copy">Copy</button></label><textarea id="net-answer" rows="2" readonly></textarea></div>
  </details>
  <div id="net-step-host" hidden><div id="net-slots"></div>
    <div class="row" id="net-host-actions"><button id="net-invite">Invite another player</button>
      <button id="net-start" disabled>Start the game</button>
      <label>input delay <input id="net-delay" type="number" min="1" max="16" value="3" style="width:3em"> retraces</label>
      <label><input type="checkbox" id="net-delay-auto" checked> automatic</label></div></div>
  <div class="row"><button id="net-leave" hidden>Leave</button></div>
  <div id="net-status"></div><div id="net-info"></div><div id="net-peers"></div><div id="net-desync" class="bad" hidden></div>
  <small>Every player needs their own GoldenEye 007 (Europe) ROM; only controller inputs travel. Chrome; 2 to 4 players.
    Online, the game uses player 1's options and a fresh save, the N64 pad controls (no mouse aim) and no rumble.</small>
</div>
<div id="netwait" hidden></div>`;
    pick.parentNode.insertBefore(box, pick.nextSibling);
    $('net-enable').checked = N.enabled;
    $('net-enable').onchange = () => { lsSet($('net-enable').checked); location.reload(); };
  }
  const panelShow = (on) => { if ($('net-panel')) $('net-panel').hidden = !on; };
  const netStatus = (s, cls) => { if ($('net-status')) { $('net-status').textContent = s; $('net-status').className = cls || ''; } };
  const copyCode = (id) => { const t = $(id); t.select(); (navigator.clipboard ? navigator.clipboard.writeText(t.value || t.textContent) : Promise.reject()).catch(() => document.execCommand('copy')); };
  const guard = async (fn) => { try { await fn(); } catch (e) { netStatus(String(e.message || e), 'bad'); N.log('[net] ' + e); } };
  const lockStart = (on) => { for (const id of ['net-host', 'net-join', 'net-room-create', 'net-room-join', 'net-offline']) $(id).disabled = on; };
  function panelWire() {
    $('net-offline').onclick = () => { if (!held) return; N.goNow = true; panelShow(false); held.go(); };
    $('net-room-create').onclick = () => guard(async () => {
      lockStart(true);
      try { $('net-room-show').textContent = await window.__online.roomCreate(); } catch (e) { lockStart(false); throw e; }
      $('net-room-box').hidden = false; $('net-step-host').hidden = false; $('net-invite').hidden = true;
    });
    $('net-room-copy').onclick = () => copyCode('net-room-show');
    $('net-room-join').onclick = () => guard(async () => {
      lockStart(true);
      try { await window.__online.roomJoin($('net-room-code').value); } catch (e) { if (!T.role) lockStart(false); throw e; }
    });
    $('net-room-code').onkeydown = (e) => { if (e.key === 'Enter') $('net-room-join').click(); };
    $('net-host').onclick = () => guard(async () => { lockStart(true); $('net-step-host').hidden = false; showSlot(await window.__online.invite()); });
    $('net-invite').onclick = () => guard(async () => { $('net-invite').disabled = true; try { showSlot(await window.__online.invite()); } finally { $('net-invite').disabled = false; } });
    $('net-start').onclick = () => guard(async () => { $('net-start').disabled = $('net-invite').disabled = true; await window.__online.start(); });
    $('net-join').onclick = () => { $('net-step-join').hidden = false; lockStart(true); };
    $('net-answer-make').onclick = () => guard(async () => { $('net-answer-make').disabled = true; $('net-answer').value = await window.__online.join($('net-offer-in').value); });
    $('net-answer-copy').onclick = () => copyCode('net-answer');
    $('net-leave').onclick = () => { window.__online.leave(); $('net-leave').hidden = true; };
  }
  function slotBox(ch) {
    const n = ch + 1;
    let e = $('net-slot-' + n);
    if (e) return e;
    e = document.createElement('div'); e.className = 'netslot'; e.id = 'net-slot-' + n;
    e.innerHTML = `<div class="row"><b>Player ${n}</b><span id="net-slot-${n}-state">invited</span></div>
      <div id="net-slot-${n}-codes"><label>1. Send this code to player ${n} <button id="net-offer-copy-${n}">Copy</button></label>
      <textarea id="net-offer-${n}" readonly rows="2"></textarea>
      <label>2. Paste player ${n}'s answer code here</label><textarea id="net-answer-in-${n}" rows="2"></textarea>
      <div class="row"><button id="net-accept-${n}">Connect player ${n}</button></div></div>`;
    $('net-slots').appendChild(e);
    $('net-offer-copy-' + n).onclick = () => copyCode('net-offer-' + n);
    $('net-accept-' + n).onclick = () => guard(async () => { await window.__online.accept($('net-answer-in-' + n).value); $('net-accept-' + n).disabled = true; });
    return e;
  }
  function showSlot({ ch, code }) {
    slotBox(ch);
    const n = ch + 1;
    $('net-slot-' + n + '-codes').hidden = false; $('net-accept-' + n).disabled = false;
    $('net-offer-' + n).value = code; $('net-answer-in-' + n).value = '';
  }
  const SLOT_TEXT = { invited: 'invited: waiting for the answer code', connecting: 'connecting…', connected: 'connected, checking the game build…',
    joined: 'joined', left: 'left', failed: 'could not connect' };
  T.on.roster = (slots) => {   // host
    let joined = 0;
    for (const s of slots) {
      slotBox(s.ch);
      const n = s.ch + 1;
      if (s.state === 'joined') joined++;
      $('net-slot-' + n + '-codes').hidden = !!T.room || !['invited', 'connecting', 'failed'].includes(s.state);
      $('net-slot-' + n + '-state').textContent = (T.room && s.state === 'invited' ? 'joined the room: connecting…' : SLOT_TEXT[s.state] || s.state) +
        (s.why && s.state !== 'joined' ? ` (${s.why})` : '') + (s.rtt && s.state === 'joined' ? ` · ping ${s.rtt} ms` : '') +
        (s.relay && s.relay.length ? ' · some links through player 1' : '');
    }
    if (!T.started) { $('net-start').disabled = !joined; $('net-start').textContent = `Start the game (${joined + 1} players)`; }
    else $('net-host-actions').hidden = $('net-room-box').hidden = true;
    $('net-invite').disabled = slots.length >= 3 && slots.every((s) => !['left', 'failed'].includes(s.state));
  };
  T.on.status = (s, cls) => {
    netStatus(s, cls);
    if (/^(connected|joined as|you are player)/.test(s) || T.started) { $('net-step-join').hidden = true; $('net-leave').hidden = false; }
    if (T.started) { $('net-start-row').hidden = true; $('net-manual-box').hidden = true; if (T.role !== 'host') $('net-room-box').hidden = true; }
  };
  let waitSince = 0, lastInfo = 0;
  T.on.tick = (t) => {   // ~200 Hz from the pump
    const now = performance.now();
    if (!t.waiting) waitSince = 0; else if (!waitSince) waitSince = now;
    $('netwait').hidden = !(waitSince && now - waitSince > 400);
    if (!$('netwait').hidden) {
      const w = (t.peers || []).filter((p) => p.waiting).map((p) => 'player ' + (p.ch + 1));
      $('netwait').textContent = `waiting for ${w.length ? w.join(', ') : 'the other players'}…`;
    }
    if (now - lastInfo > 500) {
      lastInfo = now;
      $('net-info').textContent = `you are player ${T.myCh + 1} · retrace ${t.frame} · waits ${t.stalls} · input delay ${N.cfg ? N.cfg.delay * 20 + ' ms' + (N.cfg.autoDelay ? ' (auto)' : '') : '–'}`;
      $('net-peers').textContent = (t.peers || []).map((p) => `player ${p.ch + 1}: ${p.lost ? 'left (stands still)' : (p.rtt ? 'ping ' + p.rtt + ' ms' : 'ping –') + (p.waiting ? ' · waiting' : '')}`).join(' · ');
    }
  };
  T.on.desync = (d) => {
    const e = $('net-desync'); if (!e.hidden) return;
    e.hidden = false;
    e.textContent = `Desync at retrace ${d.frame} with player ${(d.ch || 0) + 1}: this game ${d.mine.map((x) => (x >>> 0).toString(16)).join(' ')}, the other ${d.theirs.map((x) => (x >>> 0).toString(16)).join(' ')}. ` +
      'The games no longer show the same thing; please report it (the log below has the details).';
  };
  T.on.desyncReport = (r) => { const e = $('net-desync'); e.hidden = false; e.textContent += ` Where: ${r.where}; ${r.pages} memory page(s) differ.`; };
  T.on.left = () => { if (T.left) { $('netwait').hidden = true; $('net-leave').hidden = true; } };

  // ---- the API the panel buttons call (and scripts/web/netgroup.mjs, which plays every side in one browser)
  window.__online = {
    ready: () => !!held && !N.cfg,
    async host() { return (await window.__online.invite()).code; },
    async invite() { if (T.role === 'host') return N.invite(); await netLoad(); return N.host(sessionConfig); },
    async roomCreate(max = 4) { await netLoad(); return N.roomCreate(sessionConfig, max); },
    async roomJoin(code) { await netLoad(); return N.roomJoin(code); },
    room: () => (T.room ? { code: T.room.code, ch: T.room.ch, closed: !!T.room.closed } : null),
    async join(code) { await netLoad(); return N.join(code); },
    accept: (code) => N.accept(code),
    start: () => N.start(),
    slots: () => N.slots(),
    leave: () => N.leave(),
    stats() {
      const on = !!(N.M && N.base.input !== undefined);
      const hs = on ? N.hashes() : [], h = hs[hs.length - 1];
      return { state: T.state, me: T.myCh + 1, frame: on ? N.frame() : 0, rtt: Math.round(T.rtt), sent: T.sent, recv: T.recv, relayed: T.relayed,
        simDropped: T.simDropped, hashChecked: T.myHashSeen, lastHash: h ? `${h.f}:${h.h.toString(16)}:${h.s.toString(16)}` : '', desync: T.desync,
        desyncReport: T.desyncReport || null, left: T.left, stalls: on ? N.stat(0) : 0, slowedMs: on ? N.stat(7) : 0, waits: on ? N.stat(8) : 0,
        waitMs: on ? N.stat(9) : 0, delay: N.cfg ? N.cfg.delay : 0,
        peers: Object.values(T.peers).map((p) => ({ p: p.ch + 1, rtt: Math.round(p.rtt), m: +p.myMargin.toFixed(2), pm: +p.peerMargin.toFixed(2), lost: p.lost })) };
    },
  };

  // ================================================================ input recorder (.gerec, end-to-end tests)
  // ?rec=1 records a one-player det session from power-on (webnet.c ge_rec_session: det clock, retrace-indexed fibers,
  // virtual AI queue, blank EEPROM, fixed options; the 50 Hz VI stays wall paced, so a person plays it at full speed
  // with sound): the input of every retrace (from the pad, or with &recscript=1 from GE_NETPAD0) and a checkpoint (state
  // hashes H / S) every RECCHECK retraces (default 50). __rec.dump() returns the recording (JSON, run-length packed);
  // __rec.download() saves it as a .gerec file.
  // ?replay=URL plays a .gerec back (&recfast=1: retraces back to back, ~10x real time) and compares every checkpoint;
  // the result line: "[rec] replay: N checkpoints compared, M identical, first difference at retrace F (...)".
  // scripts/web/e2e_replay.sh runs it headless. Checkpoints are exact for the same build only.
  const R = { inputs: [], checks: [], expect: null, compared: 0, same: 0, firstDiff: null, ended: false };
  async function recBegin(M, cfg) {
    const every = +(Q.get('reccheck') || 50);
    M.recInput = (f, v) => { R.inputs[f - 1] = v >>> 0; };
    M.recCheck = (f, h, s, kind) => {
      R.checks.push([f, h >>> 0, s >>> 0, kind]);
      if (!R.expect) return;
      const e = R.expect.get(f);
      if (!e || kind !== 0) return;
      R.compared++;
      if (e[0] === (h >>> 0) && e[1] === (s >>> 0)) R.same++;
      else if (!R.firstDiff) { R.firstDiff = { f, want: e, got: [h >>> 0, s >>> 0] }; N.log(`[rec] FIRST DIFFERENCE at retrace ${f}: recorded ${e[0].toString(16)} ${e[1].toString(16)}, replay ${(h >>> 0).toString(16)} ${(s >>> 0).toString(16)}`); }
    };
    M.recEnd = (f) => {
      R.ended = true;
      N.log(`[rec] replay: ${R.compared} checkpoints compared, ${R.same} identical` + (R.firstDiff ? `, first difference at retrace ${R.firstDiff.f}` : ', no difference') + ` (replay ended at retrace ${f})`);
    };
    if (cfg.rec === 1) { M._ge_rec_session(1, every, 0, 0, 0, 0); N.log('[rec] recording from power-on (__rec.dump() / __rec.download())'); return; }
    const rec = await (await fetch(Q.get('replay'))).json();
    if (rec.format !== 'gerec1') throw new Error('not a .gerec recording');
    const ins = []; for (const [v, n] of rec.inputs) for (let i = 0; i < n; i++) ins.push(v);
    R.expect = new Map(rec.checks.filter((c) => c[3] === 0).map((c) => [c[0], [c[1], c[2]]]));
    const p = M._ge_web_alloc(4 * ins.length) >>> 0;   // before main(): the same on every replay
    new Uint32Array(M.HEAPU8.buffer, p, ins.length).set(ins);
    const build = (await sha256hex(await (await fetch('ge007.wasm')).arrayBuffer())).slice(0, 16);
    if (build !== rec.build) N.log(`[rec] note: recorded with build ${rec.build}, this is ${build}: state hashes are only comparable within one build`);
    M._ge_rec_session(2, rec.checkEvery || every, p, ins.length, Q.has('recfast') ? 1 : 0, 0);
    N.log(`[rec] replay of ${ins.length} retraces, ${R.expect.size} checkpoints`);
  }
  window.__rec = {
    async dump() {
      const n = R.inputs.length, packed = [];
      for (let i = 0; i < n; i++) { const v = R.inputs[i] >>> 0; if (packed.length && packed[packed.length - 1][0] === v) packed[packed.length - 1][1]++; else packed.push([v, 1]); }
      const build = (await sha256hex(await (await fetch('ge007.wasm')).arrayBuffer())).slice(0, 16);
      return JSON.stringify({ format: 'gerec1', build, rom: await N.recRomSha, checkEvery: +(Q.get('reccheck') || 50), n, inputs: packed, checks: R.checks });
    },
    async download() { const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([await window.__rec.dump()])); a.download = 'goldeneye.gerec'; a.click(); },
    result: () => ({ compared: R.compared, same: R.same, firstDiff: R.firstDiff, ended: R.ended, recorded: R.inputs.length, checks: R.checks.length }),
  };

  window.GENet = N;
  if (document.getElementById('rompick')) { panelBuild(); panelWire(); }
})();
