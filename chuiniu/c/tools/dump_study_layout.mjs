// The study's layout, read out of the study itself: the oracle for cn_lay.c and
// cn_cam.c (tests/cn_lay_test.c and tests/cn_cam_test.c pin these numbers).
//
// HOW. chuiniu/docs/UI.html is loaded in a real headless Chromium, driven over
// the DevTools protocol with Node's own WebSocket (no Playwright needed; it is
// not installed on this Mac). Before loading, three lines are spliced into the
// page's script so its closure-private functions can be reached: screen()
// stashes its locals (the board, the camera, the ring, the objects) on
// window.CN_last, and the page exposes screen() on window.CN_expose. Nothing
// else in the page changes; the numbers are the page's own arithmetic.
//
//   node chuiniu/c/tools/dump_study_layout.mjs [out.txt]
//
// The browser: $CHROME, else Playwright's cached chrome-headless-shell, else
// the system Google Chrome. Output: one block per case, plain text, see below.
import fs from 'fs';
import os from 'os';
import path from 'path';
import { spawn } from 'child_process';

const here = path.dirname(new URL(import.meta.url).pathname);
const htmlPath = path.resolve(here, '../../docs/UI.html');
const outPath = process.argv[2] || path.resolve(here, 'study_layout.txt');

function findBrowser() {
  if (process.env.CHROME) return process.env.CHROME;
  const cache = path.join(os.homedir(), 'Library/Caches/ms-playwright');
  if (fs.existsSync(cache)) {
    for (const d of fs.readdirSync(cache).filter(d => d.startsWith('chromium_headless_shell-')).sort().reverse()) {
      for (const sub of fs.readdirSync(path.join(cache, d))) {
        const p = path.join(cache, d, sub, 'chrome-headless-shell');
        if (fs.existsSync(p)) return p;
      }
    }
  }
  const sys = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  if (fs.existsSync(sys)) return sys;
  throw new Error('no Chromium found: set CHROME');
}

// ---- the splice ---------------------------------------------------------------
let html = fs.readFileSync(htmlPath, 'utf8');
function splice(anchor, text, after = true) {
  const i = html.indexOf(anchor);
  if (i < 0 || html.indexOf(anchor, i + 1) >= 0) throw new Error('anchor not found exactly once: ' + anchor);
  const at = after ? html.indexOf('\n', i) + 1 : i;
  html = html.slice(0, at) + text + '\n' + html.slice(at);
}
splice('const lay = short ? rowSeats(', 'window.CN_last = { W, H, inner, topM, boardH, short, d, ring, myR, mcx, mcy, origin, tilt, lay, hudB, mine, PICKER_H };');
splice('objs.push(myCup);', 'window.CN_last.objs = objs; window.CN_last.myCup = myCup;');
splice('const syncLay = mixer(', 'window.CN_expose = { screen, plateFrame, L, isShort, peekAngleFor, VIEW_B };', false);
const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'cn-study-'));
const tmpHtml = path.join(tmpDir, 'UI.html');
fs.writeFileSync(tmpHtml, html);

// ---- a CDP client in thirty lines ----------------------------------------------
const browser = spawn(findBrowser(), ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${tmpDir}/prof`,
  '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--allow-file-access-from-files', 'about:blank'],
  { stdio: ['ignore', 'ignore', 'pipe'] });
const wsUrl = await new Promise((res, rej) => {
  let buf = '';
  browser.stderr.on('data', d => { buf += d; const m = buf.match(/DevTools listening on (ws:\/\/\S+)/); if (m) res(m[1]); });
  browser.on('exit', c => rej(new Error('browser exited ' + c + ': ' + buf)));
  setTimeout(() => rej(new Error('browser did not start: ' + buf)), 20000);
});
const ws = new WebSocket(wsUrl);
await new Promise(r => ws.addEventListener('open', r));
let nextId = 1; const waiting = new Map(), listeners = [];
ws.addEventListener('message', ev => {
  const m = JSON.parse(ev.data);
  if (m.id && waiting.has(m.id)) { const { res, rej } = waiting.get(m.id); waiting.delete(m.id); m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result); }
  else for (const l of listeners) l(m);
});
const send = (method, params = {}, sessionId) => new Promise((res, rej) => { const id = nextId++; waiting.set(id, { res, rej }); ws.send(JSON.stringify({ id, method, params, sessionId })); });
const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
await send('Page.enable', {}, sessionId);
const loaded = new Promise(r => listeners.push(m => { if (m.method === 'Page.loadEventFired') r(); }));
await send('Page.navigate', { url: 'file://' + tmpHtml }, sessionId);
await loaded;
async function evaluate(expr) {
  const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }, sessionId);
  if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails));
  return r.result.value;
}

// ---- the cases -------------------------------------------------------------------
// The four sizes the study lays out, plus 390 by 584 (the "collapsed is 584" reading,
// which is a tall board). State 'table' is my turn (the picker is up), 'wait' theirs.
const SIZES = [[390, 340], [390, 718], [375, 541], [430, 830], [390, 584]];
const probe = `(() => {
  const X = window.CN_expose, out = [];
  for (const [W, H] of ${JSON.stringify(SIZES)}) for (const state of ['table', 'wait']) for (let n = 2; n <= 6; n++) {
    X.screen({ seats: n, state, view: 'top', arrange: 'foolish', picker: 'rows' }, W, H, { peek: true });
    const c = window.CN_last, t = c.tilt, lay = c.lay, objs = c.objs;
    const pf = c.short ? X.plateFrame(n, c.inner, c.boardH) : null;
    const seats = [], names = [];
    for (let i = 1; i < n; i++) { seats.push([i, lay.seats[i][0], lay.seats[i][1]]); names.push([i, lay.names[i][0], lay.names[i][1]]); }
    const dice = objs.filter(o => o.die).map(o => [o.seat == null ? 0 : o.seat, o.pos[0], o.pos[1], o.yaw, o.d]);
    const cups = objs.filter(o => !o.die).map(o => [o.seat == null ? 0 : o.seat, o.pos[0], o.pos[1], o.lift || 0, o.R, o.out ? 1 : 0,
      o.rot ? o.rot.flat() : [0, 0, 0, 0, 0, 0, 0, 0, 0], o.shadowR, o.shadowDX || 0, o.shadowDY || 0]);
    const ts = [-400, -150, -40, 30].map(dy => { const r = t.toScreen(dy); return [dy, r.y, r.scale]; });
    const fs = [0, 70, 250].map(y => [y, t.fromScreen(y)]);
    const pk = c.myCup.tilt;
    out.push({ W, H, state, n, topM: c.topM, boardH: c.boardH, short: c.short ? 1 : 0, d: c.d, ring: c.ring, myR: c.myR,
      mcx: c.mcx, mcy: c.mcy, ox: c.origin[0], oy: c.origin[1], theta: t.theta, D: t.D, zoom: t.zoom,
      R: lay.R, cy: lay.cy == null ? 0 : lay.cy, rx: lay.rx || 0, ry: lay.ry || 0, pad: lay.pad, padX: lay.padX,
      seats, names, pf, ts, fs, peek: pk ? [pk.angle, pk.hingeY, pk.back, pk.lift] : null, dice, cups });
  }
  return out;
})()`;
const cases = await evaluate(probe);
ws.close();
await new Promise(r => { browser.on('exit', r); browser.kill(); });
try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) { /* the profile may still be closing */ }

// ---- the text --------------------------------------------------------------------
const f = v => (Math.round(v * 1e6) / 1e6).toFixed(6);
let txt = `# chuiniu/docs/UI.html's layout, as headless Chromium computed it (tools/dump_study_layout.mjs).\n` +
  `# One block a case: W H state seats, then the board, the camera, the ring, each seat, the peek, every die and cup.\n`;
for (const c of cases) {
  txt += `case ${c.W} ${c.H} ${c.state} ${c.n}\n`;
  txt += `board topM ${f(c.topM)} boardH ${f(c.boardH)} short ${c.short} d ${f(c.d)} ring ${f(c.ring)} myR ${f(c.myR)} mcx ${f(c.mcx)} mcy ${f(c.mcy)} origin ${f(c.ox)} ${f(c.oy)}\n`;
  txt += `cam theta ${f(c.theta)} D ${f(c.D)} zoom ${f(c.zoom)}\n`;
  txt += `toScreen ${c.ts.map(r => r.map(f).join(' ')).join(' | ')}\n`;
  txt += `fromScreen ${c.fs.map(r => r.map(f).join(' ')).join(' | ')}\n`;
  txt += `ring R ${f(c.R)} cy ${f(c.cy)} rx ${f(c.rx)} ry ${f(c.ry)} pad ${c.pad} padX ${c.padX}\n`;
  for (const s of c.seats) txt += `seat ${s[0]} ${f(s[1])} ${f(s[2])}\n`;
  for (const s of c.names) txt += `name ${s[0]} ${f(s[1])} ${f(s[2])}\n`;
  txt += `plate ${c.pf ? c.pf.map(f).join(' ') : 'none'}\n`;
  txt += `peek ${c.peek ? c.peek.map(f).join(' ') : 'none'}\n`;
  for (const d of c.dice) txt += `die ${d[0]} ${f(d[1])} ${f(d[2])} ${f(d[3])} ${f(d[4])}\n`;
  for (const k of c.cups) txt += `cup ${k[0]} ${f(k[1])} ${f(k[2])} ${f(k[3])} ${f(k[4])} ${k[5]} rot ${k[6].map(f).join(' ')} shadow ${f(k[7])} ${f(k[8])} ${f(k[9])}\n`;
}
fs.writeFileSync(outPath, txt);
console.log(`wrote ${outPath}: ${cases.length} cases`);
