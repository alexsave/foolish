// Render two collect_metrics.mjs blobs (base = target branch, head = this PR)
// into a before/after Markdown comment. Pure function so it can be unit-run
// locally and required from the metrics workflow's github-script step.
'use strict';

const MARKER = '<!-- metrics-report -->';

const kb = (b) => b == null ? 'n/a' : `${(b / 1024).toFixed(1)} KB`;
const ms = (n) => n == null ? 'n/a' : `${n.toFixed(1)} ms`;
const n0 = (n) => n == null ? 'n/a' : Math.round(n).toLocaleString('en-US');

// delta cell: lowerIsBetter flips the sign that counts as an improvement.
function delta(base, head, { lowerIsBetter = true, fmt = (x) => String(x), pct = true } = {}) {
  if (base == null || head == null) return 'n/a';
  const d = head - base;
  if (Math.abs(d) < 1e-9) return '—';
  const improved = lowerIsBetter ? d < 0 : d > 0;
  const arrow = improved ? '🟢' : '🔴';
  const pctStr = pct && base !== 0 ? ` (${d > 0 ? '+' : ''}${(100 * d / base).toFixed(1)}%)` : '';
  return `${arrow} ${d > 0 ? '+' : ''}${fmt(d)}${pctStr}`;
}

function sizeRows(base, head) {
  const mods = ['rules', 'guards', 'bots'];
  const rows = mods.map((m) => {
    const b = base?.size?.[m], h = head?.size?.[m];
    return `| \`${m}.wasm\` | ${kb(b?.raw)} / ${kb(b?.gz)} | ${kb(h?.raw)} / ${kb(h?.gz)} | ${delta(b?.gz, h?.gz, { fmt: kb })} |`;
  });
  return rows.join('\n');
}

function e2eRows(base, head) {
  const byStrat = (blob) => Object.fromEntries((Array.isArray(blob?.e2e) ? blob.e2e : []).map((r) => [r.strategy, r]));
  const b = byStrat(base), h = byStrat(head);
  const strats = [...new Set([...Object.keys(b), ...Object.keys(h)])];
  if (!strats.length) return '| _no data_ | | | |';
  return strats.map((s) => {
    const bm = b[s]?.mean, hm = h[s]?.mean;
    const belief = h[s]?.beliefHydrated ? ' 🧠' : '';
    return `| \`${s}\`${belief} | ${ms(bm)} | ${ms(hm)} | ${delta(bm, hm, { fmt: (x) => `${x.toFixed(1)} ms` })} |`;
  }).join('\n');
}

function render(base, head) {
  const bMem = base?.memory || {}, hMem = head?.memory || {};
  const bSpd = base?.speed || {}, hSpd = head?.speed || {};

  return `${MARKER}
## 📊 Metrics — before (base) vs after (this PR)

### ⏱️ Thinking-bot end-to-end latency
_Full move vs real Postgres: load → belief → kernel choose → apply → commit. Lower is better; 🧠 = belief log hydrated._

| bot | base | this PR | Δ |
|---|---|---|---|
${e2eRows(base, head)}

### 🧠 Engine speed
_Higher is better._

| metric | base | this PR | Δ |
|---|---|---|---|
| games/sec | ${n0(bSpd.gamesPerSec)} | ${n0(hSpd.gamesPerSec)} | ${delta(bSpd.gamesPerSec, hSpd.gamesPerSec, { lowerIsBetter: false, fmt: n0 })} |
| actions/sec | ${n0(bSpd.actionsPerSec)} | ${n0(hSpd.actionsPerSec)} | ${delta(bSpd.actionsPerSec, hSpd.actionsPerSec, { lowerIsBetter: false, fmt: n0 })} |
| legal-evals/sec | ${n0(bSpd.legalEvalsPerSec)} | ${n0(hSpd.legalEvalsPerSec)} | ${delta(bSpd.legalEvalsPerSec, hSpd.legalEvalsPerSec, { lowerIsBetter: false, fmt: n0 })} |

### 💾 wasm linear memory (peak, MB)
_Lower is better._

| module | base | this PR | Δ |
|---|---|---|---|
| bots.wasm | ${bMem.botsWasmMB ?? 'n/a'} MB | ${hMem.botsWasmMB ?? 'n/a'} MB | ${delta(bMem.botsWasmMB, hMem.botsWasmMB, { fmt: (x) => `${x} MB`, pct: false })} |
| kernel.wasm | ${bMem.kernelWasmMB ?? 'n/a'} MB | ${hMem.kernelWasmMB ?? 'n/a'} MB | ${delta(bMem.kernelWasmMB, hMem.kernelWasmMB, { fmt: (x) => `${x} MB`, pct: false })} |

### 📦 wasm size (raw / gzip)
_Lower is better; Δ is on gzip._

| module | base raw/gz | this PR raw/gz | Δ gz |
|---|---|---|---|
${sizeRows(base, head)}

<sub>Latency/speed are wall-clock on a shared CI runner — expect a few % run-to-run noise; trust the direction, not the last digit. Coverage floors are enforced by the \`coverage\` checks.</sub>`;
}

module.exports = { render, MARKER };

// CLI: node scripts/render_metrics.cjs base.json head.json
if (require.main === module) {
  const fs = require('fs');
  const [, , baseP, headP] = process.argv;
  const base = JSON.parse(fs.readFileSync(baseP, 'utf8'));
  const head = JSON.parse(fs.readFileSync(headP, 'utf8'));
  process.stdout.write(render(base, head) + '\n');
}
