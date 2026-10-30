// GoldenEye 007 web port: page side (docs/web/PLAN.md).
// 1. get the player's ROM (file picker, IndexedDB copy, or a dev ?romurl=), normalise its byte order,
//    check it is the EU ROM, write it into the Emscripten FS as /data/ge007.pal-final.z64;
// 2. load ge007.js; before main(): fill the template binary's ROM-derived data from the ROM
//    (ge007.romfill, port/src/web/romfill.c), then get the PC sidecars (pcmodels/pccg) from the IndexedDB
//    cache or convert them from the ROM in wasm (port/src/convert/geconv.c, progress shown), and write
//    them into /data/ where the game loads them (docs/web/ROMDATA.md);
//    dev only: ?assets=URL fetches Python-generated sidecars from a local server instead;
// 3. call main().
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
  return [z, sum];
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

// --- ROM data the game needs before main() -------------------------------------------------------
const progressEl = document.getElementById('progress');
function progress(done, total) {
  if (!progressEl) return;
  progressEl.style.display = total ? '' : 'none';
  if (total) { progressEl.max = total; progressEl.value = done; }
}
const yieldToBrowser = () => new Promise((r) => setTimeout(r, 0));
const SIDECAR_FILES = ['pcmodels-pal-final/pcmodels.bin', 'pcmodels-pal-final/manifest.csv',
                       'pccg-pal-final/pccg.bin', 'pccg-pal-final/manifest.csv'];   // blob order of ge_web_geconv_blob
// Convert the ROM's models / backgrounds / stage setups into the PC sidecar layout, in wasm. ~600 steps
// (one file each); yields to the browser every ~30 ms so the progress bar moves.
async function convertSidecars(M, romp, romlen) {
  const t0 = performance.now();
  const total = M._ge_web_geconv_begin(romp, romlen);
  if (total < 0) throw new Error('sidecar conversion failed to start (see the log)');
  let left = total, last = performance.now();
  status('converting the game data from your ROM (first start only)…');
  while (left > 0) {
    left = M._ge_web_geconv_step();
    if (left < 0) throw new Error('sidecar conversion failed (see the log)');
    if (performance.now() - last > 30) { progress(total - left, total); await yieldToBrowser(); last = performance.now(); }
  }
  const out = {};
  SIDECAR_FILES.forEach((f, i) => {
    const a = M._ge_web_geconv_blob(i, 0), n = M._ge_web_geconv_blob(i, 1);
    out[f] = M.HEAPU8.slice(a, a + n);
  });
  M._ge_web_geconv_end();
  progress(0, 0);
  log(`[page] sidecars converted in wasm: ${total} steps, ${(performance.now() - t0).toFixed(0)} ms`);
  return out;
}

// dev (?fillcheck): SHA-256 over every symbol extent listed in ge007.romfill, right after the fill and
// before main(); scripts/web/fillcheck.py computes the same over the original (unstripped) binary.
async function fillCheck(M, t) {
  const dv = new DataView(t.buffer, t.byteOffset, t.byteLength), nsyms = dv.getUint32(20, true);
  const parts = [];
  for (let i = 0; i < nsyms; i++) {
    const a = dv.getUint32(24 + 8 * i, true), n = dv.getUint32(28 + 8 * i, true);
    parts.push(M.HEAPU8.slice(a, a + n));
  }
  const all = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
  let o = 0; for (const p of parts) { all.set(p, o); o += p.length; }
  const h = [...new Uint8Array(await crypto.subtle.digest('SHA-256', all))].map((x) => x.toString(16).padStart(2, '0')).join('');
  log(`[page] fillcheck: ${nsyms} symbols, ${all.length} bytes, sha256 ${h}`);
}

// --- start the game ------------------------------------------------------------------------------
let started = false;
async function start(rom, romsum) {
  // G6: with "Online multiplayer" on, the game waits for a session (or "Play offline"); net.js calls back
  if (window.GENet && GENet.hold(rom, romsum, () => start(rom, romsum))) return;
  if (started) return; started = true;
  document.getElementById('rompick').style.display = 'none';
  status('loading…');
  let dev = null;
  if (params.get('assets')) { dev = await fetchDevAssets(params.get('assets')); log('[page] dev sidecars fetched (dev URL, never deploy)'); }
  // the fill table of the template binary (absent only in unstripped dev builds: GE_NOSTRIP=1)
  const fillP = fetch('ge007.romfill').then((r) => (r.ok ? r.arrayBuffer() : null)).catch(() => null);
  window.Module = {
    canvas: document.getElementById('canvas'),
    print: (s) => log(s),
    printErr: (s) => log(s),
    noInitialRun: true,
    // dev/test: &env=NAME=VALUE (repeatable) sets a C environment variable (getenv) before main(),
    // e.g. &env=GE_WKTRACE=900 or &env=GE_INPUTSCRIPT=120:START;180:A (port/src/input.c)
    preRun: [() => {
      const envs = window.GENet ? GENet.env(params.getAll('env')) : params.getAll('env');   // G6: a session whitelists
      for (const kv of envs) {
        const i = kv.indexOf('='); Module.ENV[i < 0 ? kv : kv.slice(0, i)] = i < 0 ? '1' : kv.slice(i + 1);
      }
    }],
    onAbort: (w) => { log('[page] ABORT: ' + w); status('crashed: ' + w); },
    async onRuntimeInitialized() {
      const M = Module, FS = Module.FS;
      FS.mkdir('/data');
      FS.writeFile('/data/' + ROM_NAME, rom);
      const romp = M._ge_web_alloc(rom.length);
      M.HEAPU8.set(rom, romp);
      try {
        const fill = await fillP;
        if (fill) {
          const t = new Uint8Array(fill), tp = M._ge_web_alloc(t.length);
          M.HEAPU8.set(t, tp);
          const rc = M._ge_web_romfill(romp, rom.length, tp, t.length);
          M._ge_web_free(tp);
          if (rc !== 0) throw new Error('could not rebuild the game data from this ROM (romfill ' + rc + ')');
          if (params.has('fillcheck')) await fillCheck(M, t);   // dev: independent hash of the filled symbols
        } else log('[page] no ge007.romfill: unstripped dev build');
        let side = dev;
        const key = `sidecars-v${M._ge_web_geconv_version()}-${romsum}`;
        if (!side) { side = await idbGet(key); if (side) log('[page] sidecars from IndexedDB'); }
        if (!side) {
          side = await convertSidecars(M, romp, rom.length); await idbPut(key, side);
          // dev (&reloadonce): reload once after caching, to test the IndexedDB path in one browser profile
          if (params.has('reloadonce')) { params.delete('reloadonce'); location.replace('?' + params.toString()); return; }
        }
        if (params.has('fillcheck')) for (const [f, d] of Object.entries(side)) {   // dev: compare with the Python output
          const h = [...new Uint8Array(await crypto.subtle.digest('SHA-256', d))].map((x) => x.toString(16).padStart(2, '0')).join('');
          log(`[page] sidecar ${f}: ${d.length} bytes, sha256 ${h}`);
        }
        for (const [f, d] of Object.entries(side)) {
          const dir = '/data/' + f.split('/')[0];
          try { FS.mkdir(dir); } catch (e) {}
          FS.writeFile('/data/' + f, d);
        }
      } catch (e) {
        status(String(e.message || e)); log('[page] ' + (e.stack || e)); return;
      } finally {
        M._ge_web_free(romp);
      }
      FS.chdir('/');
      status('');
      log('[page] calling main()');
      // ?args=-level_09+-foo passes command-line arguments (as on native)
      let args = (params.get('args') || '').split(/[ +]/).filter(Boolean);
      if (window.GENet) { args = GENet.args(args); await GENet.beforeMain(M); }   // G6: online session setup
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
  try { start(...await acceptRom(await f.arrayBuffer(), true)); } catch (e) { status(String(e.message || e)); }
});
(async () => {
  const url = params.get('romurl');            // dev only: a local copy served by scripts/web/serve.sh
  if (url) {
    try { const r = await fetch(url); start(...await acceptRom(await r.arrayBuffer(), false)); }
    catch (e) { status('romurl: ' + (e.message || e)); }
    return;
  }
  const saved = await idbGet('rom');
  if (saved) { status('using the ROM stored in this browser'); try { start(...await acceptRom(saved.buffer || saved, false)); } catch (e) { status(String(e.message || e)); } }
  else status('choose your ROM to start');
})();
