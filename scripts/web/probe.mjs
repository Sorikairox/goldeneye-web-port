// Headless-Chrome probe for the web build (host Chrome, muted, its own profile; kills only what it started).
//   node scripts/web/probe.mjs URL [seconds=20] [outdir=port_work/shots] [shotEvery=5]
// Prints the page console (deduplicated tail), wasm traps / exceptions, and saves PNG screenshots
// <outdir>/shot-<t>s.png every shotEvery seconds and at the end. Screenshots are game imagery: keep
// them in the gitignored port_work/.
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path';
const [url, secs = '20', outdir = 'port_work/shots', every = '5'] = process.argv.slice(2);
if (!url) { console.error('usage: probe.mjs URL [seconds] [outdir] [shotEvery]'); process.exit(2); }
const CH = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
fs.mkdirSync(outdir, { recursive: true });
// ONE probe Chrome at a time across all sessions/worktrees: /tmp/goldeneye-chrome.lock holds the PID of the
// probe that owns the running Chrome. Wait while a live process holds it; a dead PID is a stale lock.
const LOCK = '/tmp/goldeneye-chrome.lock';
// a wrapper that already holds the lock for us (e.g. a lockrun script around route_dam.sh) is an
// ancestor of this process: then run without taking it (else the probe would wait on its own parent)
const ancestors = new Set(); {
  let pid = process.ppid;
  for (let i = 0; i < 32 && pid > 1; i++) {
    ancestors.add(pid);
    try { pid = +execFileSync('ps', ['-o', 'ppid=', '-p', String(pid)]).toString().trim(); } catch (e) { break; }
  }
}
let ownLock = true;
for (let waited = 0; ; waited++) {
  try { fs.writeFileSync(LOCK, String(process.pid), { flag: 'wx' }); break; } catch (e) {}
  let holder = 0; try { holder = +fs.readFileSync(LOCK, 'utf8').trim(); } catch (e) { continue; }
  let alive = false; try { process.kill(holder, 0); alive = true; } catch (e) { alive = e.code === 'EPERM'; }
  if (!alive) { try { fs.unlinkSync(LOCK); } catch (e) {} continue; }
  if (ancestors.has(holder)) { ownLock = false; break; }
  if (waited % 30 === 0) console.log(`probe: waiting for the Chrome lock (held by PID ${holder})`);
  await new Promise((r) => setTimeout(r, 2000));
}
const unlock = () => { if (!ownLock) return; try { if (fs.readFileSync(LOCK, 'utf8').trim() === String(process.pid)) fs.unlinkSync(LOCK); } catch (e) {} };
process.on('exit', unlock);
// PROBE_USERDIR=<dir>: use (and keep) this Chrome profile instead of a fresh temporary one, so IndexedDB /
// localStorage survive into the next run (save persistence tests); the default is a fresh profile per run
const keepProf = !!process.env.PROBE_USERDIR;
if (keepProf) fs.mkdirSync(process.env.PROBE_USERDIR, { recursive: true });
const prof = keepProf ? path.resolve(process.env.PROBE_USERDIR) : fs.mkdtempSync(path.join(os.tmpdir(), 'ge-probe-'));
const port = 9400 + (process.pid % 400);
const ch = spawn(CH, ['--headless=new', `--user-data-dir=${prof}`, `--remote-debugging-port=${port}`,
  '--mute-audio', '--autoplay-policy=no-user-gesture-required', `--use-angle=${process.env.PROBE_ANGLE || 'swiftshader'}`, '--enable-unsafe-swiftshader',
  '--window-size=800,900', 'about:blank'], { stdio: 'ignore' });
const done = (code) => { try { ch.kill('SIGKILL'); } catch (e) {} if (!keepProf) try { fs.rmSync(prof, { recursive: true, force: true }); } catch (e) {} process.exit(code); };
for (const s of ['SIGINT', 'SIGTERM']) process.on(s, () => done(130));
// the console is streamed to <outdir>/console.log as it arrives (with seconds since navigation), so a
// hung page still leaves its last lines behind
const lines = [];
const logf = fs.openSync(path.join(outdir, 'console.log'), 'w');
let tNav = Date.now();
const addLine = (l) => { lines.push(l); fs.writeSync(logf, `${((Date.now() - tNav) / 1000).toFixed(2)} ${l}\n`); };
setTimeout(() => { console.log('probe: timeout; last console lines:'); for (const l of lines.slice(-15)) console.log(l.slice(0, 300)); done(2); }, (+secs + 60) * 1000);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let page;
for (let i = 0; i < 60 && !page; i++) {
  await sleep(250);
  try { page = (await (await fetch(`http://127.0.0.1:${port}/json`)).json()).find((t) => t.type === 'page'); } catch (e) {}
}
if (!page) { console.log('probe: no DevTools page'); done(2); }
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0; const pending = new Map();
ws.addEventListener('message', (m) => {
  const d = JSON.parse(m.data);
  if (d.method === 'Runtime.consoleAPICalled') addLine(d.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
  if (d.method === 'Runtime.exceptionThrown') addLine('EXCEPTION: ' + (d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text));
  if (d.method === 'Inspector.targetCrashed') addLine('PROBE: renderer CRASHED (tab gone; often out of memory)');
  if (d.method === 'Log.entryAdded') addLine(`[${d.params.entry.level}] ${d.params.entry.text}`);
  if (pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
});
const call = (method, params = {}) => new Promise((r) => { pending.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
await call('Runtime.enable'); await call('Log.enable'); await call('Inspector.enable'); await call('Page.enable');
// PROBE_DEBUG=1: enable the debugger before navigating (a busy page may never process a late
// Debugger.enable), so the stack dump below can pause a stuck main thread
if (process.env.PROBE_DEBUG) await call('Debugger.enable');
if (process.env.PROBE_PROFILE) { await call('Profiler.enable'); await call('Profiler.setSamplingInterval', { interval: 500 }); }
// PROBE_PRELOAD="<js>": runs in the page before its own scripts (fake navigator APIs, e.g. a gamepad)
if (process.env.PROBE_PRELOAD) await call('Page.addScriptToEvaluateOnNewDocument', { source: process.env.PROBE_PRELOAD });
tNav = Date.now(); await call('Page.navigate', { url });
if (process.env.PROBE_PROFILE) setTimeout(() => call('Profiler.start'), +process.env.PROBE_PROFILE * 1000);
const t0 = Date.now();
// PROBE_FILE="3:#romfile:/abs/path/rom.z64": at <s since navigation>, put a host file into a file input
// (CDP DOM.setFileInputFiles), as a player choosing it would; the input's change event fires. For testing
// the deployed site, which ignores the dev ?romurl= path only when no ROM is stored.
for (const ent of (process.env.PROBE_FILE || '').split(',').filter(Boolean)) {
  const [t, sel, ...p] = ent.split(':');
  setTimeout(async () => {
    const doc = await call('DOM.getDocument');
    const q = await call('DOM.querySelector', { nodeId: doc.result.root.nodeId, selector: sel });
    const r = await call('DOM.setFileInputFiles', { nodeId: q.result.nodeId, files: [p.join(':')] });
    console.log(`probe: file ${p.join(':')} -> ${sel}${r.error ? ' FAILED ' + r.error.message : ''}`);
  }, +t * 1000);
}
// PROBE_KEYS="62:KeyW:3,70:Space:0.2": real keyboard events (CDP Input) at <s since navigation>,
// held for <s> -- checks the page's own SDL keyboard path (not the scripted pad)
for (const ent of (process.env.PROBE_KEYS || '').split(',').filter(Boolean)) {
  const [t, code, dur] = ent.split(':');
  const key = code.startsWith('Key') ? code.slice(3).toLowerCase() : code === 'Space' ? ' ' : code;
  const vk = code.startsWith('Key') ? code.charCodeAt(3) : code === 'Space' ? 32 : code === 'Enter' ? 13 : 0;
  const ev = (type) => call('Input.dispatchKeyEvent', { type, code, key, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk });
  setTimeout(() => { ev('keyDown'); setTimeout(() => ev('keyUp'), +dur * 1000); }, +t * 1000);
}
// PROBE_CLICKS="20:150,30": trusted left clicks at the canvas centre at <s since navigation>, button held
// <ms> (default 100) -- user gestures, e.g. for pointer lock
const canvasCentre = async () => {
  const q = await call('Runtime.evaluate', { expression: 'JSON.stringify(document.getElementById("canvas")?.getBoundingClientRect())', returnByValue: true });
  const b = JSON.parse(q.result.result.value); return [b.x + b.width / 2, b.y + b.height / 2];
};
for (const ent of (process.env.PROBE_CLICKS || '').split(',').filter(Boolean)) {
  const [t, hold = '100'] = ent.split(':');
  setTimeout(async () => {
    const [x, y] = await canvasCentre();
    await call('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y });
    await call('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    setTimeout(() => call('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 }), +hold);
  }, +t * 1000);
}
// PROBE_EVALS="12:<js>|20:<js>": evaluate at <s since navigation>, log 'probe-eval <t>: <result>' to console.log
for (const ent of (process.env.PROBE_EVALS || '').split('|').filter(Boolean)) {
  const i = ent.indexOf(':'); const t = ent.slice(0, i), js = ent.slice(i + 1);
  setTimeout(async () => {
    const r = await Promise.race([call('Runtime.evaluate', { expression: js, returnByValue: true, awaitPromise: true, userGesture: true }), sleep(5000).then(() => null)]);
    addLine(`probe-eval ${t}: ` + (r ? JSON.stringify(r.result?.result?.value ?? r.result?.exceptionDetails?.text) : 'no answer'));
  }, +t * 1000);
}
let stacked = false, paused = null;
ws.addEventListener('message', (m) => { const d = JSON.parse(m.data); if (d.method === 'Debugger.paused') paused = d.params; });
const stack = async () => {
  if (!process.env.PROBE_DEBUG) { call('Debugger.enable'); await sleep(3000); }
  call('Debugger.pause');
  for (let i = 0; i < 160 && !paused; i++) await sleep(250);
  if (!paused) { console.log('probe: could not pause the page'); return; }
  console.log('probe: main thread stack:\n' + paused.callFrames.slice(0, 25).map((f) => '  at ' + (f.functionName || '?')).join('\n'));
  call('Debugger.resume');
};
// screenshots are clipped to the game canvas (PROBE_FULL=1: whole page), scaled by PROBE_SCALE (default 0.5)
let clip = null; const sheet = [];
const shot = async (tag) => {
  if (!clip && !process.env.PROBE_FULL) {
    const q = await Promise.race([call('Runtime.evaluate', { expression: 'JSON.stringify(document.getElementById("canvas")?.getBoundingClientRect())', returnByValue: true }), sleep(3000).then(() => null)]);
    try { const b = JSON.parse(q.result.result.value); if (b.width > 0) clip = { x: b.x, y: b.y, width: b.width, height: b.height, scale: +(process.env.PROBE_SCALE || 0.5) }; } catch (e) {}
  }
  const r = await Promise.race([call('Page.captureScreenshot', clip ? { format: 'png', clip } : { format: 'png' }), sleep(10000).then(() => null)]);
  if (!r?.result?.data) {
    console.log(`probe: screenshot ${tag} failed (page busy?)`);
    if (!stacked) { stacked = true; await stack(); }      // where is the main thread stuck?
    return;
  }
  const f = path.join(outdir, `shot-${tag}.png`); fs.writeFileSync(f, Buffer.from(r.result.data, 'base64')); console.log('probe: screenshot ' + f);
  sheet.push([tag, r.result.data]);
};
for (let t = +every; t < +secs; t += +every) { await sleep(Math.max(1, t * 1000 - (Date.now() - t0))); await shot(`${t}s`); }
await sleep(Math.max(0, +secs * 1000 - (Date.now() - t0))); await shot(`${secs}s`);
if (process.env.PROBE_PROFILE) {
  const r = await Promise.race([call('Profiler.stop'), sleep(15000).then(() => null)]);
  if (!r?.result?.profile) console.log('probe: profile not returned');
  else {
    const p = r.result.profile, self = new Map(), byId = new Map(p.nodes.map((n) => [n.id, n]));
    const counts = new Map(); for (const sid of p.samples) counts.set(sid, (counts.get(sid) || 0) + 1);
    for (const [sid, c] of counts) { const n = byId.get(sid); const k = n.callFrame.functionName || '(anon)'; self.set(k, (self.get(k) || 0) + c); }
    console.log('probe: top self-time functions: ' + [...self].sort((a, b) => b[1] - a[1]).slice(0, 15).map(([k, c]) => `${k}=${c}`).join(' '));
  }
}
// is the page's main thread still answering? (a wasm busy loop blocks it)
{
  const r = await Promise.race([call('Runtime.evaluate', { expression: process.env.PROBE_EVAL || 'performance.now()', returnByValue: true, awaitPromise: true }), sleep(5000).then(() => null)]);
  console.log('probe: main thread ' + (r ? 'answers: ' + JSON.stringify(r.result?.result?.value) : 'does NOT answer (busy)'));
  if (!r && !stacked) { stacked = true; await stack(); }
}
// compact console: collapse consecutive duplicates, print the last 120 lines
const out = []; for (const l of lines) { if (out.length && out[out.length - 1].l === l) out[out.length - 1].n++; else out.push({ l, n: 1 }); }
console.log(`probe: ${lines.length} console lines (full: ${path.join(outdir, 'console.log')}); last:`);
for (const { l, n } of out.slice(-120)) console.log((n > 1 ? `(${n}x) ` : '') + l.slice(0, 400));
// contact sheet of every screenshot (<outdir>/sheet.png), drawn by the same Chrome after the run
if (sheet.length > 1 && !process.env.PROBE_NOSHEET) {
  const cells = sheet.map(([t, d]) => `<div><img src="data:image/png;base64,${d}"><br>${t}</div>`).join('');
  const html = `<style>body{margin:0;background:#222;color:#fff;font:12px sans-serif;display:flex;flex-wrap:wrap}div{margin:2px;text-align:center}img{width:240px}</style>${cells}`;
  await call('Page.navigate', { url: 'about:blank' }); await sleep(500);
  await call('Runtime.evaluate', { expression: `document.open();document.write(${JSON.stringify(html)});document.close();` });
  await sleep(1000);
  const cols = 6, rows = Math.ceil(sheet.length / cols);
  await call('Emulation.setDeviceMetricsOverride', { width: cols * 244 + 4, height: rows * 200 + 10, deviceScaleFactor: 1, mobile: false });
  await sleep(500);
  const r = await Promise.race([call('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true }), sleep(10000).then(() => null)]);
  if (r?.result?.data) { fs.writeFileSync(path.join(outdir, 'sheet.png'), Buffer.from(r.result.data, 'base64')); console.log('probe: contact sheet ' + path.join(outdir, 'sheet.png')); }
}
// a kept profile: close Chrome cleanly so it writes IndexedDB / localStorage to disk (SIGKILL may drop them)
if (keepProf) {
  const exited = new Promise((r) => ch.once('exit', r));
  try { call('Browser.close'); } catch (e) {}
  await Promise.race([exited, sleep(8000)]);
}
done(0);
