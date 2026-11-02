// GoldenEye 007 web port: service worker for offline play (G9; registered by offline.js when the page option
// "Offline play" is on, the default).
//
// sw-build.js (generated next to this file by scripts/web/swbuild.mjs at build and deploy time) sets GE_BUILD, a hash
// of every site file, and GE_FILES, the list of those files. The cache is named after GE_BUILD, so a new deploy gets a
// fresh cache: the browser sees the changed sw-build.js on its update check, installs this worker again (precaching
// the new files), and the new worker deletes every older ge007-offline-* cache when it activates. It activates on the
// next page load (offline.js asks the waiting worker to take over before the game starts) or when the player presses
// "Reload" on the "Update ready" notice.
//
// Fetch: same-origin GET requests for site files are served cache-first; a navigation (index.html with any ?query, or
// the bare directory) gets the cached index.html. Never cached, always the network: the Netlify functions (the online
// room server at /api/room, /.netlify/*), non-GET requests, other origins. The player's ROM, converted game data and
// saves are not here: they stay in IndexedDB (main.js, saves.js).
'use strict';
importScripts('sw-build.js');
const PREFIX = 'ge007-offline-';
const CACHE = PREFIX + self.GE_BUILD;
const BASE = new URL('./', self.location).href;                 // the site root (scope)

self.addEventListener('install', (ev) => {
  // 'no-cache': revalidate with the server (ETag) so a stale HTTP-cache copy of an older build is never precached
  ev.waitUntil(caches.open(CACHE).then((c) => c.addAll(self.GE_FILES.map((f) => new Request(f, { cache: 'no-cache' })))));
});

self.addEventListener('activate', (ev) => {
  ev.waitUntil((async () => {
    for (const k of await caches.keys()) if (k.startsWith(PREFIX) && k !== CACHE) await caches.delete(k);
    await self.clients.claim();   // the first visit is served offline from now on, without a reload
  })());
});

self.addEventListener('message', (ev) => {
  if (ev.data === 'skipWaiting') self.skipWaiting();
  else if (ev.data === 'build' && ev.source) ev.source.postMessage({ build: self.GE_BUILD, files: self.GE_FILES.length, bytes: self.GE_BYTES });
});

const NETWORK_ONLY = /^\/(api|\.netlify)\//;
self.addEventListener('fetch', (ev) => {
  const req = ev.request;
  if (req.method !== 'GET') return;
  const u = new URL(req.url);
  if (u.origin !== self.location.origin || !u.href.startsWith(BASE)) return;
  if (NETWORK_ONLY.test(u.pathname) || NETWORK_ONLY.test('/' + u.href.slice(BASE.length))) return;
  ev.respondWith((async () => {
    const c = await caches.open(CACHE);
    let hit = await c.match(req, { ignoreSearch: true });
    if (!hit && req.mode === 'navigate') hit = await c.match('index.html');
    return hit || fetch(req);
  })());
});
