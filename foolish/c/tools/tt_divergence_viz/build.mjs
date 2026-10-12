#!/usr/bin/env node
// Build docs/tt-divergence.html — an interactive presentation of the cordite
// transposition-table divergence model: theoretical CCDF curves P(W > 2^b)
// derived from measured per-game working-set (W) distributions, overlaid with
// directly-measured move-divergence rates. Self-contained, no external assets.
//
//   node tools/tt_divergence_viz/build.mjs [out.html]
//
// Reads:  data/ccdf.json      (per-(bot,pc) working-set CCDF; see ccdf.mjs)
//         data/measured.json  (directly-measured divergence sweeps)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..', '..');
const OUT = process.argv[2] || path.join(REPO, 'docs', 'tt-divergence.html');

// Both are optional — a from-scratch checkout with no measurements still builds
// a valid (empty) page; add data with generate.sh and rebuild.
function readJSON(p, dflt) {
  try { return JSON.parse(fs.readFileSync(p, 'utf8')); }
  catch (e) { if (e.code === 'ENOENT') return dflt; throw e; }
}
const ccdf = readJSON(path.join(HERE, 'data', 'ccdf.json'), {});
const measuredBase = readJSON(path.join(HERE, 'data', 'measured.json'), { series: [] });
const divergence = readJSON(path.join(HERE, 'data', 'divergence.json'), { series: [] });
// hand-entered points + accumulating seed-keyed divergence, drawn as one layer
const measured = { series: [...(measuredBase.series || []), ...(divergence.series || [])] };
// avg protagonist decision latency per (cell,bits), filled by the final CD_LAT pass
const latency = readJSON(path.join(HERE, 'data', 'latency.json'), {});

const DATA = JSON.stringify({ ccdf, measured, latency });
// the page's script, inlined as it stands (client.js reads DATA, declared before it)
const CLIENT_JS = fs.readFileSync(path.join(HERE, 'client.js'), 'utf8');

const html = `<meta charset="utf-8">
<title>Transposition-table divergence · foolish / cnitro</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{
  --bg:#f4f3ee; --surface:#fcfcfb; --surface-2:#efeee7; --line:#d9d7cc;
  --ink:#0b0b0b; --ink-2:#52514e; --ink-3:#87867e;
  --octogen:#2a78d6; --semtex:#1baf7a; --cordite:#eda100; --fulminate:#008300;
  --ship:#e34948; --accent:#4a3aa7;
  --grid:#e6e4da; --shadow:0 1px 2px rgba(0,0,0,.06),0 8px 24px rgba(0,0,0,.06);
}
:root[data-theme=dark]{
  --bg:#111110; --surface:#1a1a19; --surface-2:#232320; --line:#34342f;
  --ink:#ffffff; --ink-2:#c3c2b7; --ink-3:#8a897f;
  --octogen:#3987e5; --semtex:#199e70; --cordite:#c98500; --fulminate:#008300;
  --ship:#e66767; --accent:#9085e9;
  --grid:#2a2a26; --shadow:0 1px 2px rgba(0,0,0,.4),0 8px 24px rgba(0,0,0,.5);
}
@media (prefers-color-scheme:dark){
  :root:not([data-theme=light]){
    --bg:#111110; --surface:#1a1a19; --surface-2:#232320; --line:#34342f;
    --ink:#ffffff; --ink-2:#c3c2b7; --ink-3:#8a897f;
    --octogen:#3987e5; --semtex:#199e70; --cordite:#c98500; --fulminate:#008300;
    --ship:#e66767; --accent:#9085e9;
    --grid:#2a2a26; --shadow:0 1px 2px rgba(0,0,0,.4),0 8px 24px rgba(0,0,0,.5);
  }
}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--ink);
  font:15px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  padding:0}
.wrap{max-width:1040px;margin:0 auto;padding:32px 20px 96px}
h1{font-size:30px;line-height:1.15;letter-spacing:-.02em;margin:0 0 6px;font-weight:700}
h2{font-size:20px;letter-spacing:-.01em;margin:44px 0 12px;font-weight:650}
h3{font-size:15px;margin:22px 0 6px;font-weight:650;color:var(--ink)}
p{margin:0 0 12px;color:var(--ink-2)}
.lede{font-size:17px;color:var(--ink-2);max-width:66ch}
code,.mono{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace;font-size:.9em}
code{background:var(--surface-2);padding:1px 5px;border-radius:4px;color:var(--ink)}
a{color:var(--octogen)}
.tag{display:inline-block;font-size:11px;font-weight:600;letter-spacing:.04em;
  text-transform:uppercase;color:var(--ink-3);border:1px solid var(--line);
  border-radius:999px;padding:2px 9px;margin:0 6px 6px 0;vertical-align:middle}
.card{background:var(--surface);border:1px solid var(--line);border-radius:14px;
  box-shadow:var(--shadow);padding:18px;margin:18px 0}
.controls{display:flex;flex-wrap:wrap;gap:18px 26px;align-items:flex-start;
  padding:16px 18px;margin:0 0 6px}
.ctl-group{display:flex;flex-direction:column;gap:7px}
.ctl-label{font-size:11px;font-weight:600;letter-spacing:.05em;text-transform:uppercase;color:var(--ink-3)}
.chips{display:flex;flex-wrap:wrap;gap:6px}
.chip{font-size:13px;font-weight:550;padding:5px 11px;border-radius:8px;cursor:pointer;
  border:1px solid var(--line);background:var(--surface);color:var(--ink-2);
  user-select:none;transition:all .12s}
.chip:hover{border-color:var(--ink-3)}
.chip[aria-pressed=true]{background:var(--accent);border-color:var(--accent);color:#fff}
.botrow{display:flex;align-items:center;gap:7px;font-size:13px;font-weight:550;
  padding:5px 10px;border-radius:8px;cursor:pointer;border:1px solid var(--line);
  background:var(--surface);color:var(--ink-2);user-select:none;transition:all .12s}
.botrow:hover{border-color:var(--ink-3)}
.botrow[aria-pressed=false]{opacity:.42}
.sw{width:20px;height:12px;flex:none;display:inline-block}
.seg{display:inline-flex;border:1px solid var(--line);border-radius:8px;overflow:hidden}
.seg button{font:inherit;font-size:13px;font-weight:550;padding:5px 13px;border:0;cursor:pointer;
  background:var(--surface);color:var(--ink-2)}
.seg button[aria-pressed=true]{background:var(--ink);color:var(--surface)}
.seg button+button{border-left:1px solid var(--line)}
.toggle{display:flex;align-items:center;gap:7px;font-size:13px;font-weight:550;color:var(--ink-2);cursor:pointer;user-select:none}
.toggle input{accent-color:var(--accent);width:15px;height:15px}
figure{margin:0}
.chart-wrap{position:relative;width:100%;overflow-x:auto}
svg{display:block;width:100%;height:auto;font:inherit}
.axis text{fill:var(--ink-3);font-size:11px}
.axis-title{fill:var(--ink-2);font-size:12px;font-weight:600}
.gridline{stroke:var(--grid);stroke-width:1}
.axisline{stroke:var(--line);stroke-width:1}
.refline{stroke-width:1.5;stroke-dasharray:4 4}
.reflabel{font-size:10.5px;font-weight:600}
.curve{fill:none;stroke-width:2}
.pt{stroke:var(--surface);stroke-width:1.5}
.tip{position:absolute;pointer-events:none;background:var(--surface);color:var(--ink);
  border:1px solid var(--line);border-radius:9px;box-shadow:var(--shadow);
  padding:9px 11px;font-size:12.5px;line-height:1.5;min-width:150px;opacity:0;
  transform:translate(-50%,-120%);transition:opacity .08s;z-index:5}
.tip b{font-weight:650}
.tip .row{display:flex;justify-content:space-between;gap:14px}
.tip .k{color:var(--ink-3)}
.crosshair{stroke:var(--ink-3);stroke-width:1;stroke-dasharray:3 3;opacity:0}
.tbl{width:100%;border-collapse:collapse;font-size:13px;margin-top:6px}
.tbl th,.tbl td{text-align:right;padding:7px 10px;border-bottom:1px solid var(--line)}
.tbl th:first-child,.tbl td:first-child{text-align:left}
.tbl th{color:var(--ink-3);font-weight:600;font-size:11px;letter-spacing:.03em;text-transform:uppercase}
.tbl td.mono{font-variant-numeric:tabular-nums}
.tbl tr:hover td{background:var(--surface-2)}
.legend{display:flex;flex-wrap:wrap;gap:14px;margin:10px 2px 2px;font-size:12.5px;color:var(--ink-2)}
.legend .li{display:flex;align-items:center;gap:6px}
.callout{border-left:3px solid var(--accent);background:var(--surface-2);
  border-radius:0 10px 10px 0;padding:12px 16px;margin:16px 0}
.callout.ship{border-color:var(--ship)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media(max-width:680px){.grid2{grid-template-columns:1fr}}
.themebtn{position:fixed;top:14px;right:14px;z-index:9;border:1px solid var(--line);
  background:var(--surface);color:var(--ink-2);border-radius:8px;width:34px;height:34px;
  cursor:pointer;font-size:15px;box-shadow:var(--shadow)}
.foot{color:var(--ink-3);font-size:12.5px;margin-top:40px;border-top:1px solid var(--line);padding-top:14px}
.hl{color:var(--ink);font-weight:600}
.eq{background:var(--surface-2);border:1px solid var(--line);border-radius:10px;
  padding:12px 16px;margin:12px 0;font-family:ui-monospace,Menlo,monospace;font-size:14px;overflow-x:auto}
sub,sup{font-size:.72em}
.small{font-size:12.5px;color:var(--ink-3)}
</style>
<button class="themebtn" id="themebtn" title="Toggle theme">◐</button>
<div class="wrap">

<span class="tag">foolish / cnitro</span><span class="tag">cordite endgame solver</span><span class="tag">L1 budget</span>
<h1>How small can the transposition table get<br>before the bot plays a different game?</h1>
<p class="lede">The cordite Monte-Carlo endgame solver keeps a fixed-size, direct-mapped
transposition table. Shrinking it is how <code>bots.wasm</code> fits its working set toward L1
cache — but shrink too far and the solver evicts entries it needed, picks a different move, and
the bot diverges from how it would play with an unbounded table. This page derives the divergence
probability from first principles, plots it against directly-measured game data, and shows why
<em>no</em> finite table — not even the one we call &ldquo;infinite&rdquo; — is provably perfect.</p>

<h2>The interactive curve</h2>
<p>Two independent things are drawn, and they're styled to be told apart:</p>
<ul style="color:var(--ink-2);margin:0 0 12px;padding-left:22px">
  <li><b>The model (expected)</b> — a <b>line</b> with a <b>shaded 95% band</b>: the probability that
    a game's solver working set overflows a table of <span class="mono">2<sup>bits</sup></span> entries,
    with the sampling uncertainty from a finite number of games. This is derived, baseline-free.</li>
  <li><b>Raw observed</b> — <b>solid markers</b> with 95% error whiskers: the divergence rate we
    actually measured by replaying thousands of real games at two table sizes and diffing the moves.
    Independent of the model; it should land inside the band.</li>
</ul>
<p>Pick <b>player counts</b> (all of 2–8) and bots, toggle a <b>log y-axis</b> to see the deep tail,
and turn the band or the raw points on and off. Where the band's upper edge floats above zero even as
the line hits the floor, that gap is the rule-of-three bound — the reason no finite table is provably
perfect.</p>

<div class="card controls" id="controls">
  <div class="ctl-group">
    <span class="ctl-label">Player count (theory curves)</span>
    <div class="chips" id="pcChips"></div>
  </div>
  <div class="ctl-group">
    <span class="ctl-label">Bot strategy</span>
    <div class="chips" id="botChips"></div>
  </div>
  <div class="ctl-group">
    <span class="ctl-label">Y axis</span>
    <div class="seg" id="yseg">
      <button data-y="log" aria-pressed="true">Log</button>
      <button data-y="lin" aria-pressed="false">Linear</button>
    </div>
  </div>
  <div class="ctl-group">
    <span class="ctl-label">Model (expected)</span>
    <label class="toggle"><input type="checkbox" id="tCurves" checked> Curve P(W&gt;M)</label>
    <label class="toggle"><input type="checkbox" id="tBand" checked> 95% band</label>
  </div>
  <div class="ctl-group">
    <span class="ctl-label">Raw observed</span>
    <label class="toggle"><input type="checkbox" id="tRaw" checked> Measured points</label>
    <label class="toggle"><input type="checkbox" id="tBounds" checked> 95% error bars</label>
  </div>
</div>

<figure class="card" style="padding:14px 10px 8px">
  <div class="chart-wrap" id="chartWrap">
    <svg id="chart" viewBox="0 0 920 500" preserveAspectRatio="xMidYMid meet" role="img"
         aria-label="Probability of move divergence versus transposition-table size"></svg>
    <div class="tip" id="tip"></div>
  </div>
  <div class="legend" id="legend"></div>
</figure>

<h2>Octogen · pc 2 — divergence by table size <span class="small" id="divSub"></span></h2>
<p>The decision table: for the binding configuration (octogen, 2 players), the directly-measured
fraction of games that play a different move sequence than they would at the TT22 reference, with a
95% Wilson interval, accumulating seed by seed. The <span style="color:var(--ship)">shipped TT13</span>
row is highlighted. <span class="mono">rule-of-3</span> upper bounds apply where zero divergences have
been seen so far. Latency is the average protagonist decision time (filled by a dedicated pass).</p>
<div class="card" style="padding:6px 4px">
  <div style="overflow-x:auto"><table class="tbl" id="divTable"></table></div>
</div>

<h2>Working-set density — how many bits each game needs</h2>
<p>The curve above is the <em>tail</em>; this is the <em>density</em> it's the tail of. For every game,
<span class="mono">bits = &lceil;log<sub>2</sub> W&rceil;</span> — the smallest table that holds its whole
working set. Read it directly as &ldquo;how big a table this game wants.&rdquo; The
<span class="mono">no-solve</span> games (never reached the endgame solver, <span class="mono">W=0</span>)
are called out separately; everything to the right of the <span style="color:var(--ship)">TT13</span>
line is a game that would overflow the shipped table. Same bot / player-count selectors; the y-axis
toggle applies here too (log is the honest default — the no-solve share dwarfs the rest on a linear
scale).</p>

<figure class="card" style="padding:14px 10px 8px">
  <div class="chart-wrap" id="histWrap">
    <svg id="histChart" viewBox="0 0 920 420" preserveAspectRatio="xMidYMid meet" role="img"
         aria-label="Distribution of per-game working set, in bits needed"></svg>
    <div class="tip" id="htip"></div>
  </div>
  <div class="legend" id="hlegend"></div>
</figure>

<div class="grid2">
  <div class="card">
    <h3 style="margin-top:2px">Working-set percentiles</h3>
    <p class="small">Distinct keys inserted per game (W). The curve is just this
    distribution's upper tail, read at the table sizes.</p>
    <table class="tbl" id="pctTable"></table>
  </div>
  <div class="card">
    <h3 style="margin-top:2px">Reference table sizes</h3>
    <table class="tbl">
      <thead><tr><th>build</th><th>bits</th><th>entries</th><th>bytes</th></tr></thead>
      <tbody class="mono">
        <tr><td>native default</td><td>22</td><td>4,194,304</td><td>64 MiB</td></tr>
        <tr><td>old production</td><td>16</td><td>65,536</td><td>1 MiB</td></tr>
        <tr><td class="hl">shipped now</td><td>13</td><td>8,192</td><td>128 KiB</td></tr>
        <tr><td>L1 line (64 KiB)</td><td>12</td><td>4,096</td><td>64 KiB</td></tr>
      </tbody>
    </table>
    <p class="small" style="margin-top:10px">Each entry is 16 bytes. The table bump-allocates
    inside <code>bots.wasm</code> linear memory on first play — it is the single biggest buffer,
    so its size sets the module's peak page count.</p>
  </div>
</div>

<h2>The model, in one line</h2>
<p>The table is <b>direct-mapped</b>: key <span class="mono">k</span> always lands in slot
<span class="mono">k mod M</span>, where <span class="mono">M = 2<sup>bits</sup></span>. A game can
only play <em>differently</em> if the solver evicts an entry it later needs — and eviction is driven
by how many distinct keys <span class="mono">W</span> the game crowds into <span class="mono">M</span>
slots. The clean, baseline-free predictor is the <b>overflow probability</b> — the complementary CDF
(survival function) of the working set:</p>
<div class="eq">P( diverge | table = 2<sup>bits</sup> )   &le;   P( W &gt; 2<sup>bits</sup> )</div>
<p>Read it as an <b>upper bound</b>, not an identity. Overflowing the table is what makes eviction
unavoidable, so it caps the divergence rate; but an eviction only <em>flips a move</em> if the evicted
entry actually gets reused before the node budget runs out — and most don't. So the true rate sits
<em>on or below</em> this line, which is exactly where the measured markers fall. The bound is worth
having because the right side is nearly free: instrument the solver to count distinct insertions per
game (<code>-DCD_TT_STATS</code>) and you get one number per game, <span class="hl">no reference table
required</span>.</p>

<div class="callout">
  <b>Why baseline-free matters.</b> The naive way to measure divergence is to diff each game against a
  &ldquo;perfect&rdquo; reference table — but that only works where you can afford to run the reference.
  <span class="hl">P(W&nbsp;&gt;&nbsp;M) never mentions a reference:</span> W is a property of the game and
  the solver alone, so the curve is defined at every table size, including ones no reference reaches. It
  also gives a per-game <em>certainty</em>, not just an average: any game whose entire W fits below M
  cannot be forced into an eviction cascade — at pc&nbsp;4 the largest W we ever saw was
  <span class="mono" id="maxw4b">7,232</span>, under TT13's 8,192, so every pc-4 game is provably
  table-independent at the shipped size. The measured markers (which <em>do</em> use a TT22 reference)
  sit under the matching curve — that agreement is how we know the bound is tight, not how it's defined.
</div>

<h2>&ldquo;But TT22 isn&rsquo;t perfect either.&rdquo;</h2>
<p>Correct — and the model says exactly why. TT22 holds 4.2M entries; we <em>call</em> it infinite
because in tens of thousands of games we never saw a working set approach it. But
&ldquo;never saw&rdquo; is a measurement, not a proof. The honest statement about any table size is a
<b>confidence bound</b>, not a zero:</p>
<ul style="color:var(--ink-2);margin:0 0 12px;padding-left:22px">
  <li>The octogen sweep measured <span class="mono">0 / 35,000</span> divergences at TT22 vs an
    even larger table. Zero events out of N bounds the true rate by the
    <span class="hl">rule of three</span>: p&nbsp;&lt;&nbsp;3/N&nbsp;≈&nbsp;<span class="mono">8.6&times;10<sup>-5</sup></span>
    at 95% — <em>not</em> p&nbsp;=&nbsp;0.</li>
  <li>The same logic applies at the top of every curve. Turn on <b>95% bounds</b> and the measured
    zeros become upper-bound caps: the data is consistent with a small nonzero tail we simply
    haven't sampled.</li>
  <li>What we <em>can</em> prove per game: if a game's W stays below M, that game is bit-identical to
    the unbounded table — deterministically, no probability involved. At pc&nbsp;4 the largest W we
    ever observed was <span class="mono" id="maxw4">7,232</span> &lt; 8,192 (TT13), which is why
    pc&nbsp;4 shows a hard <span class="mono">0</span> at the shipped size and the curve drops to the
    floor there.</li>
</ul>
<p>So the decision isn't &ldquo;which size is perfect&rdquo; — none is. It's &ldquo;which size pushes
the divergence bound below what we care about, at the smallest memory.&rdquo;</p>

<div class="callout ship">
  <b>What shipped.</b> <span class="mono">CD_TT_BITS = 13</span> — 8,192 entries, 128&nbsp;KiB. Against
  the table that ran in production before (TT16), the shrunk table plays octogen — the most
  table-sensitive bot — <span class="hl">identically in 1,720 / 1,720 games</span>. The curves show why:
  at every shipped player count the working-set tail is exhausted well before 8,192, so TT13 and TT16
  sit on the same floor. The 1&nbsp;MiB the table used to cost is what kept <code>bots.wasm</code>'s peak
  out of L1.
</div>

<h2>How the numbers were made</h2>
<p><b>Working sets (curves).</b> The solver is built with <code>-DCD_TT_STATS</code> and run with
<code>CD_GW=1</code>, which emits one line per game — <code>GW &lt;seed&gt; &lt;W&gt;</code> — where
<span class="mono">W</span> is the largest key-set that had to coexist in the table during that game
(<span class="mono">0</span> if the game never reached the endgame solver). Games run under the production
budget (<code>CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75</code>). <code>ccdf.mjs</code> reads the records and
computes <span class="mono">P(W &gt; 2<sup>bits</sup>)</span> over the <em>distinct games</em>.</p>
<p class="small">The reference table (<code>REF_BITS</code>, default TT22) is <em>almost</em> collision-free,
not perfectly — the same &ldquo;no perfect table&rdquo; caveat, now on the ruler itself. In deep
two-player games a few keys still collide there, so TT22 undercounts <span class="mono">W</span> by a few
percent mid-range and <span class="mono">&lt;0.1%</span> for the largest working sets (measured against
TT26: 4,694 → 4,698). Immaterial for the TT13 decision — everything sits ~2&times; under the threshold —
so TT22 is the fast default; raise <code>REF_BITS</code> only to sharpen the deep TT16 tail.</p>
<p><b>Keyed on the seed, so accumulation is safe.</b> A game is a deterministic function of its
<span class="mono">(bot, opponent, player-count, seed)</span>. Each curve fixes the first three, so
records are keyed by <b>seed</b> and <b>deduped</b> — re-measuring a seed collapses to one record, and the
<span class="mono">3-player</span> outcome of a seed is never pooled with its <span class="mono">7-player</span>
outcome (different games, different files). That means you can keep adding runs of any size and the counts
reconcile correctly: pooling raw counts over the union of seeds, never averaging rates. n = distinct games,
so the confidence band tightens honestly as you measure more — including the <span class="mono">W=0</span>
no-solve games, which can't diverge and so belong in the denominator.</p>
<p><b>Divergence (markers).</b> <code>tools/tt_divergence.sh</code> builds the evaluator at two
<code>CD_TT_BITS</code> values, replays the same seeds through both, and folds each game's move sequence
into an FNV-1a hash (<code>GAME_SIG=1</code>). A hash mismatch is a diverged game; the comparison joins on
the seed, so it is robust to shard order and overlap. The reference is TT22.</p>
<p class="small">Bots differ in one structural way that dominates the curves: <b>octogen</b> and <b>semtex</b>
persist the table across a whole game, so their working sets accumulate and their tails reach far to the
right. <b>cordite</b> and <b>fulminate</b> clear the table every solve, so W stays small and their floor
sits at a much smaller table. octogen persists the most — it is the binding constraint, and the size was
chosen for it.</p>

<div class="foot" id="foot"></div>
</div>

<script>
const DATA = ${DATA};

${CLIENT_JS}
</script>
`;

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, html);
const cells = Object.keys(ccdf).length;
console.log(`wrote ${OUT}  (${cells} CCDF cells, ${measured.series.length} measured series, ${(html.length/1024).toFixed(0)} KiB)`);
