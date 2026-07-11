// Render two collect_metrics.mjs blobs (base = target branch, head = this PR)
// into a before/after Markdown comment. Pure functions so they can be unit-run
// locally and required from the metrics + coverage workflows.
//
// Two workflows co-own the one comment: the metrics job fills the perf tables,
// the coverage job fills the coverage slot when its (slower) run finishes.
// Neither clobbers the other — each preserves the other's slot on update (see
// spliceSlot / extractSlot).
'use strict';

const MARKER = '<!-- metrics-report -->';
const COV_OPEN = '<!-- cov-slot -->';
const COV_CLOSE = '<!-- /cov-slot -->';
const COV_PLACEHOLDER = '_running in the `coverage` check — numbers land here when it finishes._';

// Replace the content between COV_OPEN/COV_CLOSE in `body` with `content`.
function spliceSlot(body, content) {
  const open = body.indexOf(COV_OPEN), close = body.indexOf(COV_CLOSE);
  if (open === -1 || close === -1) return body;
  return body.slice(0, open + COV_OPEN.length) + '\n' + content + '\n' + body.slice(close);
}
// Pull the current coverage-slot content out of an existing comment body.
function extractSlot(body) {
  const open = body.indexOf(COV_OPEN), close = body.indexOf(COV_CLOSE);
  if (open === -1 || close === -1) return null;
  return body.slice(open + COV_OPEN.length, close).trim();
}

// ---- formatting ----
const mb = (bytes) => bytes == null || bytes < 0 ? 'n/a' : `${(bytes / 1048576).toFixed(2)} MB`;
const kb = (b) => b == null ? 'n/a' : `${(b / 1024).toFixed(1)} KB`;
const ms = (n) => n == null ? 'n/a' : `${n.toFixed(1)} ms`;
const n0 = (n) => n == null ? 'n/a' : Math.round(n).toLocaleString('en-US');

// delta cell with a NOISE BAND: a change smaller than `band` (fraction of base)
// is wall-clock noise, not a win/regression — show it grey (⚪ ≈), never
// green/red. `lowerIsBetter` flips which direction earns 🟢.
function deltaCell(base, head, { lowerIsBetter = true, fmt = String, pct = true, band = 0 } = {}) {
  if (base == null || head == null) return 'n/a';
  const d = head - base;
  if (Math.abs(d) < 1e-9) return '—';
  const rel = base !== 0 ? Math.abs(d) / Math.abs(base) : Infinity;
  const pctStr = pct && base !== 0 ? ` (${d > 0 ? '+' : ''}${(100 * d / base).toFixed(1)}%)` : '';
  const val = `${d > 0 ? '+' : ''}${fmt(d)}${pctStr}`;
  if (rel < band) return `⚪ ${val} ≈`; // within noise
  const improved = lowerIsBetter ? d < 0 : d > 0;
  return `${improved ? '🟢' : '🔴'} ${val}`;
}

// speed stat: base/head cell shows "median ±halfSpread"
const spd = (s) => s == null ? 'n/a' : `${n0(s.med)}${s.hi != null ? ` ±${n0((s.hi - s.lo) / 2)}` : ''}`;
// Neutral when the two [lo,hi] ranges OVERLAP — the medians are statistically
// indistinguishable, so a no-op PR never shows a green/red "win". Only a clean
// separation earns an arrow.
const spdRow = (label, b, h) => {
  let cell;
  if (b && h && h.lo != null && b.lo != null && h.lo <= b.hi && b.lo <= h.hi) {
    const d = (h.med ?? 0) - (b.med ?? 0);
    const pct = b.med ? ` (${d > 0 ? '+' : ''}${(100 * d / b.med).toFixed(1)}%)` : '';
    cell = d === 0 ? '—' : `⚪ ${d > 0 ? '+' : ''}${n0(d)}${pct} ≈`;
  } else {
    cell = deltaCell(b?.med, h?.med, { lowerIsBetter: false, fmt: n0, band: 0.03 });
  }
  return `| ${label} | ${spd(b)} | ${spd(h)} | ${cell} |`;
};

// base/head latency cell: median p50, plus ±½ the per-rep spread when present.
const lat = (r) => r?.p50 == null ? 'n/a' : `${ms(r.p50)}${r.p50hi != null && r.p50lo != null ? ` ±${((r.p50hi - r.p50lo) / 2).toFixed(1)}` : ''}`;
function e2eRows(base, head) {
  const byStrat = (blob) => Object.fromEntries((Array.isArray(blob?.e2e) ? blob.e2e : []).map((r) => [r.strategy, r]));
  const b = byStrat(base), h = byStrat(head);
  const strats = [...new Set([...Object.keys(b), ...Object.keys(h)])];
  if (!strats.length) return '| _no data_ | | | |';
  return strats.map((s) => {
    // p50 is the robust central latency (immune to the odd 600ms MC outlier);
    // now the MEDIAN p50 across E2E_REPS runs, so run-to-run jitter is squeezed.
    const br = b[s], hr = h[s];
    const belief = hr?.beliefHydrated ? ' 🧠' : '';
    let cell;
    // When the two per-rep [lo,hi] p50 ranges OVERLAP the medians are
    // indistinguishable noise — show neutral, never a green/red "win" (same
    // rule the engine-speed rows use for their lo/hi bands).
    if (br && hr && hr.p50lo != null && br.p50lo != null && hr.p50lo <= br.p50hi && br.p50lo <= hr.p50hi) {
      const d = (hr.p50 ?? 0) - (br.p50 ?? 0);
      const pct = br.p50 ? ` (${d > 0 ? '+' : ''}${(100 * d / br.p50).toFixed(1)}%)` : '';
      cell = Math.abs(d) < 1e-9 ? '—' : `⚪ ${d > 0 ? '+' : ''}${d.toFixed(1)} ms${pct} ≈`;
    } else {
      cell = deltaCell(br?.p50, hr?.p50, { fmt: (x) => `${x.toFixed(1)} ms`, band: 0.08 });
    }
    return `| \`${s}\`${belief} | ${lat(br)} | ${lat(hr)} | ${cell} |`;
  }).join('\n');
}

function sizeRows(base, head) {
  return ['rules', 'guards', 'bots'].map((m) => {
    const b = base?.size?.[m], h = head?.size?.[m];
    return `| \`${m}.wasm\` | ${kb(b?.raw)} / ${kb(b?.gz)} | ${kb(h?.raw)} / ${kb(h?.gz)} | ${deltaCell(b?.gz, h?.gz, { fmt: kb })} |`;
  }).join('\n');
}

function memRow(label, bBytes, hBytes) {
  return `| ${label} | ${mb(bBytes)} | ${mb(hBytes)} | ${deltaCell(bBytes, hBytes, { fmt: (x) => mb(Math.abs(x)).replace(' MB', '') + ' MB' })} |`;
}

// DECLARED linear memory per module (pages · KB), from the committed artifacts.
// bots.wasm is initial-only (it grows a TT at runtime — see the peak table); a
// pinned module (--initial-memory == --max-memory) is flagged 📌.
function declaredMemRows(base, head) {
  const pageStr = (lm) => lm == null ? 'n/a' : `${lm.pages} pg · ${kb(lm.bytes)}${lm.pinned ? ' 📌' : ''}`;
  return ['rules', 'guards', 'bots'].map((m) => {
    const b = base?.linearMemory?.[m], h = head?.linearMemory?.[m];
    return `| \`${m}.wasm\` | ${pageStr(b)} | ${pageStr(h)} | ${deltaCell(b?.bytes, h?.bytes, { fmt: kb })} |`;
  }).join('\n');
}

function render(base, head, coverageSlot = null) {
  const bMem = base?.memory || {}, hMem = head?.memory || {};
  const bSpd = base?.speed || {}, hSpd = head?.speed || {};
  const reps = hSpd.reps || bSpd.reps;
  const e2eReps = (Array.isArray(head?.e2e) && head.e2e[0]?.reps) || (Array.isArray(base?.e2e) && base.e2e[0]?.reps);
  const cov = coverageSlot || COV_PLACEHOLDER;

  return `${MARKER}
## 📊 Metrics — before (base) vs after (this PR)

### ⏱️ Thinking-bot end-to-end latency (p50)
_Full move vs real Postgres: load → belief → kernel choose → apply → commit${e2eReps ? `; median p50 of ${e2eReps} runs, ±½ range` : ''}. Lower is better; 🧠 = belief log hydrated._

| bot | base | this PR | Δ |
|---|---|---|---|
${e2eRows(base, head)}

### 🧠 Engine speed
_Higher is better${reps ? `; median of ${reps} runs, ±½ range` : ''}._

| metric | base | this PR | Δ |
|---|---|---|---|
${spdRow('games/sec', bSpd.gamesPerSec, hSpd.gamesPerSec)}
${spdRow('actions/sec', bSpd.actionsPerSec, hSpd.actionsPerSec)}
${spdRow('legal-evals/sec', bSpd.legalEvalsPerSec, hSpd.legalEvalsPerSec)}

### 💾 wasm linear memory — declared (initial)
_Lower is better; page-granular (64 KB). 📌 = pinned (\`--initial-memory == --max-memory\`); \`bots.wasm\` grows a TT at runtime — see peak below._

| module | base | this PR | Δ |
|---|---|---|---|
${declaredMemRows(base, head)}

### 💾 wasm linear memory (runtime peak)
_Lower is better; page-granular (64 KB)._

| module | base | this PR | Δ |
|---|---|---|---|
${memRow('bots.wasm', bMem.botsWasmBytes, hMem.botsWasmBytes)}
${memRow('kernel.wasm', bMem.kernelWasmBytes, hMem.kernelWasmBytes)}

### 📦 wasm size (raw / gzip)
_Lower is better; Δ is on gzip._

| module | base raw/gz | this PR raw/gz | Δ gz |
|---|---|---|---|
${sizeRows(base, head)}

### 🧪 Coverage
${COV_OPEN}
${cov}
${COV_CLOSE}

<sub>⚪ = change within run-to-run noise (latency/speed are wall-clock on a shared CI runner). Trust the direction, not the last digit.</sub>`;
}

// Coverage table from {cEngine:{line,branch}, server:{line,branch}, client:{line,branch}} + floors.
function renderCoverage(cov) {
  const row = (label, v, floorL, floorB) => {
    if (!v) return `| ${label} | n/a | | |`;
    const okL = v.line >= floorL, okB = v.branch >= floorB;
    return `| ${label} | ${v.line?.toFixed(1)}% ${okL ? '✅' : '❌'} | ${v.branch?.toFixed(1)}% ${okB ? '✅' : '❌'} | ${floorL}% / ${floorB}% |`;
  };
  return `| scope | lines | branches | floor (L/B) |
|---|---|---|---|
${row('C engine (game/legal/replay)', cov.cEngine, cov.floors?.cEngineLine ?? 90, cov.floors?.cEngineBranch ?? 74)}
${row('server _shared', cov.server, cov.floors?.serverLine ?? 80, cov.floors?.serverBranch ?? 78)}
${row('client rules', cov.client, cov.floors?.clientLine ?? 90, cov.floors?.clientBranch ?? 82)}`;
}

module.exports = { render, renderCoverage, spliceSlot, extractSlot, MARKER, COV_OPEN, COV_CLOSE };

// CLI: node scripts/render_metrics.cjs base.json head.json
if (require.main === module) {
  const fs = require('fs');
  const [, , baseP, headP] = process.argv;
  const base = JSON.parse(fs.readFileSync(baseP, 'utf8'));
  const head = JSON.parse(fs.readFileSync(headP, 'utf8'));
  process.stdout.write(render(base, head) + '\n');
}
