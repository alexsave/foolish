#!/usr/bin/env node
// Read the seed-keyed divergence accumulators (data/div/<bot>_pc<pc>_tt<b>.div,
// lines "<seed> <0|1>") and emit a measured-series JSON the page overlays as raw
// markers. Dedup already happened on write; here we just count distinct seeds and
// distinct diverged seeds per (cell, bits). Each point carries its own game count
// because different table sizes may have accumulated different numbers of seeds.
import fs from 'node:fs';
import path from 'node:path';

const dir = path.join(path.dirname(new URL(import.meta.url).pathname), 'data', 'div');
let files = [];
try { files = fs.readdirSync(dir).filter(f => f.endsWith('.div')); }
catch (e) { if (e.code !== 'ENOENT') throw e; }

const cells = new Map();   // "bot_pcN" -> { bot, pc, points: [] }
for (const f of files) {
  const m = f.match(/^([a-z]+)_pc(\d+)_tt(\d+)\.div$/);
  if (!m) continue;
  const bot = m[1], pc = +m[2], bits = +m[3];
  const seen = new Map();  // seed -> flag (dedup guard)
  for (const line of fs.readFileSync(path.join(dir, f), 'utf8').split('\n')) {
    const mm = line.match(/^(\d+)\s+([01])\s*$/);
    if (mm) seen.set(mm[1], +mm[2]);
  }
  const n = seen.size;
  if (!n) continue;
  let d = 0; for (const v of seen.values()) if (v) d++;
  const key = `${bot}_pc${pc}`;
  if (!cells.has(key)) cells.set(key, { bot, pc, ref_bits: 22, measured: true, points: [] });
  cells.get(key).points.push({ bits, diverged: d, games: n });
}
for (const c of cells.values()) c.points.sort((a, b) => a.bits - b.bits);
process.stdout.write(JSON.stringify({ series: [...cells.values()] }, null, 0));
