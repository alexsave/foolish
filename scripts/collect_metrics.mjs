// Collect the repo's headline health metrics as ONE JSON blob, so CI can run it
// on the PR head and the base and diff them into a before/after comment. Run
// from the repo root; measures THIS checkout's committed artifacts + source.
//
//   E2E_PG* set + TSX_TSCONFIG_PATH=e2e/tsconfig.json node scripts/collect_metrics.mjs
//
// Metrics:
//   size   — the three wasm modules (rules/guards embedded b64, bots.wasm.gz), raw + gzip bytes
//   linearMemory — each module's DECLARED linear memory (initial pages/bytes + pinned flag)
//   speed  — engine throughput (games/sec, actions/sec) from e2e/bench_engine.ts
//   memory — peak bots/kernel wasm linear memory (MB) after the MC bots ran
//   e2e    — THE headline: full thinking-bot move latency vs real Postgres
//            (load → belief → kernel choose → apply → commit), per strategy
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { execFileSync } from 'node:child_process';

const WASM = 'sdk/ts/wasm';
const tsxEnv = { ...process.env, TSX_TSCONFIG_PATH: 'e2e/tsconfig.json' };
const runNode = (args, extraEnv = {}) =>
  execFileSync('node', ['--import', 'tsx', ...args],
    { encoding: 'utf8', env: { ...tsxEnv, ...extraEnv }, maxBuffer: 64 * 1024 * 1024, stdio: ['ignore', 'pipe', 'ignore'] });

// ---- size: committed wasm artifacts (no toolchain needed) ----
// Return the decompressed wasm bytes for a base64 embed (rules/guards) …
function embeddedBytes(tsFile) {
  const src = readFileSync(`${WASM}/${tsFile}`, 'utf8');
  const m = src.match(/b64[^']*'([A-Za-z0-9+/=]+)'/);
  if (!m) return null;
  return gunzipSync(Buffer.from(m[1], 'base64'));
}
function embeddedSize(tsFile) {
  const src = readFileSync(`${WASM}/${tsFile}`, 'utf8');
  const m = src.match(/b64[^']*'([A-Za-z0-9+/=]+)'/);
  if (!m) return null;
  const gz = Buffer.from(m[1], 'base64');
  return { raw: gunzipSync(gz).length, gz: gz.length };
}
function gzFileSize(path) {
  const gz = readFileSync(path);
  return { raw: gunzipSync(gz).length, gz: gz.length };
}

// DECLARED linear memory: the (min,max) page limits in a module's memory
// section — the size the module reserves at instantiation, independent of any
// runtime bench. This is what the R0/R1/R4 rules shrink and the guards pin move.
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
      rules: linearMemOf(embeddedBytes('rules_wasm.ts')),
      guards: linearMemOf(embeddedBytes('guards_wasm.ts')),
      bots: linearMemOf(gunzipSync(readFileSync(`${WASM}/bots.wasm.gz`))),
    };
  } catch (e) { return { error: String(e.message || e) }; }
}

function size() {
  try {
    return {
      rules: embeddedSize('rules_wasm.ts'),
      guards: embeddedSize('guards_wasm.ts'),
      bots: gzFileSize(`${WASM}/bots.wasm.gz`),
    };
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

// ---- e2e latency + wasm memory (Postgres-backed) ----
// The headline latency is wall-clock on a shared CI runner, so a SINGLE run's
// p50 swings run-to-run enough to drown small real deltas. Mirror speed(): run
// the whole bench a few times and report the MEDIAN p50 per strategy, plus the
// observed lo/hi spread so the renderer can treat overlapping ranges as noise.
// Memory is page-granular and deterministic, so any rep's number will do.
function e2eAndMemory() {
  try {
    const reps = Number(process.env.E2E_REPS || 3);
    const perStrat = new Map(); // strategy -> accumulated per-rep stats
    let memory = null;
    for (let i = 0; i < reps; i++) {
      const out = runNode(['e2e/bench_bot_e2e.ts'], {
        BENCH_JSON: '1',
        // Shipped, dispatchable bots (see metrics.yml's BENCH_BOTS note): a
        // key the wasm build does not dispatch is benched as `random` under
        // that bot's name. semtex/fulminate sat in this default long after
        // they were culled, which is where the phantom deltas came from.
        // bench_bot_e2e.ts now refuses an unknown key outright.
        BENCH_BOTS: process.env.BENCH_BOTS || 'octogen,cordite,blackpowder,firecracker',
        BENCH_BOT_MOVES: process.env.BENCH_BOT_MOVES || '25',
      });
      // bench prints exactly one JSON line; take the last non-empty line.
      const line = out.trim().split('\n').filter(Boolean).pop();
      const parsed = JSON.parse(line);
      if (parsed.error) throw new Error(parsed.error);
      if (parsed.memory) memory = parsed.memory;
      for (const r of parsed.e2e || []) {
        if (!perStrat.has(r.strategy)) {
          perStrat.set(r.strategy, { strategy: r.strategy, p50: [], p90: [], mean: [], max: [], n: [], beliefHydrated: false });
        }
        const s = perStrat.get(r.strategy);
        s.p50.push(r.p50); s.p90.push(r.p90); s.mean.push(r.mean); s.max.push(r.max); s.n.push(r.n);
        s.beliefHydrated = s.beliefHydrated || r.beliefHydrated;
      }
    }
    const e2e = [...perStrat.values()].map((s) => ({
      strategy: s.strategy,
      reps: s.p50.length,
      n: Math.round(median(s.n)),
      mean: median(s.mean),
      p50: median(s.p50),
      p90: median(s.p90),
      max: s.max.length ? Math.max(...s.max) : 0,
      // observed spread of the per-rep p50 → the noise band the renderer honors.
      p50lo: s.p50.length ? Math.min(...s.p50) : null,
      p50hi: s.p50.length ? Math.max(...s.p50) : null,
      beliefHydrated: s.beliefHydrated,
    }));
    return { e2e, memory: memory ?? { error: 'no memory' } };
  } catch (e) { return { error: String(e.message || e) }; }
}

const em = e2eAndMemory();
const metrics = {
  size: size(),
  linearMemory: linearMemory(),
  speed: speed(),
  memory: em.memory ?? { error: em.error ?? 'no memory' },
  e2e: em.e2e ?? { error: em.error ?? 'no e2e' },
};
process.stdout.write(JSON.stringify(metrics, null, 2) + '\n');
