// PROBE_PRELOAD script (scripts/web/probe.mjs): a fake "standard" gamepad for headless tests of the
// Gamepad API path (SDL2's Emscripten joystick backend -> SDL_GameController -> port/src/input.c).
// It connects 3 s after load; drive it from PROBE_EVALS, e.g.
//   window.__pad.press(0, 400)        hold button 0 (A / cross, south) for 400 ms
//   window.__pad.axis(1, -1, 2000)    left stick up for 2 s
// Button indices are the W3C standard mapping (0 A, 1 B, 9 Start, 7 right trigger, 12-15 d-pad).
(() => {
  const now = () => performance.now();
  const pad = {
    id: 'GE web test pad (STANDARD GAMEPAD Vendor: 045e Product: 028e)', index: 0, connected: false,
    mapping: 'standard', timestamp: now(), axes: [0, 0, 0, 0],
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })),
    vibrationActuator: null,
  };
  const touch = () => { pad.timestamp = now(); };
  window.__pad = {
    pad,
    press(i, ms = 150) { const b = pad.buttons[i]; b.pressed = b.touched = true; b.value = 1; touch();
      setTimeout(() => { b.pressed = b.touched = false; b.value = 0; touch(); }, ms); },
    axis(i, v, ms = 1000) { pad.axes[i] = v; touch(); setTimeout(() => { pad.axes[i] = 0; touch(); }, ms); },
  };
  navigator.getGamepads = () => [pad.connected ? pad : null, null, null, null];
  setTimeout(() => {
    pad.connected = true; touch();
    const ev = new Event('gamepadconnected'); Object.defineProperty(ev, 'gamepad', { value: pad });
    window.dispatchEvent(ev);
    console.log('[fakepad] connected');
  }, 3000);
})();
