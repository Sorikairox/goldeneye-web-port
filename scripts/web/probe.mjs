// Headless-Chrome probe for the web build (host Chrome, muted, its own profile; kills only what it started).
//   node scripts/web/probe.mjs URL [seconds=20] [outdir=port_work/shots] [shotEvery=5]
// Prints the page console (deduplicated tail), wasm traps / exceptions, and saves PNG screenshots
// <outdir>/shot-<t>s.png every shotEvery seconds and at the end. Screenshots are game imagery: keep
// them in the gitignored port_work/.
import { spawn } from 'node:child_process';
import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path';
const [url, secs = '20', outdir = 'port_work/shots', every = '5'] = process.argv.slice(2);
if (!url) { console.error('usage: probe.mjs URL [seconds] [outdir] [shotEvery]'); process.exit(2); }
const CH = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
fs.mkdirSync(outdir, { recursive: true });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'ge-probe-'));
const port = 9400 + (process.pid % 400);
const ch = spawn(CH, ['--headless=new', `--user-data-dir=${prof}`, `--remote-debugging-port=${port}`,
  '--mute-audio', '--autoplay-policy=no-user-gesture-required', `--use-angle=${process.env.PROBE_ANGLE || 'swiftshader'}`, '--enable-unsafe-swiftshader',
  '--window-size=800,900', 'about:blank'], { stdio: 'ignore' });
const done = (code) => { try { ch.kill('SIGKILL'); } catch (e) {} try { fs.rmSync(prof, { recursive: true, force: true }); } catch (e) {} process.exit(code); };
setTimeout(() => { console.log('probe: timeout'); done(2); }, (+secs + 60) * 1000);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let page;
for (let i = 0; i < 60 && !page; i++) {
  await sleep(250);
  try { page = (await (await fetch(`http://127.0.0.1:${port}/json`)).json()).find((t) => t.type === 'page'); } catch (e) {}
}
if (!page) { console.log('probe: no DevTools page'); done(2); }
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0; const pending = new Map(); const lines = [];
ws.addEventListener('message', (m) => {
  const d = JSON.parse(m.data);
  if (d.method === 'Runtime.consoleAPICalled') lines.push(d.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
  if (d.method === 'Runtime.exceptionThrown') lines.push('EXCEPTION: ' + (d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text));
  if (d.method === 'Inspector.targetCrashed') lines.push('PROBE: renderer CRASHED (tab gone; often out of memory)');
  if (d.method === 'Log.entryAdded') lines.push(`[${d.params.entry.level}] ${d.params.entry.text}`);
  if (pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
});
const call = (method, params = {}) => new Promise((r) => { pending.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
await call('Runtime.enable'); await call('Log.enable'); await call('Inspector.enable'); await call('Page.enable');
if (process.env.PROBE_PROFILE) { await call('Profiler.enable'); await call('Profiler.setSamplingInterval', { interval: 500 }); }
await call('Page.navigate', { url });
if (process.env.PROBE_PROFILE) setTimeout(() => call('Profiler.start'), +process.env.PROBE_PROFILE * 1000);
const t0 = Date.now();
let stacked = false, paused = null;
ws.addEventListener('message', (m) => { const d = JSON.parse(m.data); if (d.method === 'Debugger.paused') paused = d.params; });
const stack = async () => {
  call("Debugger.enable"); await sleep(3000); call('Debugger.pause');
  for (let i = 0; i < 160 && !paused; i++) await sleep(250);
  if (!paused) { console.log('probe: could not pause the page'); return; }
  console.log('probe: main thread stack:\n' + paused.callFrames.slice(0, 25).map((f) => '  at ' + (f.functionName || '?')).join('\n'));
  call('Debugger.resume');
};
const shot = async (tag) => {
  const r = await Promise.race([call('Page.captureScreenshot', { format: 'png' }), sleep(10000).then(() => null)]);
  if (!r?.result?.data) {
    console.log(`probe: screenshot ${tag} failed (page busy?)`);
    if (!stacked) { stacked = true; await stack(); }      // where is the main thread stuck?
    return;
  }
  const f = path.join(outdir, `shot-${tag}.png`); fs.writeFileSync(f, Buffer.from(r.result.data, 'base64')); console.log('probe: screenshot ' + f);
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
  const r = await Promise.race([call('Runtime.evaluate', { expression: process.env.PROBE_EVAL || 'performance.now()', returnByValue: true }), sleep(5000).then(() => null)]);
  console.log('probe: main thread ' + (r ? 'answers: ' + JSON.stringify(r.result?.result?.value) : 'does NOT answer (busy)'));
  if (!r && !stacked) { stacked = true; await stack(); }
}
// compact console: collapse consecutive duplicates, print the last 120 lines
const out = []; for (const l of lines) { if (out.length && out[out.length - 1].l === l) out[out.length - 1].n++; else out.push({ l, n: 1 }); }
fs.writeFileSync(path.join(outdir, 'console.log'), lines.join('\n') + '\n');
console.log(`probe: ${lines.length} console lines (full: ${path.join(outdir, 'console.log')}); last:`);
for (const { l, n } of out.slice(-120)) console.log((n > 1 ? `(${n}x) ` : '') + l.slice(0, 400));
done(0);
