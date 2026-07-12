#!/usr/bin/env python3
# render(data) -> a self-contained interactive multi-bot replay X-ray page.
# Consumed by multi_page.py. Octogen seats get the full deliberation panel
# (Monte-Carlo / endgame solver / belief); random seats get their legal-move
# menu with the recorded pick highlighted. Nothing game-specific is hardcoded.
import json

CSS = r"""
:root{
  --surface-0:#f4f4f2; --surface-1:#fcfcfb; --surface-2:#ececea; --border:#dad9d4;
  --text-primary:#0b0b0b; --text-secondary:#52514e; --text-muted:#86857f;
  --win:#0a7a35; --win-bg:#dcefe1; --loss:#d23b3a; --loss-bg:#f7dcdc;
  --warn:#c74a1e; --warn-bg:#fbe2d6; --info:#2a78d6; --info-bg:#dce9fb;
  --rand:#7a5a12; --rand-bg:#f3ead6; --accent:#4a3aa7; --card:#ffffff;
  --card-red:#c0392b; --card-trump:#8a6d00;
  --shadow:0 1px 3px rgba(0,0,0,.08),0 1px 2px rgba(0,0,0,.04);
}
@media (prefers-color-scheme: dark){:root{
  --surface-0:#131312; --surface-1:#1c1c1a; --surface-2:#252523; --border:#3a3a37;
  --text-primary:#f4f4f2; --text-secondary:#c3c2b7; --text-muted:#8e8d84;
  --win:#3fbf72; --win-bg:#123322; --loss:#e66767; --loss-bg:#3a1a1a;
  --warn:#e77a4d; --warn-bg:#3a2113; --info:#57a0ee; --info-bg:#122238;
  --rand:#d6b45c; --rand-bg:#332a15; --accent:#9085e9; --card:#2b2b28;
  --card-red:#e77a6a; --card-trump:#e0b83a; --shadow:0 1px 3px rgba(0,0,0,.4);
}}
:root[data-theme="dark"]{
  --surface-0:#131312; --surface-1:#1c1c1a; --surface-2:#252523; --border:#3a3a37;
  --text-primary:#f4f4f2; --text-secondary:#c3c2b7; --text-muted:#8e8d84;
  --win:#3fbf72; --win-bg:#123322; --loss:#e66767; --loss-bg:#3a1a1a;
  --warn:#e77a4d; --warn-bg:#3a2113; --info:#57a0ee; --info-bg:#122238;
  --rand:#d6b45c; --rand-bg:#332a15; --accent:#9085e9; --card:#2b2b28;
  --card-red:#e77a6a; --card-trump:#e0b83a; --shadow:0 1px 3px rgba(0,0,0,.4);
}
:root[data-theme="light"]{
  --surface-0:#f4f4f2; --surface-1:#fcfcfb; --surface-2:#ececea; --border:#dad9d4;
  --text-primary:#0b0b0b; --text-secondary:#52514e; --text-muted:#86857f;
  --win:#0a7a35; --win-bg:#dcefe1; --loss:#d23b3a; --loss-bg:#f7dcdc;
  --warn:#c74a1e; --warn-bg:#fbe2d6; --info:#2a78d6; --info-bg:#dce9fb;
  --rand:#7a5a12; --rand-bg:#f3ead6; --accent:#4a3aa7; --card:#ffffff;
  --card-red:#c0392b; --card-trump:#8a6d00;
  --shadow:0 1px 3px rgba(0,0,0,.08),0 1px 2px rgba(0,0,0,.04);
}
*{box-sizing:border-box}
body{margin:0;background:var(--surface-0);color:var(--text-primary);
  font:15px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  -webkit-font-smoothing:antialiased;}
.wrap{max-width:1040px;margin:0 auto;padding:28px 20px 90px;}
h1{font-size:26px;line-height:1.2;margin:0 0 6px;letter-spacing:-.02em}
h2{font-size:20px;margin:40px 0 10px;letter-spacing:-.01em;padding-top:8px}
h3{font-size:14px;margin:18px 0 8px;color:var(--text-secondary);text-transform:uppercase;letter-spacing:.04em;font-weight:600}
p{margin:10px 0;color:var(--text-secondary)}
.lede{font-size:17px;color:var(--text-secondary);max-width:76ch}
code,.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.9em}
.panel{background:var(--surface-1);border:1px solid var(--border);border-radius:12px;padding:18px 20px;box-shadow:var(--shadow);margin:16px 0}
.muted{color:var(--text-muted)}
b,strong{color:var(--text-primary)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin:18px 0}
.tile{background:var(--surface-1);border:1px solid var(--border);border-radius:12px;padding:14px 16px;box-shadow:var(--shadow)}
.tile .n{font-size:22px;font-weight:700;letter-spacing:-.02em;font-variant-numeric:tabular-nums}
.tile .l{font-size:12.5px;color:var(--text-muted);margin-top:2px}
.tile.win .n{color:var(--win)}
/* standings */
.stand{display:flex;flex-wrap:wrap;gap:8px;margin:12px 0}
.srow{display:flex;align-items:center;gap:9px;background:var(--surface-1);border:1px solid var(--border);
  border-radius:10px;padding:7px 12px;box-shadow:var(--shadow);font-size:13.5px}
.srow .pl{font-weight:800;font-variant-numeric:tabular-nums;min-width:20px;color:var(--text-muted)}
.srow.p1 .pl{color:var(--win)} .srow.fool{border-color:var(--loss)}
.badge{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.03em;padding:2px 7px;border-radius:5px}
.badge.octogen{background:var(--win-bg);color:var(--win)} .badge.random{background:var(--rand-bg);color:var(--rand)}
/* playing cards */
.pc{display:inline-flex;align-items:center;justify-content:center;min-width:30px;height:40px;padding:0 6px;
  background:var(--card);border:1px solid var(--border);border-radius:7px;font-weight:700;font-size:15px;
  box-shadow:var(--shadow);font-variant-numeric:tabular-nums;color:var(--text-primary);gap:1px}
.pc.red{color:var(--card-red)} .pc.trump{border-color:var(--card-trump);border-width:2px;color:var(--card-trump)}
.pc.sm{height:30px;min-width:22px;font-size:12.5px;border-radius:6px;padding:0 4px}
.pc.hidden{color:var(--text-muted);background:var(--surface-2);border-style:dashed}
.pc .s{font-size:.82em}
.hand{display:flex;flex-wrap:wrap;gap:5px;align-items:center}
/* controls */
.ctl{display:flex;flex-wrap:wrap;gap:10px 10px;align-items:center;margin:10px 0 6px}
.ctl button.nav{border:1px solid var(--border);background:var(--surface-1);color:var(--text-primary);
  border-radius:8px;padding:7px 13px;cursor:pointer;font:inherit;font-weight:600;font-size:14px;box-shadow:var(--shadow)}
.ctl button.nav:disabled{opacity:.4;cursor:default}
.ctl button.nav.og{border-color:var(--win);color:var(--win)}
.ctl button.nav.rnd{border-color:var(--rand);color:var(--rand)}
input[type=range]{flex:1;min-width:180px;accent-color:var(--accent)}
.jump{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0}
.jump button{border:1px solid var(--warn);background:var(--warn-bg);color:var(--warn);border-radius:8px;
  padding:6px 11px;cursor:pointer;font:inherit;font-weight:600;font-size:13px}
/* board */
.board{display:grid;grid-template-columns:1fr;gap:14px;margin:14px 0}
.seatrow{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.seatchip{display:inline-flex;align-items:center;gap:6px;background:var(--surface-2);border:1px solid var(--border);
  border-radius:20px;padding:4px 11px;font-size:13px;font-weight:600}
.seatchip.def{background:var(--info-bg);border-color:var(--info);color:var(--info)}
.seatchip.og{background:var(--win-bg);border-color:var(--win);color:var(--win)}
.seatchip.rnd{background:var(--rand-bg);border-color:var(--rand);color:var(--rand)}
.seatchip.acting{outline:2px solid var(--accent);outline-offset:1px}
.seatchip.out{opacity:.4;text-decoration:line-through}
.seatchip .cnt{font-variant-numeric:tabular-nums;font-weight:500}
.tablearea{min-height:56px;background:var(--surface-2);border:1px dashed var(--border);border-radius:10px;padding:12px;
  display:flex;flex-wrap:wrap;gap:14px;align-items:center}
.battle{display:flex;flex-direction:column;align-items:center;gap:3px}
.battle .un{color:var(--text-muted);font-size:11px}
.actline{font-size:14.5px;margin:4px 0 0}
.actline .who{font-weight:700}
.tag{display:inline-block;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.03em;
  padding:2px 8px;border-radius:5px;margin-right:6px;vertical-align:1px}
.tag.attack{background:var(--warn-bg);color:var(--warn)} .tag.cover{background:var(--info-bg);color:var(--info)}
.tag.pickup{background:var(--loss-bg);color:var(--loss)} .tag.good,.tag.discard{background:var(--surface-2);color:var(--text-muted)}
.tag.pass{background:var(--surface-2);color:var(--text-secondary)} .tag.draw,.tag.defender_change,.tag.player_out,.tag.game_start{background:var(--surface-2);color:var(--text-muted)}
/* decision panel */
.dpanel{border:2px solid var(--accent);border-radius:12px;padding:16px 18px;margin:16px 0;background:var(--surface-1);box-shadow:var(--shadow)}
.dpanel.solver{border-color:var(--win)} .dpanel.og{border-color:var(--win)} .dpanel.rnd{border-color:var(--rand)}
.dhead{display:flex;flex-wrap:wrap;gap:8px 14px;align-items:center;margin-bottom:8px}
.dhead .t{font-weight:700;font-size:15px}
.bars{display:flex;flex-direction:column;gap:6px;margin-top:6px}
.brow{display:grid;grid-template-columns:180px 1fr auto;gap:10px;align-items:center}
.bmove{display:flex;gap:3px;align-items:center;justify-content:flex-end;flex-wrap:wrap}
.btrack{background:var(--surface-2);border-radius:4px;height:22px;position:relative;overflow:hidden}
.bfill{height:100%;border-radius:4px 0 0 4px;min-width:2px;background:var(--accent);opacity:.55}
.brow.best .bfill{background:var(--win);opacity:.9}
.brow.chosen .btrack{outline:2px solid var(--accent);outline-offset:1px}
.brow.best.chosen .btrack{outline-color:var(--win)}
.brow.pruned{opacity:.5}
.btag{font-size:12px;font-variant-numeric:tabular-nums;color:var(--text-secondary);white-space:nowrap;min-width:120px}
.chip{display:inline-flex;align-items:center;gap:4px;font-size:11.5px;font-weight:700;padding:2px 8px;border-radius:20px}
.chip.win{background:var(--win-bg);color:var(--win)} .chip.loss{background:var(--loss-bg);color:var(--loss)}
.chip.unknown{background:var(--surface-2);color:var(--text-muted)} .chip.match{background:var(--win-bg);color:var(--win)}
.chip.mismatch{background:var(--warn-bg);color:var(--warn)} .chip.forced{background:var(--info-bg);color:var(--info)}
.chip.trumpkeep{background:var(--card-trump);color:#fff;opacity:.92} .chip.rnd{background:var(--rand-bg);color:var(--rand)}
.btag .raw{color:var(--text-muted);text-decoration:line-through;text-decoration-thickness:1px}
.btag .dim{color:var(--text-muted)}
.keepnote{border-left:3px solid var(--card-trump);padding-left:9px}
.callout{margin-top:12px;padding:12px 14px;border-radius:9px;border:1px solid;font-size:14px}
.callout.warn{background:var(--warn-bg);border-color:var(--warn)}
.callout.rnd{background:var(--rand-bg);border-color:var(--rand)}
/* random menu */
.rmenu{display:flex;flex-direction:column;gap:6px;margin-top:8px}
.ropt{display:flex;align-items:center;gap:8px;border:1px solid var(--border);border-radius:9px;padding:7px 11px;background:var(--surface-2)}
.ropt.picked{border-color:var(--rand);background:var(--rand-bg);box-shadow:0 0 0 1px var(--rand)}
.ropt .pm{display:flex;gap:3px;align-items:center;flex-wrap:wrap}
.ropt .pick{margin-left:auto;font-size:11.5px;font-weight:700;color:var(--rand)}
.ropt .verb{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.03em;color:var(--text-muted);min-width:52px}
/* known state */
.known{margin-top:14px;border-top:1px dashed var(--border);padding-top:12px}
.kmeta{display:flex;flex-wrap:wrap;gap:8px 18px;font-size:13px;margin-bottom:6px}
.kmeta b{font-variant-numeric:tabular-nums}
.known.empty{border-color:var(--win)}
.kgroup{margin:8px 0}
.klabel{font-size:13px;color:var(--text-secondary);margin-bottom:5px}
.klabel.known-yes{color:var(--win);font-weight:600}
.pc.pinned{border-color:var(--win);border-width:2px;box-shadow:0 0 0 1px var(--win-bg)}
.pc.poolc{border-style:dashed;opacity:.9}
.themebtn{position:fixed;top:12px;right:12px;z-index:9;border:1px solid var(--border);background:var(--surface-1);
  color:var(--text-secondary);border-radius:8px;padding:6px 10px;cursor:pointer;font:inherit;font-size:13px;box-shadow:var(--shadow)}
.arrow{color:var(--text-muted);margin:0 2px}
.solvernote{font-size:12.5px;color:var(--text-muted);margin-top:6px}
@media(max-width:640px){.brow{grid-template-columns:130px 1fr}.btag{grid-column:2;min-width:0;text-align:right}}
"""

HTML = r"""<meta charset="utf-8">
<title>Eight bots, eight minds — a wasm-bot replay X-ray</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>__CSS__</style>
<button class="themebtn" id="themebtn" title="Toggle theme">theme</button>
<div class="wrap">
<h1 id="pagetitle">Eight bots, eight minds &mdash; a replay X&#8209;ray</h1>
<p class="lede" id="lede"></p>

<div class="tiles" id="tiles"></div>

<h3 style="margin-top:22px">Final standings</h3>
<div class="stand" id="stand"></div>

<div class="panel">
<h3>How this was reproduced</h3>
<p id="repro"></p>
<p class="muted">Rank convention is the correct Durak one (wire value&nbsp;13 = A). The trump suit is boxed and gold.
<span id="seatlegend"></span></p>
</div>

<h2 id="flagHead">Moves where octogen would differ</h2>
<div class="jump" id="jump"></div>

<h2>Step through the game</h2>
<div class="ctl">
  <button class="nav" id="first">&laquo; start</button>
  <button class="nav" id="prev">&lsaquo; prev</button>
  <button class="nav" id="next">next &rsaquo;</button>
  <button class="nav" id="nextd">next decision &raquo;</button>
  <button class="nav og" id="nextog">next octogen &raquo;</button>
  <button class="nav rnd" id="nextrnd">next random &raquo;</button>
</div>
<div class="ctl"><span class="mono muted" id="counter"></span></div>
<div class="ctl"><input type="range" id="slider" min="0" max="0" value="0"></div>

<div class="board panel">
  <div class="seatrow" id="seatrow"></div>
  <div>
    <h3 style="margin-top:2px">Table</h3>
    <div class="tablearea" id="tablearea"></div>
  </div>
  <div class="actline" id="actline"></div>
</div>

<div id="decision"></div>

<h2 style="margin-top:44px">How to read the panels</h2>
<div class="panel">
<ul style="color:var(--text-secondary);margin:6px 0;padding-left:20px;line-height:1.7">
<li><b style="color:var(--win)">octogen turns.</b> The full deliberation, dumped from the C engine.
Away from the endgame octogen samples many worlds (deals of the hidden cards) and rolls each candidate move to the
end; the bar is its <b>average finish</b> (1 = out first = win, higher = worse, lower is better). Once the deck is
empty it solves the position <b>exactly</b> (proven <span class="chip win">win</span>/<span class="chip loss">loss</span>).
It also shows what octogen actually <b>knows</b> about the hidden cards: the cards it has <b>pinned</b> to a seat
(watched them pick up) vs the unknown pool it samples over.</li>
<li id="howrandom"><b style="color:var(--rand)">random turns.</b> Random has no belief and no rollout &mdash; it just
picks one of its legal moves <b>uniformly at random</b>. So its &ldquo;reasoning&rdquo; is the full menu of legal moves
at that position, each with probability <b>1/N</b>; the one it actually played is highlighted. That&rsquo;s the whole
story &mdash; the contrast with octogen&rsquo;s panel is the point.</li>
<li><b>Trump&#8209;keep tax.</b> While the deck is alive octogen adds a small tax
(<span class="chip trumpkeep">trump&#8209;keep</span>) to any move that <i>leads</i> a trump, tipping near&#8209;ties
toward keeping it. Taxed rows show <span class="raw">raw</span>&nbsp;+&nbsp;tax&nbsp;=&nbsp;<b>adjusted</b>.</li>
<li><b>Determinism.</b> octogen&rsquo;s world&#8209;sampling seed is a pure function of the public board (plus a server&#8209;only
secret), independent of the random bots&rsquo; draws. Replaying its exact recorded picks through the deployed wasm reproduces
<b>every</b> decision bit&#8209;for&#8209;bit &mdash; so the panels below are precisely what the shipped bot computed, not an
approximation.</li>
</ul>
</div>
<p class="muted" style="font-size:12.5px">Deliberation dumped by <code>OG_EXPLAIN</code> (compile&#8209;time
<code>-DOG_EXPLAIN_BUILD</code>, absent from shipped bots) in <code>cnitro/src/octogen_strategy.c</code>; game driven
through the deployed wasm by <code>e2e/_wasm_multi_drive.test.ts</code>; page assembled by
<code>cnitro/tools/og_explain/multi_page.py</code>.</p>
</div>
<script>
const DATA=__DATA__;
</script>
<script>__JS__</script>
"""

JS = r"""
(function(){
const D=DATA, L=D.logs, N=L.length, M=D.meta;
const NP=M.players, TRUMP=M.trump;
const OCTO=new Set(M.octoSeats);
const SYMS=['♠','♥','♣','♦'];
let cur=0;

function isOcto(p){return OCTO.has(p);}
function seatKind(p){return isOcto(p)?'octogen':'random';}
const OCTOLIST=[]; const RANDLIST=[];
for(let p=0;p<NP;p++){ (isOcto(p)?OCTOLIST:RANDLIST).push(p); }
const nOcto=OCTOLIST.length, nRand=RANDLIST.length;
const seatSpan=(arr)=>arr.map(p=>'<b class="mono">p'+p+'</b>').join(', ');
const matchup = nRand ? (nOcto+'&times;octogen vs '+nRand+'&times;random') : (nOcto+'&times;octogen');
const matchupShort = nRand ? (nOcto+' vs '+nRand) : (nOcto+' bots');
document.getElementById('pagetitle').innerHTML =
  (nRand? 'Eight bots, eight minds' : nOcto+' octogens, one table') + ' &mdash; a wasm&#8209;bot replay X&#8209;ray';

// ---- intro / tiles / standings ----
const octoIntro = nRand
  ? 'seats '+seatSpan(OCTOLIST)+' are <b style="color:var(--win)">octogen</b> (Monte&#8209;Carlo world&#8209;sampling + '+
    'exact endgame solver); seats '+seatSpan(RANDLIST)+' are <b style="color:var(--rand)">random</b> (uniform legal move)'
  : 'all <b>'+nOcto+'</b> seats are <b style="color:var(--win)">octogen</b> (Monte&#8209;Carlo world&#8209;sampling + exact '+
    'endgame solver) &mdash; the same bot playing itself, each modelling the other seven';
document.getElementById('lede').innerHTML =
  'One recorded <b>'+NP+'&#8209;player</b> Durak game: '+octoIntro+'. Trump is <b>'+M.trumpSym+'</b>, flip '+cardHTML(M.flip,true)+'. '+
  'Step through every public move; at <b>each bot&rsquo;s own turn</b> the panel opens up what it was thinking &mdash; the full '+
  'octogen X&#8209;ray'+(nRand?' for the octogen seats, and the legal&#8209;move menu (with the pick highlighted) for the random seats':'')+'.';

const winKind=seatKind(M.winner);
const tiles=[
  ['n', matchupShort, nRand? 'octogen vs random' : 'octogen self&#8209;play (mirror)'],
  ['n', M.trumpSym+' '+suitName(TRUMP), 'trump · flip '+M.flip.str],
  [winKind==='octogen'?'win':'', 'p'+M.winner+' wins', 'winner is '+winKind],
  ['n', M.octoMatch+' / '+M.octoDecisions, 'octogen turns reproduced exactly'],
];
document.getElementById('tiles').innerHTML = tiles.map(([cls,n,l])=>
  '<div class="tile'+(cls==='win'?' win':'')+'"><div class="n">'+n+'</div><div class="l">'+l+'</div></div>').join('');

document.getElementById('stand').innerHTML = M.standings.map(s=>
  '<div class="srow p'+s.place+(s.seat===M.fool?' fool':'')+'"><span class="pl">'+ordinal(s.place)+'</span>'+
  '<b class="mono">p'+s.seat+'</b><span class="badge '+s.kind+'">'+s.kind+'</span>'+
  (s.seat===M.fool?'<span class="muted">fool (durak)</span>':'')+'</div>').join('');

document.getElementById('repro').innerHTML =
  'This game was played entirely by the <b>deployed wasm bots</b> (kernel session&#8209;log belief, exactly as on the server), and '+
  'the X&#8209;ray replays their <b>exact recorded picks</b> &mdash; not the lossy replay URL &mdash; re&#8209;dealing from the '+
  '32&#8209;byte seed <code>'+esc(M.seed.slice(0,16))+'&hellip;</code> and querying each bot at its turns. octogen is '+
  'deterministic (its world&#8209;sampling seed is a pure function of the public board), so it reproduces <b>every one</b> of its '+
  '<b>'+M.octoDecisions+'</b> decisions &mdash; <b>'+M.octoMatch+' / '+M.octoDecisions+'</b>, exactly.'+
  (M.randDecisions ? ' The <b>'+M.randDecisions+'</b> random turns are shown with their full legal&#8209;move menu &mdash; '+
   'the recorded card is one uniform draw from it.' : '');

document.getElementById('seatlegend').innerHTML = nRand
  ? 'Seats '+seatSpan(OCTOLIST)+' are <b style="color:var(--win)">octogen</b>; '+seatSpan(RANDLIST)+' are <b style="color:var(--rand)">random</b>.'
  : 'All '+nOcto+' seats are <b style="color:var(--win)">octogen</b> &mdash; the same bot playing itself.';
if(!nRand){ const hr=document.getElementById('howrandom'); if(hr) hr.style.display='none';
  const nb=document.getElementById('nextrnd'); if(nb) nb.style.display='none'; }

document.getElementById('flagHead').textContent =
  D.octoDiffer.length ? ('Moves where octogen would differ ('+D.octoDiffer.length+')') : 'octogen reproduced every non-forced move exactly';

function suitName(s){return ['spades','hearts','clubs','diamonds'][s];}
function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
function ordinal(n){return n+({1:'st',2:'nd',3:'rd'}[n%10>3||[11,12,13].includes(n%100)?0:n%10]||'th');}

// ---- card rendering ----
function cardEl(c,sm){
  const d=document.createElement('span');
  d.className='pc'+(sm?' sm':'')+(c.hidden?' hidden':(c.trump?' trump':(c.red?' red':'')));
  if(c.hidden){d.textContent='?';return d;}
  d.innerHTML=c.r+'<span class="s">'+c.sym+'</span>';
  return d;
}
function cardHTML(c,sm){return cardEl(c,sm).outerHTML;}
const VS={S:0,H:1,C:2,D:3};
function tok(t){
  const m=String(t).match(/^(10|[2-9]|[JQKA])([SHCD])(\*?)$/);
  if(!m) return {hidden:true};
  const s=VS[m[2]];
  return {r:m[1],suit:s,sym:SYMS[s],trump:s===TRUMP,red:(s===1||s===3),hidden:false};
}
function tokRow(tokens,sm){
  const f=document.createDocumentFragment();
  tokens.forEach(t=>f.appendChild(cardEl(tok(t),sm)));
  return f;
}
// render a move-label string ("attack KC", "cover 9C->10H", "pickup") into chips
function labelChips(label){
  const f=document.createElement('span'); f.className='pm';
  const parts=String(label).split(/\s+/); const verb=parts[0]; const rest=parts.slice(1);
  const vb=document.createElement('span'); vb.className='verb'; vb.textContent=verb; f.appendChild(vb);
  if(verb==='pickup'||verb==='good'||rest.length===0){ return f; }
  rest.forEach(t=>{
    if(t.includes('->')){
      const [c,tg]=t.split('->');
      f.appendChild(cardEl(tok(c),true));
      const ar=document.createElement('span');ar.className='arrow';ar.innerHTML='&rarr;';f.appendChild(ar);
      f.appendChild(cardEl(tok(tg),true));
    }else{ f.appendChild(cardEl(tok(t),true)); }
  });
  return f;
}

function renderBoard(){
  const o=L[cur];
  const sr=document.getElementById('seatrow'); sr.innerHTML='';
  const dfn=currentDefender();
  const actingSeat=o.decision?o.decision.seat:(o.action&&o.action.seat!=null?o.action.seat:-1);
  for(let p=0;p<NP;p++){
    const el=document.createElement('span');
    el.className='seatchip'+(dfn===p?' def':'')+(isOcto(p)?' og':' rnd')+(actingSeat===p?' acting':'')+(o.hc[p]<=0&&isEliminated(p)?' out':'');
    el.innerHTML='<b>p'+p+'</b> <span class="badge '+seatKind(p)+'">'+(isOcto(p)?'OG':'RND')+'</span>'+
      '<span class="cnt">'+o.hc[p]+'</span>'+(dfn===p?' <span class="muted">def</span>':'');
    sr.appendChild(el);
  }
  const ta=document.getElementById('tablearea'); ta.innerHTML='';
  if(!o.table.length){ ta.innerHTML='<span class="muted">(empty)</span>'; }
  o.table.forEach(b=>{
    const bt=document.createElement('div'); bt.className='battle';
    bt.appendChild(cardEl(b.a,false));
    if(b.d){const dd=cardEl(b.d,false);bt.appendChild(dd);}
    else{const u=document.createElement('div');u.className='un';u.textContent='uncovered';bt.appendChild(u);}
    ta.appendChild(bt);
  });
  document.getElementById('actline').innerHTML=actionText(o);
  document.getElementById('counter').textContent='log '+cur+' / '+(N-1)+'  ·  '+o.t;
  document.getElementById('slider').value=cur;
  document.getElementById('first').disabled=cur===0;
  document.getElementById('prev').disabled=cur===0;
  document.getElementById('next').disabled=cur===N-1;
  renderDecision();
}
function isEliminated(p){
  for(let i=0;i<=cur;i++){ if(L[i].t==='player_out'&&L[i].seat===p) return true; }
  return false;
}
function currentDefender(){
  let d=-1;
  for(let i=cur;i>=0;i--){ if(L[i].t==='defender_change'){d=L[i].def;break;} }
  return d;
}
function actionText(o){
  const a=o.action; if(!a) return '';
  const seat=(a.seat!=null)?'<span class="who">p'+a.seat+'</span> ':'';
  const tag='<span class="tag '+(a.kind)+'">'+a.kind.replace('_',' ')+'</span>';
  function cardsHtml(cards){const s=document.createElement('span');s.className='hand';(cards||[]).forEach(c=>s.appendChild(cardEl(c,true)));return s.outerHTML;}
  if(a.kind==='attack'||a.kind==='pass') return tag+seat+cardsHtml(a.cards);
  if(a.kind==='cover'){
    let h=tag+seat;
    for(let i=0;i<a.cards.length;i++){ h+=cardEl(a.cards[i],true).outerHTML+'<span class="arrow">&rarr;</span>'+cardEl(a.targets[i],true).outerHTML+' ';}
    return h;
  }
  if(a.kind==='pickup') return tag+seat+'takes '+a.n+' card'+(a.n>1?'s':'')+' '+cardsHtml(a.cards);
  if(a.kind==='good') return tag+seat+'accepts the defense (cards will discard)';
  if(a.kind==='discard') return tag+cardsHtml(a.cards)+' &rarr; discard';
  if(a.kind==='draw'){let h=tag+seat+'draws '+a.n+' card'+(a.n>1?'s':'');if(a.reveal&&a.reveal.length)h+=' '+cardsHtml(a.reveal)+' <span class="muted">(revealed)</span>';return h;}
  if(a.kind==='defender_change') return tag+'defender &rarr; p'+a.def;
  if(a.kind==='player_out') return tag+seat+'is eliminated';
  if(a.kind==='game_start') return tag+'deal complete';
  return tag;
}

function renderDecision(){
  const host=document.getElementById('decision'); host.innerHTML='';
  const o=L[cur];
  if(!o.decision){ host.innerHTML='<p class="muted" style="margin-left:2px">No bot decision at this log &mdash; step to a bot&rsquo;s turn (attack / cover / pass / pickup / good) to see its reasoning.</p>'; return; }
  if(o.decision.kind==='random') host.appendChild(randomPanel(o.decision));
  else host.appendChild(octogenPanel(o.decision));
}

// ---- RANDOM panel ----
function randomPanel(d){
  const panel=document.createElement('div'); panel.className='dpanel rnd';
  const n=d.choiceCount;
  let hh='<div class="dhead"><span class="t">random (p'+d.seat+') decision @ log '+d.ply+'</span>';
  hh+='<span class="chip rnd">uniform 1 / '+n+'</span>';
  hh+='<span class="muted mono">deck='+d.deck+'</span></div>';
  panel.innerHTML=hh;
  const intro=document.createElement('p'); intro.style.margin='2px 0 4px';
  intro.innerHTML='random has <b>no belief and no search</b>. It enumerated its <b>'+n+'</b> legal move'+(n>1?'s':'')+
    ' and picked one <b>uniformly at random</b> (each with probability '+ (n>1?('<b>'+(1/n).toFixed(3)+'</b>'):'<b>1.000</b>')+
    '). The recorded pick is highlighted.';
  panel.appendChild(intro);
  const menu=document.createElement('div'); menu.className='rmenu';
  const chosenN=normLabel(d.chosen);
  d.legal.forEach(lab=>{
    const row=document.createElement('div'); row.className='ropt'+(normLabel(lab)===chosenN?' picked':'');
    row.appendChild(labelChips(lab));
    if(normLabel(lab)===chosenN){ const p=document.createElement('span'); p.className='pick'; p.textContent='◀ played'; row.appendChild(p); }
    menu.appendChild(row);
  });
  panel.appendChild(menu);
  const co=document.createElement('div'); co.className='callout rnd';
  co.innerHTML='<b>Contrast with octogen.</b> Where an octogen seat would sample thousands of worlds and (in the endgame) '+
    'prove a win or loss, random treats all '+n+' of these as equally good. That&rsquo;s exactly why octogen beats it &mdash; '+
    'step to an octogen (p0&ndash;p3) turn to see the difference.';
  panel.appendChild(co);
  return panel;
}

// ---- OCTOGEN panel (full X-ray) ----
function octogenPanel(d){
  const solver=d.solver && d.solver.applied;
  const panel=document.createElement('div'); panel.className='dpanel og'+(solver?' solver':'');
  const recMove=d.recorded, match=d.match, forced=d.forced;
  let hh='<div class="dhead"><span class="t">octogen (p'+d.seat+') deliberation @ log '+d.ply+'</span>';
  hh+='<span class="chip '+(forced?'forced':(match?'match':'mismatch'))+'">'+(forced?'forced (only move)':(match?'reproduced exactly':'would differ'))+'</span>';
  hh+='<span class="muted mono">deck='+d.deck+' &middot; opp holds '+d.known.opp_count+'</span></div>';
  panel.innerHTML=hh;

  const hl=document.createElement('div'); hl.innerHTML='<h3>octogen (p'+d.seat+') hand</h3>';
  const hr=document.createElement('div'); hr.className='hand'; hr.appendChild(tokRow(d.hand,false)); hl.appendChild(hr); panel.appendChild(hl);

  const pk=document.createElement('p'); pk.style.margin='10px 0 2px';
  pk.innerHTML='<b>Recorded:</b> <span class="mono">'+esc(recMove)+'</span> &nbsp;&middot;&nbsp; <b>this build picks:</b> <span class="mono">'+esc(d.chosen)+'</span>';
  panel.appendChild(pk);

  const isSolverVerdict=solver && d.candidates.some(c=>c.verdict==='win'||c.verdict==='loss');
  const h3=document.createElement('h3');
  h3.textContent=isSolverVerdict?'Exact endgame-solver verdict per move':'Monte-Carlo candidates (avg finish, lower = better)';
  panel.appendChild(h3);
  const bars=document.createElement('div'); bars.className='bars';
  const cs=d.candidates.slice();
  const eff=(c)=>(c.adjScore!=null?c.adjScore:c.score);
  if(isSolverVerdict){ const ord={win:0,draw:1,unknown:2,none:3,loss:4,illegal:5}; cs.sort((a,b)=>(ord[a.verdict]-ord[b.verdict])); }
  else cs.sort((a,b)=>((a.score==null)-(b.score==null))||(eff(a)-eff(b)));
  const scored=cs.filter(c=>c.score!=null);
  const best=scored.length?Math.min.apply(null,scored.map(eff)):null;
  const anyTax=scored.some(c=>c.trumpTax>0);
  const recN=normLabel(recMove);
  cs.forEach(c=>{
    const row=document.createElement('div'); row.className='brow';
    if(c.score!=null && eff(c)===best) row.classList.add('best');
    if(c.chosen) row.classList.add('chosen');
    if(!c.alive) row.classList.add('pruned');
    const mv=document.createElement('div'); mv.className='bmove'; mv.appendChild(moveChips(c));
    if(normLabel(c.label)===recN){ const rt=document.createElement('span'); rt.className='chip forced'; rt.style.marginLeft='4px'; rt.textContent='recorded'; mv.appendChild(rt); }
    if(c.trumpTax>0){ const kt=document.createElement('span'); kt.className='chip trumpkeep'; kt.style.marginLeft='4px'; kt.textContent='trump‑keep +'+c.trumpTax.toFixed(3); mv.appendChild(kt); }
    row.appendChild(mv);
    const tr=document.createElement('div'); tr.className='btrack';
    const fill=document.createElement('div'); fill.className='bfill';
    if(c.score!=null){ const w=Math.max(2,Math.min(100,(2-eff(c))/1*100)); fill.style.width=w+'%'; }
    else fill.style.width='0%';
    tr.appendChild(fill); row.appendChild(tr);
    const tg=document.createElement('div'); tg.className='btag';
    if(isSolverVerdict){ tg.innerHTML='<span class="chip '+c.verdict+'">'+c.verdict+'</span>'; }
    else if(c.score!=null){
      if(c.trumpTax>0){ tg.innerHTML='<span class="raw">'+c.score.toFixed(4)+'</span> +'+c.trumpTax.toFixed(3)+' = <b>'+eff(c).toFixed(4)+'</b> <span class="dim">('+c.nsim+' sims)</span>'; }
      else{ tg.textContent=eff(c).toFixed(4)+'  ('+c.nsim+' sims)'; }
    }
    else { tg.innerHTML='<span class="chip unknown">'+(c.verdict||'n/a')+'</span>'; }
    row.appendChild(tg);
    bars.appendChild(row);
  });
  if(anyTax){ const kn=document.createElement('div'); kn.className='solvernote keepnote';
    kn.innerHTML='<b>Trump‑keep tax active.</b> While the deck is alive octogen adds <b>+'+(M.trumpKeep||0.04).toFixed(3)+'</b> to the average‑finish score of any move that <i>leads a trump</i> (per trump, attacks only), tipping near‑ties toward keeping the trump.';
    panel.appendChild(kn); }
  panel.appendChild(bars);
  if(solver && !isSolverVerdict){
    const sn=document.createElement('div'); sn.className='solvernote';
    sn.innerHTML='Endgame solver fired but could not prove a result within budget &mdash; the decision fell through to Monte&#8209;Carlo (scores above).';
    panel.appendChild(sn);
  }
  if(!match && !forced){
    const co=document.createElement('div'); co.className='callout warn';
    co.innerHTML=flaggedCallout(d,recMove,isSolverVerdict);
    panel.appendChild(co);
  }
  panel.appendChild(knownPanel(d));
  return panel;
}

function flaggedCallout(d,recMove,isSolverVerdict){
  const recN=normLabel(recMove);
  const rec=d.candidates.find(c=>normLabel(c.label)===recN);
  const chosen=d.candidates.find(c=>c.chosen) || d.candidates.find(c=>normLabel(c.label)===normLabel(d.chosen));
  let s='<b>octogen would differ.</b> Recorded was <span class="mono">'+esc(recMove)+'</span>; this build prefers <span class="mono">'+esc(d.chosen)+'</span>. ';
  if(isSolverVerdict && rec && chosen){
    s+='By the exact solver the recorded move is <span class="chip '+(rec.verdict||'unknown')+'">'+(rec.verdict||'n/a')+'</span> and octogen&rsquo;s pick is <span class="chip '+(chosen.verdict||'unknown')+'">'+(chosen.verdict||'n/a')+'</span>.';
  } else if(rec && chosen && rec.score!=null && chosen.score!=null){
    const gap=Math.abs(rec.score-chosen.score); const tie=gap<0.02;
    s+='In Monte&#8209;Carlo the recorded move scores <b>'+rec.score.toFixed(4)+'</b> vs octogen&rsquo;s <b>'+chosen.score.toFixed(4)+'</b> &mdash; a gap of '+gap.toFixed(4)+', '+(tie?'a <b>near&#8209;tie</b> inside sampling noise.':'a modest edge.');
  } else { s+='The two are close in octogen&rsquo;s evaluation.'; }
  return s;
}

function knownPanel(d){
  const k=d.known, empty=(k.deck===0);
  const proven=d.solver && d.solver.applied && d.candidates.some(c=>c.verdict==='win'||c.verdict==='loss');
  const wrap=document.createElement('div'); wrap.className='known'+(empty?' empty':'');
  let head='<h3 style="margin-top:0">octogen known state</h3>';
  head+='<div class="kmeta"><span>deck (face&#8209;down): <b>'+k.deck+'</b></span>'+
        '<span>opponents hold: <b>'+k.opp_count+'</b></span>'+
        '<span>pinned: <b>'+k.pinned_total+'</b></span></div>';
  wrap.innerHTML=head;
  k.opps.forEach(o=>{
    if(o.count<=0 && !o.pinned.length) return;
    const oppLabel='p'+o.seat;
    const g=document.createElement('div'); g.className='kgroup';
    if(o.pinned.length){
      g.innerHTML='<div class="klabel known-yes">knows '+oppLabel+' holds these '+o.pinned.length+' of '+o.count+' &mdash; watched them pick up:</div>';
      const row=document.createElement('div'); row.className='hand';
      o.pinned.forEach(c=>{const e=cardEl(c,true);e.classList.add('pinned');row.appendChild(e);});
      g.appendChild(row);
    }else{
      g.innerHTML='<div class="klabel muted">'+oppLabel+' ('+o.count+' cards): nothing pinned yet.</div>';
    }
    if((o.voids&&o.voids.length)||o.floor){
      const extra=document.createElement('div'); extra.className='klabel muted'; extra.style.marginTop='4px';
      const bits=[];
      if(o.voids&&o.voids.length) bits.push('can&rsquo;t cover '+o.voids.map(c=>cardHTML(c,true)).join(' '));
      if(o.floor) bits.push('lowest non&#8209;trump &ge; '+o.floor);
      extra.innerHTML='&hellip; deduced '+oppLabel+' '+bits.join('; ')+'.';
      g.appendChild(extra);
    }
    wrap.appendChild(g);
  });
  const poolWrap=document.createElement('div'); poolWrap.className='kgroup';
  const note=document.createElement('div'); note.className='klabel';
  if(empty && proven){
    note.innerHTML='<b>Deck empty</b> &mdash; these <b>'+k.pool.length+'</b> are the opponents&rsquo; remaining cards. With the '+k.pinned_total+' pinned above, octogen knows the <b>entire</b> unseen layout &mdash; the exact endgame solver proves the line:';
  }else if(empty){
    note.innerHTML='<b>Deck empty</b> &mdash; these <b>'+k.pool.length+'</b> are the opponents&rsquo; remaining cards. octogen knows the whole unseen layout, but with several seats still in this is past the exact solver&rsquo;s 2&#8209;player reach, so it still samples their placement across worlds:';
  }else{
    note.innerHTML='Unknown pool &mdash; these <b>'+k.pool.length+'</b> are split across the face&#8209;down deck ('+k.deck+') and opponents&rsquo; un&#8209;pinned cards. octogen samples their placement across worlds:';
  }
  poolWrap.appendChild(note);
  const pool=document.createElement('div'); pool.className='hand';
  k.pool.forEach(c=>{const e=cardEl(c,true);e.classList.add('poolc');pool.appendChild(e);});
  if(!k.pool.length){ pool.innerHTML='<span class="muted">(nothing unknown)</span>'; }
  poolWrap.appendChild(pool);
  wrap.appendChild(poolWrap);
  return wrap;
}

function moveChips(c){
  const f=document.createElement('span'); f.className='bmove';
  if(c.type==='cover'){
    for(let i=0;i<c.cards.length;i++){
      if(i){const sep=document.createElement('span');sep.className='muted';sep.style.margin='0 3px';sep.textContent='+';f.appendChild(sep);}
      f.appendChild(cardEl(tok(c.cards[i]),true));
      const ar=document.createElement('span');ar.className='arrow';ar.innerHTML='&rarr;';f.appendChild(ar);
      f.appendChild(cardEl(tok(c.target[i]),true));
    }
  } else if(c.type==='pickup'){ const s=document.createElement('span');s.className='mono';s.textContent='pickup';f.appendChild(s);}
  else if(c.type==='good'){ const s=document.createElement('span');s.className='mono';s.textContent='good';f.appendChild(s);}
  else { const lab=document.createElement('span');lab.className='mono muted';lab.style.marginRight='4px';lab.textContent=c.type;f.appendChild(lab); c.cards.forEach(t=>f.appendChild(cardEl(tok(t),true))); }
  return f;
}
function normLabel(s){
  const parts=String(s).replace(/,/g,' ').trim().split(/\s+/);
  return parts[0]+' '+parts.slice(1).sort().join(' ');
}

// jump buttons — one per octogen-would-differ decision
const jb=document.getElementById('jump');
if(!D.octoDiffer.length){ jb.innerHTML='<span class="muted">(none &mdash; every non-forced octogen move reproduced exactly)</span>'; }
D.octoDiffer.forEach(i=>{
  const o=L[i]; const rec=o.decision?o.decision.recorded:'';
  const b=document.createElement('button'); b.innerHTML='log '+i+' &middot; p'+o.decision.seat+' &middot; '+esc(rec);
  b.onclick=()=>{cur=i;renderBoard();document.getElementById('decision').scrollIntoView({behavior:'smooth',block:'center'});};
  jb.appendChild(b);
});

function jumpNext(pred){ for(let i=cur+1;i<N;i++){ if(pred(L[i])){cur=i;break;} } renderBoard(); document.getElementById('decision').scrollIntoView({behavior:'smooth',block:'nearest'}); }
document.getElementById('slider').max=N-1;
document.getElementById('first').onclick=()=>{cur=0;renderBoard();};
document.getElementById('prev').onclick=()=>{if(cur>0)cur--;renderBoard();};
document.getElementById('next').onclick=()=>{if(cur<N-1)cur++;renderBoard();};
document.getElementById('nextd').onclick=()=>jumpNext(o=>o.decision);
document.getElementById('nextog').onclick=()=>jumpNext(o=>o.decision&&o.decision.kind==='octogen');
document.getElementById('nextrnd').onclick=()=>jumpNext(o=>o.decision&&o.decision.kind==='random');
document.getElementById('slider').oninput=(e)=>{cur=+e.target.value;renderBoard();};
document.addEventListener('keydown',e=>{if(e.key==='ArrowLeft'){if(cur>0)cur--;renderBoard();}else if(e.key==='ArrowRight'){if(cur<N-1)cur++;renderBoard();}});

const tb=document.getElementById('themebtn');
tb.onclick=()=>{const r=document.documentElement;const c=r.getAttribute('data-theme')||(window.matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');r.setAttribute('data-theme',c==='dark'?'light':'dark');};

renderBoard();
})();
"""


def render(data):
    data_json = json.dumps(data, separators=(',', ':'))
    return HTML.replace('__CSS__', CSS).replace('__DATA__', data_json).replace('__JS__', JS)
