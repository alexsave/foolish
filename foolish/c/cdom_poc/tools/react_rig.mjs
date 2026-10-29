// The real React app on this Mac, for timing the port against it (tools/reacttime.mjs, the
// textures; tools/actiontime.mjs, a move's cost): c_base started as tests/live_test.mjs starts it
// (throwaway secrets and SQLite, CBASE_DEV=1; never production) on the port the app's build names,
// the static export `next build` writes to foolish/out/ served on 8733 with the rules of the site's
// CloudFront router (c_base/providers/aws/ops/site/web.yaml: / is index.html, a known page gets
// .html, any other one-segment path is the shell _shell.html), and foolish/ on 8720 for the port.
// Before it: in foolish/, `npm ci` once, then
//   NEXT_PUBLIC_BACKEND_URL=http://127.0.0.1:8731 npm run build
// (the backend's URL is read at build time; CBASE_PORT=<port> when the build used another one),
// and `make -j4` in c_base/.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, execSync } from 'node:child_process';
import { serve, FOOLISH, sleep } from './cdp.mjs';

export const CBASE_PORT = Number(process.env.CBASE_PORT || 8731), REACT_PORT = 8733;
export const REACT = `http://127.0.0.1:${REACT_PORT}`, PORT_ORIGIN = 'http://127.0.0.1:8720';
const EXPORT = path.join(FOOLISH, 'out');
const cbase = path.resolve(FOOLISH, '../c_base/build/cbase');

// The load average, and the busiest processes over one second (top's first sample has no CPU figures).
export const load = () => execSync('uptime').toString().trim().replace(/.*load averages?: /, '');
export const top = () => execSync('top -l 2 -s 1 -o cpu -n 6 -stats command,cpu').toString().trim().split('\n').slice(-6).map((l) => l.trim().replace(/\s+/g, ' ')).join('; ');

/** Starts c_base and both servers; resolves to { stop } or exits 2 naming what is missing.
 *  portHeaders: extra headers for the port's server (cross-origin isolation for exact timers). */
export async function startRig({ portHeaders = {} } = {}) {
  if (!fs.existsSync(path.join(EXPORT, '_shell.html'))) { console.log(`no ${EXPORT}/_shell.html: run the app's build first (tools/react_rig.mjs)`); process.exit(2); }
  if (!fs.existsSync(cbase)) { console.log(`no ${cbase}: run make -j4 in c_base/ first`); process.exit(2); }
  // The build must point at the c_base this starts.
  const baked = execSync(`grep -rl 'http://127.0.0.1:${CBASE_PORT}' ${path.join(EXPORT, '_next/static/chunks')} || true`).toString().trim();
  if (!baked) { console.log(`the export does not name http://127.0.0.1:${CBASE_PORT}: rebuild it with NEXT_PUBLIC_BACKEND_URL set to it`); process.exit(2); }

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cdom-reactrig-'));
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
  const portServer = await serve(8720, FOOLISH, null, portHeaders);
  return { stop: async () => { reactServer.close(); portServer.close(); await stopServer(); } };
}

/** Polls expr through page changes (the app's sign-up and create move between pages; an ask in
 *  flight while a page goes is asked again). */
export async function untilAcross(b, expr, ms) {
  for (let n = 0; n < ms / 25; n++) {
    try { if (await b.evaluate(expr)) return; } catch (e) { if (!/navigated|context/i.test(e.message)) throw e; }
    await sleep(25);
  }
  throw new Error(`timed out waiting for ${expr}`);
}

/** A fresh user signed up on the app's welcome page, a game created on the dashboard, a bot added
 *  and Ready pressed in the lobby, the hand shown: the table's url and the user's name.
 *  shot(name), when given, is called at the dashboard, the lobby and the table. */
export async function reactTable(b, shot = null) {
  const typeInto = async (sel, text) => { await b.click(sel); await b.send('Input.insertText', { text }); };
  await b.goto(REACT + '/');
  await untilAcross(b, `!!document.querySelector('#username')`, 30000);
  const name = 'RT' + crypto.randomBytes(4).toString('hex').toUpperCase(), password = crypto.randomBytes(12).toString('hex');
  await typeInto('#username', name);
  await typeInto('#password', password);
  await b.click('form .form-row--actions > :nth-child(2)'); // Sign up
  // the dashboard's one full-width wood button: CreateGameButton
  await untilAcross(b, `location.pathname === '/dashboard' && !!document.querySelector('button.btn-wood--full')`, 30000);
  if (shot) await shot('dashboard');
  await b.click('button.btn-wood--full');
  await untilAcross(b, `location.pathname.length > 2 && location.pathname !== '/dashboard' && !!document.querySelector('.btn-add-bot')`, 30000);
  const url = REACT + await b.evaluate('location.pathname');
  // a press before the bot roster has loaded is parked by the lobby until it has (Lobby.tsx)
  await b.click('.btn-add-bot');
  await untilAcross(b, `document.querySelectorAll('.lobby__player-wrapper').length === 2`, 30000);
  await sleep(1000); // the add is shown optimistically: let the server's answer land first
  if (shot) await shot('lobby');
  await b.click('.btn-ready');
  await untilAcross(b, `!!document.querySelector('[data-location="hand"][data-card]')`, 60000);
  if (shot) await shot('table');
  return { url, name };
}
