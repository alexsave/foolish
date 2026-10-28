// The contract test: cdom's network imports against a real local c_base, in headless Chrome.
// Starts `c_base/build/cbase serve` (build it first: make -j4 in c_base/) on a free port with
// throwaway secrets and a throwaway SQLite file, serves tests/live_test.c's module, and reads the
// verdict the module writes into its page (tests/live_test.c says what it checks). The server and
// Chrome are this process's children, stopped by their own PIDs. Exits non-zero on any failure.
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { launch, serve, sleep, POC, FOOLISH } from '../tools/cdp.mjs';

const out = path.join(POC, 'out');
const cbase = path.resolve(FOOLISH, '../c_base/build/cbase');
if (!fs.existsSync(cbase)) { console.log(`no ${cbase}: run make -j4 in c_base/ first`); process.exit(2); }
const shim = fs.readFileSync(path.join(POC, 'vendor/cdom.js'), 'utf8');
const page = `<!DOCTYPE html><html><head><meta charset="utf-8"/><script>${shim}
const q = new URLSearchParams(location.search);
cdom.start('live_test.wasm', 'root').then((x) => { window.__x = x; x.live_start(+q.get('port'), +q.get('dead'), +q.get('nonce')); });
</script></head><body><div id="root"></div></body></html>`;

// A port nobody listens on: taken from the system, then let go.
const dead = await new Promise((resolve) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cdom-live-'));
const server = spawn(cbase, ['serve'], {
  env: { ...process.env, CBASE_DEV: '1', CBASE_LISTEN: '127.0.0.1:0', CBASE_STORE: 'sqlite', CBASE_DB: path.join(dir, 'cbase.sqlite'),
    CBASE_JWT_SECRET: crypto.randomBytes(32).toString('hex'), CBASE_ADMIN_TOKEN: crypto.randomBytes(24).toString('hex'), CBASE_ALLOWED_ORIGINS: '*' },
  stdio: ['ignore', 'pipe', 'pipe'],
});
let serverLog = '';
const port = await new Promise((resolve) => {
  const read = (d) => { serverLog += d; const m = /listening on 127\.0\.0\.1:(\d+)/.exec(serverLog); if (m) resolve(+m[1]); };
  server.stdout.on('data', read);
  server.stderr.on('data', read);
  server.on('exit', () => resolve(0));
  setTimeout(() => resolve(0), 10000);
});
const stop = async () => {
  server.kill('SIGTERM');
  for (let n = 0; n < 30 && server.exitCode === null && server.signalCode === null; n++) await sleep(100);
  if (server.exitCode === null && server.signalCode === null) server.kill('SIGKILL');
  fs.rmSync(dir, { recursive: true, force: true });
};
if (!port) { console.log('c_base did not start:\n' + serverLog.slice(-1500)); await stop(); process.exit(2); }
console.log(`c_base on 127.0.0.1:${port} (pid ${server.pid}); a dead port ${dead}`);

const statics = await serve(8720, out, (req, res) => {
  if (req.url.startsWith('/live.html')) { res.writeHead(200, { 'content-type': 'text/html' }); res.end(page); return true; }
  return false;
});
const b = await launch();
let log = '';
try {
  const nonce = crypto.randomBytes(4).readUInt32LE(0);
  await b.goto(`http://127.0.0.1:8720/live.html?port=${port}&dead=${dead}&nonce=${nonce}`);
  for (let n = 0; n < 600 && !/^done/m.test(log); n++) { await sleep(50); log = await b.evaluate(`document.getElementById('log')?.innerText || ''`); }
} finally { await b.close(); statics.close(); await stop(); }
console.log(log.trim() || '(nothing logged)');
const ok = /^done$/m.test(log) && !/^FAIL/m.test(log);
if (!/^done/m.test(log)) console.log('FAIL the run never finished');
process.exit(ok ? 0 : 1);
