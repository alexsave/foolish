// Headless Chrome over CDP, and a static server, for the shots, the test and the timings.
// One Chrome per process on port 8729, killed by its own PID; the server on 8720.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
export const POC = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const FOOLISH = path.resolve(POC, '../..');
const TYPES = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wasm': 'application/wasm', '.png': 'image/png', '.svg': 'image/svg+xml', '.json': 'application/json',
  '.woff2': 'font/woff2', '.jpg': 'image/jpeg' };

/** Serves the foolish/ tree, so a page links /src/styles/index.css as the app does. */
export function serve(port = 8720, root = FOOLISH, extra = null, headers = {}) {
  const server = http.createServer((req, res) => {
    if (extra && extra(req, res)) return;
    const u = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    const f = path.join(root, u);
    if (!f.startsWith(root) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'content-type': TYPES[path.extname(f)] || 'application/octet-stream', 'cache-control': 'no-store', ...headers });
    fs.createReadStream(f).pipe(res);
  });
  return new Promise((r) => server.listen(port, '127.0.0.1', () => r(server)));
}

// flags: more Chrome switches. Pixel comparisons pass '--disable-gpu-rasterization': with GPU
// raster, Chrome draws one large JPEG two ways (the same bytes under a new url differed in 225,611
// of 329,160 pixels, 1 to 20 per channel, about one load in five under a load average of 60),
// and the wool is such a JPEG.
export async function launch({ width = 1024, height = 768, port = 8729, flags = [] } = {}) {
  // A Chrome already on the port (a run that died before closing its own) would be driven instead
  // of this one, with its profile and its kept game: refuse, and name it.
  const stale = await fetch(`http://127.0.0.1:${port}/json/version`).then(() => true, () => false);
  if (stale) throw new Error(`a Chrome already listens on ${port} (a run that did not close it?): stop it by its PID first`);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cdom-poc-chrome-'));
  const proc = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${dir}`, '--no-first-run',
      '--no-default-browser-check', '--disable-extensions', '--hide-scrollbars', `--window-size=${width},${height}`,
      ...flags, 'about:blank'],
    { stdio: 'ignore' });
  const close = async () => {
    try { ws?.close(); } catch { /* gone */ }
    proc.kill('SIGTERM');
    for (let n = 0; n < 20 && proc.exitCode === null && proc.signalCode === null; n++) await sleep(100);
    if (proc.exitCode === null && proc.signalCode === null) proc.kill('SIGKILL');
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* busy */ }
  };
  let ver, ws;
  try {
    for (let i = 0; i < 60 && !ver; i++) {
      try { ver = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); } catch { await sleep(200); }
    }
    if (!ver) throw new Error('chrome did not start');
    ws = new WebSocket(ver.webSocketDebuggerUrl);
    await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  } catch (e) { await close(); throw e; }
  let id = 0;
  const pend = new Map(), waiters = [], listeners = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); return; }
    for (const w of [...waiters]) if (w.method === d.method) { waiters.splice(waiters.indexOf(w), 1); w.r(d.params); }
    for (const l of listeners) if (l.method === d.method) l.fn(d.params);
  };
  const send = (method, params = {}, sessionId) => new Promise((r) => {
    const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params, sessionId }));
  });
  const { result: { targetId } } = await send('Target.createTarget', { url: 'about:blank' });
  const { result: { sessionId } } = await send('Target.attachToTarget', { targetId, flatten: true });
  const s = async (m, p) => { const r = await send(m, p, sessionId); if (r.error) throw new Error(`${m}: ${r.error.message}`); return r.result; };
  await s('Page.enable'); await s('Runtime.enable');
  await s('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
  const evaluate = async (expr) => {
    const r = await s('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || JSON.stringify(r.exceptionDetails));
    return r.result.value;
  };
  const once = (method) => new Promise((r) => waiters.push({ method, r }));
  const goto = async (url) => { const loaded = once('Page.loadEventFired'); await s('Page.navigate', { url }); await loaded; };
  /** Polls `expr` until truthy, bounded: about `ms` milliseconds. */
  const until = async (expr, ms = 5000) => {
    for (let n = 0; n < ms / 25; n++) { const v = await evaluate(expr); if (v) return v; await sleep(25); }
    throw new Error(`timed out waiting for ${expr}`);
  };
  const shot = async (file) => {
    const { data } = await s('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(file, Buffer.from(data, 'base64'));
  };
  /** A real click: mouse pressed and released at the centre of the element `sel` names. */
  // The element is clicked where it rests: a hand card slides for 0.1s after the hand changes,
  // so the rect is read until two reads a frame apart agree (bounded).
  const click = async (sel) => {
    const rect = `(() => { const r = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect();
      return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`;
    let at = await evaluate(rect);
    for (let n = 0; n < 40; n++) {
      await sleep(20);
      const again = await evaluate(rect);
      if (again.x === at.x && again.y === at.y) break;
      at = again;
    }
    for (const type of ['mousePressed', 'mouseReleased'])
      await s('Input.dispatchMouseEvent', { type, x: at.x, y: at.y, button: 'left', clickCount: 1 });
  };
  const closeAll = async () => { try { await send('Target.closeTarget', { targetId }); } catch { /* closed */ } await close(); };
  /** Every `method` event to fn until the returned function is called (a screencast's frames). */
  const listen = (method, fn) => { const l = { method, fn }; listeners.push(l); return () => listeners.splice(listeners.indexOf(l), 1); };
  return { send: s, evaluate, goto, until, shot, click, listen, close: closeAll };
}

// The table keeps its game in cdom's store (IndexedDB "cdom", object store "kv"; src/table_app.c
// GAME_KEY) and a load adopts it. openTable loads the table on a fresh game: it forgets the kept
// game first (from a page of the same origin), then waits for cdom.start and the store's answer.
// The test build (?debug): hold the bot's steps for the caller to release, or make its waits fast.
export const GAME_KEY = 'foolish-game-v1';
// The kept game deleted, from any page of the table's origin (a promise of 1, or 0 on a failure).
export const FORGET_GAME = `new Promise((resolve) => { const r = indexedDB.open('cdom', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('kv');
    r.onsuccess = () => { const d = r.result; const t = d.transaction('kv', 'readwrite'); t.objectStore('kv').delete(${JSON.stringify(GAME_KEY)});
      t.oncomplete = () => { d.close(); resolve(1); }; t.onerror = () => { d.close(); resolve(0); }; };
    r.onerror = () => resolve(0); })`;
export async function forgetGame(b, origin = 'http://127.0.0.1:8720') {
  await b.goto(origin + '/src/styles/index.css'); // any document of the origin (a 404 page has none)
  await b.evaluate(FORGET_GAME);
}
export async function openTable(b, url, { hold = false, fast = false, forget = true } = {}) {
  if (forget) await forgetGame(b, new URL(url).origin);
  await b.goto(url);
  await b.until('window.appReady');
  if (hold) await b.evaluate('window.app.then((x) => x.app_debug_hold(1))');
  if (fast) await b.evaluate('window.app.then((x) => x.app_debug_fast(1))');
  await b.until('window.app.then((x) => x.app_game_store(1) > 0)', 3000);
}
