// GoldenEye 007 web port: page side (docs/web/PLAN.md).
// 1. get the player's ROM (file picker, IndexedDB copy, or a dev ?romurl=), normalise its byte order,
//    check it is the EU ROM, write it into the Emscripten FS as /data/ge007.pal-final.z64;
// 2. TEMPORARY (G1 dev only): ?assets=URL fetches the dev-generated sidecars (pcmodels/pccg) from a local
//    server into /data/. The plan replaces this with in-browser conversion from the ROM;
// 3. load ge007.js and call main().
'use strict';
const EU_SHA1 = '167c3c433dec1f1eb921736f7d53fac8cb45ee31';
const ROM_NAME = 'ge007.pal-final.z64';
const params = new URLSearchParams(location.search);
const logEl = document.getElementById('log');
const statusEl = document.getElementById('status');
function log(s) {
  console.log(s);
  logEl.textContent += s + '\n';
  if (logEl.textContent.length > 60000) logEl.textContent = logEl.textContent.slice(-40000);
  logEl.scrollTop = logEl.scrollHeight;
}
function status(s) { statusEl.textContent = s; }

// --- ROM handling -------------------------------------------------------------------------------
function toZ64(buf) {
  const b = new Uint8Array(buf);
  const m = (b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) >>> 0;
  if (m === 0x80371240) return b;                       // .z64 big-endian
  const out = new Uint8Array(b.length);
  if (m === 0x37804012) {                               // .v64 byte-swapped
    for (let i = 0; i + 1 < b.length; i += 2) { out[i] = b[i + 1]; out[i + 1] = b[i]; }
    return out;
  }
  if (m === 0x40123780) {                               // .n64 little-endian words
    for (let i = 0; i + 3 < b.length; i += 4) { out[i] = b[i + 3]; out[i + 1] = b[i + 2]; out[i + 2] = b[i + 1]; out[i + 3] = b[i]; }
    return out;
  }
  throw new Error('not an N64 ROM (unknown header)');
}
async function sha1hex(u8) {
  const h = await crypto.subtle.digest('SHA-1', u8);
  return [...new Uint8Array(h)].map((x) => x.toString(16).padStart(2, '0')).join('');
}
function idb() {
  return new Promise((res, rej) => {
    const r = indexedDB.open('ge007-web', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('files');
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
}
async function idbGet(k) {
  try { const db = await idb(); return await new Promise((res) => { const q = db.transaction('files').objectStore('files').get(k); q.onsuccess = () => res(q.result); q.onerror = () => res(undefined); }); }
  catch (e) { return undefined; }
}
async function idbPut(k, v) {
  try { const db = await idb(); await new Promise((res) => { const t = db.transaction('files', 'readwrite'); t.objectStore('files').put(v, k); t.oncomplete = res; t.onerror = res; }); }
  catch (e) { /* private mode: no persistence, still playable */ }
}
async function acceptRom(buf, remember) {
  const z = toZ64(buf);
  const sum = await sha1hex(z);
  if (sum !== EU_SHA1) throw new Error(`this ROM is not GoldenEye 007 (Europe): SHA-1 ${sum}`);
  if (remember) await idbPut('rom', z);
  return z;
}

// --- dev-only sidecar fetch (temporary, see PLAN.md phase W2) -------------------------------------
async function fetchDevAssets(base) {
  const files = ['pcmodels-pal-final/pcmodels.bin', 'pcmodels-pal-final/manifest.csv',
                 'pccg-pal-final/pccg.bin', 'pccg-pal-final/manifest.csv'];
  const out = {};
  for (const f of files) {
    const r = await fetch(`${base}/${f}`);
    if (!r.ok) throw new Error(`dev asset ${f}: HTTP ${r.status}`);
    out[f] = new Uint8Array(await r.arrayBuffer());
  }
  return out;
}

// --- start the game ------------------------------------------------------------------------------
let started = false;
async function start(rom) {
  if (started) return; started = true;
  document.getElementById('rompick').style.display = 'none';
  status('loading…');
  let dev = null;
  if (params.get('assets')) { dev = await fetchDevAssets(params.get('assets')); log('[page] dev sidecars fetched'); }
  window.Module = {
    canvas: document.getElementById('canvas'),
    print: (s) => log(s),
    printErr: (s) => log(s),
    noInitialRun: true,
    // dev/test: &env=NAME=VALUE (repeatable) sets a C environment variable (getenv) before main(),
    // e.g. &env=GE_WKTRACE=900 or &env=GE_INPUTSCRIPT=120:START;180:A (port/src/input.c)
    preRun: [() => {
      for (const kv of params.getAll('env')) {
        const i = kv.indexOf('='); Module.ENV[i < 0 ? kv : kv.slice(0, i)] = i < 0 ? '1' : kv.slice(i + 1);
      }
    }],
    onAbort: (w) => { log('[page] ABORT: ' + w); status('crashed: ' + w); },
    onRuntimeInitialized() {
      const FS = Module.FS;
      FS.mkdir('/data');
      FS.writeFile('/data/' + ROM_NAME, rom);
      if (dev) for (const [f, d] of Object.entries(dev)) {
        const dir = '/data/' + f.split('/')[0];
        try { FS.mkdir(dir); } catch (e) {}
        FS.writeFile('/data/' + f, d);
      }
      FS.chdir('/');
      status('');
      log('[page] calling main()');
      // ?args=-level_09+-foo passes command-line arguments (as on native)
      const args = (params.get('args') || '').split(/[ +]/).filter(Boolean);
      try { Module.callMain(args); } catch (e) { if (e !== 'unwind') { log('[page] main threw: ' + e); throw e; } }
    },
  };
  const s = document.createElement('script');
  s.src = 'ge007.js';
  s.onerror = () => status('could not load ge007.js');
  document.body.appendChild(s);
}

window.addEventListener('error', (e) => log('[page] error: ' + (e.message || e)));
window.addEventListener('unhandledrejection', (e) => log('[page] rejection: ' + (e.reason && (e.reason.stack || e.reason))));
document.getElementById('romfile').addEventListener('change', async (ev) => {
  const f = ev.target.files[0]; if (!f) return;
  try { start(await acceptRom(await f.arrayBuffer(), true)); } catch (e) { status(String(e.message || e)); }
});
(async () => {
  const url = params.get('romurl');            // dev only: a local copy served by scripts/web/serve.sh
  if (url) {
    try { const r = await fetch(url); start(await acceptRom(await r.arrayBuffer(), false)); }
    catch (e) { status('romurl: ' + (e.message || e)); }
    return;
  }
  const saved = await idbGet('rom');
  if (saved) { status('using the ROM stored in this browser'); try { start(await acceptRom(saved.buffer || saved, false)); } catch (e) { status(String(e.message || e)); } }
  else status('choose your ROM to start');
})();
