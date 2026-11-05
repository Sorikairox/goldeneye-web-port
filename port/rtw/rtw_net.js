// rtw_net.js — online play of the retrowser.app build (GE_RTW; retrowser standard 1.2, spec §4.12; docs/web/RTW.md).
// Emscripten --pre-js of the rtw build only; the web site's port/web/net.js is unchanged (and not in this build).
//
// The game side stays port/src/web/webnet.c: the input ring (256 frames x 4 channels), the frame barrier (webkernel.c
// wkFrameDet: retrace N runs only when every session channel's input for N is in the ring), input delay D (the local
// input for N + D is sampled at the start of retrace N), the det mode and the state hash every K retraces. This object
// is the worker twin of net.js's packet logic with the transport replaced by the host's: the host page owns the room,
// the WebRTC mesh and its relay, the build / ROM check, the pre-start round trips, the session boot (every peer from
// power-on with the room host's options and native save) and the hidden-tab ticker. So what net.js also did is gone
// here: copy-paste codes, the room function, RTCPeerConnection, pings, the 'config' message (the options are the
// manifest's, dictated by the host; the EEPROM is the native save the host hands over: no _ge_net_eeprom_buf), and
// GENet.hold (the host boots the session). Adapted from the Conker port's port/web/rtw/rtw_net.js (same packets).
// Peers are session indexes (0 = the room host = controller 1, index i = controller i + 1).
//
// Channel 0 (unreliable, unordered): one input packet per receiver (net.js type 1 without its type and sender bytes:
// the host gives the sender's index):
//   u32 ack (the newest frame of the receiver's channel this sender holds without gaps) | u32 first frame | u8 count |
//   u32 input x count (packed: buttons << 16 | (u8)x << 8 | (u8)y) | u32 hash frame | u32 H | u32 S |
//   u32 the sender's last retrace | i16 the sender's input margin of the receiver's channel x 64 (time sync).
// Every input the receiver has not acknowledged is repeated (up to 64), so a lost packet costs nothing. A packet leaves
// as soon as a local input is sampled (webnet.c geNetRetraceStart -> Module.netAfterTick), the same packet once more
// ~8 ms later, a keep-alive every 30 ms. A packet that brings the input a waiting game needs runs the tick at once
// (webkernel.c webkernelKick through _ge_net_kick), in a microtask: never inside rtw_net_recv (the tick switches the
// game's fibers with Asyncify, which must not unwind through the host's call). The round trip is the host's
// (rtw_net_stat kind 0): no pings of our own.
// Channel 1 (reliable, ordered): net.js's 'ctl' messages in binary, u8 type first:
//   1 lost  | u8 ch                          the coordinator asks for the lost player's inputs everyone holds
//   2 held  | u8 ch | u32 top | u32 first | u16 n | n x (u8 has, u32 v)      the answer
//   3 drop  | u8 ch | u32 neutralFrom | u32 first | u16 n | n x (u8 has, u32 v) | UTF-8 why
//                                            the agreed inputs up to F - 1; channel ch reads neutral from F everywhere
//   4 desync| u32 frame | u32 mine H | u32 mine S | u32 theirs H | u32 theirs S
//   5 pages | u32 frame | u32 x (pages + 1)  the per-page hashes of the desync retrace and S (raw, not base64)
// Lost player (net.js decision: it stands still): the host says rtw_net_peer(i, 0) (left, dropped or silent 15 s); the
// coordinator (the lowest index that is neither lost nor silent) collects what everyone holds of i's input, fixes
// F = 1 + the newest frame anyone holds, sends 'drop'; from F channel i reads neutral on every peer (_ge_net_drop).
// Input delay D: rtw_net_stat(0, 3) is the worst 90th-percentile round trip of the session, the same number on every
// peer, so D needs no message: net.js autoDelay(median, p90) = clamp(ceil((median / 2 + (p90 - median) / 2) / 20) + 1,
// 2, 8) is exactly clamp(ceil(p90 / 40) + 1, 2, 8). Option 18 (Online input delay, the room host's) fixes D instead.
// Option 17 (Online play, the room host's): off = every peer of the session plays alone.
// State hash ranges: files.data ge007.netregions (scripts/web/netregions.py on this build's link map: R lines = the
// game's .data/.bss, S lines = symbol names for the desync report) + the live EEPROM image; webnet.c adds the DRAM.
var rtwNet = (function () {
  'use strict';
  const RING = 256, HASH_EVERY = 10, MAX_IN = 64;   // HASH_EVERY = rtw.c RTW_HASH_EVERY
  const HASH_MASK = ['g_GfxMemPos', 'g_gfxTaskSettings'];   // display-list records (rtw.c __wrap_geNetRetraceDone)
  const CH_IN = 0, CH_CTL = 1;
  const PEER_GONE = 0, PEER_CONNECTED = 1, PEER_SILENT = 2;
  const T_LOST = 1, T_HELD = 2, T_DROP = 3, T_DESYNC = 4, T_PAGES = 5;
  const OPT_ONLINE = 17, OPT_NETDELAY = 18;
  const DRAM_BASE = 0x12000000, DRAM_SIZE = 0x800000;   // N64 DRAM in wasm memory (portaddr.h PORT_DRAM_V1_BASE, PLAN 2.2)
  const wasm = (name) => Module['_' + name];
  const say = (s) => out(s);
  const S = {
    active: false, off: false, players: 0, me: 0, delay: 0, autoDelay: false, worst: -1, roster: [], base: {},
    peers: {}, sent: 0, recv: 0, refused: 0, bad: 0, kicks: 0, desync: null, desyncReport: null, myHashSeen: 0,
    hashesMatched: 0, beganAt: -1, errors: 0, lastSend: 0, pumps: 0, drop: null, drops: [], playing: false, slowAsked: 0,
    started: false, sessionSet: false, regions: 0, syms: null,
  };
  const pn = (ch) => `player ${ch + 1}`;
  function guard(what, fn) {   // an exception in a timer is logged (the first few), not lost in the worker's console
    try { fn(); } catch (e) {
      S.errors++;
      if (S.errors <= 3) say(`[net] ${what}: ${String(e && e.stack || e).split('\n').slice(0, 8).join('\n')}`);
    }
  }
  let u32 = null;
  const U32 = () => { if (!u32 || u32.buffer !== HEAPU8.buffer) u32 = new Uint32Array(HEAPU8.buffer); return u32; };

  // ---- the game side (webnet.c), as net.js N.*
  const frame = () => wasm('ge_net_frame')() >>> 0;   // the last retrace run
  const ready = (f) => wasm('ge_net_ready')(f) !== 0;   // may retrace f run (the frame barrier)
  const stat = (i) => U32()[S.base.stat + i];   // webnet.c ge_net_addr(4)
  const has = (f, ch) => U32()[S.base.frame + (f % RING) * 4 + ch] === f;
  const get = (f, ch) => (has(f, ch) ? U32()[S.base.input + (f % RING) * 4 + ch] : -1);
  function put(f, ch, v) {   // a peer's input for frame f
    const run = frame();
    if (f <= run || f > run + RING - 16 || has(f, ch)) return false;   // already run / too far ahead / already there
    const i = (f % RING) * 4 + ch, H = U32();
    H[S.base.input + i] = v >>> 0; H[S.base.frame + i] = f;
    return true;
  }
  const localTop = () => frame() + S.delay;   // newest local frame sampled so far
  function hashes() {   // [{f, h, s}] of the hash ring, oldest first
    const H = U32(), list = [];
    for (let i = 0; i < 64; i++) { const p = S.base.hash + i * 3; if (H[p]) list.push({ f: H[p], h: H[p + 1], s: H[p + 2] }); }
    return list.sort((a, b) => a.f - b.f);
  }
  // run the waiting retrace now (webkernel.c webkernelKick). Never from inside rtw_net_recv (see the top)
  let kickQueued = false;
  const kick = () => {
    if (kickQueued) return;
    kickQueued = true;
    queueMicrotask(() => { kickQueued = false; S.kicks++; guard('kick', () => wasm('ge_net_kick')()); });
  };

  // ---- the host's transport (rtw.c ge_rtw_net_send / ge_rtw_net_stat -> env.rtw_net_send / rtw_net_stat)
  function send(peer, channel, u8) {
    if (u8.length > 65536) return -1;
    HEAPU8.set(u8, wasm('ge_rtw_net_txbuf')() >>> 0);
    const r = wasm('ge_rtw_net_send')(peer, channel, u8.length);
    S.lastSend = r;
    if (r === -1) S.refused++;
    return r;
  }
  const netStat = (peer, kind) => wasm('ge_rtw_net_stat')(peer, kind);

  function peer(ch) {
    return S.peers[ch] || (S.peers[ch] = { ch, recvTop: 0, peerAck: 0, hash: new Map(), myMargin: 0, peerMargin: 0, peerFrame: 0,
      marginAt: 0, rtt: 0, lastRecv: 0, lastSendTop: 0, lastSendAt: 0, lastPacket: null, repeated: true,
      lost: false, suspect: false, dropping: false, state: PEER_CONNECTED });
  }
  const others = () => S.roster.filter((c) => c !== S.me);
  const livePeers = () => others().map(peer).filter((p) => !p.lost);
  // the coordinator of a drop: the lowest index among me and the players that are neither lost nor silent / gone
  const amCoordinator = () => livePeers().every((p) => p.suspect || p.dropping || p.ch > S.me);

  // ---- session start: rtw_net_session(players, me), after rtw_option and before rtw_start (the host's lockstep boot)
  // D = clamp(ceil(p90 / 40) + 1, 2, 8) (see the top); no measurement (-1): 4 as net.js
  const autoDelay = (p90) => (p90 >= 0 ? Math.max(2, Math.min(8, Math.ceil(p90 / 40) + 1)) : 4);
  function begin(players, me) {
    if (S.active || S.off) { say(`[net] rtw_net_session(${players}, ${me}) again: ignored`); return; }
    if (S.started || S.sessionSet) { say(`[net] rtw_net_session(${players}, ${me}) after the start: ignored (lockstep sessions boot from power-on)`); return; }
    if (!(players >= 2 && players <= 4 && me >= 0 && me < players)) { S.off = true; say(`[net] session (${players} players, me ${me}) refused: 2 to 4 players`); return; }
    if (!wasm('ge_rtw_option')(OPT_ONLINE, 1)) {   // option 17 is the room host's, so every peer of the session takes this branch
      S.off = true;
      say(`[net] session of ${players} players: option "Online play" is off (the room host's options): this game runs alone`);
      return;
    }
    S.players = players; S.me = me; S.roster = [...Array(players).keys()];
    S.worst = netStat(0, 3);
    const fixed = wasm('ge_rtw_option')(OPT_NETDELAY, 0) | 0;
    S.autoDelay = !(fixed > 0);
    S.delay = S.autoDelay ? autoDelay(S.worst) : Math.max(1, Math.min(16, fixed));
    for (const [k, i] of [['input', 0], ['frame', 1], ['hash', 2], ['pad', 3], ['stat', 4]]) S.base[k] = (wasm('ge_net_addr')(i) >>> 0) >>> 2;
    const now = performance.now();
    for (const p of others().map(peer)) { p.recvTop = S.delay; p.peerAck = S.delay; p.lastRecv = now; }   // frames 1..D are neutral
    Module.netAfterTick = () => guard('send', afterTick);   // webnet.c geNetRetraceStart, right after the local sample
    S.active = true;
    say(`[net] session: ${players} players, I am ${pn(me)}, input delay ${S.delay}` +
        `${S.autoDelay ? ` (auto: worst round trip p90 ${S.worst} ms)` : ' (option)'}, hash every ${HASH_EVERY} retraces`);
  }
  // gertwBootAsync, right before ge_rtw_before_main / main(): the hashed ranges, then webnet.c's session (det mode,
  // session mask = the game sees exactly the session's controllers, local channel = me)
  function beforeMain() {
    if (!S.active) return;
    const files = Module.files || {};
    const key = Object.keys(files).find((k) => k === 'ge007.netregions' || k.endsWith('/ge007.netregions'));
    if (key) {
      S.syms = [];
      for (const l of new TextDecoder().decode(files[key]).split('\n')) {
        const f = l.split(' ');
        if (f[0] === 'R' && wasm('ge_net_region_add')(parseInt(f[1], 16), parseInt(f[2], 16)) > 0) S.regions++;
        else if (f[0] === 'S' && f.length >= 4) S.syms.push([parseInt(f[1], 16), parseInt(f[2], 16), f[3]]);
      }
    }
    if (!S.regions) say('[net] ge007.netregions has no ranges: only the N64 DRAM is hashed');
    // left out of the hash with the display lists (rtw.c __wrap_geNetRetraceDone): records that describe them
    const masked = [];
    for (const [a, n, name] of S.syms || []) if (HASH_MASK.includes(name) && wasm('ge_rtw_hash_mask_add')(a, n) > 0) masked.push(name);
    say(`[net] left out of the hash with the display lists: ${masked.join(' ') || 'none'}`);
    if (wasm('ge_web_eeprom_image')) wasm('ge_net_region_add')(wasm('ge_web_eeprom_image')() >>> 0, 2048);   // the live EEPROM image (libultra.c)
    wasm('ge_net_session')((1 << S.players) - 1, 1 << S.me, S.delay, HASH_EVERY);
    S.sessionSet = true;
    S.beganAt = frame();   // 0: before the first retrace (the lockstep boot)
  }
  // rtw_net_peer(peer, state): 1 connected / back, 2 silent (3 s), 0 gone (final)
  function peerState(ch, state) {
    if (!S.active || ch === S.me || ch < 0 || ch >= S.players) return;
    const p = peer(ch), was = p.state;
    p.state = state;
    if (p.lost) return;
    if (state === PEER_CONNECTED) {
      if (was !== PEER_CONNECTED) say(`[net] ${pn(ch)} is back`);
      p.suspect = false; p.lastRecv = performance.now();
    } else if (state === PEER_SILENT) {
      p.suspect = true;
      say(`[net] ${pn(ch)} is silent (nothing heard for 3 s)`);
    } else if (state === PEER_GONE) {
      p.suspect = true;
      say(`[net] ${pn(ch)} is gone (left, dropped, or silent for 15 s)`);
      if (S.started && amCoordinator()) declareLost(ch, `${pn(ch)} left`);
      // a coordinator that went silent too: the next one takes over when the host says so (its own state change)
      if (S.started) for (const q of livePeers()) if (q.state === PEER_GONE && !q.dropping && amCoordinator()) declareLost(q.ch, `${pn(q.ch)} left`);
    }
  }

  // ---- after main(): the pump sends inputs, keeps alive, checks the hashes; the time sync
  function startTransport() {
    if (!S.active || S.started) return;
    S.started = true;
    for (const p of livePeers()) advanceRecvTop(p);
    say(`[net] waiting for the other player${others().length > 1 ? 's' : ''}`);
    setInterval(() => guard('pump', pump), 5);
    setInterval(() => guard('time sync', timeSync), 250);
    // peers the host already reported gone at the boot
    for (const p of livePeers()) if (p.state === PEER_GONE && amCoordinator()) declareLost(p.ch, `${pn(p.ch)} left`);
  }
  function advanceRecvTop(p) { while (has(p.recvTop + 1, p.ch)) p.recvTop++; }
  function sendInputsTo(p, now) {
    const top = localTop();
    const first = Math.max(p.peerAck + 1, S.delay + 1, top - (MAX_IN - 1));
    const count = Math.max(0, top - first + 1);
    const b = new Uint8Array(9 + 4 * count + 18), dv = new DataView(b.buffer);
    dv.setUint32(0, p.recvTop, true); dv.setUint32(4, first, true); dv.setUint8(8, count);
    for (let i = 0; i < count; i++) dv.setUint32(9 + 4 * i, get(first + i, S.me) >>> 0, true);
    const hs = hashes(), h = hs[hs.length - 1] || { f: 0, h: 0, s: 0 }, o = 9 + 4 * count;
    dv.setUint32(o, h.f, true); dv.setUint32(o + 4, h.h, true); dv.setUint32(o + 8, h.s, true);
    dv.setUint32(o + 12, frame(), true); dv.setInt16(o + 16, Math.max(-32768, Math.min(32767, Math.round(p.myMargin * 64))), true);
    if (send(p.ch, CH_IN, b) === 0) S.sent++;
    p.lastSendTop = top; p.lastSendAt = now; p.lastPacket = b; p.repeated = false;
  }
  // webnet.c geNetRetraceStart (start of each retrace, right after the local input for N + D was sampled)
  function afterTick() { if (S.active && S.started) { const now = performance.now(); for (const p of livePeers()) sendInputsTo(p, now); } }
  function pump() {
    S.pumps++;
    const now = performance.now(), lastRun = frame();
    for (const p of livePeers()) {
      const margin = p.recvTop - lastRun;   // the input margin of p, time-averaged (500 ms)
      if (p.marginAt) { const a = Math.min(1, (now - p.marginAt) / 500); p.myMargin += (margin - p.myMargin) * a; } else p.myMargin = margin;
      p.marginAt = now;
      if (localTop() !== p.lastSendTop || now - p.lastSendAt > 30) sendInputsTo(p, now);
      else if (!p.repeated && now - p.lastSendAt >= 8) { if (send(p.ch, CH_IN, p.lastPacket) === 0) S.sent++; p.repeated = true; }
      const r = netStat(p.ch, 0);
      if (r >= 0) p.rtt = r;
    }
    checkHashes();
    if (!S.playing && lastRun > S.delay + 1) { S.playing = true; say(`[net] playing (retrace ${lastRun})`); }
  }
  let lastSyncLog = 0;
  function timeSync() {   // net.js: if this peer runs ahead of any player, slow down by half the largest difference
    let ahead = 0;
    for (const p of livePeers()) if (p.peerFrame) ahead = Math.max(ahead, (p.peerMargin - p.myMargin) / 2);
    S.slowAsked = ahead > 0.3 ? Math.min(15, ahead * 20 * 0.6) : 0;
    wasm('ge_net_slow')(S.slowAsked);
    const now = performance.now();
    if (now - lastSyncLog > 5000) {
      lastSyncLog = now;
      say(`[net] sync: retrace ${frame()}, ` + livePeers().map((p) => `${pn(p.ch)} margin mine ${p.myMargin.toFixed(2)} theirs ${p.peerMargin.toFixed(2)} rtt ${Math.round(p.rtt)}`).join('; ') +
          `, slow ${S.slowAsked.toFixed(1)} ms, slowed ${stat(7)} ms total, waits ${stat(8)}, hashes matched ${S.hashesMatched} (to ${S.myHashSeen})`);
    }
  }

  // ---- rtw_net_recv(peer, channel, len): the bytes are copied out here (rtw.c reuses its buffer)
  function recv(from, channel, ptr, len) {
    if (!S.active || from === S.me || from < 0 || from >= S.players) return;
    const p = peer(from);
    if (p.lost) return;
    p.lastRecv = performance.now();
    const b = HEAPU8.slice(ptr, ptr + len);
    S.recv++;
    try {
      if (channel === CH_IN) onInputs(p, b);
      else if (channel === CH_CTL) onCtl(p, b);
    } catch (e) { S.bad++; if (S.bad < 5) say(`[net] bad packet from ${pn(from)} on channel ${channel}: ${e.message || e}`); }
  }
  function onInputs(p, b) {
    if (b.length < 9) throw new Error('short input packet');
    const dv = new DataView(b.buffer), ack = dv.getUint32(0, true), first = dv.getUint32(4, true), count = dv.getUint8(8);
    if (count > MAX_IN || b.length < 9 + 4 * count + 18) throw new Error('input packet length');
    if (ack > p.peerAck && ack <= localTop()) p.peerAck = ack;
    for (let i = 0; i < count; i++) put(first + i, p.ch, dv.getUint32(9 + 4 * i, true));
    const lastRun = frame();
    advanceRecvTop(p);
    if (S.sessionSet && p.recvTop > lastRun && ready(lastRun + 1)) kick();   // no need to wait for the next tick
    const o = 9 + 4 * count, hf = dv.getUint32(o, true);
    if (hf && hf % HASH_EVERY === 0) p.hash.set(hf, [dv.getUint32(o + 4, true), dv.getUint32(o + 8, true)]);
    const peerFrame = dv.getUint32(o + 12, true);   // time sync: newer packets only (they arrive out of order)
    if (peerFrame >= p.peerFrame) { p.peerFrame = peerFrame; p.peerMargin = dv.getInt16(o + 16, true) / 64; }
  }
  function checkHashes() {
    if (S.desync) return;
    const mine = hashes();
    for (const p of livePeers()) {
      if (!p.hash.size) continue;
      for (const h of mine) {
        const q = p.hash.get(h.f);
        if (!q) continue;
        if (q[0] !== h.h || q[1] !== h.s) {
          S.desync = { frame: h.f, mine: [h.h, h.s], theirs: q, ch: p.ch };
          say(`[net] DESYNC at retrace ${h.f} with ${pn(p.ch)}: mine ${h.h.toString(16)} ${h.s.toString(16)}, theirs ${q[0].toString(16)} ${q[1].toString(16)}`);
          const b = new Uint8Array(21), dv = new DataView(b.buffer);
          dv.setUint8(0, T_DESYNC); dv.setUint32(1, h.f, true); dv.setUint32(5, h.h, true); dv.setUint32(9, h.s, true);
          dv.setUint32(13, q[0], true); dv.setUint32(17, q[1], true);
          send(p.ch, CH_CTL, b);
          sendPages(h.f, p.ch);
          return;
        }
        if (h.f > S.myHashSeen) { S.myHashSeen = h.f; S.hashesMatched++; }   // hash points checked (each once)
        p.hash.delete(h.f);
      }
      if (p.hash.size > 256) for (const k of [...p.hash.keys()].slice(0, 128)) p.hash.delete(k);
    }
  }

  // ---- channel 1
  const entriesSize = (n) => 5 * n;
  function writeEntries(dv, o, v) {   // v[i] = input of frame first + i, or -1 (not held)
    for (let i = 0; i < v.length; i++, o += 5) { dv.setUint8(o, v[i] === -1 ? 0 : 1); dv.setUint32(o + 1, v[i] === -1 ? 0 : v[i] >>> 0, true); }
    return o;
  }
  function readEntries(dv, o, n) {
    if (n > 1024 || o + entriesSize(n) > dv.byteLength) throw new Error('entries length');
    const v = [];
    for (let i = 0; i < n; i++, o += 5) v.push(dv.getUint8(o) ? dv.getUint32(o + 1, true) : -1);
    return v;
  }
  function onCtl(p, b) {
    if (!b.length) throw new Error('empty message');
    const dv = new DataView(b.buffer), type = b[0];
    const chOk = (ch) => { if (!(ch >= 0 && ch < S.players)) throw new Error('channel'); return ch; };
    if (type === T_LOST) { if (b.length < 2) throw new Error('lost'); onLost(chOk(b[1]), p.ch); }
    else if (type === T_HELD) {
      if (b.length < 12) throw new Error('held');
      const ch = chOk(b[1]), top = dv.getUint32(2, true), first = dv.getUint32(6, true), n = dv.getUint16(10, true);
      onHeld({ ch, top, first, v: readEntries(dv, 12, n) }, p.ch);
    } else if (type === T_DROP) {
      if (b.length < 12) throw new Error('drop');
      const ch = chOk(b[1]), neutralFrom = dv.getUint32(2, true), first = dv.getUint32(6, true), n = dv.getUint16(10, true);
      const v = readEntries(dv, 12, n), why = new TextDecoder().decode(b.subarray(12 + entriesSize(n), 12 + entriesSize(n) + 200));
      if (ch === S.me) return;   // I was dropped by the others: they go on without me (the host ends my session)
      applyDrop({ ch, neutralFrom, first, v, why });
    } else if (type === T_DESYNC) {
      if (b.length < 21) throw new Error('desync');
      const f = dv.getUint32(1, true);
      say(`[net] ${pn(p.ch)} reports a DESYNC at retrace ${f}: theirs ${dv.getUint32(5, true).toString(16)} ${dv.getUint32(9, true).toString(16)}`);
      if (!S.desync) S.desync = { frame: f, mine: [dv.getUint32(13, true), dv.getUint32(17, true)], theirs: [dv.getUint32(5, true), dv.getUint32(9, true)], ch: p.ch, remote: true };
      sendPages(f, p.ch);
    } else if (type === T_PAGES) {
      if (b.length < 5 || (b.length - 5) % 4) throw new Error('pages');
      comparePages(dv.getUint32(1, true), new Uint32Array(b.buffer.slice(5)), p.ch);
    } else throw new Error(`message type ${type}`);
  }
  const sendCtl = (ch, b) => send(ch, CH_CTL, b);

  // ---- lost player (net.js)
  function heldOf(ch) {   // the lost player's inputs this game holds (contiguous up to recvTop; the ring keeps 256 frames)
    const p = peer(ch), v = [], first = Math.max(S.delay + 1, p.recvTop - 200);
    for (let f = first; f <= p.recvTop; f++) v.push(get(f, ch));
    return { ch, top: p.recvTop, first, v };
  }
  function declareLost(ch, why) {
    const p = peer(ch);
    if (p.lost || p.dropping || !S.active) return;
    p.dropping = true;
    say(`[net] ${pn(ch)} lost (${why}): collecting the inputs every player holds`);
    const ask = livePeers().filter((q) => q.ch !== ch && !q.suspect);
    S.drop = { ch, why, held: [heldOf(ch)], wait: new Set(ask.map((q) => q.ch)) };
    for (const q of ask) sendCtl(q.ch, Uint8Array.of(T_LOST, ch));
    S.drop.timer = setTimeout(() => guard('drop', finishDrop), 2500);
    if (!ask.length) finishDrop();
  }
  function onLost(ch, from) {
    const p = peer(ch); p.dropping = true; p.suspect = true;
    const h = heldOf(ch), b = new Uint8Array(12 + entriesSize(h.v.length)), dv = new DataView(b.buffer);
    dv.setUint8(0, T_HELD); dv.setUint8(1, ch); dv.setUint32(2, h.top, true); dv.setUint32(6, h.first, true); dv.setUint16(10, h.v.length, true);
    writeEntries(dv, 12, h.v);
    sendCtl(from, b);
  }
  function onHeld(m, from) {
    const d = S.drop;
    if (!d || d.ch !== m.ch || !d.wait.has(from)) return;
    d.held.push(m); d.wait.delete(from);
    if (!d.wait.size) finishDrop();
  }
  function finishDrop() {
    const d = S.drop;
    if (!d || d.done) return;
    d.done = true; clearTimeout(d.timer);
    const neutralFrom = Math.max(...d.held.map((h) => h.top)) + 1, first = Math.min(...d.held.map((h) => h.first)), v = [];
    for (let f = first; f < neutralFrom; f++) {
      let x = -1;
      for (const h of d.held) if (f >= h.first && f <= h.top && h.v[f - h.first] !== -1) { x = h.v[f - h.first]; break; }
      v.push(x);
    }
    const why = new TextEncoder().encode(d.why).subarray(0, 200);
    const b = new Uint8Array(12 + entriesSize(v.length) + why.length), dv = new DataView(b.buffer);
    dv.setUint8(0, T_DROP); dv.setUint8(1, d.ch); dv.setUint32(2, neutralFrom, true); dv.setUint32(6, first, true); dv.setUint16(10, v.length, true);
    b.set(why, writeEntries(dv, 12, v));
    for (const q of livePeers()) if (q.ch !== d.ch) sendCtl(q.ch, b);
    applyDrop({ ch: d.ch, neutralFrom, first, v, why: d.why });
  }
  function applyDrop(m) {
    const p = peer(m.ch);
    if (p.lost) return;
    for (let i = 0; i < m.v.length; i++) if (m.v[i] !== -1) put(m.first + i, m.ch, m.v[i]);
    advanceRecvTop(p);
    wasm('ge_net_drop')(m.ch, m.neutralFrom);
    p.lost = true; p.dropping = true;
    S.drops.push({ ch: m.ch, neutralFrom: m.neutralFrom, at: frame() });
    if (S.drop && S.drop.ch === m.ch) S.drop = null;
    say(`[net] ${m.why || pn(m.ch) + ' left'}: controller ${m.ch + 1} stands still from retrace ${m.neutralFrom}`);
    if (ready(frame() + 1)) kick();
  }

  // ---- desync report (net.js): both peers swap the per-page hashes of the desync retrace and name the first page
  // that differs, with the game symbols in it (ge007.netregions S lines)
  const pagesSent = new Set();
  const pageCount = () => wasm('ge_net_page_info')(0, 0) >>> 0;
  function pageVector(f) {
    const p = wasm('ge_net_page_hashes')(f) >>> 0;
    if (!p) return null;
    const n = pageCount() + wasm('ge_net_page_info')(1, 0);
    return new Uint32Array(HEAPU8.buffer.slice(p, p + 4 * n));
  }
  function sendPages(f, ch) {
    if (!f || pagesSent.has(f + ':' + ch)) return;
    pagesSent.add(f + ':' + ch);
    const v = pageVector(f);
    if (!v) { say(`[net] desync report: the page hashes of retrace ${f} are no longer kept`); return; }
    const b = new Uint8Array(5 + v.byteLength);
    b[0] = T_PAGES; new DataView(b.buffer).setUint32(1, f, true); b.set(new Uint8Array(v.buffer), 5);
    if (b.length > 65536) { say(`[net] desync report: ${b.length} bytes of page hashes do not fit a channel-1 packet`); return; }
    sendCtl(ch, b);
  }
  const hex8 = (x) => (x >>> 0).toString(16).toUpperCase().padStart(8, '0');
  function where(a, size) {
    if (a >= DRAM_BASE && a < DRAM_BASE + DRAM_SIZE) return `N64 DRAM ${hex8(0x80000000 + a - DRAM_BASE)}..${hex8(0x80000000 + a - DRAM_BASE + size - 1)}`;
    const names = (S.syms || []).filter(([sa, sl]) => sa < a + size && sa + sl > a).map((x) => x[2]);
    return `wasm ${hex8(a)}..${hex8(a + size - 1)}${names.length ? ': ' + names.slice(0, 6).join(' ') + (names.length > 6 ? ' …' : '') : ''}`;
  }
  function comparePages(f, theirs, from) {
    const mine = pageVector(f);
    sendPages(f, from);   // the other side needs mine too
    if (!mine) return;
    const pages = pageCount();
    if (theirs.length !== mine.length) { say(`[net] desync report from ${pn(from)}: ${theirs.length} page hashes, mine ${mine.length}`); return; }
    const diff = [];
    for (let i = 0; i < pages; i++) if (mine[i] !== theirs[i]) diff.push(i);
    const stacks = mine[pages] !== theirs[pages];
    const at = diff.length ? where(wasm('ge_net_page_info')(2, diff[0]) >>> 0, wasm('ge_net_page_info')(3, diff[0]) >>> 0) : '';
    const text = diff.length ? `first different page ${diff[0]} (${at})` : 'no hashed page differs';
    S.desyncReport = { frame: f, ch: from, where: text, pages: diff.length, firstPages: diff.slice(0, 8), stacks };
    say(`[net] DESYNC REPORT retrace ${f} (this game vs ${pn(from)}): ${text}; ${diff.length} page(s) differ` +
        `${diff.length ? ' [' + diff.slice(0, 8).join(' ') + (diff.length > 8 ? ' …' : '') + ']' : ''}, thread stacks ${stacks ? 'differ' : 'equal'}`);
  }

  // test aid / status (scripts/rtw/rtw_net_e2e.mjs through globalThis.__rtwGoldenEye in the game's worker)
  function status() {
    if (!S.active) return { active: false, off: S.off };
    return { active: true, sessionSet: S.sessionSet, beganAt: S.beganAt, players: S.players, me: S.me, delay: S.delay, autoDelay: S.autoDelay,
      worst: S.worst, frame: frame(), regions: S.regions, hashes: hashes(), hashesMatched: S.hashesMatched, hashChecked: S.myHashSeen,
      desync: S.desync, desyncReport: S.desyncReport, drops: S.drops.slice(), errors: S.errors, lastSend: S.lastSend, pumps: S.pumps,
      sent: S.sent, recv: S.recv, refused: S.refused, bad: S.bad, kicks: S.kicks, playing: S.playing,
      waits: stat(8), waitMs: stat(9), slowedMs: stat(7), stalledTicks: stat(0),
      peers: others().map((c) => { const p = peer(c); return { ch: c, state: p.state, lost: p.lost, suspect: p.suspect, rtt: p.rtt, recvTop: p.recvTop, peerAck: p.peerAck }; }) };
  }
  // the ring's entries of channel ch for frames a..b (tests: the remote inputs that arrived)
  function ring(ch, a, b) { const list = []; for (let f = a; f <= b; f++) list.push(get(f, ch)); return list; }
  globalThis.__rtwGoldenEye = { Module, net: status, ring, frame: () => wasm('ge_net_frame')() >>> 0,
    option: (id) => wasm('ge_rtw_option')(id, -1), optionNow: (id) => wasm('ge_rtw_option_now')(id),
    settings: () => ({ ...(typeof gertwSettings === 'object' && gertwSettings.rec || {}) }),
    // test aid: set one of this player's own settings as the overlay would (record + live)
    setOwn: (id, v) => { if (typeof gertwSettings === 'object') gertwSettings.put(id, v); wasm('ge_rtw_own_option')(id, v); } };
  return { begin, beforeMain, peerState, startTransport, recv, status, ring, get active() { return S.active; } };
})();
