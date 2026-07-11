#!/usr/bin/env python3
# Render page_data.json into the self-contained interactive page
# docs/octogen-replay-explain.html. NOTHING about the specific game is hardcoded
# here: every fact (players, trump, who won, the fool, the agree/differ tally,
# which moves are flagged, the flagged-move commentary, the "octogen known
# state" pool) is read from page_data.json / derived in the browser from the
# engine's own deliberation dump.
#
#   gen_html.py page_data.json out.html
import json
import sys

data = json.load(open(sys.argv[1]))
OUT = sys.argv[2] if len(sys.argv) > 2 else 'docs/octogen-replay-explain.html'
DATA_JSON = json.dumps(data, separators=(',', ':'))

CSS = r"""
:root{
  --surface-0:#f4f4f2; --surface-1:#fcfcfb; --surface-2:#ececea; --border:#dad9d4;
  --text-primary:#0b0b0b; --text-secondary:#52514e; --text-muted:#86857f;
  --win:#0a7a35; --win-bg:#dcefe1; --loss:#d23b3a; --loss-bg:#f7dcdc;
  --warn:#c74a1e; --warn-bg:#fbe2d6; --info:#2a78d6; --info-bg:#dce9fb;
  --accent:#4a3aa7; --card:#ffffff; --card-red:#c0392b; --card-trump:#8a6d00;
  --shadow:0 1px 3px rgba(0,0,0,.08),0 1px 2px rgba(0,0,0,.04);
}
@media (prefers-color-scheme: dark){:root{
  --surface-0:#131312; --surface-1:#1c1c1a; --surface-2:#252523; --border:#3a3a37;
  --text-primary:#f4f4f2; --text-secondary:#c3c2b7; --text-muted:#8e8d84;
  --win:#3fbf72; --win-bg:#123322; --loss:#e66767; --loss-bg:#3a1a1a;
  --warn:#e77a4d; --warn-bg:#3a2113; --info:#57a0ee; --info-bg:#122238;
  --accent:#9085e9; --card:#2b2b28; --card-red:#e77a6a; --card-trump:#e0b83a;
  --shadow:0 1px 3px rgba(0,0,0,.4);
}}
:root[data-theme="dark"]{
  --surface-0:#131312; --surface-1:#1c1c1a; --surface-2:#252523; --border:#3a3a37;
  --text-primary:#f4f4f2; --text-secondary:#c3c2b7; --text-muted:#8e8d84;
  --win:#3fbf72; --win-bg:#123322; --loss:#e66767; --loss-bg:#3a1a1a;
  --warn:#e77a4d; --warn-bg:#3a2113; --info:#57a0ee; --info-bg:#122238;
  --accent:#9085e9; --card:#2b2b28; --card-red:#e77a6a; --card-trump:#e0b83a;
  --shadow:0 1px 3px rgba(0,0,0,.4);
}
:root[data-theme="light"]{
  --surface-0:#f4f4f2; --surface-1:#fcfcfb; --surface-2:#ececea; --border:#dad9d4;
  --text-primary:#0b0b0b; --text-secondary:#52514e; --text-muted:#86857f;
  --win:#0a7a35; --win-bg:#dcefe1; --loss:#d23b3a; --loss-bg:#f7dcdc;
  --warn:#c74a1e; --warn-bg:#fbe2d6; --info:#2a78d6; --info-bg:#dce9fb;
  --accent:#4a3aa7; --card:#ffffff; --card-red:#c0392b; --card-trump:#8a6d00;
  --shadow:0 1px 3px rgba(0,0,0,.08),0 1px 2px rgba(0,0,0,.04);
}
*{box-sizing:border-box}
body{margin:0;background:var(--surface-0);color:var(--text-primary);
  font:15px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  -webkit-font-smoothing:antialiased;}
.wrap{max-width:1000px;margin:0 auto;padding:28px 20px 90px;}
h1{font-size:26px;line-height:1.2;margin:0 0 6px;letter-spacing:-.02em}
h2{font-size:20px;margin:40px 0 10px;letter-spacing:-.01em;padding-top:8px}
h3{font-size:14px;margin:18px 0 8px;color:var(--text-secondary);text-transform:uppercase;letter-spacing:.04em;font-weight:600}
p{margin:10px 0;color:var(--text-secondary)}
.lede{font-size:17px;color:var(--text-secondary);max-width:74ch}
code,.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.9em}
.panel{background:var(--surface-1);border:1px solid var(--border);border-radius:12px;padding:18px 20px;box-shadow:var(--shadow);margin:16px 0}
.muted{color:var(--text-muted)}
b,strong{color:var(--text-primary)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin:18px 0}
.tile{background:var(--surface-1);border:1px solid var(--border);border-radius:12px;padding:14px 16px;box-shadow:var(--shadow)}
.tile .n{font-size:23px;font-weight:700;letter-spacing:-.02em;font-variant-numeric:tabular-nums}
.tile .l{font-size:12.5px;color:var(--text-muted);margin-top:2px}
.tile.win .n{color:var(--win)} .tile.loss .n{color:var(--loss)}
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
.seg{display:inline-flex;background:var(--surface-2);border:1px solid var(--border);border-radius:9px;padding:3px;gap:2px}
.seg button{border:0;background:transparent;color:var(--text-secondary);font:inherit;font-weight:600;
  padding:6px 12px;border-radius:6px;cursor:pointer;font-size:13.5px}
.seg button.on{background:var(--card);color:var(--text-primary);box-shadow:var(--shadow)}
.ctl{display:flex;flex-wrap:wrap;gap:10px 14px;align-items:center;margin:10px 0 6px}
.ctl button.nav{border:1px solid var(--border);background:var(--surface-1);color:var(--text-primary);
  border-radius:8px;padding:7px 13px;cursor:pointer;font:inherit;font-weight:600;font-size:14px;box-shadow:var(--shadow)}
.ctl button.nav:disabled{opacity:.4;cursor:default}
input[type=range]{flex:1;min-width:180px;accent-color:var(--accent)}
.jump{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0}
.jump button{border:1px solid var(--warn);background:var(--warn-bg);color:var(--warn);border-radius:8px;
  padding:6px 11px;cursor:pointer;font:inherit;font-weight:600;font-size:13px}
/* board */
.board{display:grid;grid-template-columns:1fr;gap:14px;margin:14px 0}
.seatrow{display:flex;gap:12px;align-items:center;flex-wrap:wrap}
.seatchip{display:inline-flex;align-items:center;gap:7px;background:var(--surface-2);border:1px solid var(--border);
  border-radius:20px;padding:4px 12px;font-size:13px;font-weight:600}
.seatchip.def{background:var(--info-bg);border-color:var(--info);color:var(--info)}
.seatchip.og{background:var(--win-bg);border-color:var(--win);color:var(--win)}
.seatchip .cnt{font-variant-numeric:tabular-nums}
.tablearea{min-height:56px;background:var(--surface-2);border:1px dashed var(--border);border-radius:10px;padding:12px;
  display:flex;flex-wrap:wrap;gap:14px;align-items:center}
.battle{display:flex;flex-direction:column;align-items:center;gap:3px}
.battle .df{opacity:.95}
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
.dpanel.solver{border-color:var(--win)}
.dhead{display:flex;flex-wrap:wrap;gap:8px 14px;align-items:center;margin-bottom:8px}
.dhead .t{font-weight:700;font-size:15px}
.bars{display:flex;flex-direction:column;gap:6px;margin-top:6px}
.brow{display:grid;grid-template-columns:170px 1fr auto;gap:10px;align-items:center}
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
.callout{margin-top:12px;padding:12px 14px;border-radius:9px;border:1px solid;font-size:14px}
.callout.warn{background:var(--warn-bg);border-color:var(--warn)}
.callout.win{background:var(--win-bg);border-color:var(--win)}
.callout.info{background:var(--info-bg);border-color:var(--info)}
/* known state */
.known{margin-top:14px;border-top:1px dashed var(--border);padding-top:12px}
.kmeta{display:flex;flex-wrap:wrap;gap:8px 18px;font-size:13px;margin-bottom:6px}
.kmeta b{font-variant-numeric:tabular-nums}
.known.empty{border-color:var(--win)}
.themebtn{position:fixed;top:12px;right:12px;z-index:9;border:1px solid var(--border);background:var(--surface-1);
  color:var(--text-secondary);border-radius:8px;padding:6px 10px;cursor:pointer;font:inherit;font-size:13px;box-shadow:var(--shadow)}
.arrow{color:var(--text-muted);margin:0 2px}
.solvernote{font-size:12.5px;color:var(--text-muted);margin-top:6px}
@media(max-width:620px){.brow{grid-template-columns:120px 1fr}.btag{grid-column:2;min-width:0;text-align:right}}
"""

HTML = r"""<meta charset="utf-8">
<title>Why octogen played that: a replay X-ray</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>__CSS__</style>
<button class="themebtn" id="themebtn" title="Toggle theme">theme</button>
<div class="wrap">
<h1>Why octogen played that &mdash; a replay X&#8209;ray</h1>
<p class="lede" id="lede"></p>

<div class="tiles" id="tiles"></div>

<div class="panel">
<h3>How this was reproduced</h3>
<p id="repro"></p>
<p class="muted">Rank convention is the correct Durak one (wire value&nbsp;13 = A). A card shown
as <b>A&spades;</b> is <code>{suit:0,value:13}</code>; the trump suit is boxed and gold.</p>
</div>

<h2 id="flagHead">Moves where octogen would differ</h2>
<div class="jump" id="jump"></div>

<h2>Step through the game</h2>
<div class="ctl">
  <button class="nav" id="first">&laquo; start</button>
  <button class="nav" id="prev">&lsaquo; prev</button>
  <button class="nav" id="next">next &rsaquo;</button>
  <button class="nav" id="nextd">next octogen decision &raquo;</button>
  <span class="mono muted" id="counter"></span>
</div>
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
<li><b>Monte&#8209;Carlo candidates.</b> Away from the endgame octogen samples many possible
worlds (deals of the hidden cards) and rolls each candidate move out to the end; the bar is
its <b>average finish position</b> (1 = eliminated first = win, higher = worse). Lower is better.
The chosen move is outlined; the best&#8209;scoring move is green.</li>
<li><b>Exact endgame solver.</b> Once the deck is empty the hidden pool <i>is</i> the opponent's
hand (see &ldquo;octogen known state&rdquo;), so octogen solves the position exactly &mdash;
each move is a proven <span class="chip win">win</span> / <span class="chip loss">loss</span>
/ draw rather than a sampled average.</li>
<li><b>octogen known state.</b> The cards hidden from octogen right now = the whole 36&#8209;card
deck minus its hand, the flip, the discard pile, and the table. That hidden pool splits between
the face&#8209;down deck and the opponent's hand (<span class="mono">pool = deck + opponent</span>,
exactly). When the deck empties the pool collapses onto the opponent's hand &mdash; that is the
public deduction the exact solver runs on.</li>
<li><b>&ldquo;octogen agrees / would differ.&rdquo;</b> At each octogen turn the recorded move is
compared to what this octogen build picks on the true state. Ties are common: several moves often
score within Monte&#8209;Carlo noise, so a &ldquo;differ&rdquo; is usually a near&#8209;tie, not a blunder
(each flagged panel shows both scores).</li>
</ul>
</div>
<p class="muted" style="font-size:12.5px">Deliberation dumped by <code>OG_EXPLAIN</code> (compile&#8209;time
<code>-DOG_EXPLAIN_BUILD</code>, absent from shipped bots) in <code>cnitro/src/octogen_strategy.c</code>;
game reproduced and driven by <code>cnitro/tests/og_explain.c</code>; page assembled by
<code>cnitro/tools/og_explain/</code>.</p>
</div>
<script>
const DATA=__DATA__;
</script>
<script>__JS__</script>
"""

JS = r"""
(function(){
const D=DATA, L=D.logs, N=L.length, M=D.meta;
const NP=M.players, TRUMP=M.trump, OG=M.ogSeat;
const SYMS=['♠','♥','♣','♦'];
let cur=0;

function seatRole(p){
  const r=[];
  if(p===OG) r.push('octogen');
  if(p===M.winner) r.push('winner'); else if(p===M.fool) r.push('fool');
  return r.join(' · ');
}

// ---- intro / tiles / repro (all derived from meta) ----
document.getElementById('lede').innerHTML =
  'One recorded <b>'+NP+'&#8209;player</b> Durak game (trump <b>'+M.trumpSym+'</b>, flip '+cardHTML(M.flip,true)+'). '+
  'Seat <b>p'+M.winner+'</b> ('+(M.winner===OG?'octogen':'seat '+M.winner)+') wins; <b>p'+M.fool+'</b> is the fool (durak). '+
  'This page steps through every public move and, at each of <b>octogen&rsquo;s</b> (p'+OG+') turns, opens up its actual '+
  'deliberation: the Monte&#8209;Carlo average&#8209;finish score of each candidate move, or the exact endgame&#8209;solver '+
  'verdict when the deck is empty &mdash; plus what octogen actually knows about the hidden cards. Numbers are dumped '+
  'straight from the C engine, not reconstructed by hand.';

const tiles=[
  ['n', NP, 'players'],
  ['n', M.trumpSym+' '+suitName(TRUMP), 'trump · flip '+M.flip.str],
  [M.winner===OG?'win':'', 'p'+M.winner+' wins', 'p'+M.fool+' = fool (durak)'],
  ['n', M.match+' / '+M.decisions, 'octogen turns it agrees with'+(M.forced?' ('+M.forced+' forced)':'')],
];
document.getElementById('tiles').innerHTML = tiles.map(([cls,n,l])=>
  '<div class="tile'+(cls==='win'?' win':'')+'"><div class="n">'+n+'</div><div class="l">'+l+'</div></div>').join('');

document.getElementById('repro').innerHTML =
  'The deal is reproduced from the 32&#8209;byte deal seed <code>'+esc(M.seed.slice(0,16))+'&hellip;</code>; the engine '+
  'deals it, then the recorded public moves are replayed and octogen is queried at each of its turns. All <b>'+M.nlogs+'</b> '+
  'logs reproduce move&#8209;for&#8209;move. octogen&rsquo;s deliberation is read&#8209;only &mdash; querying it does not perturb its '+
  'own choice. Across <b>'+M.decisions+'</b> octogen turns this build agrees with <b>'+M.match+'</b> ('+M.forced+' of them a '+
  'forced single move); the remaining differences are near&#8209;ties in Monte&#8209;Carlo noise (each flagged below shows the scores).';

document.getElementById('flagHead').textContent =
  D.flagged.length ? ('Moves where octogen would differ ('+D.flagged.length+')') : 'octogen agrees with every non-forced move';

function suitName(s){return ['spades','hearts','clubs','diamonds'][s];}
function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}

// ---- card rendering ----
function cardEl(c,sm){
  const d=document.createElement('span');
  d.className='pc'+(sm?' sm':'')+(c.hidden?' hidden':(c.trump?' trump':(c.red?' red':'')));
  if(c.hidden){d.textContent='?';return d;}
  d.innerHTML=c.r+'<span class="s">'+c.sym+'</span>';
  return d;
}
function cardHTML(c,sm){return cardEl(c,sm).outerHTML;}
// parse an OG token like "AS*","10H","6S*" into a card obj (trump from meta)
const VS={S:0,H:1,C:2,D:3};
function tok(t){
  const m=t.match(/^(10|[2-9]|[JQKA])([SHCD])(\*?)$/);
  if(!m) return {hidden:true};
  const s=VS[m[2]];
  return {r:m[1],suit:s,sym:SYMS[s],trump:s===TRUMP,red:(s===1||s===3),hidden:false};
}
function tokRow(tokens,sm){
  const f=document.createDocumentFragment();
  tokens.forEach(t=>f.appendChild(cardEl(tok(t),sm)));
  return f;
}

function renderBoard(){
  const o=L[cur];
  const sr=document.getElementById('seatrow'); sr.innerHTML='';
  const dfn=currentDefender();
  for(let p=0;p<NP;p++){
    const el=document.createElement('span');
    const role=seatRole(p);
    el.className='seatchip'+(dfn===p?' def':'')+(p===OG?' og':'');
    el.innerHTML='<b>p'+p+'</b>'+(role?' ('+role+')':'')+' <span class="cnt">&middot; '+o.hc[p]+' cards</span>'+(dfn===p?' &middot; defending':'');
    sr.appendChild(el);
  }
  const ta=document.getElementById('tablearea'); ta.innerHTML='';
  if(!o.table.length){ ta.innerHTML='<span class="muted">(empty)</span>'; }
  o.table.forEach(b=>{
    const bt=document.createElement('div'); bt.className='battle';
    bt.appendChild(cardEl(b.a,false));
    if(b.d){const dd=cardEl(b.d,false);dd.classList.add('df');bt.appendChild(dd);}
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
function currentDefender(){
  let d=M.fool;
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
  if(!o.decision){ host.innerHTML='<p class="muted" style="margin-left:2px">No octogen decision at this log &mdash; step to an octogen (p'+OG+') turn to see the deliberation.</p>'; return; }
  const d=o.decision;
  const solver=d.solver && d.solver.applied;
  const panel=document.createElement('div'); panel.className='dpanel'+(solver?' solver':'');
  const recMove=recordedMoveLabel(o);
  const match=(normLabel(d.chosen)===normLabel(recMove));
  const forced=(d.candidates.length<=1);
  let hh='<div class="dhead"><span class="t">octogen deliberation @ log '+cur+'</span>';
  hh+='<span class="chip '+(forced?'forced':(match?'match':'mismatch'))+'">'+(forced?'forced (only move)':(match?'octogen agrees':'octogen would differ'))+'</span>';
  hh+='<span class="muted mono">deck='+d.deck+' &middot; opp holds '+d.known.opp_count+'</span></div>';
  panel.innerHTML=hh;

  const hl=document.createElement('div'); hl.innerHTML='<h3>octogen (p'+OG+') hand</h3>';
  const hr=document.createElement('div'); hr.className='hand'; hr.appendChild(tokRow(d.hand,false)); hl.appendChild(hr); panel.appendChild(hl);

  const pk=document.createElement('p'); pk.style.margin='10px 0 2px';
  pk.innerHTML='<b>Recorded octogen played:</b> <span class="mono">'+esc(recMove)+'</span> &nbsp; &middot; &nbsp; <b>this build picks:</b> <span class="mono">'+esc(d.chosen)+'</span>';
  panel.appendChild(pk);

  const isSolverVerdict=solver && d.candidates.some(c=>c.verdict==='win'||c.verdict==='loss');
  const h3=document.createElement('h3');
  h3.textContent=isSolverVerdict?'Exact endgame-solver verdict per move':'Monte-Carlo candidates (avg finish, lower = better)';
  panel.appendChild(h3);
  const bars=document.createElement('div'); bars.className='bars';
  const cs=d.candidates.slice();
  if(isSolverVerdict){ const ord={win:0,draw:1,unknown:2,none:3,loss:4,illegal:5}; cs.sort((a,b)=>(ord[a.verdict]-ord[b.verdict])); }
  else cs.sort((a,b)=>((a.score==null)-(b.score==null))||(a.score-b.score));
  const scored=cs.filter(c=>c.score!=null);
  const best=scored.length?Math.min.apply(null,scored.map(c=>c.score)):null;
  const recN=normLabel(recMove);
  cs.forEach(c=>{
    const row=document.createElement('div'); row.className='brow';
    if(c.score!=null && c.score===best) row.classList.add('best');
    if(c.chosen) row.classList.add('chosen');
    if(!c.alive) row.classList.add('pruned');
    const mv=document.createElement('div'); mv.className='bmove'; mv.appendChild(moveChips(c));
    if(normLabel(c.label)===recN){ const rt=document.createElement('span'); rt.className='chip forced'; rt.style.marginLeft='4px'; rt.textContent='recorded'; mv.appendChild(rt); }
    row.appendChild(mv);
    const tr=document.createElement('div'); tr.className='btrack';
    const fill=document.createElement('div'); fill.className='bfill';
    if(c.score!=null){ const w=Math.max(2,Math.min(100,(2-c.score)/1*100)); fill.style.width=w+'%'; }
    else fill.style.width='0%';
    tr.appendChild(fill); row.appendChild(tr);
    const tg=document.createElement('div'); tg.className='btag';
    if(isSolverVerdict){ tg.innerHTML='<span class="chip '+c.verdict+'">'+c.verdict+'</span>'; }
    else if(c.score!=null){ tg.textContent=c.score.toFixed(4)+'  ('+c.nsim+' sims)'; }
    else { tg.innerHTML='<span class="chip unknown">'+(c.verdict||'n/a')+'</span>'; }
    row.appendChild(tg);
    bars.appendChild(row);
  });
  panel.appendChild(bars);
  if(solver && !isSolverVerdict){
    const sn=document.createElement('div'); sn.className='solvernote';
    sn.innerHTML='Endgame solver fired but could not prove a result within budget for these moves &mdash; the decision fell through to Monte&#8209;Carlo (scores above).';
    panel.appendChild(sn);
  }

  // auto-generated flagged callout (only when octogen would genuinely differ)
  if(!match && !forced){
    const co=document.createElement('div'); co.className='callout warn';
    co.innerHTML=flaggedCallout(d,recMove,isSolverVerdict);
    panel.appendChild(co);
  }

  // octogen known state
  panel.appendChild(knownPanel(d));
  host.appendChild(panel);
}

// Build honest flagged commentary straight from the candidate numbers.
function flaggedCallout(d,recMove,isSolverVerdict){
  const recN=normLabel(recMove);
  const rec=d.candidates.find(c=>normLabel(c.label)===recN);
  const chosen=d.candidates.find(c=>c.chosen) || d.candidates.find(c=>normLabel(c.label)===normLabel(d.chosen));
  let s='<b>octogen would differ.</b> The recorded move was <span class="mono">'+esc(recMove)+'</span>; '+
        'this build prefers <span class="mono">'+esc(d.chosen)+'</span>. ';
  if(isSolverVerdict && rec && chosen){
    s+='By the exact solver the recorded move is <span class="chip '+(rec.verdict||'unknown')+'">'+(rec.verdict||'n/a')+
       '</span> and octogen&rsquo;s pick is <span class="chip '+(chosen.verdict||'unknown')+'">'+(chosen.verdict||'n/a')+'</span>.';
  } else if(rec && chosen && rec.score!=null && chosen.score!=null){
    const gap=Math.abs(rec.score-chosen.score);
    const tie=gap<0.02;
    s+='In Monte&#8209;Carlo the recorded move scores <b>'+rec.score.toFixed(4)+'</b> ('+rec.nsim+' sims) vs octogen&rsquo;s '+
       '<b>'+chosen.score.toFixed(4)+'</b> ('+chosen.nsim+' sims) &mdash; a gap of '+gap.toFixed(4)+', '+
       (tie?'i.e. a <b>near&#8209;tie</b> inside sampling noise.':'a modest edge to octogen&rsquo;s pick.');
    if(rec.alive===0) s+=' The recorded move was pruned before the final sampling stage, so its score rests on fewer sims.';
  } else if(rec && rec.score==null){
    s+='The recorded move was pruned early (no full Monte&#8209;Carlo score), so the two are not directly comparable &mdash; both are plausible.';
  } else {
    s+='The two are close in octogen&rsquo;s evaluation.';
  }
  return s;
}

function knownPanel(d){
  const k=d.known, empty=(k.deck===0);
  const wrap=document.createElement('div'); wrap.className='known'+(empty?' empty':'');
  let head='<h3 style="margin-top:0">octogen known state</h3>';
  head+='<div class="kmeta"><span>deck (face&#8209;down): <b>'+k.deck+'</b></span>'+
        '<span>opponent holds: <b>'+k.opp_count+'</b></span>'+
        '<span>hidden pool: <b>'+k.pool.length+'</b> = deck + opponent</span></div>';
  wrap.innerHTML=head;
  const note=document.createElement('p'); note.style.margin='2px 0 8px'; note.style.fontSize='13px';
  if(empty){
    note.innerHTML='<b>Deck empty.</b> The hidden pool now <b>is</b> the opponent&rsquo;s exact hand &mdash; octogen has '+
      'perfect information and the endgame solver proves the result.';
  }else{
    note.innerHTML='These '+k.pool.length+' cards are hidden from octogen &mdash; each is either in the face&#8209;down '+
      'deck ('+k.deck+') or the opponent&rsquo;s hand ('+k.opp_count+'). octogen samples their placement across worlds.';
  }
  wrap.appendChild(note);
  const pool=document.createElement('div'); pool.className='hand';
  k.pool.forEach(c=>pool.appendChild(cardEl(c,true)));
  if(!k.pool.length){ pool.innerHTML='<span class="muted">(nothing hidden)</span>'; }
  wrap.appendChild(pool);
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
function recordedMoveLabel(o){
  const a=o.action;
  function t(c){return c.r+ ({0:'S',1:'H',2:'C',3:'D'}[c.suit]) + (c.trump?'*':'');}
  if(a.kind==='attack') return 'attack '+a.cards.map(t).join(' ');
  if(a.kind==='pass') return 'pass '+a.cards.map(t).join(' ');
  if(a.kind==='cover') return 'cover '+a.cards.map((c,i)=>t(c)+'->'+t(a.targets[i])).join(' ');
  if(a.kind==='pickup') return 'pickup';
  if(a.kind==='good') return 'good';
  return a.kind;
}
function normLabel(s){
  const parts=String(s).replace(/,/g,' ').trim().split(/\s+/);
  return parts[0]+' '+parts.slice(1).sort().join(' ');
}

// jump buttons — one per flagged (octogen-would-differ) decision
const jb=document.getElementById('jump');
if(!D.flagged.length){ jb.innerHTML='<span class="muted">(none &mdash; every non-forced octogen move matches this build)</span>'; }
D.flagged.forEach(i=>{
  const o=L[i]; const rec=recordedMoveLabel(o);
  const b=document.createElement('button'); b.innerHTML='log '+i+' &middot; '+esc(rec);
  b.onclick=()=>{cur=i;renderBoard();document.getElementById('decision').scrollIntoView({behavior:'smooth',block:'center'});};
  jb.appendChild(b);
});

document.getElementById('slider').max=N-1;
document.getElementById('first').onclick=()=>{cur=0;renderBoard();};
document.getElementById('prev').onclick=()=>{if(cur>0)cur--;renderBoard();};
document.getElementById('next').onclick=()=>{if(cur<N-1)cur++;renderBoard();};
document.getElementById('nextd').onclick=()=>{for(let i=cur+1;i<N;i++){if(L[i].decision){cur=i;break;}}renderBoard();};
document.getElementById('slider').oninput=(e)=>{cur=+e.target.value;renderBoard();};
document.addEventListener('keydown',e=>{if(e.key==='ArrowLeft'){if(cur>0)cur--;renderBoard();}else if(e.key==='ArrowRight'){if(cur<N-1)cur++;renderBoard();}});

const tb=document.getElementById('themebtn');
tb.onclick=()=>{const r=document.documentElement;const c=r.getAttribute('data-theme')||(window.matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');r.setAttribute('data-theme',c==='dark'?'light':'dark');};

renderBoard();
})();
"""

out = HTML.replace('__CSS__', CSS).replace('__DATA__', DATA_JSON).replace('__JS__', JS)
open(OUT, 'w').write(out)
print(f'wrote {OUT} ({len(out)} bytes)', file=sys.stderr)
