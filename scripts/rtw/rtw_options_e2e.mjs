// In-game options of the retrowser.app build (standard 1.2 "inGame" options, docs/web/RTW.md "Options") end to end:
// one page in a muted headless Chrome against the retrowser host's built dist (in-process). The extra button Options
// (key 4) opens the port's options overlay; the pad (arrows = stick, X = A, C = B) changes settings in it; every change
// of a mapped setting reaches the host (env.rtw_option_changed), which keeps it; after a reload the game starts with it.
//   node scripts/rtw/rtw_options_e2e.mjs <build dir> <out dir> [rom]
// Env: RTW_REPO, RTW_HOST_DIST, CHROME_PATH, PORT (default 8797), STEPS (overlay key sequence, default below).
// Screenshots go to <out dir> (local only: game pictures, never commit). Prints PASS / FAIL and "ALL PASS".
import { mkdirSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [buildDir, out, romArg] = process.argv.slice(2);
if (!buildDir || !out) { console.error('usage: rtw_options_e2e.mjs <build dir> <out dir> [rom]'); process.exit(2); }
const RTW = process.env.RTW_REPO || join(process.env.HOME, 'Code/retrowser');
const ROM = romArg || join(process.env.HOME, 'roms/GoldenEye_007_Europe.z64');
const { chromium } = createRequire(join(RTW, 'package.json'))('playwright-core');
const { startMockGithub } = await import(pathToFileURL(join(RTW, 'tools/mock-github.ts')).href);
const { serveWorker } = await import(pathToFileURL(join(RTW, 'tools/serve-worker.ts')).href);
const REPO = 'rtw-test/goldeneye-eu', COMMIT = 'eeee007eeee007eeee007eeee007eeee00712121', PORT = Number(process.env.PORT || 8797);
mkdirSync(out, { recursive: true });
const t0 = Date.now(), say = (s) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${s}`);
let fails = 0;
const check = (ok, what) => { say(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) fails++; return ok; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const mock = await startMockGithub({ [REPO]: { refs: { main: COMMIT }, commits: [{ sha: COMMIT, dir: resolve(buildDir) }] } });
const served = await serveWorker(PORT, process.env.RTW_HOST_DIST ? resolve(process.env.RTW_HOST_DIST) : undefined);
const site = `http://localhost:${PORT}`;
const browser = await chromium.launch({ executablePath: process.env.CHROME_PATH || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  headless: true, args: ['--mute-audio', '--use-angle=metal', '--autoplay-policy=no-user-gesture-required'] });
const ctx = await browser.newContext({ viewport: { width: 960, height: 720 } });
await ctx.addInitScript((ep) => { try { if (location.hostname === 'localhost') localStorage.setItem('rtw:test-endpoints', ep); } catch { /* */ } }, JSON.stringify(mock.endpoints));
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
const log = async () => (await page.getAttribute('[data-state=playing]', 'data-log').catch(() => null)) ?? '';
const press = async (key, ms = 120) => { await page.keyboard.down(key); await sleep(ms); await page.keyboard.up(key); await sleep(250); };
let shotN = 0;
const shot = (name) => page.locator('[data-field=screen]').screenshot({ path: join(out, `${String(shotN++).padStart(2, '0')}-${name}.png`) }).catch(() => {});
async function inWorker(fn, arg) {
  for (const w of page.workers()) {
    try { if (await w.evaluate(() => typeof globalThis.__rtwGoldenEye === 'object')) return await w.evaluate(fn, arg); } catch { /* gone */ }
  }
  return null;
}
const stored = () => page.evaluate(() => { try { return JSON.parse(localStorage.getItem('rtw:options:goldeneye-eu') || '{}'); } catch { return {}; } });
async function open(useStored) {
  await page.goto(`${site}/g/${REPO}`);
  await page.click('[data-action=download]', { timeout: 60000 });
  if (useStored) { await page.waitForSelector('[data-action=use-stored]', { timeout: 60000 }); await page.click('[data-action=use-stored]'); }
  else {
    await page.waitForSelector('[data-field=rom]', { state: 'attached', timeout: 120000 });
    const remember = await page.$('[data-field=remember]');
    if (remember) await remember.check();
    await page.setInputFiles('[data-field=rom]', ROM);
  }
  await page.waitForFunction(() => Number(document.querySelector('[data-state=playing]')?.getAttribute('data-frames') ?? 0) > 10, null, { timeout: 120000 });
  await page.click('[data-state=persist-warning] button', { timeout: 1000 }).catch(() => {});
}

try {
  await open(false);
  // the host menu shows the start-up options only
  await page.click('[data-action=menu]', { timeout: 5000 }).catch(() => {});
  await sleep(500);
  const menuText = await page.evaluate(() => document.body.innerText);
  writeFileSync(join(out, 'menu.txt'), menuText);
  check(/Skip intro/.test(menuText) && /Resolution/.test(menuText) && /Online play/.test(menuText) && !/Field of view/.test(menuText) && !/Control style/.test(menuText),
    'host menu: Resolution, Skip intro, Online play shown; inGame options (Field of view, Control style) hidden');
  await page.click('[data-action=close-menu]', { timeout: 3000 }).catch(() => {});
  await sleep(15000);   // into the intro: the overlay works on any screen
  await page.locator('[data-field=screen]').click().catch(() => {});
  await press('Digit4');
  await sleep(500); await shot('overlay-open');
  check(/optionsoverlay: opened/.test(await log()), 'extra button Options opened the options overlay');
  const steps = (process.env.STEPS || 'X,ArrowRight,ArrowRight,C,ArrowDown,X,ArrowRight,ArrowDown,ArrowRight,C,ArrowDown,ArrowDown,X,ArrowRight,ArrowRight').split(',');
  for (const k of steps) { await press(k); }
  await shot('overlay-changed');
  if (await page.$('.play .error')) throw new Error('the game stopped with an error');
  await sleep(500);
  const changed = [...(await log()).matchAll(/rtw: option (\d+) = (-?\d+) changed in the game \(([^)]+)\)/g)].map((m) => ({ id: +m[1], v: +m[2], key: m[3] }));
  say(`changed in the game: ${changed.map((c) => `${c.id}=${c.v} (${c.key})`).join(', ') || 'none'}`);
  const st = await stored();
  say(`host stored: ${JSON.stringify(st)}`);
  const last = {};
  for (const c of changed) last[c.id] = c.v;
  check(changed.length > 0 && Object.entries(last).every(([id, v]) => st[id] === v), 'every change reached the host and is stored');
  for (let i = 0; i < 4 && /optionsoverlay: opened/.test((await log()).split('optionsoverlay: closed').pop()); i++) await press('C');
  check(/optionsoverlay: closed/.test(await log()), 'B closes the overlay');
  await open(true);
  await sleep(3000);
  const back = {};
  for (const id of Object.keys(last)) back[id] = await inWorker((i) => globalThis.__rtwGoldenEye.option(+i), id);
  check(Object.entries(last).every(([id, v]) => back[id] === v), `after a reload the game starts with them: ${JSON.stringify(back)}`);
  writeFileSync(join(out, 'host_log.txt'), await log());
} catch (e) {
  say('ERROR ' + (e && e.stack || e)); fails++;
  writeFileSync(join(out, 'page_text.txt'), await page.evaluate(() => { document.querySelectorAll('details').forEach((d) => { d.open = true; }); return document.body.innerText; }).catch(() => ''));
  writeFileSync(join(out, 'host_log.txt'), await log());
}
if (errors.length) say('page errors: ' + errors.slice(0, 3).join(' | '));
await browser.close().catch(() => {});
await mock.close();
await new Promise((ok) => { served.closeAllConnections?.(); served.close(ok); });
say(fails ? `${fails} FAIL` : 'ALL PASS');
process.exit(fails ? 1 : 0);
