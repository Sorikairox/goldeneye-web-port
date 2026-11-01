// GoldenEye 007 web port: on-screen touch controls (G8; design from the Conker port's touch controls, P361 / P368 / P369).
//
// A full N64 pad for phones and tablets: a floating analogue stick (lower left), A, B, Z (fire, the big button), R (aim),
// L, the four C buttons and Start. Each control follows its own finger (pointer events + pointer capture), so the stick
// and any buttons work at the same time. The state goes to the game through ge_web_touch() (port/src/web/webtouch.c),
// which input.c merges into controller 0 where keyboard keys and gamepad buttons land: offline play, the online
// lockstep sample and the recorder all see it the same way. While the layer is up the port gives seat 0 the game's
// default 1.1 control style (stick = walk + turn).
//
// Layout (layout(), from the window size): upright (portrait) the 4:3 picture sits at the top and the controls take the
// space below it; turned (landscape) the picture is centred at full height and the controls lie over the sides. The
// buttons are plain DOM faces (no canvases, so nothing is drawn while the layer is hidden); the stick's resting ring is
// placed from the stick zone's box only when that box is laid out (Conker P369 lesson), and every window resize /
// rotation re-runs the layout, also when it happens while the page menu is open.
//
// Setting "Touch controls": Auto (default: on when the device has a touch screen: a coarse pointer, or touch points
// on an iOS / Android / mobile browser) / On / Off. Only the player's explicit On / Off is stored (localStorage
// ge007-touch); Auto stores nothing, so one wrong detection never sticks. Dev / test: ?touch=1 / ?touch=0 (not stored).
'use strict';
(function () {
  const $ = (id) => document.getElementById(id);
  const N64 = { A: 0x8000, B: 0x4000, Z: 0x2000, START: 0x1000, L: 0x0020, R: 0x0010, CU: 0x0008, CD: 0x0004, CL: 0x0002, CR: 0x0001 };
  const STICK_MAX = 80;                       // the port's stick range (input.c STICK_MAX)
  const LS = 'ge007-touch';
  const Q = new URLSearchParams(location.search);
  const UA = navigator.userAgent;
  const IOS = /iPad|iPhone|iPod/.test(UA) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const detected = !!(window.matchMedia && matchMedia('(pointer: coarse)').matches) ||
    ((navigator.maxTouchPoints > 0 || 'ontouchstart' in window) && (IOS || /Android|Mobi/i.test(UA)));
  const lsGet = () => { try { return localStorage.getItem(LS); } catch (e) { return null; } };
  const lsSet = (v) => { try { if (v) localStorage.setItem(LS, v); else localStorage.removeItem(LS); } catch (e) { /* private mode */ } };
  const setting = () => { const v = lsGet(); return v === 'on' || v === 'off' ? v : 'auto'; };
  const wanted = () => {
    if (Q.has('touch')) return Q.get('touch') !== '0';
    const s = setting(); return s === 'on' || (s === 'auto' && detected);
  };

  // phones: bigger tap targets on the page; iOS greys out files whose extension it does not know (.z64 / .eep) and
  // Android's picker may hide them, so no type filter there (the ROM is checked by SHA-1, the save by size + CRC)
  if (detected) {
    document.body.classList.add('touchdev');
    for (const id of ['romfile', 'save-import']) { const e = $(id); if (e) e.removeAttribute('accept'); }
  }

  const t = { on: false, playing: false, menu: false, b: 0, x: 0, y: 0, stickId: null, cx: 0, cy: 0, travel: 50 };
  const tplay = () => t.on && t.playing && !t.menu;

  // ---- input to the game ----------------------------------------------------------------------------
  let sent = '';
  function send() {
    const M = window.Module;
    if (!M || typeof M._ge_web_touch !== 'function' || !window.geMainCalled) return;
    const live = t.on && t.playing;           // menu open: the 1.1 style stays, the pad is idle
    const b = live && !t.menu ? t.b : 0, x = live && !t.menu ? t.x : 0, y = live && !t.menu ? t.y : 0;
    const k = `${live}|${b}|${x}|${y}`;
    if (k === sent) return;
    sent = k;
    M._ge_web_touch(live ? 1 : 0, b, x, y);
  }
  function release() {
    t.b = 0; t.x = t.y = 0; t.stickId = null;
    for (const el of document.querySelectorAll('#touch .tb.on')) el.classList.remove('on');
    $('tk-knob').style.transform = '';
  }

  // ---- show / hide ------------------------------------------------------------------------------------
  function apply() {
    const show = tplay();
    $('touch').hidden = !show;
    $('tk-back').hidden = !(t.on && t.playing && t.menu);
    document.body.classList.toggle('tplay', show);
    document.body.classList.toggle('tmenu', t.on && t.playing && t.menu);
    if (!show) release();
    layout();
    send();
    const st = $('opt-touch-state');
    if (st) st.textContent = Q.has('touch') ? `(?touch=${Q.get('touch')} in the address)` :
      setting() === 'auto' ? (detected ? '(touch screen found: on)' : '(no touch screen found: off)') : '';
  }
  function setOn(on) { t.on = on; apply(); }

  // ---- layout -----------------------------------------------------------------------------------------
  function safe() {
    const cs = getComputedStyle($('tk-safe'));
    return { t: parseFloat(cs.paddingTop) || 0, r: parseFloat(cs.paddingRight) || 0, b: parseFloat(cs.paddingBottom) || 0, l: parseFloat(cs.paddingLeft) || 0 };
  }
  function put(id, cx, cy, w, h = w) {
    const s = $(id).style;
    s.left = Math.round(cx - w / 2) + 'px'; s.top = Math.round(cy - h / 2) + 'px';
    s.width = Math.round(w) + 'px'; s.height = Math.round(h) + 'px';
  }
  let zoneBox = null;   // the stick zone in page px, from layout()
  function layout() {
    const cv = $('canvas');
    if (!tplay()) { cv.style.left = cv.style.top = cv.style.width = cv.style.height = ''; return; }
    const W = innerWidth, H = innerHeight, sa = safe();
    if (W < 50 || H < 50) return;              // not laid out (yet)
    const portrait = H >= W;
    let pic;
    if (portrait) {
      let w = W - sa.l - sa.r, h = w * 3 / 4;
      if (h > (H - sa.t) * 0.55) { h = (H - sa.t) * 0.55; w = h * 4 / 3; }
      pic = { x: (W - w) / 2, y: sa.t, w, h };
    } else {
      let h = H, w = h * 4 / 3;
      if (w > W - 200) { w = Math.max(W - 200, W * 0.6); h = w * 3 / 4; }
      pic = { x: (W - w) / 2, y: (H - h) / 2, w, h };
    }
    cv.style.left = pic.x + 'px'; cv.style.top = pic.y + 'px';
    cv.style.width = pic.w + 'px'; cv.style.height = pic.h + 'px';
    const R = W - sa.r, B = H - sa.b, Lx = sa.l;
    let k;
    if (portrait) {
      const A0 = pic.y + pic.h, area = B - A0;
      k = Math.max(0.75, Math.min(1.5, W / 390, area / 420));
      // top row of the control area: menu, full screen, L | START
      put('tk-menu', Lx + 26 * k, A0 + 28 * k, 40 * k);
      put('tk-fs', Lx + 74 * k, A0 + 28 * k, 40 * k);
      put('tk-START', W / 2, A0 + 28 * k, 76 * k, 34 * k);
      put('tk-L', Lx + 50 * k, A0 + 80 * k, 76 * k, 36 * k);
      // right thumb: Z (fire) and R (aim) next to each other at the bottom, B / A above, the C diamond on top
      put('tk-Z', R - 62 * k, B - 84 * k, 92 * k);
      put('tk-R', R - 158 * k, B - 52 * k, 66 * k);
      put('tk-B', R - 148 * k, B - 152 * k, 56 * k);
      put('tk-A', R - 60 * k, B - 190 * k, 60 * k);
      const cx = R - 104 * k, cy = Math.max(A0 + 120 * k, B - 300 * k), d = 46 * k, c = 44 * k;
      put('tk-CU', cx, cy - d, c); put('tk-CD', cx, cy + d, c); put('tk-CL', cx - d, cy, c); put('tk-CR', cx + d, cy, c);
      zoneBox = { x: 0, y: A0 + 104 * k, w: Math.min(W * 0.55, R - 200 * k), h: H - (A0 + 104 * k) };
      zoneBox.rx = Lx + Math.max(80 * k, zoneBox.w * 0.45); zoneBox.ry = B - 120 * k;
    } else {
      k = Math.max(0.7, Math.min(1.5, H / 390));
      const side = pic.x;                        // free width on each side of the picture
      put('tk-menu', Lx + 26 * k, sa.t + 26 * k, 40 * k);
      put('tk-fs', Lx + 74 * k, sa.t + 26 * k, 40 * k);
      put('tk-START', Lx + 50 * k, sa.t + 74 * k, 76 * k, 32 * k);
      put('tk-L', Lx + 50 * k, sa.t + 118 * k, 76 * k, 34 * k);
      put('tk-Z', R - 56 * k, B - 72 * k, 86 * k);
      put('tk-R', R - 146 * k, B - 46 * k, 62 * k);
      put('tk-B', R - 128 * k, B - 136 * k, 50 * k);
      put('tk-A', R - 50 * k, B - 176 * k, 54 * k);
      const cx = R - 72 * k, cy = sa.t + 86 * k, d = 38 * k, c = 40 * k;
      put('tk-CU', cx, cy - d, c); put('tk-CD', cx, cy + d, c); put('tk-CL', cx - d, cy, c); put('tk-CR', cx + d, cy, c);
      const zy = sa.t + 146 * k;
      zoneBox = { x: 0, y: zy, w: Math.max(side + 30 * k, 190 * k), h: H - zy };
      zoneBox.rx = Lx + Math.max(78 * k, Math.min(side / 2, 110 * k)); zoneBox.ry = B - 92 * k;
    }
    const z = $('tk-stick').style;
    z.left = zoneBox.x + 'px'; z.top = zoneBox.y + 'px'; z.width = zoneBox.w + 'px'; z.height = zoneBox.h + 'px';
    const base = 124 * k, knob = 56 * k;
    t.travel = 50 * k;
    Object.assign($('tk-base').style, { width: base + 'px', height: base + 'px', marginLeft: -base / 2 + 'px', marginTop: -base / 2 + 'px' });
    Object.assign($('tk-knob').style, { width: knob + 'px', height: knob + 'px', marginLeft: -knob / 2 + 'px', marginTop: -knob / 2 + 'px' });
    if (t.stickId === null) rest();
  }
  // the stick's resting ring: from the zone's real box only (an empty box = the layer is hidden: skip)
  function rest() {
    const r = $('tk-stick').getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0 || !zoneBox) return;
    placeBase(zoneBox.rx, zoneBox.ry);
    $('tk-knob').style.transform = '';
  }
  function placeBase(x, y) {
    const r = $('tk-stick').getBoundingClientRect();
    $('tk-base').style.left = (x - r.left) + 'px'; $('tk-base').style.top = (y - r.top) + 'px';
  }

  // ---- controls -----------------------------------------------------------------------------------------
  const capture = (el, e) => { try { el.setPointerCapture(e.pointerId); } catch (_) { /* synthetic or gone */ } };
  for (const el of document.querySelectorAll('#touch .tb')) {
    const name = el.id.slice(3), bit = N64[name];
    if (!bit) continue;
    const ids = new Set();
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault(); capture(el, e); ids.add(e.pointerId);
      t.b |= bit; el.classList.add('on'); send();
      if (navigator.vibrate) try { navigator.vibrate(8); } catch (_) { /* not allowed */ }
    });
    const up = (e) => { ids.delete(e.pointerId); if (!ids.size) { t.b &= ~bit; el.classList.remove('on'); send(); } };
    for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) el.addEventListener(ev, up);
  }
  {
    const zone = $('tk-stick'), knob = $('tk-knob');
    zone.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      if (t.stickId !== null) return;
      capture(zone, e);
      t.stickId = e.pointerId; t.cx = e.clientX; t.cy = e.clientY;
      placeBase(e.clientX, e.clientY); knob.style.transform = '';
    });
    zone.addEventListener('pointermove', (e) => {
      if (e.pointerId !== t.stickId) return;
      let dx = e.clientX - t.cx, dy = e.clientY - t.cy;
      const m = Math.hypot(dx, dy), T = t.travel;
      if (m > T) { dx *= T / m; dy *= T / m; }
      knob.style.transform = `translate(${dx}px, ${dy}px)`;
      let x = dx / T, y = -dy / T;
      if (Math.hypot(x, y) < 0.12) x = y = 0;   // small dead zone
      t.x = Math.round(x * STICK_MAX); t.y = Math.round(y * STICK_MAX);
      send();
    });
    const end = (e) => { if (e.pointerId !== t.stickId) return; t.stickId = null; t.x = t.y = 0; rest(); send(); };
    for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) zone.addEventListener(ev, end);
    if (window.ResizeObserver) new ResizeObserver(() => { if (t.stickId === null) rest(); }).observe(zone);
  }
  // page menu (☰): the normal page (save, options, online panel, log) with a "Back to the game" button
  const menuBtn = (id, fn) => $(id).addEventListener('pointerup', (e) => { e.preventDefault(); fn(); });
  menuBtn('tk-menu', () => { t.menu = true; apply(); scrollTo(0, 0); });
  const back = (e) => { e.preventDefault(); if (!t.menu) return; t.menu = false; apply(); };   // pointerup: a tap that the
  for (const ev of ['pointerup', 'click']) $('tk-back').addEventListener(ev, back);           // browser took as a scroll never clicks

  // ---- full screen --------------------------------------------------------------------------------------
  const root = document.documentElement;
  const fsOK = !!(root.requestFullscreen || root.webkitRequestFullscreen) && (document.fullscreenEnabled || document.webkitFullscreenEnabled);
  const fsToggle = () => {
    try {
      if (document.fullscreenElement || document.webkitFullscreenElement) (document.exitFullscreen || document.webkitExitFullscreen).call(document);
      else { const p = (root.requestFullscreen || root.webkitRequestFullscreen).call(root, { navigationUI: 'hide' }); if (p && p.catch) p.catch(() => {}); }
    } catch (e) { /* refused */ }
  };
  if (fsOK) { $('opt-fs').hidden = false; $('opt-fs').onclick = fsToggle; menuBtn('tk-fs', fsToggle); }
  else $('tk-fs').style.visibility = 'hidden';   // iPhone Safari: no page full screen (Add to Home Screen instead)

  // ---- no zoom / scroll / callouts while playing -------------------------------------------------------
  const block = (e) => { if (tplay()) e.preventDefault(); };
  for (const ev of ['gesturestart', 'gesturechange', 'dblclick', 'contextmenu']) document.addEventListener(ev, block, { passive: false });
  document.addEventListener('touchmove', block, { passive: false });
  $('touch').addEventListener('contextmenu', (e) => e.preventDefault());
  let lastEnd = 0;   // iOS: a quick second tap zooms even with touch-action; swallow it on the picture
  document.addEventListener('touchend', (e) => { const now = e.timeStamp; if (tplay() && now - lastEnd < 350 && e.cancelable && !e.target.closest('#touch')) e.preventDefault(); lastEnd = now; }, { passive: false });

  // relayout on every size / orientation change (also while the page menu is open: apply() on close re-runs it)
  addEventListener('resize', layout);
  addEventListener('orientationchange', () => setTimeout(layout, 250));
  if (window.visualViewport) visualViewport.addEventListener('resize', layout);

  // ---- setting ------------------------------------------------------------------------------------------
  const sel = $('opt-touch');
  sel.value = setting();
  sel.addEventListener('change', () => { lsSet(sel.value === 'auto' ? null : sel.value); setOn(wanted()); });
  addEventListener('blur', () => { if (t.b || t.x || t.y) { release(); send(); } });   // app switch: no stuck buttons
  document.addEventListener('visibilitychange', () => { if (document.hidden) { release(); send(); } });

  window.GETouch = {
    // main.js, right after main(): the game reads the pad from now on
    started() { t.playing = true; sent = ''; apply(); },
    state: () => ({ on: t.on, playing: t.playing, menu: t.menu, b: t.b, x: t.x, y: t.y, w: innerWidth, h: innerHeight }),
    layout,
  };
  t.on = wanted();
  apply();
})();
