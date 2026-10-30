// G6 (from the Conker web port, P345): a local copy of the room-code server for tests (port/netlify/functions/room/core.mjs on a Map instead of
// Netlify Blobs), so browser tests never touch the production function. It answers POST /api/room with CORS (the test
// pages come from another localhost port and use ?roomapi=http://127.0.0.1:PORT/api/room).
//   node scripts/web/roomserver.mjs PORT        (prints "roomserver: listening on PORT"; stop it with kill)
//   node scripts/web/roomserver.mjs --selftest  (checks the API: errors, slots in join order, CAS, expiry)
import http from 'node:http';
import { httpHandle, handle, ROOM_TTL_MS } from '../../port/netlify/functions/room/core.mjs';

export function memStore() {
  const m = new Map(); let n = 0;
  return {
    m,
    async read(k) { const e = m.get(k); return e ? { data: structuredClone(e.data), etag: e.etag } : null; },
    async write(k, data, o) {
      const e = m.get(k);
      if (o.isNew ? e : (!e || e.etag !== o.etag)) return false;
      m.set(k, { data: structuredClone(data), etag: 'e' + ++n }); return true;
    },
    async del(k) { m.delete(k); },
    async keys() { return [...m.keys()]; },
  };
}

async function selftest() {
  const st = memStore(), B = 'ab'.repeat(8), R = 'cd'.repeat(32);
  let ok = 0, bad = 0;
  const t = (name, cond, got) => { if (cond) ok++; else { bad++; console.log('FAIL ' + name + ': ' + JSON.stringify(got)); } };
  const c = await handle({ op: 'create', build: B, rom: R, max: 3 }, st);
  t('create', c.ok && /^[A-Z0-9]{5}$/.test(c.code) && c.secret.length === 32, c);
  let r = await handle({ op: 'join', code: 'QQQQQ', build: B, rom: R }, st); t('unknown', r.code === 'unknown' && r.status === 404, r);
  r = await handle({ op: 'join', code: 'K7', build: B, rom: R }, st); t('badcode', r.code === 'badcode', r);
  r = await handle({ op: 'join', code: c.code, build: 'ff'.repeat(8), rom: R }, st); t('build', r.code === 'build', r);
  r = await handle({ op: 'join', code: c.code, build: B, rom: 'ee'.repeat(32) }, st); t('rom', r.code === 'rom', r);
  const [g1, g2] = await Promise.all([handle({ op: 'join', code: c.code.toLowerCase(), build: B, rom: R }, st), handle({ op: 'join', code: c.code, build: B, rom: R }, st)]);
  t('two joins at once get slots 1 and 2', [g1.ch, g2.ch].sort().join() === '1,2', [g1, g2]);
  r = await handle({ op: 'join', code: c.code, build: B, rom: R }, st); t('full', r.code === 'full' && /full/.test(r.error), r);
  r = await handle({ op: 'host', code: c.code, secret: 'x' }, st); t('host secret', r.code === 'secret', r);
  r = await handle({ op: 'host', code: c.code, secret: c.secret }, st); t('host poll', r.slots.length === 2 && r.slots.every((s) => s.state === 'waiting'), r);
  r = await handle({ op: 'offer', code: c.code, secret: c.secret, ch: g1.ch, gen: 1, sdp: 'v=0 offer' }, st); t('offer', r.ok, r);
  r = await handle({ op: 'guest', code: c.code, ch: g1.ch, token: g2.token }, st); t('guest token', r.code === 'token', r);
  r = await handle({ op: 'guest', code: c.code, ch: g1.ch, token: g1.token }, st); t('guest poll', r.offer === 'v=0 offer', r);
  r = await handle({ op: 'answer', code: c.code, ch: g1.ch, token: g1.token, sdp: 'x'.repeat(10001) }, st); t('sdp size', r.code === 'bad', r);
  r = await handle({ op: 'answer', code: c.code, ch: g1.ch, token: g1.token, sdp: 'v=0 answer' }, st); t('answer', r.ok, r);
  r = await handle({ op: 'host', code: c.code, secret: c.secret }, st); t('host sees answer', r.slots.find((s) => s.ch === g1.ch).answer === 'v=0 answer', r);
  // an idle slot without an answer goes to the next guest (gen 2)
  r = await handle({ op: 'join', code: c.code, build: B, rom: R }, st, { now: Date.now() + 25000 }); t('idle slot reused', r.ch === g2.ch, r);
  r = await handle({ op: 'host', code: c.code, secret: c.secret }, st, { now: Date.now() + 25000 }); t('gen 2', r.slots.find((s) => s.ch === g2.ch).gen === 2, r);
  r = await handle({ op: 'host', code: c.code, secret: c.secret }, st, { now: Date.now() + ROOM_TTL_MS + 1 }); t('expired', r.code === 'unknown' && /expired/.test(r.error) && !st.m.size, r);
  const c2 = await handle({ op: 'create', build: B, rom: R }, st);
  r = await handle({ op: 'close', code: c2.code, secret: c2.secret }, st); t('close', r.ok && !st.m.size, r);
  r = await handle({ op: 'join', code: c2.code, build: B, rom: R }, st); t('closed = unknown', r.code === 'unknown', r);
  console.log(`roomserver selftest: ${ok} ok, ${bad} failed`);
  process.exit(bad ? 1 : 0);
}

if (process.argv[2] === '--selftest') await selftest();
else {
  const port = +process.argv[2] || 8899, st = memStore();
  const cors = { 'access-control-allow-origin': '*', 'access-control-allow-methods': 'POST, OPTIONS', 'access-control-allow-headers': 'content-type' };
  http.createServer(async (req, res) => {
    if (req.method === 'OPTIONS') { res.writeHead(204, cors); return res.end(); }
    if (!req.url.startsWith('/api/room')) { res.writeHead(404, cors); return res.end(); }
    const chunks = []; for await (const c of req) chunks.push(c);
    const r = await httpHandle(new Request('http://x/api/room', { method: req.method, body: req.method === 'POST' ? Buffer.concat(chunks) : undefined }), st, req.socket.remoteAddress);
    const body = await r.text();
    try { const j = JSON.parse(body); console.log(`roomserver: ${JSON.parse(chunks.join('') || '{}').op} -> ${r.status}${j.error ? ' ' + j.error : ''}${j.code && !j.error ? ' ' + j.code : ''}`); } catch (e) { /* log only */ }
    res.writeHead(r.status, Object.assign({ 'content-type': 'application/json' }, cors)); res.end(body);
  }).listen(port, '127.0.0.1', () => console.log('roomserver: listening on ' + port));
}
