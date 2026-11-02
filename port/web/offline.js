// GoldenEye 007 web port, G9: offline play. Page option "Offline play" (default ON, the user asked for it; an explicit
// Off is stored in localStorage ge007-offline = 'off'; dev / test ?offline=0|1, not stored).
//   On:  registers the service worker sw.js (precaches every site file; cache named after the build, see sw.js), asks
//        for persistent storage (navigator.storage.persist(), so the browser does not evict the cache, the ROM and the
//        saves under storage pressure), and shows a small "Update ready" notice when a new deploy has been precached.
//        A waiting new version takes over on the next page load: GEOffline.ready (awaited by main.js before the game
//        starts) tells the waiting worker to activate and reloads once, so the game never runs on mixed versions.
//   Off: unregisters the worker and deletes its caches; the page then loads from the network exactly as before G9.
// The ROM, the converted game data and the saves were already in IndexedDB (main.js, saves.js) and stay there.
'use strict';
(function () {
  const $ = (id) => document.getElementById(id);
  const LS = 'ge007-offline', PREFIX = 'ge007-offline-';
  const Q = new URLSearchParams(location.search);
  const lsGet = () => { try { return localStorage.getItem(LS); } catch (e) { return null; } };
  const lsSet = (v) => { try { if (v) localStorage.setItem(LS, v); else localStorage.removeItem(LS); } catch (e) { /* private mode */ } };
  const wanted = () => (Q.has('offline') ? Q.get('offline') !== '0' : lsGet() !== 'off');
  const supported = 'serviceWorker' in navigator && window.isSecureContext;
  const log = (s) => (window.geLog ? window.geLog(s) : console.log(s));
  const stateEl = () => $('opt-offline-state');
  const setState = (s) => { const e = stateEl(); if (e) e.textContent = s; };
  const O = { ready: Promise.resolve(), info: null };
  window.GEOffline = O;

  async function clearAll() {
    if (supported) for (const r of await navigator.serviceWorker.getRegistrations()) await r.unregister();
    if (window.caches) for (const k of await caches.keys()) if (k.startsWith(PREFIX)) await caches.delete(k);
  }

  function notice(reg) {
    let n = $('ge-update');
    if (!n) {
      n = document.createElement('div'); n.id = 'ge-update';
      n.innerHTML = 'Update ready <button id="ge-update-go">Reload</button> <button id="ge-update-x" title="Later">×</button>';
      document.body.appendChild(n);
      $('ge-update-x').onclick = () => { n.hidden = true; };
    }
    n.hidden = false;
    $('ge-update-go').onclick = () => activate(reg);
  }
  // the waiting worker takes over, then the page reloads on it (the controllerchange handler below)
  let reloading = false;
  function activate(reg) {
    if (!reg.waiting) { location.reload(); return; }
    reg.waiting.postMessage('skipWaiting');
  }

  async function describe() {
    const c = navigator.serviceWorker.controller;
    if (!c) { setState('preparing offline copy…'); return; }
    const info = await new Promise((res) => {
      const t = setTimeout(() => res(null), 3000);
      navigator.serviceWorker.addEventListener('message', function h(ev) {
        if (ev.data && ev.data.build) { clearTimeout(t); navigator.serviceWorker.removeEventListener('message', h); res(ev.data); }
      });
      c.postMessage('build');
    });
    if (!info) return;
    O.info = info;
    let persisted = null;
    try { persisted = navigator.storage && navigator.storage.persisted ? await navigator.storage.persisted() : null; } catch (e) {}
    setState(`ready: playable offline after this visit (${(info.bytes / 1048576).toFixed(1)} MB cached, build ${info.build.slice(0, 8)}` +
      (persisted === null ? ')' : persisted ? ', storage persistent)' : ', storage may be cleared by the browser when space is low)'));
    if (O.logged !== info.build + persisted) { O.logged = info.build + persisted; log(`[offline] cache build ${info.build}: ${info.files} files, ${info.bytes} bytes; persistent storage: ${persisted}`); }
  }

  async function enable() {
    if (!supported) { setState('not available in this browser (needs a service worker and https)'); return; }
    navigator.serviceWorker.addEventListener('controllerchange', () => {
      if (O.hadController && !reloading) { reloading = true; location.reload(); }   // a new version took over
      else describe();
    });
    O.hadController = !!navigator.serviceWorker.controller;
    let reg;
    try { reg = await navigator.serviceWorker.register('sw.js'); }
    catch (e) { setState('could not start: ' + (e.message || e)); log('[offline] register failed: ' + e); return; }
    // a version precached on an earlier visit is waiting: take it now, before the game starts (next load = new version)
    if (reg.waiting && O.hadController && !window.geMainCalled) {
      log('[offline] a new version is ready: switching to it');
      activate(reg);
      await new Promise((r) => setTimeout(r, 4000));   // the reload normally comes first; if not, carry on
    }
    const watch = (w) => w && w.addEventListener('statechange', () => {
      if (w.state === 'installed' && navigator.serviceWorker.controller) notice(reg);
      if (w.state === 'activated') describe();
    });
    watch(reg.installing);
    reg.addEventListener('updatefound', () => watch(reg.installing));
    if (reg.waiting && navigator.serviceWorker.controller) notice(reg);
    if (navigator.storage && navigator.storage.persist) {
      navigator.storage.persist().then((p) => { log('[offline] navigator.storage.persist(): ' + p); describe(); }, () => {});
    }
    describe();
    // a long session checks for a new deploy now and then (cheap: the browser compares sw.js + sw-build.js)
    setInterval(() => { if (navigator.onLine) reg.update().catch(() => {}); }, 30 * 60 * 1000);
  }

  function wire() {
    const box = $('opt-offline');
    if (box) {
      box.checked = wanted();
      box.onchange = async () => {
        if (box.checked) { lsSet(null); setState('turning on…'); await enable(); }
        else { lsSet('off'); setState('turning off…'); await clearAll(); setState('off: the page needs the network to load'); log('[offline] off: worker unregistered, cache deleted'); }
      };
    }
    if (wanted()) O.ready = enable().catch((e) => log('[offline] ' + e));
    else { setState('off: the page needs the network to load'); O.ready = clearAll().catch(() => {}); }
  }
  wire();
})();
