// rtw_lib.js — Emscripten JS library of the retrowser.app build (GE_RTW; docs/web/RTW.md).
// - env.rtw_* host imports (spec §4.10), wired to Module.rtw, which the host passes to the factory;
// - the WebGL2 context on the host's OffscreenCanvas (Module.canvas), used by port/rtw/gfx_rtw.cpp;
// - the start-up rtw_start() hands over (gertw_boot): ROM check, template fill from files.data
//   ge007.romfill, sidecars from this sandbox origin's IndexedDB or converted in wasm, then main().
// Runs in the host's worker: no document, no window, no network. Names not owned by the spec are gertw_*.
addToLibrary({
  // ---- host imports --------------------------------------------------------------------------------
  rtw_log__sig: 'ipi',
  rtw_log: (p, n) => (Module.rtw && Module.rtw.rtw_log ? Module.rtw.rtw_log(Number(p), n) : 0),
  rtw_build_id__sig: 'ip',
  rtw_build_id: (p) => {
    if (Module.rtw && Module.rtw.rtw_build_id) return Module.rtw.rtw_build_id(Number(p));
    HEAPU8.fill(0, Number(p), Number(p) + 32);
    return 0;
  },
  rtw_rumble__sig: 'iii',
  rtw_rumble: (port, on) => (Module.rtw && Module.rtw.rtw_rumble ? Module.rtw.rtw_rumble(port, on) : 0),
  rtw_export_file__sig: 'ipppi',
  rtw_export_file: (name, mime, p, n) =>
    (Module.rtw && Module.rtw.rtw_export_file ? Module.rtw.rtw_export_file(Number(name), Number(mime), Number(p), n) : -1),
  rtw_pointer_lock__sig: 'ii',
  rtw_pointer_lock: (on) => (Module.rtw && Module.rtw.rtw_pointer_lock ? Module.rtw.rtw_pointer_lock(on) : 0),

  // ---- WebGL2 on the OffscreenCanvas ---------------------------------------------------------------
  gertw_gl_create__deps: ['$GL'],
  gertw_gl_create__sig: 'iii',
  gertw_gl_create: (w, h) => {
    const c = Module.canvas;
    if (!c || typeof c.getContext !== 'function') { err('[rtw] no canvas from the host'); return 0; }
    c.width = w; c.height = h;
    // the attributes SDL's Emscripten context had in the web build (depth 24, stencil 8, no AA, extensions on)
    const handle = GL.createContext(c, {
      majorVersion: 2, minorVersion: 0, alpha: false, depth: true, stencil: true, antialias: false,
      premultipliedAlpha: true, preserveDrawingBuffer: false, powerPreference: 'default',
      failIfMajorPerformanceCaveat: false, enableExtensionsByDefault: true,
    });
    if (!handle) { err('[rtw] WebGL2 context creation failed'); return 0; }
    GL.makeContextCurrent(handle);
    return handle;
  },
  gertw_canvas_size__sig: 'vii',
  gertw_canvas_size: (w, h) => {
    const c = Module.canvas;
    if (c && (c.width !== w || c.height !== h)) { c.width = w; c.height = h; }
  },

  // ---- start-up ----------------------------------------------------------------------------------------
  $gertwBootAsync__deps: ['$FS'],
  $gertwBootAsync: async (romp, romlen) => {
    const M = Module;
    const EU_SHA1 = '167c3c433dec1f1eb921736f7d53fac8cb45ee31';
    const ROM_NAME = 'ge007.pal-final.z64';
    const SIDECARS = ['pcmodels-pal-final/pcmodels.bin', 'pcmodels-pal-final/manifest.csv',
                      'pccg-pal-final/pccg.bin', 'pccg-pal-final/manifest.csv'];   // ge_web_geconv_blob order
    const log = (s) => out('[rtw] ' + s);
    const hex = (buf) => [...new Uint8Array(buf)].map((x) => x.toString(16).padStart(2, '0')).join('');
    const t0 = performance.now();

    const rom = HEAPU8.slice(romp, romp + romlen);
    if (romlen < 0x1000 || ((rom[0] << 24 | rom[1] << 16 | rom[2] << 8 | rom[3]) >>> 0) !== 0x80371240) {
      throw new Error('not an N64 ROM in big-endian (.z64) order');
    }
    const sum = hex(await crypto.subtle.digest('SHA-1', rom));
    if (sum !== EU_SHA1) throw new Error(`this ROM is not GoldenEye 007 (Europe): SHA-1 ${sum}`);
    FS.mkdir('/data');
    FS.writeFile('/data/' + ROM_NAME, rom);

    // template + fill: the build's ROM-derived data comes back from the player's ROM (docs/web/ROMDATA.md)
    const files = M.files || {};
    const fk = Object.keys(files).find((k) => k === 'ge007.romfill' || k.endsWith('/ge007.romfill'));
    if (!fk) throw new Error('files.data ge007.romfill is missing');
    const t = files[fk];
    const tp = M._ge_web_alloc(t.length);
    HEAPU8.set(t, tp);
    const rc = M._ge_web_romfill(romp, romlen, tp, t.length);
    M._ge_web_free(tp);
    if (rc !== 0) throw new Error('could not rebuild the game data from this ROM (romfill ' + rc + ')');

    // sidecars: this sandbox origin's IndexedDB (the host never reads it), else converted in wasm
    const key = `sidecars-v${M._ge_web_geconv_version()}-${sum}`;
    const db = () => new Promise((ok, bad) => {
      const r = indexedDB.open('ge007-rtw', 1);
      r.onupgradeneeded = () => r.result.createObjectStore('files');
      r.onsuccess = () => ok(r.result);
      r.onerror = () => bad(r.error);
    });
    const idbGet = async (k) => {
      try {
        const d = await db();
        return await new Promise((ok) => {
          const q = d.transaction('files').objectStore('files').get(k);
          q.onsuccess = () => { ok(q.result); d.close(); };
          q.onerror = () => { ok(undefined); d.close(); };
        });
      } catch (e) { return undefined; }
    };
    const idbPut = async (k, v) => {
      try {
        const d = await db();
        await new Promise((ok) => {
          const tr = d.transaction('files', 'readwrite');
          tr.objectStore('files').put(v, k);
          tr.oncomplete = tr.onerror = tr.onabort = () => { d.close(); ok(); };
        });
      } catch (e) { log('sidecar cache not stored: ' + e); }
    };
    let side = typeof indexedDB !== 'undefined' ? await idbGet(key) : undefined;
    if (side && SIDECARS.every((f) => side[f] instanceof Uint8Array)) log('sidecars from IndexedDB');
    else {
      const t1 = performance.now();
      const total = M._ge_web_geconv_begin(romp, romlen);
      if (total < 0) throw new Error('sidecar conversion failed to start (see the log)');
      let left = total, last = performance.now();
      while (left > 0) {
        left = M._ge_web_geconv_step();
        if (left < 0) throw new Error('sidecar conversion failed (see the log)');
        if (performance.now() - last > 30) { await new Promise((r) => setTimeout(r, 0)); last = performance.now(); }
      }
      side = {};
      SIDECARS.forEach((f, i) => {
        const a = M._ge_web_geconv_blob(i, 0), n = M._ge_web_geconv_blob(i, 1);
        side[f] = HEAPU8.slice(a, a + n);
      });
      M._ge_web_geconv_end();
      log(`sidecars converted in wasm: ${total} steps, ${(performance.now() - t1).toFixed(0)} ms`);
      if (typeof indexedDB !== 'undefined') await idbPut(key, side);
    }
    for (const f of SIDECARS) {
      const dir = '/data/' + f.split('/')[0];
      try { FS.mkdir(dir); } catch (e) { /* exists */ }
      FS.writeFile('/data/' + f, side[f]);
    }
    M._ge_rtw_rom_free();

    M._ge_rtw_before_main();
    FS.chdir('/');
    log(`calling main() after ${(performance.now() - t0).toFixed(0)} ms`);
    try { M.callMain([]); } catch (e) { if (e !== 'unwind') throw e; }
    M._ge_rtw_after_main();
  },
  gertw_boot__deps: ['$gertwBootAsync'],
  gertw_boot__sig: 'vpi',
  gertw_boot: (romp, romlen) => {
    // not awaited: rtw_start returns at once (loop "mainloop"); a failure is an unhandled rejection, which
    // the host's runner reports as the game's error
    gertwBootAsync(Number(romp), romlen);
  },
});
