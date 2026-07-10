#!/usr/bin/env node
// Turn per-game, seed-keyed working-set records into empirical CCDF curves.
//
// Input: data/W/<bot>_pc<pc>.gw — one line per game, "<seed> <W>", where W is
// the largest key-set that had to coexist in the direct-mapped table during that
// game (0 if the game never invoked the endgame solver). The seed is the game's
// identity; the whole tuple (bot, player-count, seed) identifies a game, and
// each cell file already fixes bot + player-count, so seed is the key WITHIN a
// cell. We DEDUP on seed — a game is a deterministic function of its seed, so a
// re-measured seed is the same outcome and must be counted once. Different
// player counts live in different files and are never pooled.
//
// A game plays identically to an unbounded table unless its working set forces a
// reused-key eviction; the necessary condition is overflow, so
//   CCDF(bits) = P(W > 2^bits) = (#games with W > 2^bits) / (#distinct games)
// is a baseline-free upper bound on the per-game divergence rate. n = distinct
// games (INCLUDING the W=0 no-solve games — they can't diverge, so they belong
// in the denominator), which is what the measured divergence rate compares to.
import fs from 'node:fs';
import path from 'node:path';

const dir = process.argv[2] || path.join(path.dirname(new URL(import.meta.url).pathname), 'data', 'W');
let files = [];
try { files = fs.readdirSync(dir).filter(f => f.endsWith('.gw')); }
catch (e) { if (e.code !== 'ENOENT') throw e; }   // no data yet -> empty CCDF

function parseGW(txt) {
  const bySeed = new Map();               // seed -> W (dedup: deterministic, so identical on repeat)
  for (const line of txt.split('\n')) {
    const m = line.match(/^(?:GW\s+)?(\d+)\s+(-?\d+)\s*$/);   // "<seed> <W>" or "GW <seed> <W>"
    if (m) bySeed.set(m[1], +m[2]);
  }
  return bySeed;
}

function pctile(sorted, q) {               // sorted ascending array of W
  if (!sorted.length) return 0;
  const idx = Math.min(sorted.length - 1, Math.floor(q * sorted.length));
  return sorted[idx];
}

const out = {};
for (const f of files) {
  const m = f.match(/^([a-z]+)_pc(\d+)\.gw$/);
  if (!m) continue;
  const bot = m[1], pc = +m[2];
  const bySeed = parseGW(fs.readFileSync(path.join(dir, f), 'utf8'));
  const n = bySeed.size;
  if (n === 0) continue;
  const Ws = [...bySeed.values()];
  const sorted = Ws.slice().sort((a, b) => a - b);
  const maxW = sorted[sorted.length - 1];
  const ccdf = [];
  for (let b = 1; b <= 24; b++) {
    const thr = 2 ** b;
    let gt = 0;
    for (const w of Ws) if (w > thr) gt++;
    ccdf.push({ bits: b, entries: thr, p: gt / n, k: gt });
  }
  out[`${bot}_pc${pc}`] = {
    bot, pc, games: n, maxW,
    median: pctile(sorted, 0.5),
    p90: pctile(sorted, 0.90),
    p99: pctile(sorted, 0.99),
    p999: pctile(sorted, 0.999),
    ccdf,
  };
}
process.stdout.write(JSON.stringify(out, null, 0));
console.error(`processed ${Object.keys(out).length} cells: ${Object.keys(out).join(', ')}`);
