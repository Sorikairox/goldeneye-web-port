// G8 test helper: press the page's touch controls (port/web/touch.js) with synthetic pointer events, through their
// own handlers. Evaluate this file's text, then: __tp('tk-Z', 300) holds Z for 300 ms; __tstick(0, -50, 2000) drags the
// stick 50 px up (forward) for 2 s. Used where CDP touch emulation is not available (netgroup.mjs GUEST_EVALS).
(() => {
  const ev = (el, type, id, x, y) => el.dispatchEvent(new PointerEvent(type, { pointerId: id, pointerType: 'touch', isPrimary: false,
    bubbles: true, cancelable: true, clientX: x, clientY: y }));
  const centre = (el) => { const r = el.getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; };
  window.__tp = (id, ms = 200) => {
    const el = document.getElementById(id), [x, y] = centre(el), pid = 100 + Math.floor(Math.random() * 1000);
    ev(el, 'pointerdown', pid, x, y);
    setTimeout(() => ev(el, 'pointerup', pid, x, y), ms);
    return id + ' ' + ms + ' ms';
  };
  window.__tstick = (dx, dy, ms = 1000) => {
    const z = document.getElementById('tk-stick'), [x, y] = centre(document.getElementById('tk-base')), pid = 7;
    ev(z, 'pointerdown', pid, x, y); ev(z, 'pointermove', pid, x + dx / 2, y + dy / 2); ev(z, 'pointermove', pid, x + dx, y + dy);
    setTimeout(() => ev(z, 'pointerup', pid, x + dx, y + dy), ms);
    return JSON.stringify(window.GETouch && GETouch.state());
  };
  return 'touchpress ready';
})();
