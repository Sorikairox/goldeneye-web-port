// GoldenEye 007 web port: saves that persist in this browser, plus export / import (docs/web/HANDOFF.md, G5).
//
// The game writes its EEPROM image /data/ge007.eep (2048 bytes, the native port's format) and its config
// /data/ge007.ini into the Emscripten MEMFS, which is gone when the page closes. Here:
//  - before main(): restore() writes the stored copies (IndexedDB 'ge007-web' / 'files', keys
//    'save-<ROM SHA-1>/ge007.eep' and '.../ge007.ini') back into /data, so the game's first EEPROM read
//    and configLoad see them;
//  - while it runs: the C side calls Module.geSaveWritten(what) after every write (port/src/web/websave.c);
//    the file is marked dirty and copied to IndexedDB 400 ms after the last write of a burst;
//  - when the tab is hidden or the page closes: flushed at once; on pagehide (after beforeunload's
//    configSave) anything still dirty is also written synchronously to localStorage
//    ('ge007-pending-<sha1>'), which wins on the next start (an IndexedDB write that a closing page
//    started may not finish);
//  - export: the EEPROM image as a .eep file (same format as the PC port's data/ge007.eep; emulator .eep
//    files import too, the game converts them on boot); import: 2048 bytes with at least one region whose
//    checksum the game accepts (ge_web_eep_check), stored, then the page reloads so the game boots with it.
//  - ?nosave: nothing is restored or stored (a blank save in memory for this page load).
// Online seam (multiplayer playbook 10.2 item 8): GESaves.sandbox(bytes) -> the game boots with the host's
// image (or a blank one), its writes stay in wasm RAM, nothing is persisted for the rest of this page load.
'use strict';
(function () {
  const FILES = { 1: 'ge007.eep', 2: 'ge007.ini' };
  const EEP_SIZE = 2048;
  const DEBOUNCE_MS = 400;
  const params = new URLSearchParams(location.search);
  const S = {
    sha1: null, M: null, persist: !params.has('nosave'), blocked: false, sandboxed: false,
    sandboxImage: null, dirty: new Set(), timer: null, writes: 0, flushes: 0,
  };
  const $ = (id) => document.getElementById(id);
  const say = (s) => { const e = $('save-status'); if (e) e.textContent = s; };
  const log = (s) => { if (window.geLog) window.geLog(s); else console.log(s); };
  const key = (f) => `save-${S.sha1}/${f}`;
  const pendingKey = () => `ge007-pending-${S.sha1}`;

  // ---- IndexedDB (same database as the ROM / sidecar cache in main.js); every call closes its connection
  function open() {
    return new Promise((res, rej) => {
      const r = indexedDB.open('ge007-web', 1);
      r.onupgradeneeded = () => r.result.createObjectStore('files');
      r.onsuccess = () => res(r.result);
      r.onerror = () => rej(r.error);
      r.onblocked = () => rej(new Error('IndexedDB blocked'));
    });
  }
  async function tx(mode, fn) {
    const db = await open();
    try {
      return await new Promise((res, rej) => {
        const t = db.transaction('files', mode), st = t.objectStore('files');
        let out; const q = fn(st); if (q) q.onsuccess = () => { out = q.result; };
        t.oncomplete = () => res(out); t.onerror = () => rej(t.error); t.onabort = () => rej(t.error || new Error('aborted'));
      });
    } finally { db.close(); }
  }
  const withTimeout = (p, ms, what) => Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`${what}: no answer in ${ms / 1000} s`)), ms))]);
  const b64 = (u8) => { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); };
  const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

  function readFs(f) { try { return S.M.FS.readFile('/data/' + f); } catch (e) { return null; } }
  function updateButtons() {
    const on = !!S.sha1;
    if ($('save-export')) $('save-export').disabled = !on;
    if ($('save-import')) $('save-import').disabled = !(on && S.M);
    if ($('save-delete')) $('save-delete').disabled = !on;
  }

  // ---- before main(): stored copies -> /data
  async function restore(M, sha1) {
    S.M = M; S.sha1 = sha1; updateButtons();
    M.geSaveWritten = written;
    if (S.sandboxed) { applySandbox(); return; }
    if (!S.persist) { log('[save] ?nosave: blank save in memory, nothing is stored'); say('Saving is off for this page load (?nosave).'); return; }
    let eep = null, ini = null, from = 'IndexedDB';
    try {
      const pend = localStorage.getItem(pendingKey());
      if (pend) {
        const p = JSON.parse(pend);
        if (p.eep) eep = unb64(p.eep);
        if (p.ini != null) ini = new TextEncoder().encode(p.ini);
        from = 'the copy kept at the last page close';
      }
    } catch (e) { /* no localStorage */ }
    try {
      // IndexedDB can stall (blocked storage): do not hold the boot on it
      const [e, i] = await withTimeout(Promise.all([tx('readonly', (s) => s.get(key(FILES[1]))), tx('readonly', (s) => s.get(key(FILES[2])))]), 4000, 'IndexedDB');
      if (!eep && e) eep = new Uint8Array(e);
      if (!ini && i) ini = new Uint8Array(i);
    } catch (err) {
      if (!eep && !ini) {
        S.blocked = true;   // never overwrite a save we could not read with a blank one
        log('[save] could not read the save storage: ' + (err.message || err) + '; saving is off for this page load');
        say('Could not read the save storage: saving is off for this page load.');
        return;
      }
    }
    if (eep && eep.length === EEP_SIZE) M.FS.writeFile('/data/' + FILES[1], eep);
    if (ini) M.FS.writeFile('/data/' + FILES[2], ini);
    log(`[save] restored from ${from}: ${eep ? 'EEPROM ' + eep.length + ' bytes' : 'no EEPROM (new save)'}, ${ini ? 'config ' + ini.length + ' bytes' : 'no config'}`);
    if (from !== 'IndexedDB') { for (const w of [1, 2]) if (readFs(FILES[w])) S.dirty.add(w); flush('restore'); }
    say(eep ? 'Your save was loaded from this browser.' : 'No save yet in this browser: the game starts a new one.');
  }

  // ---- while running: C -> dirty -> debounced IndexedDB copy
  function written(what) {
    if (!S.persist || S.blocked || S.sandboxed || !FILES[what]) return;
    S.dirty.add(what); S.writes++;
    clearTimeout(S.timer); S.timer = setTimeout(() => flush('debounce'), DEBOUNCE_MS);
  }
  async function flush(why) {
    clearTimeout(S.timer); S.timer = null;
    if (!S.persist || S.blocked || S.sandboxed || !S.dirty.size || !S.M) return;
    const items = [...S.dirty].map((w) => [w, readFs(FILES[w])]).filter(([, d]) => d);
    S.dirty.clear();
    try {
      await tx('readwrite', (s) => { for (const [w, d] of items) s.put(d, key(FILES[w])); });
      S.flushes++;
      try { localStorage.removeItem(pendingKey()); } catch (e) { /* ignore */ }
      log(`[save] stored ${items.map(([w, d]) => FILES[w] + ' ' + d.length).join(', ')} in IndexedDB (${why})`);
      if (items.some(([w]) => w === 1)) say(`Saved in this browser at ${new Date().toLocaleTimeString()}.`);
    } catch (e) {
      for (const [w] of items) S.dirty.add(w);   // retried at the next write / hide / close
      log('[save] could not store the save: ' + (e.message || e));
      say('Could not store the save in this browser (' + (e.message || e) + '). Export it to keep a copy.');
    }
  }
  // closing: keep a synchronous copy of anything not yet in IndexedDB, then try IndexedDB too
  function closing() {
    if (!S.persist || S.blocked || S.sandboxed || !S.dirty.size || !S.M) return;
    try {
      const p = {}; const eep = S.dirty.has(1) && readFs(FILES[1]), ini = S.dirty.has(2) && readFs(FILES[2]);
      // keep the stored half that is not dirty: the pending copy replaces both on the next start
      if (eep) p.eep = b64(eep);
      if (ini) p.ini = new TextDecoder().decode(ini);
      localStorage.setItem(pendingKey(), JSON.stringify(p));
    } catch (e) { /* no localStorage: IndexedDB only */ }
    flush('page close');
  }
  addEventListener('pagehide', closing);
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') flush('tab hidden'); });

  // ---- export / import / delete
  async function current() {
    if (S.M && !S.sandboxed) { const d = readFs(FILES[1]); if (d) return d; }
    if (!S.sha1) return null;
    try { const d = await tx('readonly', (s) => s.get(key(FILES[1]))); return d ? new Uint8Array(d) : null; } catch (e) { return null; }
  }
  async function exportBlob() {
    const d = await current();
    return d ? new Blob([d], { type: 'application/octet-stream' }) : null;
  }
  async function exportSave() {
    const blob = await exportBlob();
    if (!blob) { say('No save yet: play until the game saves (it saves after a mission or when you change options).'); return; }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'goldeneye-eu.eep';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    say('Exported goldeneye-eu.eep (your save as a file; it also works with the PC port as data/ge007.eep).');
  }
  // validation: exact size, then the game's own checksums (wasm ge_web_eep_check)
  function check(raw) {
    if (raw.length !== EEP_SIZE) return `not a GoldenEye save: ${raw.length} bytes (a save file is exactly ${EEP_SIZE} bytes)`;
    if (!S.M) return 'the game is not loaded yet';
    const p = S.M._ge_web_alloc(raw.length); S.M.HEAPU8.set(raw, p);
    const mask = S.M._ge_web_eep_check(p, raw.length); S.M._ge_web_free(p);
    if (mask <= 0) return 'not a GoldenEye save: no part of it has a valid GoldenEye checksum';
    return null;
  }
  async function importBytes(raw, name) {
    raw = new Uint8Array(raw);
    if (!S.sha1) { say('Choose your ROM first.'); return false; }
    const err = check(raw);
    if (err) { log(`[save] import of ${name || 'file'} refused: ${err}`); say(`Import refused: ${err}. Your current save is unchanged.`); return false; }
    S.persist = false;   // this page's game must not write its old save over the imported one
    clearTimeout(S.timer); S.dirty.clear();
    try { localStorage.removeItem(pendingKey()); } catch (e) { /* ignore */ }
    try { await tx('readwrite', (s) => s.put(raw, key(FILES[1]))); }
    catch (e) { say('Import failed: could not store it in this browser (' + (e.message || e) + ').'); return false; }
    log(`[save] imported ${name || 'file'} (${raw.length} bytes) into ${key(FILES[1])}; reloading`);
    say('Save imported. Restarting the game with it…');
    if (!params.has('noreload')) setTimeout(() => location.reload(), 300);
    return true;
  }
  async function deleteSave() {
    if (!S.sha1) return;
    if (!confirm('Delete the GoldenEye save stored in this browser? Export it first if you want to keep it.')) return;
    S.persist = false; clearTimeout(S.timer); S.dirty.clear();
    try { localStorage.removeItem(pendingKey()); } catch (e) { /* ignore */ }
    try { await tx('readwrite', (s) => s.delete(key(FILES[1]))); } catch (e) { /* ignore */ }
    log('[save] deleted; reloading');
    say('Save deleted. Restarting the game…');
    setTimeout(() => location.reload(), 300);
  }

  // ---- online seam: boot with the host's image, never persist (call before the page calls main())
  function applySandbox() {
    const M = S.M; if (!M) return;   // applied by restore() once the runtime is up
    M._ge_web_save_sandbox(1);
    const img = S.sandboxImage || new Uint8Array(EEP_SIZE);
    const p = M._ge_web_alloc(EEP_SIZE); M.HEAPU8.fill(0, p, p + EEP_SIZE); M.HEAPU8.set(img.subarray(0, EEP_SIZE), p);
    M._ge_web_eeprom_load(p, EEP_SIZE); M._ge_web_free(p);
    log(`[save] sandbox: ${S.sandboxImage ? 'the given' : 'a blank'} EEPROM image, nothing is stored for this page load`);
    say('Online session: the host\'s save is used and nothing is saved in this browser.');
  }
  function sandbox(bytes) {
    if (bytes && bytes.length !== EEP_SIZE) throw new Error(`sandbox: EEPROM image must be ${EEP_SIZE} bytes`);
    S.sandboxed = true; S.persist = false; S.sandboxImage = bytes ? new Uint8Array(bytes) : null;
    clearTimeout(S.timer); S.dirty.clear();
    if (S.M) { applySandbox(); if (window.geMainCalled) log('[save] sandbox: set after main(); the game may already have read its EEPROM'); }
  }

  if ($('save-export')) $('save-export').onclick = exportSave;
  if ($('save-import')) $('save-import').addEventListener('change', async (ev) => {
    const f = ev.target.files[0]; ev.target.value = ''; if (!f) return;
    await importBytes(await f.arrayBuffer(), f.name);
  });
  if ($('save-delete')) $('save-delete').onclick = deleteSave;
  updateButtons();

  window.GESaves = {
    restore, flush, exportBlob, importBytes, sandbox,
    // the player's own stored EEPROM image (for a host to send to its guests), or null
    image: current,
    get state() { return { sha1: S.sha1, persist: S.persist, blocked: S.blocked, sandboxed: S.sandboxed, dirty: [...S.dirty], writes: S.writes, flushes: S.flushes }; },
  };
})();
