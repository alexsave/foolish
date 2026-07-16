#!/usr/bin/env python3
# One-off analysis page for the trump-dumping replay (octogen loses). Renders a
# board step-through of the PUBLIC game (no deal seed needed) with octogen's
# trump plays flagged, plus the diagnosis + the controlled-experiment evidence.
#   gen_replay2.py rd2_page.json rd2_timeline.json out.html
import json, sys

page = json.load(open(sys.argv[1]))
timeline = json.load(open(sys.argv[2]))
OUT = sys.argv[3]
DATA = json.dumps({'page': page, 'timeline': timeline}, separators=(',', ':'))

CSS = open(__file__.replace('gen_replay2.py', 'gen_html.py')).read()
CSS = CSS[CSS.index('CSS = r"""') + 10: CSS.index('"""', CSS.index('CSS = r"""') + 10)]
CSS += """
.trumpflag{background:var(--loss-bg);border:1px solid var(--loss);color:var(--loss);
  border-radius:8px;padding:10px 14px;margin:10px 0;font-size:14px}
.trumpflag.high{font-weight:600}
.tlrow{display:grid;grid-template-columns:70px 1fr;gap:10px;align-items:center;padding:6px 0;border-bottom:1px solid var(--border)}
.tlrow.trump{background:var(--loss-bg)}
.deckbar{height:10px;background:var(--info);border-radius:5px}
.deckwrap{background:var(--surface-2);border-radius:5px;overflow:hidden;height:10px}
"""

HTML = r"""<meta charset="utf-8">
<title>octogen dumps its trumps — a losing replay</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>__CSS__</style>
<button class="themebtn" id="themebtn">theme</button>
<div class="wrap">
<h1>octogen dumps its trumps &mdash; a losing replay</h1>
<p class="lede">A recorded 2&#8209;player game, trump&nbsp;<b>&diams;</b> (flip&nbsp;J&diams;). <b>octogen is p1</b> and
<b>loses</b> &mdash; it is the fool (durak); <b>p0 wins</b>. The reason is visible in its own moves:
octogen repeatedly <b>attacks with trumps while the deck is still full</b>, including high trumps
(Q&diams;, K&diams;), throwing away the exact cards it needs for defense and the endgame.</p>

<div class="tiles">
  <div class="tile loss"><div class="n">p1 loses</div><div class="l">octogen = fool (durak)</div></div>
  <div class="tile"><div class="n">&diams; diamonds</div><div class="l">trump &middot; flip J&diams;</div></div>
  <div class="tile loss"><div class="n" id="ntrump">7</div><div class="l">trump plays with a live deck</div></div>
  <div class="tile loss"><div class="n" id="nhigh">2</div><div class="l">of them HIGH trumps (Q&diams;, K&diams;)</div></div>
</div>

<h2>What&rsquo;s weird: the trump timeline</h2>
<p>Each row is an octogen play; the bar is how full the deck still was. In good Durak you <b>hoard</b>
trumps (you never need one to <i>attack</i>) and spend them only to defend or in the endgame. octogen
does the opposite &mdash; note the high trumps spent at deck&nbsp;15 and deck&nbsp;7.</p>
<div class="panel" id="timeline"></div>

<h2>Why it happens (root cause)</h2>
<div class="panel">
<p>octogen does <b>not</b> literally assume the deck is empty &mdash; its Monte&#8209;Carlo rollouts do model
future draws. The bug is subtler: it <b>over&#8209;values attacking with strong / trump cards</b>. Attacking with
a card the opponent can&rsquo;t beat forces a <b>pickup</b>, which bumps the opponent&rsquo;s card count &mdash; and
octogen&rsquo;s objective (average finish position, more cards = closer to being the fool) rewards that
immediately. What the rollout <b>fails to price in</b> is the long&#8209;term cost: the trump it attacked with
goes <i>into the opponent&rsquo;s hand</i> on the pickup. Handing the opponent your A&diams;/K&diams; to shave one card
off the count is a terrible trade &mdash; worst early, when trumps decide the game.</p>
<h3>Controlled experiment (full deck, octogen holds two high trumps)</h3>
<p>Deal octogen <span class="mono">6&spades; 7&hearts; 8&spades; 9&hearts; K&diams; A&diams;</span> as first attacker with a full deck and
dump its deliberation. It (correctly) attacks 9&hearts; &mdash; but look at where it ranks dumping the <b>Ace of trumps</b>:</p>
<div class="bars" id="exp"></div>
<p class="muted" style="margin-top:8px">Attacking with A&diams; scores <b>second&#8209;best</b> &mdash; above attacking with a 6, 7 or 8. Spending the
game&rsquo;s single most valuable card to force a pickup should be near&#8209;<i>worst</i>. That inversion is the
same pull that makes octogen bleed trumps in the replay above.</p>
</div>

<h2>Step through the game</h2>
<div class="ctl">
  <button class="nav" id="first">&laquo; start</button>
  <button class="nav" id="prev">&lsaquo; prev</button>
  <button class="nav" id="next">next &rsaquo;</button>
  <button class="nav" id="nextt">next octogen trump play &raquo;</button>
  <span class="mono muted" id="counter"></span>
</div>
<div class="ctl"><input type="range" id="slider" min="0" max="0" value="0"></div>
<div id="flag"></div>
<div class="board panel">
  <div class="seatrow" id="seatrow"></div>
  <div><h3 style="margin-top:2px">Table</h3><div class="tablearea" id="tablearea"></div></div>
  <div class="actline" id="actline"></div>
</div>
<p class="muted" style="font-size:12.5px;margin-top:30px">Board reconstructed from the public replay (the deal
seed wasn&rsquo;t available, so octogen&rsquo;s hidden hand isn&rsquo;t reconstructed here &mdash; but its <i>moves</i> are exact,
and the moves are the story). Controlled&#8209;experiment numbers dumped by <code>OG_EXPLAIN</code>.</p>
</div>
<script>const DATA=__DATA__;</script>
<script>__JS__</script>
"""

# controlled-experiment candidate scores (from the OG_EXPLAIN run, hardcoded facts)
EXP = [
    {"label": "attack 9♥", "score": 1.4537, "nsim": 864, "trump": False, "chosen": True},
    {"label": "attack A♦", "score": 1.4803, "nsim": 864, "trump": True, "chosen": False},
    {"label": "attack 6♠", "score": 1.5000, "nsim": 192, "trump": False, "chosen": False},
    {"label": "attack K♦", "score": 1.5189, "nsim": 528, "trump": True, "chosen": False},
    {"label": "attack 7♥", "score": 1.5365, "nsim": 192, "trump": False, "chosen": False},
    {"label": "attack 8♠", "score": 1.5469, "nsim": 192, "trump": False, "chosen": False},
]

JS = r"""
(function(){
const P=DATA.page, TL=DATA.timeline, L=P.logs, N=L.length, M=P.meta, EXP=__EXP__;
const NP=M.players; let cur=0;
document.getElementById('ntrump').textContent = TL.filter(t=>t.trumps.length&&t.deck>0).length;
document.getElementById('nhigh').textContent  = TL.filter(t=>t.highTrump&&t.deck>0).length;

function cardEl(c,sm){const d=document.createElement('span');
  d.className='pc'+(sm?' sm':'')+(c.hidden?' hidden':(c.trump?' trump':(c.red?' red':'')));
  if(c.hidden){d.textContent='?';return d;} d.innerHTML=c.r+'<span class="s">'+c.sym+'</span>';return d;}
function cardsHtml(cards){const s=document.createElement('span');s.className='hand';(cards||[]).forEach(c=>s.appendChild(cardEl(c,true)));return s.outerHTML;}

// timeline
const tlHost=document.getElementById('timeline');
TL.forEach(t=>{
  const row=document.createElement('div'); row.className='tlrow'+(t.trumps.length&&t.deck>0?' trump':'');
  const w=Math.round(t.deck/24*100);
  row.innerHTML='<div><div class="deckwrap"><div class="deckbar" style="width:'+w+'%"></div></div>'+
    '<div class="muted" style="font-size:11px">deck '+t.deck+'</div></div>';
  const right=document.createElement('div');
  right.innerHTML='<span class="tag '+t.kind+'">'+t.kind+'</span> '+cardsHtml(t.cards)+
    (t.trumps.length&&t.deck>0?' <span class="chip loss">spent trump'+(t.trumps.length>1?'s':'')+' with a live deck</span>':'');
  right.style.cursor='pointer';
  right.onclick=()=>{cur=t.log;render();document.getElementById('slider').scrollIntoView({behavior:'smooth',block:'center'});};
  row.appendChild(right); tlHost.appendChild(row);
});

// controlled experiment bars
const expHost=document.getElementById('exp');
const best=Math.min.apply(null,EXP.map(e=>e.score));
EXP.slice().sort((a,b)=>a.score-b.score).forEach(e=>{
  const row=document.createElement('div'); row.className='brow'+(e.score===best?' best':'')+(e.chosen?' chosen':'');
  const mv=document.createElement('div'); mv.className='bmove';
  mv.innerHTML='<span class="mono">'+e.label+'</span>'+(e.trump?' <span class="chip loss">trump</span>':'');
  const tr=document.createElement('div'); tr.className='btrack';
  const f=document.createElement('div'); f.className='bfill'; f.style.width=Math.max(2,(2-e.score)*100)+'%'; tr.appendChild(f);
  const tg=document.createElement('div'); tg.className='btag'; tg.textContent=e.score.toFixed(4)+'  ('+e.nsim+' sims)';
  row.appendChild(mv); row.appendChild(tr); row.appendChild(tg); expHost.appendChild(row);
});

function currentDefender(){let d=M.fool;for(let i=cur;i>=0;i--){if(L[i].t==='defender_change'){d=L[i].def;break;}}return d;}
function render(){
  const o=L[cur], sr=document.getElementById('seatrow'); sr.innerHTML=''; const dfn=currentDefender();
  for(let p=0;p<NP;p++){const el=document.createElement('span');
    const role=p===M.winner?'winner':(p===M.fool?'octogen · fool':'');
    el.className='seatchip'+(dfn===p?' def':'')+(p===M.ogSeat?' og':'');
    el.innerHTML='<b>p'+p+'</b>'+(role?' ('+role+')':'')+' <span class="cnt">&middot; '+o.hc[p]+'</span>'+(dfn===p?' &middot; def':'');
    sr.appendChild(el);}
  const ta=document.getElementById('tablearea'); ta.innerHTML=''; if(!o.table.length) ta.innerHTML='<span class="muted">(empty)</span>';
  o.table.forEach(b=>{const bt=document.createElement('div');bt.className='battle';bt.appendChild(cardEl(b.a,false));
    if(b.d){const dd=cardEl(b.d,false);dd.classList.add('df');bt.appendChild(dd);}else{const u=document.createElement('div');u.className='un';u.textContent='uncovered';bt.appendChild(u);}ta.appendChild(bt);});
  document.getElementById('actline').innerHTML=actionText(o);
  document.getElementById('counter').textContent='log '+cur+' / '+(N-1)+'  ·  '+o.t;
  document.getElementById('slider').value=cur;
  // trump flag
  const fh=document.getElementById('flag'); fh.innerHTML='';
  const tl=TL.find(t=>t.log===cur);
  if(tl&&tl.trumps.length&&tl.deck>0){
    const f=document.createElement('div'); f.className='trumpflag'+(tl.highTrump?' high':'');
    f.innerHTML='&#9888; octogen just spent '+cardsHtml(tl.trumps)+' as a<b> '+tl.kind+'</b> with <b>'+tl.deck+'</b> cards still in the deck'+
      (tl.highTrump?' &mdash; a HIGH trump. It will want this back for the endgame it is about to lose.':' &mdash; trumps are for defense, not attacking.');
    fh.appendChild(f);
  }
}
function actionText(o){const a=o.action;if(!a)return '';const seat=(a.seat!=null)?'<span class="who">p'+a.seat+'</span> ':'';
  const tag='<span class="tag '+a.kind+'">'+a.kind.replace('_',' ')+'</span>';
  if(a.kind==='attack'||a.kind==='pass')return tag+seat+cardsHtml(a.cards);
  if(a.kind==='cover'){let h=tag+seat;for(let i=0;i<a.cards.length;i++)h+=cardEl(a.cards[i],true).outerHTML+'<span class="arrow">&rarr;</span>'+cardEl(a.targets[i],true).outerHTML+' ';return h;}
  if(a.kind==='pickup')return tag+seat+'takes '+a.n+' '+cardsHtml(a.cards);
  if(a.kind==='good')return tag+seat+'accepts';
  if(a.kind==='discard')return tag+cardsHtml(a.cards)+' &rarr; discard';
  if(a.kind==='draw'){let h=tag+seat+'draws '+a.n;if(a.reveal&&a.reveal.length)h+=' '+cardsHtml(a.reveal);return h;}
  if(a.kind==='defender_change')return tag+'defender &rarr; p'+a.def;
  if(a.kind==='player_out')return tag+seat+'eliminated';
  if(a.kind==='game_start')return tag+'deal';
  return tag;}
document.getElementById('first').onclick=()=>{cur=0;render();};
document.getElementById('prev').onclick=()=>{if(cur>0)cur--;render();};
document.getElementById('next').onclick=()=>{if(cur<N-1)cur++;render();};
document.getElementById('nextt').onclick=()=>{const nx=TL.find(t=>t.log>cur&&t.trumps.length&&t.deck>0);if(nx){cur=nx.log;render();}};
document.getElementById('slider').max=N-1;
document.getElementById('slider').oninput=e=>{cur=+e.target.value;render();};
document.addEventListener('keydown',e=>{if(e.key==='ArrowLeft'&&cur>0){cur--;render();}else if(e.key==='ArrowRight'&&cur<N-1){cur++;render();}});
const tb=document.getElementById('themebtn');tb.onclick=()=>{const r=document.documentElement;const c=r.getAttribute('data-theme')||(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');r.setAttribute('data-theme',c==='dark'?'light':'dark');};
render();
})();
"""

JS = JS.replace('__EXP__', json.dumps(EXP))
out = HTML.replace('__CSS__', CSS).replace('__DATA__', DATA).replace('__JS__', JS)
open(OUT, 'w').write(out)
print(f'wrote {OUT} ({len(out)} bytes)')
