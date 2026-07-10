#!/usr/bin/env node
// Emit the octogen-pc2 divergence decision table as Markdown (-> stdout and
// docs/OCTOGEN_PC2_DIVERGENCE.md): measured divergence % with a 95% Wilson
// interval per CD_TT_BITS, the model P(W>M) for comparison, and avg decision
// latency from the CD_LAT pass. Reads the same data/ files the HTML page uses.
import fs from 'node:fs';
import path from 'node:path';
const HERE = path.dirname(new URL(import.meta.url).pathname);
const rd = (p, d) => { try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch { return d; } };

const div = rd(path.join(HERE, 'data', 'divergence.json'), { series: [] });
const ccdf = rd(path.join(HERE, 'data', 'ccdf.json'), {});
const lat = rd(path.join(HERE, 'data', 'latency.json'), {});
const CELL = process.argv[2] || 'octogen_pc2';
const [bot, pcs] = CELL.split('_pc');
const pc = +pcs;

const s = (div.series || []).find(x => x.bot === bot && x.pc === pc);
const model = ccdf[CELL];
const L = lat[CELL] || {};

function wilson(k, n) { // 95%
  if (!n) return { p: 0, lo: 0, hi: 0 };
  const p = k / n, z = 1.96;
  if (k === 0) return { p: 0, lo: 0, hi: 3 / n };
  const c = z * z / n, mid = (p + c / 2) / (1 + c);
  const half = z * Math.sqrt(p * (1 - p) / n + c / (4 * n)) / (1 + c);
  return { p, lo: Math.max(0, mid - half), hi: Math.min(1, mid + half) };
}
const pct = x => x <= 0 ? '0' : (x * 100 < 0.01 ? (x * 100).toFixed(4) : x * 100 < 1 ? (x * 100).toFixed(3) : (x * 100).toFixed(2)) + '%';
const bytes = b => { const B = 2 ** b * 16; return B >= 1 << 20 ? (B / (1 << 20)) + ' MiB' : B >= 1024 ? (B / 1024) + ' KiB' : B + ' B'; };
const ent = b => { const e = 2 ** b; return e >= 1e6 ? (e / 1e6) + 'M' : e >= 1e3 ? (e / 1e3) + 'k' : '' + e; };

let md = `# octogen · pc 2 — transposition-table divergence\n\n`;
md += `Directly-measured fraction of games whose move sequence differs from the TT22 reference, `;
md += `by \`CD_TT_BITS\`, with a 95% Wilson interval. Seed-keyed and deduped, so this is poolable and `;
md += `keeps tightening as more seeds run. Model column is the baseline-free upper bound P(W > 2^bits) `;
md += `from the working-set distribution; latency is avg protagonist decision CPU time (CD_LAT pass).\n\n`;
if (!s || !s.points || !s.points.length) {
  md += `_No divergence data yet. Run \`tools/tt_divergence_viz/accrue_div.sh\`._\n`;
} else {
  const gmax = Math.max(...s.points.map(p => p.games));
  md += `Reference: TT${s.ref_bits || 22} · up to **${gmax.toLocaleString()} seeds** per size.\n\n`;
  md += `| CD_TT_BITS | entries | bytes | games | diverged | divergence | 95% interval | model P(W>M) | avg decision |\n`;
  md += `|---|--:|--:|--:|--:|--:|--:|--:|--:|\n`;
  for (const p of s.points.slice().sort((a, b) => a.bits - b.bits)) {
    const w = wilson(p.diverged, p.games);
    const mc = model ? model.ccdf.find(d => d.bits === p.bits) : null;
    const mp = mc ? pct(mc.p) : '—';
    const ci = p.diverged > 0 ? `${pct(w.lo)} – ${pct(w.hi)}` : `< ${pct(w.hi)}`;
    const ll = L[String(p.bits)];
    const lat_s = ll && ll.avg_ms != null ? `${ll.avg_ms.toFixed(ll.avg_ms < 10 ? 2 : 1)} ms` : '—';
    const ship = p.bits === 13 ? ' **◄ shipped**' : '';
    md += `| **TT${p.bits}**${ship} | ${ent(p.bits)} | ${bytes(p.bits)} | ${p.games.toLocaleString()} | ${p.diverged.toLocaleString()} | ${pct(w.p)} | ${ci} | ${mp} | ${lat_s} |\n`;
  }
  md += `\n_“diverged” counts games where octogen played a different move sequence than at TT22. `;
  md += `A zero row means none seen yet — the interval is the rule-of-three upper bound, not proof of zero._\n`;
}
const outfile = path.resolve(HERE, '..', '..', '..', 'docs', 'OCTOGEN_PC2_DIVERGENCE.md');
fs.writeFileSync(outfile, md);
process.stdout.write(md);
