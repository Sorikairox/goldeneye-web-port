// Online play of the retrowser.app build (standard 1.2, docs/web/RTW.md "Online play") end to end: 2-4 pages (one
// browser context each) in ONE muted headless Chrome join one room on the retrowser host's local room server and play
// a lockstep session. Adapted from the Conker port's tools/port/scripts/rtw_net_e2e.mjs.
//   node scripts/rtw/rtw_net_e2e.mjs <build dir> <out dir> [rom]
// <build dir> holds a manifest at its root (build/rtw, or a repo-layout copy: root retrowser.json + retrowser/); the
// retrowser host's GitHub mock serves it as rtw-test/goldeneye-eu. The host is retrowser's built dist served in this
// process (tools/serve-worker.ts: the Worker handler + its room server in memory, like its e2e/net.test.ts). Host
// candidates only (no STUN: localStorage rtw:test-net {"ice":[]}).
// Env: PLAYERS (2..4, default 2), FRAMES (retraces of play with inputs before the checks, default 600), PORT (default
// 8796), RTW_REPO (retrowser checkout), RTW_HOST_DIST (the host's built dist, default <RTW_REPO>/dist), CHROME_PATH,
// DROP=1 (close the last player's page: the others agree on its neutral frame and go on, hashes compared after the drop
// too), DESYNC=1 (at the end: flip one byte of N64 DRAM on player 2 -> both report it), LEAVE=1 (then player 1 leaves
// the session: its game restarts alone), SOLO=1 (afterwards: a fresh page plays alone), RTW_OPTIONS_HOST (stored
// options of player 1, default {"2":1}: skip intro) and RTW_OPTIONS_GUEST (of the others, default {"2":0}: the
// session must use player 1's), RTW_OWN_HOST / RTW_OWN_GUEST (the players' own settings, not in the manifest, set in
// their game's settings record before the session, e.g. {"7":1,"13":100}; checked: each peer keeps its own in the
// session), LEVEL=1 (the room host walks the menus into the first mission before the inputs: display lists, HUD and
// crosshair in play), OVERLAY=1 (player 2 opens the options overlay in the session: the room host's rows are locked,
// an own one changes; hashes compared after), GUEST_LEAVE=1 (then player 2 leaves: its own values of the room host's
// options are back, its stored host options untouched).
// Prints PASS / FAIL lines and "ALL PASS" at the end; exit code 0 only then. Logs (no pictures) go to <out dir>.
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [buildDir, out, romArg] = process.argv.slice(2);
if (!buildDir || !out) { console.error('usage: rtw_net_e2e.mjs <build dir> <out dir> [rom]'); process.exit(2); }
const RTW = process.env.RTW_REPO || join(process.env.HOME, 'Code/retrowser');
const ROM = romArg || join(process.env.HOME, 'roms/GoldenEye_007_Europe.z64');
const { chromium } = createRequire(join(RTW, 'package.json'))('playwright-core');
const { startMockGithub } = await import(pathToFileURL(join(RTW, 'tools/mock-github.ts')).href);
const { serveWorker } = await import(pathToFileURL(join(RTW, 'tools/serve-worker.ts')).href);
const REPO = 'rtw-test/goldeneye-eu', COMMIT = 'eeee007eeee007eeee007eeee007eeee00712120';
const PLAYERS = Math.max(2, Math.min(4, Number(process.env.PLAYERS || 2)));
const FRAMES = Number(process.env.FRAMES || 600), PORT = Number(process.env.PORT || 8796);
mkdirSync(out, { recursive: true });
const t0 = Date.now(), say = (s) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${s}`);
let fails = 0;
const check = (ok, what) => { say(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) fails++; return ok; };

const mock = await startMockGithub({ [REPO]: { refs: { main: COMMIT }, commits: [{ sha: COMMIT, dir: resolve(buildDir) }] } });
const served = await serveWorker(PORT, process.env.RTW_HOST_DIST ? resolve(process.env.RTW_HOST_DIST) : undefined);
const site = `http://localhost:${PORT}`;
const chrome = process.env.CHROME_PATH || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const browser = await chromium.launch({ executablePath: chrome, headless: true,
  args: ['--mute-audio', '--use-angle=metal', '--autoplay-policy=no-user-gesture-required', '--disable-features=WebRtcHideLocalIpsWithMdns'] });
const players = [];
const finish = async (code) => {
  for (const p of players) {
    try { writeFileSync(join(out, `host_log_p${p.n}.txt`), (await hostLog(p).catch(() => '')) + '\n'); } catch { /* closed */ }
    if (p.errors.length) say(`P${p.n} page errors: ${p.errors.slice(0, 3).join(' | ')}`);
  }
  await browser.close().catch(() => {});
  await mock.close();
  await new Promise((ok) => { served.closeAllConnections?.(); served.close(ok); });
  process.exit(code);
};
const timer = setTimeout(() => { say('TIMEOUT'); fails++; finish(3); }, Number(process.env.RTW_TIMEOUT || 540000));

async function newPlayer(n, options) {
  const ctx = await browser.newContext({ viewport: { width: 800, height: 600 } });
  await ctx.addInitScript(([ep, opts]) => {
    try {
      if (location.hostname !== 'localhost') return;
      localStorage.setItem('rtw:test-endpoints', ep);
      localStorage.setItem('rtw:test-net', '{"ice":[]}');
      if (opts) localStorage.setItem('rtw:options:goldeneye-eu', opts);
    } catch { /* ignore */ }
  }, [JSON.stringify(mock.endpoints), options]);
  const page = await ctx.newPage();
  const p = { n, ctx, page, errors: [] };
  page.on('pageerror', (e) => p.errors.push(e.message));
  players.push(p);
  return p;
}
const hostLog = (p) => p.page.getAttribute('[data-state=playing]', 'data-log', { timeout: 2000 }).then((v) => v || '');
const hostFrames = (p) => p.page.getAttribute('[data-state=playing]', 'data-frames', { timeout: 2000 }).then((v) => Number(v || 0));
async function worker(p) {   // the game's worker (the sandbox's dedicated worker with __rtwGoldenEye); a new one after a restart
  for (const w of p.page.workers()) {
    try { if (await w.evaluate(() => typeof globalThis.__rtwGoldenEye === 'object')) return w; } catch { /* gone */ }
  }
  return null;
}
async function inWorker(p, fn, arg) { const w = await worker(p); return w ? w.evaluate(fn, arg).catch(() => null) : null; }
const net = (p) => inWorker(p, () => globalThis.__rtwGoldenEye.net());
const frameOf = (p) => inWorker(p, () => globalThis.__rtwGoldenEye.frame());
async function open(p) {
  await p.page.goto(`${site}/g/${REPO}`);
  await p.page.click('[data-action=download]', { timeout: 60000 });
  await p.page.locator('[data-field=rom]').setInputFiles(ROM, { timeout: 180000 });
  await p.page.waitForFunction(() => Number(document.querySelector('[data-state=playing]')?.getAttribute('data-frames') ?? 0) > 10, null, { timeout: 180000 });
  await p.page.click('[data-state=persist-warning] button', { timeout: 1000 }).catch(() => {});
}
async function until(what, fn, ms = 60000, every = 250) {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn().catch(() => null);
    if (v) return v;
    if (Date.now() > end) throw new Error(`timeout: ${what}`);
    await new Promise((r) => setTimeout(r, every));
  }
}
// hash agreement over the given players: for every retrace in all their hash rings, are {H, S} equal?
async function compareHashes(list, label) {
  const sts = await Promise.all(list.map(net));
  const rings = sts.map((s) => new Map((s ? s.hashes : []).map((h) => [h.f, h])));
  let same = 0, differ = 0, first = 0, last = 0;
  for (const [f, h] of rings[0]) {
    if (!rings.every((r) => r.has(f))) continue;
    if (rings.every((r) => r.get(f).h === h.h && r.get(f).s === h.s)) { same++; first = first || f; last = Math.max(last, f); } else differ++;
  }
  check(same >= 10 && differ === 0, `${label}: ${same} common hash points equal (retraces ${first}..${last}), ${differ} differ; ` +
    sts.map((s, i) => `P${list[i].n} matched ${s ? s.hashesMatched : '?'} to ${s ? s.hashChecked : '?'}, desync ${s && s.desync ? s.desync.frame : 'none'}`).join(', '));
  return { same, differ };
}
// the game's current_menu (bondconstants.h MENU: 5 file select .. 10 briefing, 11 run stage), from this build's netregions
const NETREG = [join(buildDir, 'ge007.netregions'), join(buildDir, 'retrowser/ge007.netregions')].find((f) => existsSync(f));
const MENU_ADDR = NETREG ? parseInt((readFileSync(NETREG, 'utf8').match(/^S ([0-9a-f]+) 4 current_menu$/m) || [])[1] || '0', 16) : 0;
const menuOf = (p) => inWorker(p, (a) => { const h = globalThis.__rtwGoldenEye.Module.HEAPU8; return new DataView(h.buffer).getInt32(a, true); }, MENU_ADDR);
const optsNow = (p, ids) => inWorker(p, (l) => Object.fromEntries(l.map((i) => [i, globalThis.__rtwGoldenEye.optionNow(i)])), ids);
const press1 = async (p, key, ms = 120) => { await p.page.keyboard.down(key); await p.page.waitForTimeout(ms); await p.page.keyboard.up(key); };
async function toLevel(A) {   // the room host (controller 1) presses A (Start on the briefing) until the mission runs
  const seen = [];
  for (let t = Date.now(); Date.now() - t < 150000;) {
    const m = await menuOf(A);
    if (seen[seen.length - 1] !== m) { seen.push(m); say(`menu ${m}`); }
    if (m === 11) return seen;
    await press1(A, m === 10 ? 'Enter' : 'KeyX');
    await new Promise((r) => setTimeout(r, 900));
  }
  throw new Error(`no mission after 150 s (menus ${seen.join(' ')})`);
}
const KEYS = ['Enter', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'KeyX', 'KeyC', 'KeyZ', 'KeyA'];
async function playInputs(list, target) {   // random presses on every page until each ran `target` retraces
  let i = 0, best = -1, bestAt = Date.now();
  for (;;) {
    const fs = await Promise.all(list.map(frameOf));
    if (fs.every((f) => f >= target)) return fs;
    const low = Math.min(...fs);
    if (low > best) { best = low; bestAt = Date.now(); }
    else if (Date.now() - bestAt > 30000) {   // stalled: keep what explains it
      const st = await Promise.all(list.map(net));
      writeFileSync(join(out, 'stall.json'), JSON.stringify(st, null, 1));
      for (const p of list) writeFileSync(join(out, `stall_log_p${p.n}.txt`), (await hostLog(p).catch(() => '')) + '\n');
      throw new Error(`stalled at retraces ${fs.join(' ')} for 30 s (stall.json)`);
    }
    if (++i % 20 === 0) say(`retraces ${fs.join(' ')}`);
    await Promise.all(list.map(async (p, k) => {
      const key = KEYS[(i * 7 + k * 3 + Math.floor(Math.random() * KEYS.length)) % KEYS.length];
      await p.page.keyboard.down(key); await p.page.waitForTimeout(60 + 40 * k); await p.page.keyboard.up(key);
    }));
    await new Promise((r) => setTimeout(r, 150));
  }
}

try {
  const hostOpts = process.env.RTW_OPTIONS_HOST || '{"2":1}', guestOpts = process.env.RTW_OPTIONS_GUEST || '{"2":0}';
  for (let n = 1; n <= PLAYERS; n++) await newPlayer(n, n === 1 ? hostOpts : guestOpts);
  for (const p of players) { await open(p); say(`P${p.n} plays alone`); }
  const ownHost = JSON.parse(process.env.RTW_OWN_HOST || '{}'), ownGuest = JSON.parse(process.env.RTW_OWN_GUEST || '{}');
  const hostDict = Object.keys(JSON.parse(hostOpts)).map(Number).filter((i) => i !== 2);   // room-host options under test
  for (const p of players) {
    const own = p.n === 1 ? ownHost : ownGuest;
    for (const [id, v] of Object.entries(own)) await inWorker(p, ([i, x]) => globalThis.__rtwGoldenEye.setOwn(i, x), [+id, v]);
  }
  const ownIds = [...new Set([...Object.keys(ownHost), ...Object.keys(ownGuest)].map(Number))];
  const alone = await Promise.all(players.map((p) => optsNow(p, [...hostDict, ...ownIds])));
  say('alone: ' + alone.map((o, i) => `P${i + 1} ${JSON.stringify(o)}`).join(' '));
  check((await Promise.all(players.map(net))).every((s) => s && !s.active), 'before the session nobody is in one');
  const A = players[0];
  await A.page.click('[data-action=online]');
  await A.page.click('[data-action=net-create]');
  await A.page.waitForSelector('[data-field=room-code]');
  const code = (await A.page.textContent('[data-field=room-code]')).trim();
  say(`room ${code}`);
  for (const p of players.slice(1)) {
    await p.page.click('[data-action=online]');
    await p.page.fill('[data-field=net-code]', code);
    await p.page.click('[data-action=net-join]');
  }
  await until('every guest connected and matched', async () => (await A.page.$$('[data-field=net-players] li[data-match=ok][data-net-state="1"]')).length >= PLAYERS - 1, 60000);
  await A.page.waitForSelector('[data-action=net-start]:not([disabled])');
  await A.page.waitForTimeout(2500);   // ping reports for the boot's shared table
  await A.page.click('[data-action=net-start]');
  say('start pressed');
  const sts = await until('every player in the session', async () => {
    const s = await Promise.all(players.map(net));
    return s.every((x) => x && x.active && x.frame > 5) ? s : null;
  }, 180000);
  check(sts.every((s, i) => s.me === i && s.players === PLAYERS), `session indexes ${sts.map((s) => `${s.me}/${s.players}`).join(' ')}`);
  check(new Set(sts.map((s) => s.delay)).size === 1, `the same input delay everywhere: ${sts.map((s) => `${s.delay}${s.autoDelay ? ' auto (worst p90 ' + s.worst + ' ms)' : ''}`).join(', ')}`);
  check(sts.every((s) => s.regions > 0), `hashed ranges from ge007.netregions: ${sts.map((s) => s.regions).join(' ')}`);
  const opts = await Promise.all(players.map((p) => inWorker(p, () => globalThis.__rtwGoldenEye.option(2))));
  check(opts.every((o) => o === 1), `every player boots with the room host's options (skip intro: ${opts.join(' ')})`);
  check(sts.every((s) => s.beganAt === 0), `the session is set before the first retrace (lockstep boot): ${sts.map((s) => s.beganAt).join(' ')}`);
  if (hostDict.length || ownIds.length) {
    const inS = await Promise.all(players.map((p) => optsNow(p, [...hostDict, ...ownIds])));
    say('in the session: ' + inS.map((o, i) => `P${i + 1} ${JSON.stringify(o)}`).join(' '));
    const hv = JSON.parse(hostOpts);
    check(inS.every((o) => o && hostDict.every((i) => o[i] === hv[i])), `every peer uses the room host's view options ${JSON.stringify(hostDict)}`);
    check(inS.every((o, k) => o && ownIds.every((i) => o[i] === alone[k][i])), `every peer keeps its own settings ${JSON.stringify(ownIds)}`);
  }
  let base = 0;
  if (process.env.LEVEL === '1') {
    if (!MENU_ADDR) throw new Error('current_menu not found in ge007.netregions');
    const seen = await toLevel(A);
    base = Math.max(...(await Promise.all(players.map(frameOf))));
    check(true, `the room host walked into the mission (menus ${seen.join(' ')}) at retrace ${base}`);
    const ms = await Promise.all(players.map(menuOf));
    check(ms.every((m) => m === 11), `every peer runs the mission: ${ms.join(' ')}`);
    {   // where the display lists are (left out of the hash): N64 DRAM offsets
      const sym = (n) => parseInt((readFileSync(NETREG, 'utf8').match(new RegExp(`^S ([0-9a-f]+) [0-9a-f]+ ${n}$`, 'm')) || [])[1] || '0', 16);
      const pools = await inWorker(A, ([g, v]) => { const d = new DataView(globalThis.__rtwGoldenEye.Module.HEAPU8.buffer); const r = (a) => [0, 8, 16].map((o) => (d.getUint32(a + o, true) - 0x12000000).toString(16)); return { gfx: r(g), vtx: r(v) }; }, [sym('g_GfxBuffers'), sym('g_VtxBuffers')]);
      say(`display-list pools (DRAM offsets): ${JSON.stringify(pools)}`);
    }
    // the room host aims (R: the crosshair is drawn) and fires for a while; pictures of both screens (local only)
    await A.page.waitForTimeout(3000);
    await A.page.keyboard.down('KeyS');
    for (let i = 0; i < 12; i++) { await press1(A, i % 3 ? 'ArrowLeft' : 'KeyZ', 150); await A.page.waitForTimeout(200); }
    for (const p of players) await p.page.locator('[data-field=screen]').screenshot({ path: join(out, `aim_p${p.n}.png`) }).catch(() => {});
    await A.page.keyboard.up('KeyS');
    base = Math.max(...(await Promise.all(players.map(frameOf))));
  }
  const fs = await playInputs(players, base + FRAMES);
  say(`played to retraces ${fs.join(' ')}`);
  await new Promise((r) => setTimeout(r, 1500));
  await compareHashes(players, `${PLAYERS} players after ${FRAMES} retraces with inputs`);
  // the remote inputs that arrived = what the sender sampled (ring of player 2's channel on player 1 vs on player 2)
  {
    const B = players[1], lo = Math.max(10, Math.min(...fs) - 200), hi = Math.min(...fs) - 10;
    const [ra, rb] = await Promise.all([inWorker(A, ([a, b]) => globalThis.__rtwGoldenEye.ring(1, a, b), [lo, hi]), inWorker(B, ([a, b]) => globalThis.__rtwGoldenEye.ring(1, a, b), [lo, hi])]);
    const both = ra && rb ? ra.filter((v, i) => v !== -1 && v === rb[i]).length : 0, pressed = rb ? rb.filter((v) => v > 0).length : 0;
    check(both > 50 && pressed > 0, `player 2's inputs on player 1 = player 2's own: ${both} of ${hi - lo + 1} ring frames still kept equal, ${pressed} with buttons / stick`);
  }
  const st2 = await Promise.all(players.map(net));
  say('stats: ' + st2.map((s) => `P${s.me + 1} sent ${s.sent} recv ${s.recv} refused ${s.refused} bad ${s.bad} waits ${s.waits} (${s.waitMs} ms) slowed ${s.slowedMs} ms kicks ${s.kicks} errors ${s.errors}`).join('; '));
  check(st2.every((s) => s.bad === 0 && s.refused === 0 && s.errors === 0), 'no bad or refused packets, no glue errors');
  writeFileSync(join(out, 'net_status.json'), JSON.stringify(st2, null, 1));

  if (process.env.OVERLAY === '1') {   // player 2: the overlay is open; try the room host's FOV, change an own setting
    const B = players[1];
    const before = await optsNow(B, [5, 6, 12, 13, 14]);
    await press1(B, 'Digit4'); await B.page.waitForTimeout(800);
    const lg0 = await hostLog(B);
    check(/options overlay: online session/.test(lg0) && /optionsoverlay: opened/.test(lg0), 'player 2 opened the options overlay during the session');
    await B.page.locator('[data-field=screen]').screenshot({ path: join(out, 'overlay_root_p2.png') }).catch(() => {});
    // session root list: Video, Mouse, Controller, Game (Audio / Key bindings hidden); Game page: Field of view (locked),
    // No hit flash (locked), Show crosshair, HUD scale (own), Skip intro, All unlocked (locked), Back
    const nav = (process.env.OVERLAY_KEYS || 'ArrowDown,ArrowDown,ArrowDown,KeyX,ArrowRight,ArrowRight,ArrowDown,KeyX,ArrowDown,ArrowDown,ArrowLeft,ArrowLeft,ArrowLeft').split(',');
    for (const [i, k] of nav.entries()) {
      await press1(B, k); await B.page.waitForTimeout(300);
      if (i === 5) await B.page.locator('[data-field=screen]').screenshot({ path: join(out, 'overlay_fov_p2.png') }).catch(() => {});   // the lock reason
    }
    await B.page.locator('[data-field=screen]').screenshot({ path: join(out, 'overlay_game_p2.png') }).catch(() => {});
    const after = await optsNow(B, [5, 6, 12, 13, 14]);
    const lg = await hostLog(B);
    writeFileSync(join(out, 'overlay_log_p2.txt'), lg + '\n');
    say(`player 2 overlay: before ${JSON.stringify(before)} after ${JSON.stringify(after)}`);
    check(after[5] === before[5] && after[6] === before[6] && after[12] === before[12] && !/is the room host's during online play/.test(lg),
      `the room host's FOV / draw distance / no hit flash stay locked in the overlay (${after[5]} / ${after[6]} / ${after[12]})`);
    const ownChanged = [13, 14].filter((i) => after[i] !== before[i]);
    const rec = await inWorker(B, () => globalThis.__rtwGoldenEye.settings());
    check(ownChanged.length > 0 && ownChanged.every((i) => rec[i] === after[i]), `an own setting changed in the session and went into the record: ${ownChanged.map((i) => `${i}=${after[i]}`).join(' ')} ${JSON.stringify(rec)}`);
    for (let i = 0; i < 4 && (await inWorker(B, () => 1)) && /optionsoverlay: opened/.test((await hostLog(B)).split('optionsoverlay: closed').pop()); i++) await press1(B, 'KeyC');
    const f0 = Math.max(...(await Promise.all(players.map(frameOf))));
    await playInputs(players, f0 + 300);
    await new Promise((r) => setTimeout(r, 1500));
    await compareHashes(players, 'after the overlay was used on player 2');
  }

  if (process.env.GUEST_LEAVE === '1') {   // player 2 leaves: its own values of the room host's options come back
    const B = players[1];
    const storedBefore = await B.page.evaluate(() => localStorage.getItem('rtw:options:goldeneye-eu'));
    await B.page.click('[data-action=online]');
    await B.page.click('[data-action=net-leave]');
    await until('player 2 alone again', async () => { const s = await net(B); return s && !s.active && (await hostFrames(B)) > 100; }, 90000);
    await B.page.waitForTimeout(5000);   // past main(): the options of this start are applied
    const o = await optsNow(B, [...hostDict, ...ownIds]);
    const storedAfter = await B.page.evaluate(() => localStorage.getItem('rtw:options:goldeneye-eu'));
    const rec = await inWorker(B, () => globalThis.__rtwGoldenEye.settings());
    check(hostDict.every((i) => o[i] === alone[1][i]) && ownIds.every((i) => o[i] === (rec[i] ?? alone[1][i])),
      `after leaving, player 2 has its own values again: ${JSON.stringify(o)} (alone before: ${JSON.stringify(alone[1])})`);
    check(storedAfter === storedBefore, `player 2's stored host options untouched by the session: ${storedAfter}`);
    players.splice(1, 1);   // no further comparisons with player 2
  }

  if (process.env.DROP === '1') {
    const gone = players[PLAYERS - 1], rest = players.slice(0, -1);
    say(`closing P${gone.n}'s page (no goodbye)`);
    await gone.page.close();
    const ds = await until('the others agreed on the drop', async () => {
      const s = await Promise.all(rest.map(net));
      return s.every((x) => x && x.drops.some((d) => d.ch === gone.n - 1)) ? s : null;
    }, 90000);
    const nf = ds.map((s) => s.drops.find((d) => d.ch === gone.n - 1).neutralFrom);
    check(new Set(nf).size === 1, `drop of player ${gone.n} agreed: controller ${gone.n} neutral from retrace ${nf.join(' / ')}`);
    const f0 = Math.max(...(await Promise.all(rest.map(frameOf))));
    const fs2 = await playInputs(rest, f0 + 300);
    check(fs2.every((f) => f > f0 + 250), `the others go on after the drop (retraces ${f0} -> ${fs2.join(' ')})`);
    await new Promise((r) => setTimeout(r, 1500));
    if (rest.length > 1) await compareHashes(rest, `the ${rest.length} remaining players after the drop`);
    players.splice(PLAYERS - 1, 1);
    await gone.ctx.close();
  }

  if (process.env.DESYNC === '1' && players.length > 1) {
    const B = players[1];
    await inWorker(B, () => { globalThis.__rtwGoldenEye.Module.HEAPU8[0x12000000 + 0x200000] ^= 1; });   // N64 DRAM + 2 MB, as GE_NETDESYNC
    say('flipped one byte of N64 DRAM on player 2');
    const rs = await until('both report the desync', async () => {
      const s = await Promise.all([net(A), net(B)]);
      return s.every((x) => x && x.desync && x.desyncReport) ? s : null;
    }, 30000);
    check(rs.every((s) => /N64 DRAM 80200000/.test(s.desyncReport.where)), `desync report: ${rs.map((s) => `P${s.me + 1} retrace ${s.desync.frame}: ${s.desyncReport.where}`).join(' | ')}`);
  }

  if (process.env.LEAVE === '1') {   // the room host leaves the session: the host restarts its game alone (own save)
    await A.page.click('[data-action=online]');
    await A.page.click('[data-action=net-leave]');
    await until('player 1 alone again', async () => { const s = await net(A); return s && !s.active && (await hostFrames(A)) > 100; }, 90000);
    check(true, 'player 1 left the session and plays alone (no session)');
  }

  if (process.env.SOLO === '1') {
    const S = await newPlayer(9, '');
    await open(S);
    await new Promise((r) => setTimeout(r, 20000));
    const s = await net(S), log = await hostLog(S), f = await hostFrames(S);
    const pace = (log.match(/pacing: [\d.]+ s: retraces ([\d.]+)\/s/g) || []).pop() || '';
    check(s && !s.active && f > 250 && /retraces (4[5-9]|5\d)\./.test(pace), `single player: ${f} frames, no session, ${pace || 'no pacing line'}`);
    await S.ctx.close(); players.splice(players.indexOf(S), 1);
  }
} catch (e) {
  say('ERROR ' + (e && e.stack || e)); fails++;
  for (const p of players) {   // what each page shows (text only)
    const t = await p.page.evaluate(() => document.body.innerText).catch(() => '');
    writeFileSync(join(out, `page_text_p${p.n}.txt`), t);
  }
}
clearTimeout(timer);
say(fails ? `${fails} FAIL` : 'ALL PASS');
await finish(fails ? 1 : 0);
