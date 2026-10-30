// G6 room codes (docs/web/ONLINE.md), copied from the Conker web port (P345, its docs/design/ONLINE_MULTIPLAYER.md section 11).
// The logic of the room server, independent of where rooms are stored: room.mjs (the Netlify Function) runs it on
// Netlify Blobs, tools/port/scripts/roomserver.mjs (local tests) on a Map. It only carries the WebRTC offer / answer
// between the host (player 1) and each guest (players 2..4); everything after that goes peer to peer (net.js).
//
// API: POST JSON {op, ...} to /api/room. Answers are JSON: {ok: true, ...} or {error: 'text for the player', code} with
// an HTTP 4xx status.
//   create {build, rom, max}           host  -> {code, secret, max, ttl}       (max = players 2..4)
//   join   {code, build, rom}          guest -> {ch, token, max}               slots 1..max-1 (players 2..max) in join order
//   host   {code, secret}              host poll -> {slots: [{ch, gen, state, answer}], ttl}
//   offer  {code, secret, ch, gen, sdp} host: the offer for a waiting guest slot
//   guest  {code, ch, token}           guest poll -> {offer}                   (offer = sdp or null while waiting)
//   answer {code, ch, token, sdp}      guest: its answer
//   close  {code, secret}              host: delete the room (also done when the game starts)
// A room holds nothing but the build id / ROM SHA-256 to compare, the slots' SDPs, random tokens and times; no names,
// no addresses (the SDPs carry the peers' ICE candidates, as the copy-paste codes do). Rooms expire 10 minutes after
// they were made. A guest slot that never got an answer and whose guest stopped polling for 20 s is given to the next
// guest who joins a full room (its gen grows, so the host makes a new offer for it).
export const ROOM_TTL_MS = 10 * 60 * 1000;
export const SLOT_IDLE_MS = 20 * 1000;
const ALPHABET = 'ACDEFHJKMNPQRTUVWXY3479';   // no 0/O, 1/I/L, 2/Z, 5/S, 6/G, 8/B
const CODE_LEN = 5;                           // 23^5 = 6.4 million codes
const MAX_BODY = 16384, MAX_SDP = 10000;

const hex = (n) => Array.from(crypto.getRandomValues(new Uint8Array(n)), (b) => b.toString(16).padStart(2, '0')).join('');
export function newCode() {
  const r = crypto.getRandomValues(new Uint32Array(CODE_LEN));
  return Array.from(r, (x) => ALPHABET[x % ALPHABET.length]).join('');
}
// what the player typed -> the code (upper case, no spaces / dashes), or null when it cannot be one
export function cleanCode(s) {
  const c = String(s || '').toUpperCase().replace(/[\s-]+/g, '');
  return new RegExp(`^[${ALPHABET}]{${CODE_LEN}}$`).test(c) ? c : null;
}
class Fail extends Error { constructor(status, code, msg) { super(msg); this.status = status; this.code = code; } }
const fail = (status, code, msg) => { throw new Fail(status, code, msg); };
const need = (ok, what) => { if (!ok) fail(400, 'bad', `bad request (${what})`); };
const key = (code) => 'room-' + code;

// light rate limit, per server instance (best effort: Netlify runs several instances): requests per IP per minute,
// rooms made per IP per 10 minutes
const RATE = { req: 600, create: 20 }, seen = new Map();
function rateLimit(ip, op, now) {
  if (!ip) return;
  let e = seen.get(ip);
  if (!e || now - e.t > 60000) { e = { t: now, n: 0, c: e && now - e.ct < 600000 ? e.c : 0, ct: e && now - e.ct < 600000 ? e.ct : now }; seen.set(ip, e); }
  if (++e.n > RATE.req) fail(429, 'busy', 'too many requests, wait a minute');
  if (op === 'create' && ++e.c > RATE.create) fail(429, 'busy', 'too many rooms made from this address, wait a few minutes');
  if (seen.size > 5000) seen.clear();
}

// store: {read(key) -> {data, etag} | null, write(key, data, {etag} | {isNew: true}) -> modified?, del(key), keys()}
async function load(store, code, now) {
  const r = await store.read(key(code));
  if (!r || !r.data) fail(404, 'unknown', `No room with the code ${code}. Check the code (rooms close when the game starts and expire after 10 minutes).`);
  if (now - r.data.created > ROOM_TTL_MS) { await store.del(key(code)); fail(404, 'unknown', `The room ${code} has expired (rooms last 10 minutes): ask player 1 for a new code.`); }
  return r;
}
// read-modify-write with the store's compare-and-swap (two guests may join at the same moment)
async function update(store, code, now, fn) {
  for (let i = 0; i < 8; i++) {
    const r = await load(store, code, now);
    const res = fn(r.data);
    if (res === undefined) return undefined;   // nothing to write
    if (await store.write(key(code), r.data, { etag: r.etag })) return res;
  }
  fail(503, 'busy', 'the room is busy, try again');
}
const hostOnly = (room, b) => { if (typeof b.secret !== 'string' || b.secret !== room.secret) fail(403, 'secret', 'only player 1 (who made the room) can do this'); };
const slotOf = (room, b) => {
  const s = room.slots[b.ch];
  if (!s || typeof b.token !== 'string' || b.token !== s.token) fail(403, 'token', 'this room slot was given to another player (join again)');
  return s;
};

export async function handle(body, store, { ip = '', now = Date.now() } = {}) {
  try {
    rateLimit(ip, body && body.op, now);
    need(body && typeof body === 'object', 'body');
    const op = body.op;
    if (op === 'create') {
      need(/^[0-9a-f]{16}$/.test(body.build) && /^[0-9a-f]{64}$/.test(body.rom), 'build / rom');
      const max = body.max === undefined ? 4 : body.max;
      need(Number.isInteger(max) && max >= 2 && max <= 4, 'max');
      // tidy up rooms that expired without anybody looking at them (rare: rooms are deleted when the game starts)
      try { for (const k of (await store.keys()).slice(0, 50)) { const r = await store.read(k); if (r && r.data && now - r.data.created > ROOM_TTL_MS) await store.del(k); } } catch (e) { /* best effort */ }
      for (let i = 0; i < 10; i++) {
        const code = newCode(), secret = hex(16);
        const room = { v: 1, created: now, build: body.build, rom: body.rom, max, secret, slots: {} };
        if (await store.write(key(code), room, { isNew: true })) return { ok: true, code, secret, max, ttl: ROOM_TTL_MS };
      }
      fail(503, 'busy', 'could not make a room, try again');
    }
    const code = cleanCode(body.code);
    if (!code) fail(400, 'badcode', `"${String(body.code || '').slice(0, 12)}" is not a room code (${CODE_LEN} letters / digits, e.g. K7QXM)`);
    const ch = body.ch;
    if (['offer', 'guest', 'answer'].includes(op)) need(Number.isInteger(ch) && ch >= 1 && ch <= 3, 'ch');
    if (['offer', 'answer'].includes(op)) need(typeof body.sdp === 'string' && body.sdp.length > 0 && body.sdp.length <= MAX_SDP, 'sdp');
    switch (op) {
      case 'join': {
        need(typeof body.build === 'string' && typeof body.rom === 'string', 'build / rom');
        return await update(store, code, now, (room) => {
          if (room.started) fail(409, 'started', `The game in room ${code} has already started.`);
          if (body.build !== room.build) fail(409, 'build', 'Player 1 runs a different version of the game: both players should reload the page (to get the latest version), then try again.');
          if (body.rom !== room.rom) fail(409, 'rom', 'Your ROM is not the same as player 1\'s: every player needs the GoldenEye 007 EU ROM.');
          let free = null;
          for (let c = 1; c < room.max && free === null; c++) if (!room.slots[c]) free = c;
          for (let c = 1; c < room.max && free === null; c++) { const s = room.slots[c]; if (!s.answer && now - s.seen > SLOT_IDLE_MS) free = c; }
          if (free === null) fail(409, 'full', `Room ${code} is full (${room.max} players).`);
          const old = room.slots[free];
          room.slots[free] = { token: hex(16), gen: old ? old.gen + 1 : 1, seen: now, offer: null, answer: null };
          return { ok: true, ch: free, token: room.slots[free].token, max: room.max };
        });
      }
      case 'host': {
        const r = await load(store, code, now);
        hostOnly(r.data, body);
        const slots = Object.entries(r.data.slots).map(([c, s]) => ({ ch: +c, gen: s.gen, state: s.answer ? 'answered' : s.offer ? 'offered' : 'waiting',
          idle: now - s.seen > SLOT_IDLE_MS, answer: s.answer }));
        return { ok: true, slots, ttl: ROOM_TTL_MS - (now - r.data.created) };
      }
      case 'offer':
        return await update(store, code, now, (room) => {
          hostOnly(room, body);
          const s = room.slots[ch];
          if (!s || s.gen !== body.gen) fail(409, 'gone', 'that player left the room');
          s.offer = body.sdp;
          return { ok: true };
        });
      case 'guest': {
        let r = await load(store, code, now);
        const s = slotOf(r.data, body);
        if (now - s.seen > 5000 && !s.answer)   // keep the slot (a slot idle for 20 s may go to another guest)
          await update(store, code, now, (room) => { const t = slotOf(room, body); t.seen = now; return true; });
        return { ok: true, offer: s.offer };
      }
      case 'answer':
        return await update(store, code, now, (room) => { const s = slotOf(room, body); s.answer = body.sdp; s.seen = now; return { ok: true }; });
      case 'close': {
        const r = await load(store, code, now);
        hostOnly(r.data, body);
        await store.del(key(code));
        return { ok: true };
      }
      default: fail(400, 'bad', 'unknown op');
    }
  } catch (e) {
    if (e instanceof Fail) return { error: e.message, code: e.code, status: e.status };
    return { error: 'server error: ' + (e && e.message || e), code: 'server', status: 500 };
  }
}

// HTTP wrapper shared by the Netlify Function and the local test server (Web Request / Response)
export async function httpHandle(req, store, ip) {
  const head = { 'content-type': 'application/json', 'cache-control': 'no-store' };
  if (req.method !== 'POST') return new Response(JSON.stringify({ error: 'POST only', code: 'bad' }), { status: 405, headers: head });
  const text = await req.text();
  if (text.length > MAX_BODY) return new Response(JSON.stringify({ error: 'request too large', code: 'bad' }), { status: 413, headers: head });
  let body = null; try { body = JSON.parse(text); } catch (e) { /* bad json */ }
  const r = await handle(body, store, { ip });
  const status = r.status || 200; delete r.status;
  return new Response(JSON.stringify(r), { status, headers: head });
}
export { MAX_SDP, CODE_LEN };
