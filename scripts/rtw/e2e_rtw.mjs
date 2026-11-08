// End-to-end check of the retrowser.app build (docs/web/RTW.md): the retrowser host serves the build from its
// GitHub mock; one muted headless Chrome loads the player's ROM and plays.
//   node scripts/rtw/e2e_rtw.mjs <phase> <build dir> <out dir> [rom]
//   phase "dev":     host = `npm run dev` (Vite, no service workers): boot to the title and menus, keyboard input,
//                    audio drained, pointer-lock request, a save stored and handed back after a reload.
//   phase "offline": host = the built host (dist/, tools/serve-worker.ts): play once to the title screen, stop every
//                    server, reload offline, play again from the caches and the remembered ROM, to the title screen.
//   <build dir> holds a manifest at its root: build/rtw, or a copy of the repo layout (root retrowser.json pointing
//   into retrowser/, e.g. `git archive HEAD retrowser.json retrowser | tar -x -C DIR`): the mock serves it as the
//   default branch, so the host link is the plain owner/repo.
// Env: RTW_REPO (retrowser checkout, default $HOME/Code/retrowser), CHROME_PATH, RTW_SECS (boot wait, default 70).
// Screenshots go to <out dir> (local only: game pictures, never commit). Every server and Chrome it starts is
// stopped by PID at the end. Prints PASS/FAIL lines.
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { existsSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { regions } from './eepcheck.mjs';

const [phase, buildDir, outDir, romArg] = process.argv.slice(2);
if (!phase || !buildDir || !outDir) { console.error('usage: e2e_rtw.mjs dev|offline <build dir> <out dir> [rom]'); process.exit(2); }
const RTW = process.env.RTW_REPO || join(process.env.HOME, 'Code/retrowser');
const ROM = romArg || join(process.env.HOME, 'roms/GoldenEye_007_Europe.z64');
const SECS = Number(process.env.RTW_SECS || 70);
mkdirSync(outDir, { recursive: true });
const { startMockGithub } = await import(pathToFileURL(join(RTW, 'tools/mock-github.ts')).href);
const { chromium } = createRequire(join(RTW, 'package.json'))('playwright-core');
const chromePath = process.env.CHROME_PATH || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

const results = [];
const check = (name, ok, detail = '') => { results.push(ok); console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const COMMIT = 'eeee007eeee007eeee007eeee007eeee007eeee0';
const mock = await startMockGithub({ 'rtw-test/goldeneye-eu': { refs: { main: COMMIT }, commits: [{ sha: COMMIT, dir: resolve(buildDir) }] } });

let host = null, site;
async function startHost() {
  if (phase === 'dev') {
    const port = 5199;
    host = spawn(process.execPath, [join(RTW, 'node_modules/vite/bin/vite.js'), '--port', String(port), '--strictPort'], { cwd: RTW, stdio: ['ignore', 'pipe', 'pipe'] });   // = npm run dev
    await new Promise((ok, bad) => {
      const t = setTimeout(() => bad(new Error('vite did not start')), 30000);
      host.stdout.on('data', (d) => { if (/Local:|ready in/.test(String(d))) { clearTimeout(t); ok(); } });
      host.on('exit', (c) => bad(new Error('vite exited ' + c)));
    });
    site = `http://localhost:${port}`;
  } else {
    const { serveWorker } = await import(pathToFileURL(join(RTW, 'tools/serve-worker.ts')).href);
    const port = 8788 + 11;
    host = await serveWorker(port);
    site = `http://localhost:${port}`;
  }
  console.log(`host ${phase} on ${site} (pid ${host.pid ?? 'in-process'}), mock ${mock.base}`);
}
async function stopHost() {
  if (!host) return;
  if (host.pid) { host.kill('SIGTERM'); await sleep(300); try { process.kill(host.pid, 0); host.kill('SIGKILL'); } catch { /* gone */ } }
  else await new Promise((ok) => { host.closeAllConnections(); host.close(() => ok()); });
  host = null;
}

await startHost();
const profile = join(outDir, 'profile');
rmSync(profile, { recursive: true, force: true });
const ctx = await chromium.launchPersistentContext(profile, {
  executablePath: chromePath, headless: true, viewport: { width: 1024, height: 768 },
  reducedMotion: 'reduce', // the game page's cartridge floats otherwise, and a click waits for it to stand still
  args: ['--mute-audio', '--enable-unsafe-swiftshader', '--autoplay-policy=no-user-gesture-required'],
});
await ctx.addInitScript(([ep, dev]) => {
  try {
    if (location.hostname !== 'localhost') return;
    localStorage.setItem('rtw:test-endpoints', ep);
    // Vite dev: the host's AudioWorklet module does not register there (host issue); its ScriptProcessor fallback does
    if (dev && !localStorage.getItem('rtw:audio')) localStorage.setItem('rtw:audio', JSON.stringify({ fallback: true }));
  } catch { /* */ }
}, [JSON.stringify(mock.endpoints), phase === 'dev']);
const page = ctx.pages()[0] ?? await ctx.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
const logOf = async () => (await page.getAttribute('[data-state=playing]', 'data-log').catch(() => null)) ?? '';
const attr = async (a) => page.getAttribute('[data-state=playing]', a).catch(() => null);
const audio = async () => JSON.parse((await attr('data-audio')) ?? '{}');
let shotN = 0;
let rememberState = 'absent';
async function shot(name) {
  const f = join(outDir, `${String(shotN++).padStart(2, '0')}-${name}.png`);
  await page.locator('[data-field=screen]').screenshot({ path: f }).catch((e) => console.log('shot failed ' + e.message));
  return f;
}
const press = async (key, ms = 150) => { await page.keyboard.down(key); await sleep(ms); await page.keyboard.up(key); };
async function waitLog(re, secs) {
  const t0 = Date.now();
  while (Date.now() - t0 < secs * 1000) {
    const l = await logOf();
    if (re.test(l)) return true;
    if (await page.$('.play .error')) return false;
    await sleep(500);
  }
  return false;
}
async function openGame(stored) {
  await page.goto(`${site}/g/rtw-test/goldeneye-eu`);
  // the game page is the cartridge alone (retrowser 2026-11-07): tapping it downloads; a remembered ROM starts at once
  await page.click('.cart', { timeout: 120000 });
  if (stored) {
    await page.waitForSelector('[data-state=playing], [data-action=use-stored]', { timeout: 120000 });
    if (await page.$('[data-action=use-stored]')) await page.click('[data-action=use-stored]');
  }
  else {
    await page.waitForSelector('[data-field=rom]', { state: 'attached', timeout: 180000 });
    const remember = await page.$('[data-field=remember]');
    if (remember) { await remember.check(); rememberState = await remember.isChecked(); }
    await page.setInputFiles('[data-field=rom]', ROM);
  }
  await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
}
// Start through the intro (~20 s of logos, then the gun barrel) to the title / file select; pictures checked by eye
async function toTitle(tag) {
  await sleep(Math.max(0, (SECS - 35) * 1000));
  for (let i = 0; i < 3; i++) { await press('Enter'); await sleep(2500); await shot(`${tag}-start${i}`); }
  check(`${tag}: Start reached the module`, /pad0 buttons 00001000/.test(await logOf()));
}
function dumpLog(name, l) { writeFileSync(join(outDir, name), l); }

try {
  if (phase === 'dev') {
    await openGame(false);
    const started = await waitLog(/rtw: game started/, 120);
    check('boot: main() ran in the worker', started);
    const err = await page.$('.play .error');
    if (err) check('no game error', false, await err.textContent());
    await sleep(8000); await shot('logos');
    // input: Start through the intro to the title / file select (the game needs ~20 s of logos first)
    await sleep(Math.max(0, (SECS - 50) * 1000));
    const a0 = await audio();
    for (let i = 0; i < 6; i++) { await press('Enter'); await sleep(2500); await shot('start' + i); }
    await press('KeyX'); await sleep(3000); await shot('a1');
    await press('KeyX'); await sleep(3000); await shot('a2');
    // walk: stick up + left for 2 s (pad 0 = keyboard on the host)
    await page.keyboard.down('ArrowUp'); await page.keyboard.down('ArrowLeft'); await sleep(2000);
    await shot('walk'); await page.keyboard.up('ArrowLeft'); await page.keyboard.up('ArrowUp');
    await press('KeyZ', 300); await sleep(1500); await shot('fire');
    { const l = await logOf(); check('input: pad 0 reached the module (Start, A, Z, stick)', /pad0 buttons 00001000/.test(l) && /pad0 buttons 00008000/.test(l) && /pad0 buttons 00002000/.test(l) && /stick -80,80/.test(l)); }
    check('pointer lock requested in a stage', !!(await page.$('[data-field=mouse-catch]')));
    // extra button 3 (host key Digit3) = Screenshot -> PNG through rtw_export_file -> the host's Saved files
    await press('Digit3', 200); await sleep(2500);
    check('screenshot exported (rtw_export_file)', /rtw: screenshot \d+x\d+ exported/.test(await logOf()) && !!(await page.$('[data-action=files]')));
    check('options applied (no unregistered keys)', !/rtw: option \d+: .* not registered/.test(await logOf()) && /build id [0-9a-f]{16}/.test(await logOf()));
    const a1 = await audio();
    check('audio: the host drains and plays', (a1.played ?? 0) > (a0.played ?? 0) || (a1.queuedMs ?? 0) > 0, JSON.stringify(a1));
    let l = await logOf();
    dumpLog('log1.txt', l);
    const gen = (await page.evaluate(async () => {
      const db = await new Promise((ok) => { const r = indexedDB.open('retrowser'); r.onsuccess = () => ok(r.result); });
      const names = [...db.objectStoreNames];
      const v = names.includes('saves') ? await new Promise((ok) => { const r = db.transaction('saves').objectStore('saves').get('goldeneye-eu:eeprom'); r.onsuccess = () => ok(r.result); r.onerror = () => ok(null); }) : null;
      db.close();
      return v ? { n: v.bytes.length, nz: v.bytes.reduce((s, x) => s + (x ? 1 : 0), 0), hex: [...v.bytes].map((x) => x.toString(16).padStart(2, '0')).join('') } : null;
    }));
    check('save: the host stored a native image', !!gen && gen.n === 512 && gen.nz > 0, gen ? `${gen.n} bytes` : 'none');
    if (gen) {
      const bytes = Uint8Array.from(gen.hex.match(/../g).map((h) => parseInt(h, 16)));
      writeFileSync(join(outDir, 'stored.eep'), bytes);   // a save: local only
      const rs = regions(bytes);
      check('save: block 0 and slot 1 valid in N64 byte order (game CRC), none in port order',
        rs[0].native && rs[1].native && !rs.some((r) => r.port), JSON.stringify(rs.map((r) => (r.native ? 'N' : r.port ? 'P' : '-')).join('')));
    }
    // reload: the stored save is handed back before main()
    await page.reload();
    await sleep(3000);
    writeFileSync(join(outDir, 'reload-page.txt'), await page.evaluate(() => document.body.innerText));
    if (await page.$('.cart')) await page.click('.cart');
    await sleep(2000);
    if (await page.$('[data-action=use-stored]')) await page.click('[data-action=use-stored]');
    else if (await page.$('[data-field=rom]')) await page.setInputFiles('[data-field=rom]', ROM);
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    const again = await waitLog(/rtw: game started \(save from the host\)/, 120);
    check('save: handed back after a reload', again);
    const eep = await waitLog(/eeprom: image of 2048 bytes loaded/, 5);
    check('save: the game read it as its EEPROM', eep);
    await sleep(25000); await shot('reloaded');
    l = await logOf();
    dumpLog('log2.txt', l);
    check('sidecars from the sandbox IndexedDB on the second start', /sidecars from IndexedDB/.test(l));
  } else {
    await openGame(false);
    check('online: boot', await waitLog(/rtw: game started/, 120));
    await sleep(15000); await shot('online');
    await toTitle('online');
    console.log('stored ROMs on the host: ' + await page.evaluate(async () => {
      const db = await new Promise((ok) => { const r = indexedDB.open('retrowser'); r.onsuccess = () => ok(r.result); });
      const n = await new Promise((ok) => { const r = db.transaction('roms').objectStore('roms').count(); r.onsuccess = () => ok(r.result); r.onerror = () => ok(-1); });
      db.close(); return n;
    }).catch((e) => 'error ' + e.message));
    console.log('remember checkbox was: ' + rememberState);
    const before = mock.log.length;
    await ctx.setOffline(true);
    await stopHost();
    await mock.close();
    await page.reload();
    await page.waitForSelector('.cart', { timeout: 60000 });
    await page.click('.cart');
    await sleep(3000);
    if (await page.$('[data-action=use-stored]')) { await page.click('[data-action=use-stored]'); console.log('offline: remembered ROM'); }
    else if (await page.$('[data-field=rom]')) { await page.setInputFiles('[data-field=rom]', ROM); console.log('offline: ROM chosen again (none remembered)'); }
    else console.log('offline: remembered ROM (started at once)');
    await page.waitForSelector('[data-state=playing]', { timeout: 120000 });
    check('offline: boot from the caches', await waitLog(/rtw: game started/, 120));
    check('offline: sidecars from the sandbox IndexedDB', /sidecars from IndexedDB/.test(await logOf()));
    await sleep(15000); await shot('offline');
    await toTitle('offline');
    { const au = await audio(); check('offline: audio drained into the host AudioWorklet', au.kind === 'worklet' && (au.played ?? 0) > 0, JSON.stringify(au)); }
    check('offline: no request reached the mock', mock.log.length === before);
    dumpLog('log-offline.txt', await logOf());
  }
  check('no page errors', errors.length === 0, errors.slice(0, 3).join(' | '));
} catch (e) {
  check('run', false, e.stack || String(e));
  dumpLog('log-fail.txt', await logOf());
  writeFileSync(join(outDir, 'fail-page.txt'), await page.evaluate(() => document.body.innerText).catch(() => ''));
  await shot('fail');
} finally {
  await ctx.close().catch(() => {});
  await stopHost();
  await mock.close().catch(() => {});
}
console.log(results.every(Boolean) ? 'e2e: PASS' : 'e2e: FAIL');
process.exit(results.every(Boolean) ? 0 : 1);
