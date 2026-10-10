// Is `.wasm.gz` + JS inflate + compile-from-bytes (what the browser does today,
// sdk/ts/wasm/wasm_asset.ts) slower than a plain `.wasm` response handed to
// WebAssembly.instantiateStreaming? Measured in real Chrome over CDP, no
// Playwright: Node 26 has a global WebSocket.
//
//   node e2e/bench_wasm_streaming.mjs [rounds=10]
//
// THE THREE WAYS, per module:
//   gz      today's path. The .gz fetched as opaque bytes, inflated by the
//           vendored tiny-inflate, then the app's own call for that module
//           (web: new Module + new Instance; oracle: instantiate(bytes);
//           oracle-mt: compile(bytes) + an Instance over a shared Memory).
//   st-gz   the raw .wasm as `application/wasm`, `Content-Encoding: gzip`
//           (the very same .gz bytes on the wire), into the streaming call.
//   st-br   the same with brotli, which is what a CDN would actually send.
//
// THE THREE VISITS:
//   cold    a fresh Chrome profile in a fresh process. Nothing cached.
//   repeat  the SAME profile, relaunched: the HTTP cache is warm, and so is
//           V8's compiled-code cache if the first visit earned one.
//   cold4g  a fresh profile behind DevTools' "Fast 4G" (9 Mbit/s, 165 ms).
//
// A FRESH PROCESS PER SAMPLE is not fussiness. V8 keeps compiled modules in a
// per-process cache keyed on the wire bytes, so a second compile of the same
// module in one renderer measures a hash lookup.
//
// The clock starts at the fetch and stops when the instance exists; `init`
// then calls wasm_init() where the module has one, since both paths compile
// lazily and a module that has never run has not paid for itself yet.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtempSync, readFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { gunzipSync, brotliCompressSync, constants as Z } from 'node:zlib';
import { setTimeout as sleep } from 'node:timers/promises';

const ROUNDS = Number(process.argv[2] ?? 10);
const CHROME = process.env.CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const here = new URL('.', import.meta.url);
const MODULES = {
    web: new URL('../sdk/ts/wasm/web.wasm.gz', here),
    oracle: new URL('../public/oracle.wasm.gz', here),
    'oracle-mt': new URL('../public/oracle-mt.wasm.gz', here),
};
const WAYS = ['gz', 'st-gz', 'st-br'];
const VISITS = ['cold', 'repeat', 'cold4g'];
// How long the first visit idles before Chrome is closed, so the top tier can
// finish and the code cache can be written. Without it `repeat` never has one.
const SETTLE_MS = 3000;

const assets = {};
for (const [name, url] of Object.entries(MODULES)) {
    const gz = readFileSync(url);
    const raw = gunzipSync(gz);
    const br = brotliCompressSync(raw, { params: { [Z.BROTLI_PARAM_QUALITY]: 11, [Z.BROTLI_PARAM_SIZE_HINT]: raw.length } });
    assets[name] = { gz, raw, br };
}

const PAGE = `<!doctype html><meta charset="utf-8"><script type="module">
import tinf from '/tiny_inflate.mjs';
// sdk/ts/wasm/gunzip.ts, verbatim in behaviour.
function gunzip(gz) {
    const flg = gz[3]; let p = 10;
    if (flg & 0x04) p += 2 + (gz[p] | (gz[p + 1] << 8));
    if (flg & 0x08) while (gz[p++] !== 0) {}
    if (flg & 0x10) while (gz[p++] !== 0) {}
    if (flg & 0x02) p += 2;
    const n = gz.length;
    const isize = (gz[n - 4] | (gz[n - 3] << 8) | (gz[n - 2] << 16) | (gz[n - 1] << 24)) >>> 0;
    const out = new Uint8Array(isize);
    tinf(gz.subarray(p, n - 8), out);
    return out;
}
const mtImports = () => ({ env: { memory: new WebAssembly.Memory({ initial: 64, maximum: 2048, shared: true }) } });
window.run = async (name, way) => {
    const mt = name === 'oracle-mt';
    const imports = mt ? mtImports() : {};
    const t0 = performance.now();
    let instance, fetched = 0, inflated = 0;
    if (way === 'gz') {
        const gz = new Uint8Array(await (await fetch('/gz/' + name + '.wasm.gz')).arrayBuffer());
        fetched = performance.now();
        const bytes = gunzip(gz);
        inflated = performance.now();
        if (name === 'web') instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), imports);
        else if (mt) instance = await WebAssembly.instantiate(await WebAssembly.compile(bytes), imports);
        else instance = (await WebAssembly.instantiate(bytes, imports)).instance;
    } else {
        const resp = fetch('/' + way + '/' + name + '.wasm');
        if (mt) instance = await WebAssembly.instantiate(await WebAssembly.compileStreaming(resp), imports);
        else instance = (await WebAssembly.instantiateStreaming(resp, imports)).instance;
    }
    const ready = performance.now();
    if (!mt && instance.exports.wasm_init) instance.exports.wasm_init();
    const init = performance.now();
    return [ready - t0, init - t0, fetched ? fetched - t0 : 0, inflated ? inflated - fetched : 0];
};
window.ok = true;
</script>`;

// What a static host sends for a content-hashed asset: cacheable forever.
const CACHE = { 'Cache-Control': 'public, max-age=31536000, immutable' };
// oracle-mt needs a shared Memory, so the document is cross-origin isolated.
const ISOLATE = { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' };
const server = createServer((req, res) => {
    const [, dir, file] = req.url.split('?')[0].split('/');
    const name = (file ?? '').replace(/\.wasm(\.gz)?$/, '');
    const a = assets[name];
    if (dir === 'page') { res.writeHead(200, { 'Content-Type': 'text/html', ...ISOLATE }); return res.end(PAGE); }
    if (dir === 'tiny_inflate.mjs') {
        res.writeHead(200, { 'Content-Type': 'text/javascript', ...CACHE });
        return res.end(readFileSync(new URL('../sdk/ts/wasm/tiny_inflate.mjs', here)));
    }
    if (a && dir === 'gz') { res.writeHead(200, { 'Content-Type': 'application/gzip', 'Content-Length': a.gz.length, ...CACHE }); return res.end(a.gz); }
    if (a && dir === 'st-gz') { res.writeHead(200, { 'Content-Type': 'application/wasm', 'Content-Encoding': 'gzip', 'Content-Length': a.gz.length, ...CACHE }); return res.end(a.gz); }
    if (a && dir === 'st-br') { res.writeHead(200, { 'Content-Type': 'application/wasm', 'Content-Encoding': 'br', 'Content-Length': a.br.length, ...CACHE }); return res.end(a.br); }
    res.writeHead(404); res.end();
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${server.address().port}`;

/** One Chrome process on `profile`, one page, one run() - then a clean close so the caches reach the disk. */
async function sample(profile, name, way, { throttle = false, settle = 0 } = {}) {
    const portFile = join(profile, 'DevToolsActivePort');
    if (existsSync(portFile)) rmSync(portFile);
    const proc = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
        '--no-first-run', '--no-default-browser-check', '--disable-extensions', 'about:blank'], { stdio: 'ignore' });
    const exited = new Promise((r) => proc.once('exit', r));
    try {
        // The file appears before both of its lines do.
        let port, path;
        for (let i = 0; !path; i++) {
            if (i > 400) throw new Error('chrome never opened its debugging port');
            await sleep(25);
            if (existsSync(portFile)) [port, path] = readFileSync(portFile, 'utf8').trim().split('\n');
        }
        const ws = new WebSocket(`ws://127.0.0.1:${port}${path}`);
        await new Promise((r, j) => { ws.onopen = r; ws.onerror = () => j(new Error('cdp socket')); });
        let id = 0; const waiting = new Map(); const events = [];
        ws.onmessage = (m) => {
            const msg = JSON.parse(m.data);
            if (msg.id && waiting.has(msg.id)) { const [r, j] = waiting.get(msg.id); waiting.delete(msg.id); msg.error ? j(new Error(msg.error.message)) : r(msg.result); }
            else if (msg.method) events.push(msg.method);
        };
        const send = (method, params = {}, sessionId) => new Promise((r, j) => { waiting.set(++id, [r, j]); ws.send(JSON.stringify({ id, method, params, sessionId })); });
        const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
        const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
        await send('Network.enable', {}, sessionId);
        if (throttle) await send('Network.emulateNetworkConditions', { offline: false, latency: 165, downloadThroughput: 9e6 / 8, uploadThroughput: 1.5e6 / 8 }, sessionId);
        await send('Page.enable', {}, sessionId);
        await send('Page.navigate', { url: `${ORIGIN}/page` }, sessionId);
        for (let i = 0; ; i++) {
            const r = await send('Runtime.evaluate', { expression: 'window.ok === true' }, sessionId);
            if (r.result.value) break;
            if (i > 400) throw new Error('the page never loaded');
            await sleep(25);
        }
        const r = await send('Runtime.evaluate', { expression: `run(${JSON.stringify(name)}, ${JSON.stringify(way)})`, awaitPromise: true, returnByValue: true }, sessionId);
        if (r.exceptionDetails) throw new Error(`${name}/${way}: ${r.exceptionDetails.exception?.description ?? r.exceptionDetails.text}`);
        if (settle) await sleep(settle);
        await send('Browser.close').catch(() => {});
        return r.result.value;
    } finally {
        const t = setTimeout(() => proc.kill('SIGKILL'), 5000);
        await exited; clearTimeout(t);
    }
}

const out = {};   // out[name][way][visit] = [[ready, init, fetched, inflate], ...]
for (const name of Object.keys(MODULES)) { out[name] = {}; for (const way of WAYS) { out[name][way] = {}; for (const v of VISITS) out[name][way][v] = []; } }
// Interleaved, so machine drift lands on every cell alike.
for (let round = 0; round < ROUNDS; round++) {
    for (const name of Object.keys(MODULES)) for (const way of WAYS) {
        const profile = mkdtempSync(join(tmpdir(), 'wasm-stream-'));
        try {
            out[name][way].cold.push(await sample(profile, name, way, { settle: SETTLE_MS }));
            out[name][way].repeat.push(await sample(profile, name, way));
        } finally { rmSync(profile, { recursive: true, force: true }); }
        const slow = mkdtempSync(join(tmpdir(), 'wasm-stream-'));
        try { out[name][way].cold4g.push(await sample(slow, name, way, { throttle: true })); }
        finally { rmSync(slow, { recursive: true, force: true }); }
    }
    process.stderr.write(`round ${round + 1}/${ROUNDS}\n`);
}
server.close();

const med = (xs) => { const s = [...xs].sort((a, b) => a - b); return s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2; };
const col = (rows, i) => rows.map((r) => r[i]);
const f = (x) => x.toFixed(1).padStart(7);
console.log(`Chrome, ${ROUNDS} rounds, median ms from fetch to instance (and to wasm_init done); min..max in brackets\n`);
for (const name of Object.keys(MODULES)) {
    const a = assets[name];
    console.log(`${name}: raw ${a.raw.length} B, gzip ${a.gz.length} B, brotli ${a.br.length} B`);
    for (const v of VISITS) for (const way of WAYS) {
        const rows = out[name][way][v], ready = col(rows, 0);
        const parts = way === 'gz' ? `  fetch ${f(med(col(rows, 2)))}  inflate ${f(med(col(rows, 3)))}` : '';
        console.log(`  ${v.padEnd(7)} ${way.padEnd(6)} ready ${f(med(ready))} [${f(Math.min(...ready))} ..${f(Math.max(...ready))}]  init ${f(med(col(rows, 1)))}${parts}`);
    }
    console.log('');
}
