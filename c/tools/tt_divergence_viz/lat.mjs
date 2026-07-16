#!/usr/bin/env node
// Fold the latency accumulators (data/lat/<bot>_pc<pc>_tt<b>.lat, lines
// "<total_ns> <decisions> <games>") into data/latency.json:
//   { "<bot>_pc<pc>": { "<bits>": { avg_ms, games, decisions } } }
import fs from 'node:fs';
import path from 'node:path';

const dir = path.join(path.dirname(new URL(import.meta.url).pathname), 'data', 'lat');
let files = [];
try { files = fs.readdirSync(dir).filter(f => f.endsWith('.lat')); }
catch (e) { if (e.code !== 'ENOENT') throw e; }

const out = {};
for (const f of files) {
  const m = f.match(/^([a-z]+)_pc(\d+)_tt(\d+)\.lat$/);
  if (!m) continue;
  const cell = `${m[1]}_pc${m[2]}`, bits = m[3];
  let ns = 0, dec = 0, games = 0;
  for (const line of fs.readFileSync(path.join(dir, f), 'utf8').split('\n')) {
    const mm = line.match(/^(\d+)\s+(\d+)\s+(\d+)\s*$/);
    if (mm) { ns += +mm[1]; dec += +mm[2]; games += +mm[3]; }
  }
  if (!dec) continue;
  (out[cell] ||= {})[bits] = { avg_ms: ns / dec / 1e6, games, decisions: dec };
}
process.stdout.write(JSON.stringify(out, null, 0));
