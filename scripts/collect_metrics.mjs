// Collect the repo's headline health metrics as ONE JSON blob, so CI can run it
// on the PR head and the base and diff them into a before/after comment. Run
// from the repo root; measures THIS checkout's committed artifacts + source.
//
//   E2E_PG* set + TSX_TSCONFIG_PATH=e2e/tsconfig.json node scripts/collect_metrics.mjs
//
// Metrics:
//   size   — the three wasm modules (rules/guards embedded b64, bots.wasm.gz), raw + gzip bytes
//   speed  — engine throughput (games/sec, actions/sec) from e2e/bench_engine.ts
//   memory — peak bots/kernel wasm linear memory (MB) after the MC bots ran
//   e2e    — THE headline: full thinking-bot move latency vs real Postgres
//            (load → belief → kernel choose → apply → commit), per strategy
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { execFileSync } from 'node:child_process';

const WASM = 'supabase/functions/_shared/wasm';
const tsxEnv = { ...process.env, TSX_TSCONFIG_PATH: 'e2e/tsconfig.json' };
const runNode = (args, extraEnv = {}) =>
  execFileSync('node', ['--import', 'tsx', ...args],
    { encoding: 'utf8', env: { ...tsxEnv, ...extraEnv }, maxBuffer: 64 * 1024 * 1024, stdio: ['ignore', 'pipe', 'ignore'] });

// ---- size: committed wasm artifacts (no toolchain needed) ----
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
function speed() {
  try {
    const out = runNode(['e2e/bench_engine.ts'], { BENCH_GAMES: process.env.BENCH_GAMES || '300' });
    const g = out.match(/games\/sec:\s*([\d.]+)/);
    const a = out.match(/actions\/sec:\s*([\d.]+)/);
    const l = out.match(/legal-evals\/sec:\s*([\d.]+)/);
    return { gamesPerSec: g ? +g[1] : null, actionsPerSec: a ? +a[1] : null, legalEvalsPerSec: l ? +l[1] : null };
  } catch (e) { return { error: String(e.message || e) }; }
}

// ---- e2e latency + wasm memory (one Postgres-backed run) ----
function e2eAndMemory() {
  try {
    const out = runNode(['e2e/bench_bot_e2e.ts'], {
      BENCH_JSON: '1',
      BENCH_BOTS: process.env.BENCH_BOTS || 'octogen,semtex,cordite,fulminate',
      BENCH_BOT_MOVES: process.env.BENCH_BOT_MOVES || '25',
    });
    // bench prints exactly one JSON line; take the last non-empty line.
    const line = out.trim().split('\n').filter(Boolean).pop();
    return JSON.parse(line);
  } catch (e) { return { error: String(e.message || e) }; }
}

const em = e2eAndMemory();
const metrics = {
  size: size(),
  speed: speed(),
  memory: em.memory ?? { error: em.error ?? 'no memory' },
  e2e: em.e2e ?? { error: em.error ?? 'no e2e' },
};
process.stdout.write(JSON.stringify(metrics, null, 2) + '\n');
