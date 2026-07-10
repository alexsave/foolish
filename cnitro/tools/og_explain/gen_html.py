import json
data=json.load(open('page_data.json'))
DATA_JSON=json.dumps(data,separators=(',',':'))

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
<p class="lede">One recorded 2&#8209;player Durak game (trump&nbsp;<b>&spades;</b>, flip&nbsp;6&spades;).
<b>p1</b> attacks first and is eliminated first &mdash; <b>p1 wins</b>; <b>p0</b> is the fool. This page steps through every
public move and, at each of <b>p1</b>'s turns, opens up octogen's actual deliberation: the Monte&#8209;Carlo
average&#8209;finish score of each candidate move, or the exact endgame&#8209;solver verdict when the deck is empty.
Numbers are dumped straight from the C engine (<code>OG_EXPLAIN</code>), not reconstructed by hand.</p>

<div class="tiles">
  <div class="tile"><div class="n">2</div><div class="l">players (heads&#8209;up)</div></div>
  <div class="tile"><div class="n">&spades; spades</div><div class="l">trump &middot; flip 6&spades;</div></div>
  <div class="tile win"><div class="n">p1 wins</div><div class="l">p0 = fool (durak)</div></div>
  <div class="tile"><div class="n">22 / 37</div><div class="l">p1 moves octogen agrees with</div></div>
</div>

<div class="panel">
<h3>How this was reproduced (read me)</h3>
<p><b>Self&#8209;play from the seed does not reproduce this game</b> &mdash; p0 diverges on its very first response,
so p0 was not this octogen (a human or other bot). The supplied 32&#8209;byte deal seed also does <b>not</b> reproduce
the recorded draw order under the native engine (it matches only the first ~27 logs by loose coincidence, then p0
is dealt a card the seed puts deep in the deck). So the deck was <b>reconstructed directly from the recorded moves</b>
(the 36&#8209;card set is known; every card p1 ever holds is eventually revealed, and p1 empties its hand at the end,
so p1's hand is pinned exactly at every turn). Injecting that reconstructed deal into the engine and driving the
recorded moves reproduces <b>all 131 logs move&#8209;for&#8209;move</b>. octogen is then queried at each p1 turn on the
true state; its deliberation is faithful, deterministic, and does not perturb its own choice.</p>
<p class="muted">Rank convention is the correct Durak one (wire value 13 = A): the bundled <code>replay_pretty.txt</code>
renderer is off&#8209;by&#8209;one (it prints A as K, etc.) &mdash; this page and the task descriptions use the engine's
correct mapping, so a card shown here as <b>A&spades;</b> is <code>{suit:0,value:13}</code>.</p>
</div>

<h2>The three flagged moves</h2>
<div class="jump" id="jump"></div>

<h2>Step through the game</h2>
<div class="ctl">
  <button class="nav" id="first">&laquo; start</button>
  <button class="nav" id="prev">&lsaquo; prev</button>
  <button class="nav" id="next">next &rsaquo;</button>
  <button class="nav" id="nextd">next p1 decision &raquo;</button>
  <span class="mono muted" id="counter"></span>
</div>
<div class="ctl"><input type="range" id="slider" min="0" max="130" value="0"></div>

<div class="board panel">
  <div class="seatrow" id="seatrow"></div>
  <div>
    <h3 style="margin-top:2px">Table</h3>
    <div class="tablearea" id="tablearea"></div>
  </div>
  <div class="actline" id="actline"></div>
</div>

<div id="decision"></div>

<h2 style="margin-top:44px">Notes &amp; findings</h2>
<div class="panel">
<ul style="color:var(--text-secondary);margin:6px 0;padding-left:20px;line-height:1.7">
<li><b>Log 31 (flagged) &mdash; covering J&hearts; with the trump Ace is cosmetically odd but materially neutral.</b>
octogen would use the <b>Queen</b>; the two single&#8209;covers are a near&#8209;tie in MC &mdash; the recorded ace&#8209;cover
scored 1.4271 (192 sims) vs octogen's queen&#8209;cover 1.4329 (864 sims), i.e. the ace&#8209;cover was marginally
<i>lower</i> but got pruned before the final stage, so octogen's queen preference is noise&#8209;level. Because the whole
pile is picked up next turn, the covering trump returns to hand &mdash; no trump is actually lost. p1 <b>did</b> hold
trumps (Q&spades;, A&spades;) able to cover the 10&diams; (even both attacks at once), but leaving 10&diams; and picking up
was correct (log 32: pickup 1.4155 beats cover&#8209;10&diams; 1.4537).</li>
<li><b>Log 116 (flagged) &mdash; the three&#8209;Ace attack is a sound unload, not a waste.</b> The exact solver could
not resolve this 6&#8209;vs&#8209;6 attack position even at 200M nodes, so octogen used Monte&#8209;Carlo, which scored the
A&clubs;&nbsp;A&spades;&nbsp;A&diams; attack a <b>certain win</b> (finish&nbsp;1 in all 864 sampled worlds). Three same&#8209;rank
Aces are a legal multi&#8209;card attack that p0 cannot cover; p0 picks up, and p1 sheds its high cards on the way to the win.</li>
<li><b>Endgame solver works where the tree is small.</b> By log 127 (p1 holds a lone 6&spades;) the solver <b>proves</b>
the position: cover 6&spades;&rarr;A&clubs; = <span class="chip win">win</span>, pickup = <span class="chip loss">loss</span>.
octogen takes the proven win.</li>
<li><b>p1 is only partly this octogen build.</b> Across 37 p1 turns octogen agrees on 22 (7 of them forced single&#8209;move).
The 15 disagreements &mdash; including the log&#8209;31 Ace&#8209;cover &mdash; mean the recorded p1 was not consistently
this octogen configuration. The panels still show octogen's honest evaluation of each true position.</li>
</ul>
</div>
<p class="footnote">Deliberation dumped by <code>OG_EXPLAIN</code> in <code>cnitro/src/octogen_strategy.c</code>;
game reproduced by <code>cnitro/tests/og_explain.c</code> (reconstructed&#8209;deal driven replay). MC score = mean finish
position over sampled worlds (1 = eliminated first = win; 2 = durak). Lower is better.</p>
</div>
<script>
const DATA=__DATA__;
</script>
<script>__JS__</script>
"""

JS = r"""
(function(){
const D=DATA, L=D.logs, N=L.length;
let cur=0;
const flaggedText={
 29:{cls:'info',title:'Log 29 &mdash; cover 10&hearts; with J&spades; (octogen AGREES)',
     html:"p1 holds three trumps able to cover 10&hearts; (J&spades;, Q&spades;, A&spades;) plus the pickup option. octogen's MC ranks the <b>cheapest</b> trump best: J&spades; 1.4387 &lt; Q&spades; 1.441 &lt; pickup 1.4564 &lt; A&spades; 1.4844. Textbook low&#8209;trump defense &mdash; matches the recorded move."},
 31:{cls:'warn',title:'Log 31 &mdash; cover J&hearts; with the trump ACE (octogen would use the Queen)',
     html:"The recorded move is the <b>single</b> cover J&hearts;&rarr;<b>A&spades;</b> (leaving 10&diams; uncovered). octogen instead covers J&hearts; with <b>Q&spades;</b> &mdash; both are single covers of J&hearts;. In octogen's MC the two are a <b>near&#8209;tie</b>: the recorded ace&#8209;cover actually scored marginally <i>better</i> (1.4271 over 192 sims) than octogen's queen&#8209;cover (1.4329 over 864 sims), but octogen <b>pruned the ace&#8209;cover before the final stage</b>, so its queen preference is noise&#8209;level, not a strong judgement. Materially it barely matters: <b>because p1 picks up everything next turn (log 32), whichever trump covered J&hearts; returns to hand &mdash; the Ace is churned, not lost.</b> And yes, p1 <b>held trumps (Q&spades;, A&spades;) that could have covered the 10&diams;</b> &mdash; even both attacks at once (candidate <span class='mono'>cover Q&spades;&rarr;J&hearts;,A&spades;&rarr;10&diams;</span>) &mdash; but leaving 10&diams; and picking up was fine (log 32)."},
 32:{cls:'info',title:'Log 32 &mdash; pick up (octogen AGREES)',
     html:"Facing the still&#8209;uncovered 10&diams; with hand [J&clubs; Q&spades; 6&hearts; 7&hearts; 7&diams;], octogen scores <b>pickup 1.4155</b> below <b>cover 10&diams;&rarr;Q&spades; 1.4537</b>. Correct to stop covering and take the cards rather than sink another trump."},
 116:{cls:'win',title:'Log 116 &mdash; attack with three Aces A&clubs; A&spades; A&diams; (octogen AGREES)',
     html:"Deck empty &mdash; endgame. The exact solver could not resolve this 6&#8209;vs&#8209;6 attack even at 200M nodes, so octogen used MC: the three&#8209;Ace attack scores a <b>certain win (1.0 over 864 worlds)</b>. p0 cannot cover three same&#8209;rank Aces and must pick up; p1 unloads its high cards and wins. A sound end&#8209;game dump, not a waste."},
};

function cardEl(c,sm){
  const d=document.createElement('span');
  d.className='pc'+(sm?' sm':'')+(c.hidden?' hidden':(c.trump?' trump':(c.red?' red':'')));
  if(c.hidden){d.textContent='?';return d;}
  d.innerHTML=c.r+'<span class="s">'+c.sym+'</span>';
  return d;
}
// parse an OG token like "AS*","10H","6S*" into a card obj
const VS={S:0,H:1,C:2,D:3}, SYM={0:'♠',1:'♥',2:'♣',3:'♦'};
function tok(t){
  const m=t.match(/^(10|[2-9]|[JQKA])([SHCD])(\*?)$/);
  if(!m) return {hidden:true};
  const s=VS[m[2]];
  return {r:m[1],suit:s,sym:SYM[s],trump:s===0,red:(s===1||s===3),hidden:false};
}
function tokRow(tokens,sm){
  const f=document.createDocumentFragment();
  tokens.forEach(t=>f.appendChild(cardEl(tok(t),sm)));
  return f;
}

function renderBoard(){
  const o=L[cur];
  // seat row
  const sr=document.getElementById('seatrow'); sr.innerHTML='';
  const dfn = currentDefender();
  for(let p=0;p<2;p++){
    const el=document.createElement('span');
    el.className='seatchip'+(dfn===p?' def':'');
    el.innerHTML='<b>p'+p+'</b> '+(p===1?'(octogen? &middot; winner)':'(fool)')+' <span class="cnt">&middot; '+o.hc[p]+' cards</span>'+(dfn===p?' &middot; defending':'');
    sr.appendChild(el);
  }
  // table
  const ta=document.getElementById('tablearea'); ta.innerHTML='';
  if(!o.table.length){ ta.innerHTML='<span class="muted">(empty)</span>'; }
  o.table.forEach(b=>{
    const bt=document.createElement('div'); bt.className='battle';
    bt.appendChild(cardEl(b.a,false));
    if(b.d){const dd=cardEl(b.d,false);dd.classList.add('df');bt.appendChild(dd);}
    else{const u=document.createElement('div');u.className='un';u.textContent='uncovered';bt.appendChild(u);}
    ta.appendChild(bt);
  });
  // action line
  const al=document.getElementById('actline'); al.innerHTML=actionText(o);
  // counter + slider
  document.getElementById('counter').textContent='log '+cur+' / '+(N-1)+'  ·  '+o.t;
  document.getElementById('slider').value=cur;
  document.getElementById('first').disabled=cur===0;
  document.getElementById('prev').disabled=cur===0;
  document.getElementById('next').disabled=cur===N-1;
  renderDecision();
}
function currentDefender(){
  let d=0;
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
  const o=L[cur]; if(!o.decision){ host.innerHTML='<p class="muted" style="margin-left:2px">No octogen decision at this log &mdash; step to a p1 turn (attack / cover / pass / pickup / good) to see the deliberation.</p>'; return; }
  const d=o.decision;
  const solver = d.solver && d.solver.applied;
  const panel=document.createElement('div'); panel.className='dpanel'+(solver?' solver':'');
  // header
  const recMove = recordedMoveLabel(o);
  const match = (normLabel(d.chosen)===normLabel(recMove));
  let hh='<div class="dhead"><span class="t">octogen deliberation @ log '+cur+'</span>';
  hh+='<span class="chip '+(match?'match':'mismatch')+'">'+(match?'octogen agrees':'octogen would differ')+'</span>';
  hh+='<span class="muted mono">deck='+d.deck+' &middot; p0 holds '+d.opp_counts[0]+'</span></div>';
  panel.innerHTML=hh;
  // hand
  const hl=document.createElement('div'); hl.innerHTML='<h3>octogen (p1) hand</h3>';
  const hr=document.createElement('div'); hr.className='hand'; hr.appendChild(tokRow(d.hand,false)); hl.appendChild(hr); panel.appendChild(hl);
  // recorded vs octogen pick
  const pk=document.createElement('p'); pk.style.margin='10px 0 2px';
  pk.innerHTML='<b>Recorded p1 played:</b> <span class="mono">'+recMove+'</span> &nbsp; &middot; &nbsp; <b>octogen picks:</b> <span class="mono">'+d.chosen+'</span>';
  panel.appendChild(pk);
  // candidates
  const isSolverVerdict = solver && d.candidates.some(c=>c.verdict==='win'||c.verdict==='loss');
  const h3=document.createElement('h3'); h3.textContent= isSolverVerdict? 'Exact endgame&#8209;solver verdict per move' : 'Monte&#8209;Carlo candidates (avg finish, lower = better)';
  h3.innerHTML=h3.textContent; panel.appendChild(h3);
  const bars=document.createElement('div'); bars.className='bars';
  // sort: solver -> win first; MC -> by score asc (None last)
  const cs=d.candidates.slice();
  if(isSolverVerdict){ const ord={win:0,draw:1,unknown:2,none:3,loss:4,illegal:5}; cs.sort((a,b)=>(ord[a.verdict]-ord[b.verdict])); }
  else cs.sort((a,b)=>((a.score==null)-(b.score==null))|| (a.score-b.score));
  const scored=cs.filter(c=>c.score!=null);
  const best= scored.length? Math.min.apply(null,scored.map(c=>c.score)):null;
  const recN=normLabel(recMove);
  cs.forEach(c=>{
    const row=document.createElement('div'); row.className='brow';
    if(c.score!=null && c.score===best) row.classList.add('best');
    if(c.chosen) row.classList.add('chosen');
    if(!c.alive) row.classList.add('pruned');
    // move label as chips
    const mv=document.createElement('div'); mv.className='bmove';
    mv.appendChild(moveChips(c));
    if(normLabel(c.label)===recN){ const rt=document.createElement('span'); rt.className='chip forced'; rt.style.marginLeft='4px'; rt.textContent='recorded'; mv.appendChild(rt); }
    row.appendChild(mv);
    // track
    const tr=document.createElement('div'); tr.className='btrack';
    const fill=document.createElement('div'); fill.className='bfill';
    if(c.score!=null){ const w=Math.max(2,Math.min(100,(2-c.score)/1*100)); fill.style.width=w+'%'; }
    else fill.style.width='0%';
    tr.appendChild(fill); row.appendChild(tr);
    // tag
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
  // flagged callout
  if(flaggedText[cur]){
    const co=document.createElement('div'); co.className='callout '+flaggedText[cur].cls;
    co.innerHTML='<b>'+flaggedText[cur].title+'</b><br>'+flaggedText[cur].html;
    panel.appendChild(co);
  }
  host.appendChild(panel);
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
// recorded move label in the SAME token grammar as octogen 'chosen'
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
// Normalize a move label so card ORDER doesn't affect equality
function normLabel(s){
  const parts=s.replace(/,/g,' ').trim().split(/\s+/);
  const type=parts[0];
  const rest=parts.slice(1).sort();
  return type+' '+rest.join(' ');
}

// jump buttons
const jb=document.getElementById('jump');
[['Log 29 &middot; cover 10♥',29],['Log 31 &middot; cover J♥ with A♠',31],['Log 32 &middot; pick up',32],['Log 116 &middot; attack 3 Aces',116]].forEach(([lab,i])=>{
  const b=document.createElement('button'); b.innerHTML=lab; b.onclick=()=>{cur=i;renderBoard();document.getElementById('decision').scrollIntoView({behavior:'smooth',block:'center'});}; jb.appendChild(b);
});

document.getElementById('first').onclick=()=>{cur=0;renderBoard();};
document.getElementById('prev').onclick=()=>{if(cur>0)cur--;renderBoard();};
document.getElementById('next').onclick=()=>{if(cur<N-1)cur++;renderBoard();};
document.getElementById('nextd').onclick=()=>{for(let i=cur+1;i<N;i++){if(L[i].decision){cur=i;break;}}renderBoard();};
document.getElementById('slider').oninput=(e)=>{cur=+e.target.value;renderBoard();};
document.addEventListener('keydown',e=>{if(e.key==='ArrowLeft'){if(cur>0)cur--;renderBoard();}else if(e.key==='ArrowRight'){if(cur<N-1)cur++;renderBoard();}});

// theme toggle
const tb=document.getElementById('themebtn');
tb.onclick=()=>{const r=document.documentElement;const cur=r.getAttribute('data-theme')|| (window.matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');const nx=cur==='dark'?'light':'dark';r.setAttribute('data-theme',nx);};

renderBoard();
})();
"""

out=HTML.replace('__CSS__',CSS).replace('__DATA__',DATA_JSON).replace('__JS__',JS)
open('/home/user/foolish/docs/octogen-replay-explain.html','w').write(out)
print('wrote docs/octogen-replay-explain.html', len(out),'bytes')
