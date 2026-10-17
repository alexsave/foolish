// Collect the repo's headline health metrics as ONE JSON blob, so CI can run it
// on the PR head and the base and diff them into a before/after comment. Run
// from the repo root; measures THIS checkout's committed artifacts + source.
//
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node scripts/collect_metrics.mjs
//
// Metrics:
//   size   — the kernel the browser downloads (web.wasm.gz), raw + gzip bytes,
//            plus the replay oracle's two modules (public/oracle.wasm.gz,
//            public/oracle-mt.wasm.gz). `web` is the download number: it is
//            what a visitor pays.
//   webBundle — first-load JS of `/` and `/[game_id]`, gzip bytes, from a real
//            `next build` (scripts/measure_web_bundle.mjs)
//   linearMemory — each module's DECLARED linear memory (initial pages/bytes + pinned flag)
//   speed  — engine throughput (games/sec, actions/sec) from e2e/bench_engine.ts
//
// The thinking-bot move latency against Postgres (and the bots' runtime memory
// peak it measured) went with the TypeScript server in stage 4; the server's
// move latency is c_base's `cbase-bench roundtrip` (c_base/DESIGN.md "The bench").
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';

const WASM = 'sdk/ts/wasm';
const tsxEnv = { ...process.env, TSX_TSCONFIG_PATH: 'e2e/tsconfig.json' };
const runNode = (args, extraEnv = {}) =>
  execFileSync('node', ['--import', 'tsx', ...args],
    { encoding: 'utf8', env: { ...tsxEnv, ...extraEnv }, maxBuffer: 64 * 1024 * 1024, stdio: ['ignore', 'pipe', 'ignore'] });

// ---- size: committed wasm artifacts (no toolchain needed) ----
function gzFileSize(path) {
  const gz = readFileSync(path);
  return { raw: gunzipSync(gz).length, gz: gz.length };
}

// DECLARED linear memory: the (min,max) page limits in a module's memory
// section — the size the module reserves at instantiation, independent of any
// runtime bench.
// Deterministic + toolchain-free, so it runs on both base and head everywhere.
const PAGE = 65536;
function linearMemOf(bytes) {
  if (!bytes) return null;
  let p = 8; // skip the 8-byte module header
  const leb = () => { let r = 0, s = 0, b; do { b = bytes[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return r >>> 0; };
  while (p < bytes.length) {
    const id = bytes[p++], len = leb(), end = p + len;
    if (id === 5) { // memory section
      leb(); // count (always 1 here)
      const flags = leb(), min = leb();
      const max = (flags & 1) ? leb() : null;
      return { pages: min, bytes: min * PAGE, maxPages: max, pinned: max === min };
    }
    p = end;
  }
  return null;
}
function linearMemory() {
  try {
    return {
      web: existsSync(`${WASM}/web.wasm.gz`)
        ? linearMemOf(gunzipSync(readFileSync(`${WASM}/web.wasm.gz`))) : null,
    };
  } catch (e) { return { error: String(e.message || e) }; }
}

function size() {
  try {
    return {
      // The browser's link of the same objects - the number a visitor actually
      // downloads. A checkout that predates it reports null rather than failing.
      web: existsSync(`${WASM}/web.wasm.gz`) ? gzFileSize(`${WASM}/web.wasm.gz`) : null,
      // The oracle modules are served from public/ rather than embedded; a
      // checkout that predates one reports null for it rather than failing.
      oracle: existsSync('public/oracle.wasm.gz') ? gzFileSize('public/oracle.wasm.gz') : null,
      'oracle-mt': existsSync('public/oracle-mt.wasm.gz') ? gzFileSize('public/oracle-mt.wasm.gz') : null,
    };
  } catch (e) { return { error: String(e.message || e) }; }
}

// ---- web bundle: first-load JS of the main routes ----
// A real `next build`, so this is the slowest metric after the e2e bench. The
// module is imported from the collector's own directory: metrics.yml copies it
// into the base checkout alongside this file.
async function webBundle() {
  try {
    const { measureWebBundle } = await import(new URL('./measure_web_bundle.mjs', import.meta.url).href);
    return measureWebBundle();
  } catch (e) { return { error: String(e.message || e) }; }
}

// ---- speed: engine throughput ----
// Single-run throughput on a shared CI runner swings several % — so run it a few
// times and report the MEDIAN plus the observed spread (lo/hi). The renderer
// only paints a delta green/red when it clears that noise band.
const median = (a) => { const s = [...a].sort((x, y) => x - y); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
function speed() {
  try {
    const reps = Number(process.env.SPEED_REPS || 5);
    const g = [], a = [], l = [];
    for (let i = 0; i < reps; i++) {
      const out = runNode(['e2e/bench_engine.ts'], { BENCH_GAMES: process.env.BENCH_GAMES || '300' });
      const gm = out.match(/games\/sec:\s*([\d.]+)/); if (gm) g.push(+gm[1]);
      const am = out.match(/actions\/sec:\s*([\d.]+)/); if (am) a.push(+am[1]);
      const lm = out.match(/legal-evals\/sec:\s*([\d.]+)/); if (lm) l.push(+lm[1]);
    }
    const stat = (arr) => arr.length ? { med: median(arr), lo: Math.min(...arr), hi: Math.max(...arr), n: arr.length } : null;
    return { reps, gamesPerSec: stat(g), actionsPerSec: stat(a), legalEvalsPerSec: stat(l) };
  } catch (e) { return { error: String(e.message || e) }; }
}

const metrics = {
  size: size(),
  linearMemory: linearMemory(),
  speed: speed(),
  webBundle: await webBundle(),
};
process.stdout.write(JSON.stringify(metrics, null, 2) + '\n');
