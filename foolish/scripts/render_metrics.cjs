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
const kb = (b) => b == null ? 'n/a' : `${(b / 1024).toFixed(1)} KB`;
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


function sizeRows(base, head) {
  // `web` first: it is the kernel a visitor downloads on first visit; the
  // Oracle's two are fetched on the first Oracle press.
  return ['web', 'oracle', 'oracle-mt'].map((m) => {
    const b = base?.size?.[m], h = head?.size?.[m];
    return `| \`${m}.wasm\` | ${kb(b?.raw)} / ${kb(b?.gz)} | ${kb(h?.raw)} / ${kb(h?.gz)} | ${deltaCell(b?.gz, h?.gz, { fmt: kb })} |`;
  }).join('\n');
}

// First-load JS per route plus the union, from scripts/measure_web_bundle.mjs.
function webBundleRows(base, head) {
  const b = base?.webBundle, h = head?.webBundle;
  const rows = [...Object.keys(h?.routes || b?.routes || {}).map((r) => [`\`${r}\``, b?.routes?.[r], h?.routes?.[r]]),
    ['total (union)', b?.total, h?.total]];
  return rows.map(([label, bv, hv]) =>
    `| ${label} | ${kb(bv?.raw)} / ${kb(bv?.gz)} | ${kb(hv?.raw)} / ${kb(hv?.gz)} | ${deltaCell(bv?.gz, hv?.gz, { fmt: kb })} |`).join('\n');
}

// DECLARED linear memory per module (pages · KB), from the built artifacts. A
// pinned module (--initial-memory == --max-memory) is flagged 📌.
function declaredMemRows(base, head) {
  const pageStr = (lm) => lm == null ? 'n/a' : `${lm.pages} pg · ${kb(lm.bytes)}${lm.pinned ? ' 📌' : ''}`;
  return ['web'].map((m) => {
    const b = base?.linearMemory?.[m], h = head?.linearMemory?.[m];
    return `| \`${m}.wasm\` | ${pageStr(b)} | ${pageStr(h)} | ${deltaCell(b?.bytes, h?.bytes, { fmt: kb })} |`;
  }).join('\n');
}

function render(base, head, coverageSlot = null) {
  const bSpd = base?.speed || {}, hSpd = head?.speed || {};
  const reps = hSpd.reps || bSpd.reps;
  const cov = coverageSlot || COV_PLACEHOLDER;

  return `${MARKER}
## 📊 Metrics — before (base) vs after (this PR)

### 🧠 Engine speed
_Higher is better${reps ? `; median of ${reps} runs, ±½ range` : ''}._

| metric | base | this PR | Δ |
|---|---|---|---|
${spdRow('games/sec', bSpd.gamesPerSec, hSpd.gamesPerSec)}
${spdRow('actions/sec', bSpd.actionsPerSec, hSpd.actionsPerSec)}
${spdRow('legal-evals/sec', bSpd.legalEvalsPerSec, hSpd.legalEvalsPerSec)}

### 💾 wasm linear memory — declared (initial)
_Lower is better; page-granular (64 KB). 📌 = pinned (\`--initial-memory == --max-memory\`)._

| module | base | this PR | Δ |
|---|---|---|---|
${declaredMemRows(base, head)}

### 📦 wasm size (raw / gzip)
_Lower is better; Δ is on gzip._

| module | base raw/gz | this PR raw/gz | Δ gz |
|---|---|---|---|
${sizeRows(base, head)}

### 🌐 web bundle, first-load JS (raw / gzip)
_Lower is better; Δ is on gzip. Runtime chunks plus each route's entry chunks from \`next build\`._

| route | base raw/gz | this PR raw/gz | Δ gz |
|---|---|---|---|
${webBundleRows(base, head)}

### 🧪 Coverage
${COV_OPEN}
${cov}
${COV_CLOSE}

<sub>⚪ = change within run-to-run noise (latency/speed are wall-clock on a shared CI runner). Trust the direction, not the last digit.</sub>`;
}

// Coverage table from {cEngine:{line,branch}, client:{line,branch}} + floors.
function renderCoverage(cov) {
  const row = (label, v, floorL, floorB) => {
    if (!v) return `| ${label} | n/a | | |`;
    const okL = v.line >= floorL, okB = v.branch >= floorB;
    return `| ${label} | ${v.line?.toFixed(1)}% ${okL ? '✅' : '❌'} | ${v.branch?.toFixed(1)}% ${okB ? '✅' : '❌'} | ${floorL}% / ${floorB}% |`;
  };
  return `| scope | lines | branches | floor (L/B) |
|---|---|---|---|
${row('C engine (game/legal/replay)', cov.cEngine, cov.floors?.cEngineLine ?? 90, cov.floors?.cEngineBranch ?? 74)}
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
