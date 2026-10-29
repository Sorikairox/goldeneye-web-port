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
  '--mute-audio', '--autoplay-policy=no-user-gesture-required', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
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
  if (d.method === 'Log.entryAdded') lines.push(`[${d.params.entry.level}] ${d.params.entry.text}`);
  if (pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
});
const call = (method, params = {}) => new Promise((r) => { pending.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
await call('Runtime.enable'); await call('Log.enable'); await call('Page.enable');
await call('Page.navigate', { url });
const t0 = Date.now();
const shot = async (tag) => {
  const r = await Promise.race([call('Page.captureScreenshot', { format: 'png' }), sleep(10000).then(() => null)]);
  if (!r?.result?.data) { console.log(`probe: screenshot ${tag} failed (page busy?)`); return; }
  const f = path.join(outdir, `shot-${tag}.png`); fs.writeFileSync(f, Buffer.from(r.result.data, 'base64')); console.log('probe: screenshot ' + f);
};
for (let t = +every; t < +secs; t += +every) { await sleep(t * 1000 - (Date.now() - t0)); await shot(`${t}s`); }
await sleep(Math.max(0, +secs * 1000 - (Date.now() - t0))); await shot(`${secs}s`);
// compact console: collapse consecutive duplicates, print the last 120 lines
const out = []; for (const l of lines) { if (out.length && out[out.length - 1].l === l) out[out.length - 1].n++; else out.push({ l, n: 1 }); }
fs.writeFileSync(path.join(outdir, 'console.log'), lines.join('\n') + '\n');
console.log(`probe: ${lines.length} console lines (full: ${path.join(outdir, 'console.log')}); last:`);
for (const { l, n } of out.slice(-120)) console.log((n > 1 ? `(${n}x) ` : '') + l.slice(0, 400));
done(0);
