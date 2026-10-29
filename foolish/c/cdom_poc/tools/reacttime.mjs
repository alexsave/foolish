// The textures' timing against the REAL React app: its production build (the static export
// `next build` writes to foolish/out/), signed in to a local c_base and showing a game against a
// bot, timed load by load against the port's page and the harness page (out/ts_tex.html, which
// tools/textime.mjs builds: the app's own texture functions and nothing else), in one Chrome,
// paired and interleaved, so a busy machine weighs on every arm alike.
//   node tools/reacttime.mjs [rounds] [cpu-throttle]
// Before it: in foolish/, `npm ci` once, then
//   NEXT_PUBLIC_BACKEND_URL=http://127.0.0.1:8731 npm run build
// (the backend's URL is read at build time; CBASE_PORT=<port> here if the build used another one),
// `make -j4` in c_base/, ./build.sh here, and node tools/textime.mjs once (it writes out/ts_tex.html;
// without it the harness arm is left out).
// It starts c_base/build/cbase serve on that port with throwaway secrets and a throwaway SQLite
// file (CBASE_DEV=1, as tests/live_test.mjs; never production), serves the export on 8733 with the
// rules of the site's CloudFront router (c_base/providers/aws/ops/site/web.yaml: / is index.html,
// a known page gets .html, any other one-segment path is the shell _shell.html), and serves
// foolish/ on 8720 for the port as textime.mjs does. Unthrottled, it then signs a fresh user up on
// the app's own welcome page, creates a game on the dashboard, adds a bot and presses Ready in the
// lobby, and waits for the hand: the table's route is /<game id>, and every load after that opens it.
// Each round: both origins' IndexedDB emptied, then a cold load of each arm (the order rotates
// round by round), then a warm load of each in the same order. Every load starts from about:blank,
// a new document, so the app's module-level texture variables start empty too.
// What is timed, ms after navigation, by one observer (a MutationObserver installed before any
// script of the page runs), the same for every page:
//   React: .bg-wool's background-image a blob url (the wool); an element with a blob
//   background-image sized 100% 100% (a card back or a mini card: the fern); any other element
//   with a blob background-image (a TexturedSurface or a lobby button: the wood);
//   the port: #root's --tex-wool, --tex-fern and --tex-wood set (what its styles read);
//   both: the first hand card in the DOM ([data-location="hand"][data-card]), and the first
//   contentful paint; then all three pictures decoded. The harness page reports its own times.
// The port's fourth picture (the end screen's wood, made last) is not on the table and not in
// "all decoded"; its time set is printed beside, from the port's own record (app_tex_at(3)).
// CPU throttling slows a page's main thread: the React app's wool weaves on a worker, which it
// leaves alone (cold, its wool takes 2 times as long at x4, not 4).
// The store is checked, not assumed: as a React page starts (indexedDB.databases(), asked by the
// observer before the page's scripts run) a cold load finds no TextureCacheDB and a warm one finds it,
// and after every React load it keeps fern_v1, wood_v2 and wool_v1; a port load finds its
// texture keys none kept (cold) or all kept (warm).
// PROBE=1 stops after the setup with screenshots in out/reacttime_*.png.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, execSync } from 'node:child_process';
import { launch, serve, FOOLISH, POC, sleep } from './cdp.mjs';

const ROUNDS = Number(process.argv[2] || 9), CPU = Number(process.argv[3] || 1);
const CBASE_PORT = Number(process.env.CBASE_PORT || 8731), REACT_PORT = 8733;
const EXPORT = path.join(FOOLISH, 'out');
const cbase = path.resolve(FOOLISH, '../c_base/build/cbase');
const out = path.join(POC, 'out');
if (!fs.existsSync(path.join(EXPORT, '_shell.html'))) { console.log(`no ${EXPORT}/_shell.html: run the app's build first (see the header)`); process.exit(2); }
if (!fs.existsSync(cbase)) { console.log(`no ${cbase}: run make -j4 in c_base/ first`); process.exit(2); }
// The build must point at the c_base this starts.
const baked = execSync(`grep -rl 'http://127.0.0.1:${CBASE_PORT}' ${path.join(EXPORT, '_next/static/chunks')} || true`).toString().trim();
if (!baked) { console.log(`the export does not name http://127.0.0.1:${CBASE_PORT}: rebuild it with NEXT_PUBLIC_BACKEND_URL set to it`); process.exit(2); }
const HARNESS = fs.existsSync(path.join(out, 'ts_tex.html'));

const load = () => execSync('uptime').toString().trim().replace(/.*load averages?: /, '');
// the busiest processes over one second (top's first sample has no CPU figures)
const top = () => execSync('top -l 2 -s 1 -o cpu -n 6 -stats command,cpu').toString().trim().split('\n').slice(-6).map((l) => l.trim().replace(/\s+/g, ' ')).join('; ');

// ---- c_base, as tests/live_test.mjs starts it, on the port the build names
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cdom-reacttime-'));
const server = spawn(cbase, ['serve'], {
  env: { ...process.env, CBASE_DEV: '1', CBASE_LISTEN: `127.0.0.1:${CBASE_PORT}`, CBASE_STORE: 'sqlite', CBASE_DB: path.join(dir, 'cbase.sqlite'),
    CBASE_JWT_SECRET: crypto.randomBytes(32).toString('hex'), CBASE_ADMIN_TOKEN: crypto.randomBytes(24).toString('hex'), CBASE_ALLOWED_ORIGINS: '*' },
  stdio: ['ignore', 'pipe', 'pipe'],
});
let serverLog = '';
const up = await new Promise((resolve) => {
  const read = (d) => { serverLog += d; if (/listening on 127\.0\.0\.1:\d+/.test(serverLog)) resolve(true); };
  server.stdout.on('data', read);
  server.stderr.on('data', read);
  server.on('exit', () => resolve(false));
  setTimeout(() => resolve(false), 10000);
});
const stopServer = async () => {
  server.kill('SIGTERM');
  for (let n = 0; n < 30 && server.exitCode === null && server.signalCode === null; n++) await sleep(100);
  if (server.exitCode === null && server.signalCode === null) server.kill('SIGKILL');
  fs.rmSync(dir, { recursive: true, force: true });
};
if (!up) { console.log('c_base did not start:\n' + serverLog.slice(-1500)); await stopServer(); process.exit(2); }

// ---- the export, routed as the site's CloudFront function routes it
const pages = new Set(fs.readdirSync(EXPORT).filter((f) => f.endsWith('.html')).map((f) => f.slice(0, -5)));
const route = (req) => {
  const u = new URL(req.url, 'http://x');
  let p = u.pathname.replace(/\/+$/, '') || '/';
  if (p === '/') p = '/index.html';
  else if (p.slice(p.lastIndexOf('/')).includes('.')) { /* a file */ }
  else if (pages.has(p.slice(1))) p += '.html';
  else if (p.indexOf('/', 1) < 0) p = '/_shell.html';
  else p = '/404.html';
  req.url = p + u.search;
  return false;
};
const reactServer = await serve(REACT_PORT, EXPORT, route);
const portServer = await serve(8720, FOOLISH, null, { 'cross-origin-opener-policy': 'same-origin', 'cross-origin-embedder-policy': 'require-corp' });
const REACT = `http://127.0.0.1:${REACT_PORT}`, PORT_ORIGIN = 'http://127.0.0.1:8720';
const PORT = PORT_ORIGIN + '/c/cdom_poc/out/index.html', TSP = PORT_ORIGIN + '/c/cdom_poc/out/ts_tex.html';

// ---- the observer, in every document before its scripts
const OBSERVER = `(() => {
  const t = window.__tex = {};
  // the origin's databases as the document starts, before the page's own scripts open any
  indexedDB.databases().then((list) => { t.databases = list.map((db) => db.name).join(' '); });
  const mark = (k) => { if (t[k] === undefined) t[k] = performance.now(); };
  const blob = (el) => el.style && el.style.backgroundImage.includes('blob:');
  const look = (el) => {
    if (el.nodeType !== 1) return;
    if (el.id === 'root') {
      if (el.style.getPropertyValue('--tex-wool')) mark('wool');
      if (el.style.getPropertyValue('--tex-fern')) mark('fern');
      if (el.style.getPropertyValue('--tex-wood')) mark('wood');
    }
    if (blob(el)) {
      if (el.classList.contains('bg-wool')) mark('wool');
      else if (el.style.backgroundSize === '100% 100%') mark('fern');
      else mark('wood');
    }
  };
  const scan = (el) => {
    if (el.nodeType !== 1) return;
    look(el);
    for (const e of el.querySelectorAll('[style]')) look(e);
    if (t.board === undefined && (el.matches('[data-location="hand"][data-card]') || el.querySelector('[data-location="hand"][data-card]'))) mark('board');
  };
  new MutationObserver((records) => {
    for (const r of records) {
      if (r.type === 'attributes') look(r.target);
      else for (const n of r.addedNodes) scan(n);
    }
  }).observe(document, { subtree: true, childList: true, attributes: true, attributeFilter: ['style'] });
})();`;

// FLAGS=... more Chrome switches (PROBE=1 FLAGS=--disable-gpu-rasterization: the shots as tools/shots.mjs draws them)
const b = await launch({ flags: process.env.FLAGS ? process.env.FLAGS.split(' ') : [] });
await b.send('Page.addScriptToEvaluateOnNewDocument', { source: OBSERVER });
const arms = {};
for (const k of ['port_cold', 'react_cold', 'ts_cold', 'port_warm', 'react_warm', 'ts_warm']) arms[k] = [];
const blank = async () => { await b.goto('about:blank'); await sleep(300); };
const clear = async (origin) => b.send('Storage.clearDataForOrigin', { origin, storageTypes: 'indexeddb' });
const typeInto = async (sel, text) => { await b.click(sel); await b.send('Input.insertText', { text }); };
const shot = (name) => b.shot(path.join(out, `reacttime_${name}.png`));

// The app's sign-up and create move between pages; an ask in flight while a page goes is asked again.
const until = async (expr, ms) => {
  for (let n = 0; n < ms / 25; n++) {
    try { if (await b.evaluate(expr)) return; } catch (e) { if (!/navigated|context/i.test(e.message)) throw e; }
    await sleep(25);
  }
  throw new Error(`timed out waiting for ${expr}`);
};

/** A fresh user, a game against a bot, started: the table's url. */
async function setup() {
  await b.goto(REACT + '/');
  await until(`!!document.querySelector('#username')`, 30000);
  const name = 'RT' + crypto.randomBytes(4).toString('hex').toUpperCase(), password = crypto.randomBytes(12).toString('hex');
  await typeInto('#username', name);
  await typeInto('#password', password);
  await b.click('form .form-row--actions > :nth-child(2)'); // Sign up
  // the dashboard's one full-width wood button: CreateGameButton
  await until(`location.pathname === '/dashboard' && !!document.querySelector('button.btn-wood--full')`, 30000);
  if (process.env.PROBE) await shot('dashboard');
  await b.click('button.btn-wood--full');
  await until(`location.pathname.length > 2 && location.pathname !== '/dashboard' && !!document.querySelector('.btn-add-bot')`, 30000);
  const url = REACT + await b.evaluate('location.pathname');
  // a press before the bot roster has loaded is parked by the lobby until it has (Lobby.tsx)
  await b.click('.btn-add-bot');
  await until(`document.querySelectorAll('.lobby__player-wrapper').length === 2`, 30000);
  await sleep(1000); // the add is shown optimistically: let the server's answer land first
  if (process.env.PROBE) await shot('lobby');
  await b.click('.btn-ready');
  await until(`!!document.querySelector('[data-location="hand"][data-card]')`, 60000);
  if (process.env.PROBE) await shot('table');
  return { url, name };
}

const PORT_STORED = `window.app.then((x) => [0, 1, 2, 3].map((k) => x.app_tex_stored(k)).join(''))`;
const REACT_STORED = `new Promise((resolve) => { const r = indexedDB.open('TextureCacheDB', 1);
  r.onupgradeneeded = () => r.result.createObjectStore('textures', { keyPath: 'id' });
  r.onsuccess = () => { const db = r.result; const q = db.transaction('textures').objectStore('textures').getAllKeys();
    q.onsuccess = () => { db.close(); resolve(q.result.filter((k) => ['fern_v1', 'wood_v2', 'wool_v1'].includes(k)).sort().join(' ')); };
    q.onerror = () => { db.close(); resolve('error'); }; };
  r.onerror = () => resolve('error'); })`;
// The three pictures' urls on the page, decoded; then the observer's times.
const DECODED = (urls) => `(async () => { const u = ${urls};
  await Promise.all(u.map((s) => { const i = new Image(); i.src = s; return i.decode(); }));
  // bytes: what the page fetched (served uncompressed and uncached, as cdp.mjs serves everything)
  const fetched = [...performance.getEntriesByType('navigation'), ...performance.getEntriesByType('resource')];
  return { ...window.__tex, decoded: performance.now(), fcp: performance.getEntriesByName('first-contentful-paint')[0]?.startTime,
    bytes: fetched.reduce((sum, e) => sum + (e.encodedBodySize || 0), 0), requests: fetched.length }; })()`;
const URL_OF = (sel) => `(() => { const e = ${sel}; return e ? e.style.backgroundImage.replace(/^url\\("?|"?\\)$/g, '') : ''; })()`;
const REACT_URLS = `[${URL_OF(`document.querySelector('.bg-wool')`)}, ${URL_OF(`[...document.querySelectorAll('[style]')].find((e) => e.style.backgroundImage.includes('blob:') && e.style.backgroundSize === '100% 100%')`)},
  ${URL_OF(`[...document.querySelectorAll('[style]')].find((e) => e.style.backgroundImage.includes('blob:') && e.style.backgroundSize !== '100% 100%' && !e.classList.contains('bg-wool'))`)}]`;
const PORT_URLS = `['--tex-fern', '--tex-wood', '--tex-wool'].map((v) => document.getElementById('root').style.getPropertyValue(v).slice(4, -1))`;

async function react(arm, url) {
  await blank();
  await b.goto(url);
  await b.until('window.__tex.databases !== undefined', 5000);
  const atStart = await b.evaluate('window.__tex.databases');
  if (atStart.includes('TextureCacheDB') !== (arm === 'react_warm')) throw new Error(`${arm}: the databases at the start: "${atStart}"`);
  await b.until(`window.__tex.wool !== undefined && window.__tex.fern !== undefined && window.__tex.wood !== undefined && window.__tex.board !== undefined`, 300000);
  const r = await b.evaluate(DECODED(REACT_URLS));
  await sleep(1000); // setCachedTexture is fire-and-forget
  r.stored = await b.evaluate(REACT_STORED);
  const want = 'fern_v1 wood_v2 wool_v1';
  if (r.stored !== want) throw new Error(`${arm}: TextureCacheDB holds "${r.stored}" after the load`);
  arms[arm].push(r);
}
async function port(arm) {
  await blank();
  await b.goto(PORT);
  await b.until('window.appReady && window.app.then((x) => x.app_game_store(1) > 0)', 30000);
  // the port's fourth picture (the end screen's wood) is made last; this waits for the three on the table
  await b.until(`window.__tex.wool !== undefined && window.__tex.fern !== undefined && window.__tex.wood !== undefined && window.__tex.board !== undefined`, 300000);
  const r = await b.evaluate(DECODED(PORT_URLS));
  r.stored = await b.evaluate(PORT_STORED);
  const want = arm === 'port_warm' ? '1111' : '0000';
  if (r.stored !== want) throw new Error(`${arm}: stored ${r.stored}`);
  await b.until(`window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`, 300000);
  r.woodEnd = await b.evaluate('window.app.then((x) => x.app_tex_at(3))'); // the end screen's wood, set (as textime.mjs reads it)
  await sleep(500); // the store's puts are fire-and-forget
  arms[arm].push(r);
}
async function harness(arm) {
  await blank();
  await b.goto(TSP);
  arms[arm].push(await b.evaluate('window.texAll'));
  await sleep(500);
}

const notes = [];
let table;
try {
  table = await setup();
  console.log(`user ${table.name}, the table at ${table.url}`);
  if (process.env.PROBE) {
    await clear(REACT);
    await react('react_cold', table.url);
    await sleep(1000);
    await shot('react_cold');
    await react('react_warm', table.url);
    await port('port_cold');
    await sleep(1000);
    await shot('port_cold');
    console.log(JSON.stringify(arms, null, 1));
  } else {
    notes.push({ when: 'before', load: load(), top: top() });
    if (CPU > 1) await b.send('Emulation.setCPUThrottlingRate', { rate: CPU });
    const order = ['port', 'react', ...(HARNESS ? ['ts'] : [])];
    const run = { port: (w) => port('port_' + w), react: (w) => react('react_' + w, table.url), ts: (w) => harness('ts_' + w) };
    for (let i = 0; i < ROUNDS; i++) {
      const o = order.map((_, k) => order[(i + k) % order.length]);
      await clear(REACT);
      await clear(PORT_ORIGIN);
      for (const a of o) await run[a]('cold');
      for (const a of o) await run[a]('warm');
    }
    notes.push({ when: 'after', load: load(), top: top() });
  }
} catch (e) { console.error(e); process.exitCode = 1; if (process.env.PROBE) await shot('error').catch(() => {}); }
finally { await b.close(); reactServer.close(); portServer.close(); await stopServer(); }
if (process.env.PROBE || process.exitCode) process.exit();

const q = (a, p) => { const s = a.filter((v) => typeof v === 'number').sort((x, y) => x - y); return s.length ? s[Math.min(s.length - 1, Math.floor(p * (s.length - 1) + 0.5))] : NaN; };
const cell = (a) => `${q(a, 0.5).toFixed(0)} (${q(a, 0.25).toFixed(0)}-${q(a, 0.75).toFixed(0)})`;
console.log(`textures against the real React app, ${ROUNDS} rounds, CPU x${CPU}: median (25th-75th) ms after navigation`);
for (const n of notes) console.log(`load ${n.when}: ${n.load} | ${n.top}`);
for (const arm of Object.keys(arms)) {
  if (!arms[arm].length) continue;
  const col = (k) => cell(arms[arm].map((r) => r[k])).padEnd(15);
  console.log(`${arm.padEnd(10)} fcp ${col('fcp')} board ${col('board')} fern ${col('fern')} wood ${col('wood')} wool ${col('wool')} all decoded ${col('decoded')}`
    + (arm.startsWith('port') ? ` end-screen wood set ${col('woodEnd')}` : '')
    + (arm.startsWith('ts') ? '' : ` fetched ${(q(arms[arm].map((r) => r.bytes), 0.5) / 1024).toFixed(0)} KiB in ${q(arms[arm].map((r) => r.requests), 0.5)} requests`));
}
const med = (arm, k) => q(arms[arm].map((r) => r[k]), 0.5);
for (const w of ['cold', 'warm'])
  console.log(`port / React, ${w}: ${['fern', 'wood', 'wool', 'decoded', 'board', 'fcp'].map((k) => `${k} ${(med('port_' + w, k) / med('react_' + w, k)).toFixed(2)}`).join(', ')}`
    + (HARNESS ? `; harness / React: all decoded ${(med('ts_' + w, 'decoded') / med('react_' + w, 'decoded')).toFixed(2)}` : ''));
