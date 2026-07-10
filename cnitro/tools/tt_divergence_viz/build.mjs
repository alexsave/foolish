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

const ccdf = JSON.parse(fs.readFileSync(path.join(HERE, 'data', 'ccdf.json'), 'utf8'));
const measured = JSON.parse(fs.readFileSync(path.join(HERE, 'data', 'measured.json'), 'utf8'));

const DATA = JSON.stringify({ ccdf, measured });

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
${clientJs()}
</script>
`;

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, html);
const cells = Object.keys(ccdf).length;
console.log(`wrote ${OUT}  (${cells} CCDF cells, ${measured.series.length} measured series, ${(html.length/1024).toFixed(0)} KiB)`);

function clientJs() {
  return String.raw`
const BOTS = ['octogen','semtex','cordite','fulminate'];
const BOTCOLOR = {octogen:'var(--octogen)',semtex:'var(--semtex)',cordite:'var(--cordite)',fulminate:'var(--fulminate)'};
const SHAPE = {octogen:'circle',semtex:'square',cordite:'triangle',fulminate:'diamond'};
const PRETTY = {octogen:'octogen',semtex:'semtex',cordite:'cordite',fulminate:'fulminate'};

// --- shape state ---
const cssVar = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
function color(bot){ return cssVar('--'+bot); }

// available (bot,pc) theory cells
const cells = DATA.ccdf; // key bot_pcN
const pcsAvail = [...new Set(Object.values(cells).map(c=>c.pc))].sort((a,b)=>a-b);
const botsAvail = [...new Set(Object.values(cells).map(c=>c.bot))];

const state = {
  pcs: new Set(pcsAvail.length? [pcsAvail.includes(2)?2:pcsAvail[0]] : []),
  bots: new Set(BOTS.filter(b=>botsAvail.includes(b))),
  y: 'log', curves:true, band:true, raw:true, bounds:true,
};

const B_MIN=4, B_MAX=24;
const FLOOR_P = 1e-6;           // log-axis floor; zeros parked just below
const DEC = [1,1e-1,1e-2,1e-3,1e-4,1e-5,1e-6];

// ---------- controls ----------
const pcChips = document.getElementById('pcChips');
pcsAvail.forEach(pc=>{
  const b=document.createElement('button');
  b.className='chip'; b.textContent='pc '+pc; b.setAttribute('aria-pressed', state.pcs.has(pc));
  b.onclick=()=>{ state.pcs.has(pc)?state.pcs.delete(pc):state.pcs.add(pc);
    b.setAttribute('aria-pressed', state.pcs.has(pc)); draw(); };
  pcChips.appendChild(b);
});
const botChips=document.getElementById('botChips');
BOTS.forEach(bot=>{
  const on=state.bots.has(bot);
  const b=document.createElement('button');
  b.className='botrow'; b.setAttribute('aria-pressed', on);
  b.innerHTML='<span class="sw" style="background:'+color(bot)+'"></span>'+PRETTY[bot];
  b.onclick=()=>{ state.bots.has(bot)?state.bots.delete(bot):state.bots.add(bot);
    b.setAttribute('aria-pressed', state.bots.has(bot)); draw(); };
  botChips.appendChild(b);
});
document.querySelectorAll('#yseg button').forEach(bt=>{
  bt.onclick=()=>{ state.y=bt.dataset.y;
    document.querySelectorAll('#yseg button').forEach(x=>x.setAttribute('aria-pressed', x===bt));
    draw(); };
});
document.getElementById('tCurves').onchange=e=>{state.curves=e.target.checked;draw();};
document.getElementById('tBand').onchange=e=>{state.band=e.target.checked;draw();};
document.getElementById('tRaw').onchange=e=>{state.raw=e.target.checked;draw();};
document.getElementById('tBounds').onchange=e=>{state.bounds=e.target.checked;draw();};

// theme
const tb=document.getElementById('themebtn');
tb.onclick=()=>{ const cur=document.documentElement.getAttribute('data-theme');
  const next = cur==='dark'?'light':(cur==='light'?'dark': (matchMedia('(prefers-color-scheme:dark)').matches?'light':'dark'));
  document.documentElement.setAttribute('data-theme', next); draw(); };

// ---------- geometry ----------
const W=920,H=500, M={l:64,r:24,t:20,b:64};
const plotW=W-M.l-M.r, plotH=H-M.t-M.b;
const xOf = b => M.l + (b-B_MIN)/(B_MAX-B_MIN)*plotW;
function yOf(p){
  if(state.y==='lin') return M.t + (1-p)*plotH;
  const lp = Math.log10(Math.max(p,FLOOR_P));
  const top=0, bot=Math.log10(FLOOR_P); // 0 .. -6
  return M.t + (top-lp)/(top-bot)*plotH;
}
const yZero = () => M.t + plotH; // baseline for p=0

// rule of three / Wilson bounds for measured k/n
function bounds(k,n){
  const p=k/n; const z=1.96;
  if(k===0) return {p:0, lo:0, hi:3/n};        // rule of three
  const c=z*z/n, mid=(p+c/2)/(1+c);
  const half=z*Math.sqrt(p*(1-p)/n + c/(4*n))/(1+c);
  return {p, lo:Math.max(0,mid-half), hi:Math.min(1,mid+half)};
}

const SVG='http://www.w3.org/2000/svg';
const el=(n,a={})=>{const e=document.createElementNS(SVG,n);for(const k in a)e.setAttribute(k,a[k]);return e;};
function marker(cx,cy,shape,fill,r=4.5){
  if(shape==='circle') return el('circle',{cx,cy,r,fill,class:'pt'});
  if(shape==='square') return el('rect',{x:cx-r,y:cy-r,width:2*r,height:2*r,rx:1,fill,class:'pt'});
  if(shape==='triangle'){const p=[[cx,cy-r*1.2],[cx-r*1.1,cy+r*0.9],[cx+r*1.1,cy+r*0.9]];
    return el('polygon',{points:p.map(q=>q.join(',')).join(' '),fill,class:'pt'});}
  const p=[[cx,cy-r*1.3],[cx+r*1.2,cy],[cx,cy+r*1.3],[cx-r*1.2,cy]]; // diamond
  return el('polygon',{points:p.map(q=>q.join(',')).join(' '),fill,class:'pt'});
}

const svg=document.getElementById('chart');
const tip=document.getElementById('tip');
const wrap=document.getElementById('chartWrap');

function draw(){
  while(svg.firstChild) svg.removeChild(svg.firstChild);
  // --- grid + axes ---
  // x gridlines at each integer bit (every 2)
  for(let b=B_MIN;b<=B_MAX;b+=2){
    const x=xOf(b);
    svg.appendChild(el('line',{x1:x,y1:M.t,x2:x,y2:M.t+plotH,class:'gridline'}));
    const t=el('text',{x,y:M.t+plotH+18,'text-anchor':'middle',class:''}); t.setAttribute('class','');
    t.textContent=b; t.setAttribute('fill','var(--ink-3)'); t.style.fontSize='11px';
    svg.appendChild(wrapText(x,M.t+plotH+18,String(b)));
    svg.appendChild(wrapText(x,M.t+plotH+31, fmtEntries(2**b)));
  }
  // y gridlines
  if(state.y==='log'){
    DEC.forEach(p=>{ const y=yOf(p);
      svg.appendChild(el('line',{x1:M.l,y1:y,x2:M.l+plotW,y2:y,class:'gridline'}));
      svg.appendChild(ylabel(y, fmtP(p)));
    });
    // zero baseline
    const yz=yZero();
    svg.appendChild(el('line',{x1:M.l,y1:yz,x2:M.l+plotW,y2:yz,class:'axisline'}));
    svg.appendChild(ylabel(yz+2,'0'));
  } else {
    for(let i=0;i<=5;i++){ const p=i/5; const y=yOf(p);
      svg.appendChild(el('line',{x1:M.l,y1:y,x2:M.l+plotW,y2:y,class:'gridline'}));
      svg.appendChild(ylabel(y, (p*100).toFixed(0)+'%'));
    }
  }
  // axis frame
  svg.appendChild(el('line',{x1:M.l,y1:M.t,x2:M.l,y2:M.t+plotH,class:'axisline'}));
  svg.appendChild(el('line',{x1:M.l,y1:M.t+plotH,x2:M.l+plotW,y2:M.t+plotH,class:'axisline'}));
  // axis titles
  const xt=el('text',{x:M.l+plotW/2,y:H-8,'text-anchor':'middle',class:'axis-title'});
  xt.textContent='transposition-table size  —  CD_TT_BITS  (entries below)'; svg.appendChild(xt);
  const yt=el('text',{x:16,y:M.t+plotH/2,'text-anchor':'middle',class:'axis-title',
    transform:'rotate(-90 16 '+(M.t+plotH/2)+')'});
  yt.textContent='P( game diverges from unbounded table )'; svg.appendChild(yt);

  // --- reference verticals --- (staggered labels so adjacent bits don't collide)
  refline(12,'var(--accent)','64 KiB L1', 24, 'end');
  refline(13,'var(--ship)','TT13 shipped · 128 KiB', 11, 'start');
  refline(16,'var(--ink-3)','TT16 old prod · 1 MiB', 24, 'start');

  // --- theory curves (model = expected) with a 95% sampling band ---
  const hot=[]; // hover points {x,y,html}
  if(state.curves){
    for(const bot of state.bots){
      for(const pc of state.pcs){
        const cell=cells[bot+'_pc'+pc]; if(!cell) continue;
        const pts=cell.ccdf.filter(d=>d.bits>=B_MIN&&d.bits<=B_MAX);
        // confidence band: Wilson CI on the empirical CCDF at each bit (n = games).
        // Its upper edge stays above zero even where the estimate hits the floor
        // (rule of three) — that gap is "no table is provably perfect", drawn.
        if(state.band){
          const up=[], dn=[];
          pts.forEach(pt=>{
            const k=(pt.k!=null?pt.k:Math.round(pt.p*cell.games)), bd=bounds(k,cell.games);
            const x=xOf(pt.bits);
            const yhi=yOf(Math.max(bd.hi, state.y==='log'?FLOOR_P:0));
            const ylo=yOf(Math.max(bd.lo, state.y==='log'?FLOOR_P:0));
            up.push([x,yhi]); dn.push([x,ylo]);
          });
          const poly=up.concat(dn.reverse());
          svg.appendChild(el('polygon',{points:poly.map(q=>q[0].toFixed(1)+','+q[1].toFixed(1)).join(' '),
            fill:color(bot),opacity:.13,stroke:'none'}));
        }
        // the expected line + raw empirical vertices
        let d=''; let started=false;
        pts.forEach(pt=>{
          const x=xOf(pt.bits), y = pt.p>0 ? yOf(pt.p) : (state.y==='log'?yZero():yOf(0));
          d += (started?'L':'M')+x.toFixed(1)+' '+y.toFixed(1)+' '; started=true;
          hot.push({x,y,bot,html:tipCurve(bot,pc,pt,cell)});
        });
        svg.appendChild(el('path',{d,class:'curve',stroke:color(bot)}));
      }
    }
  }
  // --- measured markers ---
  if(state.raw){
    DATA.measured.series.forEach(s=>{
      if(!state.bots.has(s.bot)) return;
      s.points.forEach(pt=>{
        if(pt.bits<B_MIN||pt.bits>B_MAX) return;
        const bd=bounds(pt.diverged, s.games);
        const x=xOf(pt.bits);
        const y = bd.p>0? yOf(bd.p) : (state.y==='log'? yOf(FLOOR_P*1.4) : yOf(0));
        // bounds whisker
        if(state.bounds){
          const yhi=yOf(Math.max(bd.hi,FLOOR_P)), ylo=yOf(Math.max(bd.lo,FLOOR_P));
          svg.appendChild(el('line',{x1:x,y1:yhi,x2:x,y2: bd.p>0?ylo:y ,
            stroke:color(s.bot),'stroke-width':1.25,opacity:.5}));
          svg.appendChild(el('line',{x1:x-3,y1:yhi,x2:x+3,y2:yhi,stroke:color(s.bot),'stroke-width':1.25,opacity:.5}));
        }
        svg.appendChild(marker(x,y,SHAPE[s.bot],color(s.bot)));
        hot.push({x,y,bot:s.bot,html:tipRaw(s,pt,bd)});
      });
    });
  }

  // --- hover layer ---
  const cross=el('line',{class:'crosshair',y1:M.t,y2:M.t+plotH}); svg.appendChild(cross);
  const hit=el('rect',{x:M.l,y:M.t,width:plotW,height:plotH,fill:'transparent'}); svg.appendChild(hit);
  hit.addEventListener('mousemove',ev=>{
    const pt=svg.createSVGPoint(); pt.x=ev.clientX; pt.y=ev.clientY;
    const loc=pt.matrixTransform(svg.getScreenCTM().inverse());
    let best=null,bd=1e9;
    for(const h of hot){const dd=(h.x-loc.x)**2+(h.y-loc.y)**2; if(dd<bd){bd=dd;best=h;}}
    if(best && bd<900){
      cross.setAttribute('x1',best.x); cross.setAttribute('x2',best.x); cross.style.opacity=.6;
      const r=wrap.getBoundingClientRect(), cr=svg.getBoundingClientRect();
      const sx=cr.width/W, sy=cr.height/H;
      tip.innerHTML=best.html; tip.style.opacity=1;
      tip.style.left=(best.x*sx)+'px'; tip.style.top=(best.y*sy)+'px';
    } else { tip.style.opacity=0; cross.style.opacity=0; }
  });
  hit.addEventListener('mouseleave',()=>{tip.style.opacity=0;cross.style.opacity=0;});

  buildLegend(); buildTable(); buildFoot();
}

function refline(bits,stroke,label,dy=11,anchor='start'){
  const x=xOf(bits);
  svg.appendChild(el('line',{x1:x,y1:M.t,x2:x,y2:M.t+plotH,class:'refline',stroke}));
  const t=el('text',{x:x+(anchor==='end'?-4:4),y:M.t+dy,class:'reflabel',fill:stroke,'text-anchor':anchor});
  t.textContent=label; svg.appendChild(t);
}
function ylabel(y,txt){const t=el('text',{x:M.l-8,y:y+3,'text-anchor':'end'});t.setAttribute('fill','var(--ink-3)');t.style.fontSize='11px';t.textContent=txt;return t;}
function wrapText(x,y,txt){const t=el('text',{x,y,'text-anchor':'middle'});t.setAttribute('fill','var(--ink-3)');t.style.fontSize='10.5px';t.textContent=txt;return t;}

function fmtEntries(n){ if(n>=1e6)return (n/1e6)+'M'; if(n>=1e3)return (n/1e3)+'k'; return ''+n; }
function fmtP(p){ if(p>=1)return '1'; return '1e'+Math.round(Math.log10(p)); }
function pctP(p){ if(p===0)return '0'; if(p>=0.01)return (p*100).toFixed(2)+'%';
  return p.toExponential(1); }

function tipCurve(bot,pc,pt,cell){
  return '<b style="color:'+color(bot)+'">'+PRETTY[bot]+' · pc '+pc+'</b> — model'
   +'<div class="row"><span class="k">table</span><span>TT'+pt.bits+' · '+fmtEntries(pt.entries)+'</span></div>'
   +'<div class="row"><span class="k">P(W &gt; '+fmtEntries(pt.entries)+')</span><span>'+pctP(pt.p)+'</span></div>'
   +'<div class="row"><span class="k">max W seen</span><span>'+cell.maxW.toLocaleString()+'</span></div>';
}
function tipRaw(s,pt,bd){
  const pcl = s.pc==='mixed'?'mixed pc':'pc '+s.pc;
  let b = bd.p>0 ? pctP(bd.p) : '0 of '+s.games.toLocaleString();
  let bound = 'p &lt; '+pctP(bd.hi)+' (95%)';
  return '<b style="color:'+color(s.bot)+'">'+PRETTY[s.bot]+' · '+pcl+'</b> — measured'
   +'<div class="row"><span class="k">table</span><span>TT'+pt.bits+'</span></div>'
   +'<div class="row"><span class="k">diverged</span><span>'+pt.diverged+' / '+s.games.toLocaleString()+'</span></div>'
   +'<div class="row"><span class="k">rate</span><span>'+b+'</span></div>'
   +'<div class="row"><span class="k">bound</span><span>'+bound+'</span></div>';
}

function buildLegend(){
  const L=document.getElementById('legend'); L.innerHTML='';
  BOTS.filter(b=>state.bots.has(b)).forEach(bot=>{
    const d=document.createElement('span'); d.className='li';
    d.innerHTML='<svg width="34" height="14">'
      +'<line x1="2" y1="7" x2="20" y2="7" stroke="'+color(bot)+'" stroke-width="2"/>'
      +shapeSvg(27,7,SHAPE[bot],color(bot))+'</svg>'+PRETTY[bot];
    L.appendChild(d);
  });
  const note=document.createElement('span'); note.className='li'; note.style.color='var(--ink-3)';
  note.innerHTML='<svg width="30" height="14">'
     +'<rect x="2" y="3" width="24" height="8" fill="var(--ink-3)" opacity=".25"/>'
     +'<line x1="2" y1="7" x2="26" y2="7" stroke="var(--ink-3)" stroke-width="2"/></svg>'
     +'line+band = model (expected ±95%) &nbsp;·&nbsp; solid marker = raw observed &nbsp;·&nbsp; whisker = 95% error';
  L.appendChild(note);
}
function shapeSvg(cx,cy,shape,fill){
  const r=4.2;
  if(shape==='circle')return '<circle cx="'+cx+'" cy="'+cy+'" r="'+r+'" fill="'+fill+'"/>';
  if(shape==='square')return '<rect x="'+(cx-r)+'" y="'+(cy-r)+'" width="'+2*r+'" height="'+2*r+'" fill="'+fill+'"/>';
  if(shape==='triangle')return '<polygon points="'+cx+','+(cy-r*1.2)+' '+(cx-r*1.1)+','+(cy+r*0.9)+' '+(cx+r*1.1)+','+(cy+r*0.9)+'" fill="'+fill+'"/>';
  return '<polygon points="'+cx+','+(cy-r*1.3)+' '+(cx+r*1.2)+','+cy+' '+cx+','+(cy+r*1.3)+' '+(cx-r*1.2)+','+cy+'" fill="'+fill+'"/>';
}

function buildTable(){
  const T=document.getElementById('pctTable');
  const rows=[];
  Object.values(cells).filter(c=>state.bots.has(c.bot)).sort((a,b)=>a.bot<b.bot?-1:a.bot>b.bot?1:a.pc-b.pc)
    .forEach(c=>rows.push(c));
  let h='<thead><tr><th>bot · pc</th><th>games</th><th>median</th><th>p99</th><th>max W</th><th>&ge;TT13?</th></tr></thead><tbody class="mono">';
  rows.forEach(c=>{
    const fits = c.maxW < 8192;
    h+='<tr><td style="color:'+color(c.bot)+';font-weight:600">'+PRETTY[c.bot]+' · pc'+c.pc+'</td>'
      +'<td>'+c.games.toLocaleString()+'</td><td>'+c.median+'</td><td>'+c.p99.toLocaleString()+'</td>'
      +'<td>'+c.maxW.toLocaleString()+'</td>'
      +'<td>'+(fits?'<span style="color:var(--fulminate)">fits</span>':'<span style="color:var(--ship)">spills</span>')+'</td></tr>';
  });
  if(!rows.length) h+='<tr><td colspan="6" style="color:var(--ink-3)">select a bot</td></tr>';
  T.innerHTML=h+'</tbody>';
}

function buildFoot(){
  const c4=cells['octogen_pc4'];
  if(c4){ const s=c4.maxW.toLocaleString();
    const a=document.getElementById('maxw4'), b=document.getElementById('maxw4b');
    if(a)a.textContent=s; if(b)b.textContent=s; }
  const nCells=Object.keys(cells).length;
  document.getElementById('foot').innerHTML=
    'Model curves from '+nCells+' working-set distributions (CD_TT_STATS, production budget). '
    +'Measured markers from FNV-1a move-hash replay (tools/tt_divergence.sh, reference TT22). '
    +'Direct-mapped table, 16 bytes/entry. Generated by tools/tt_divergence_viz/build.mjs.';
}

draw();
`;
}
