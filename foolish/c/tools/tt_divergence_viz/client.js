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
const hsvg=document.getElementById('histChart');
const htip=document.getElementById('htip');
const hwrap=document.getElementById('histWrap');

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
      // numeric-pc series (e.g. the accumulating octogen pc2 sweep) follow the
      // player-count selector so they overlay their own model curve; string-pc
      // series ("mixed", "4,8") are always shown.
      if(typeof s.pc==='number' && state.pcs.size && !state.pcs.has(s.pc)) return;
      s.points.forEach(pt=>{
        if(pt.bits<B_MIN||pt.bits>B_MAX) return;
        const bd=bounds(pt.diverged, pt.games||s.games);
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
  drawHist(); buildDivTable();
}

// ---- the decision table: octogen pc2 divergence by table size, with CI + latency
function buildDivTable(){
  const T=document.getElementById('divTable'); if(!T) return;
  const s=(DATA.measured.series||[]).find(x=>x.bot==='octogen'&&x.pc===2&&x.measured);
  const model=cells['octogen_pc2'];
  const lat=(DATA.latency&&DATA.latency['octogen_pc2'])||{};
  const sub=document.getElementById('divSub');
  if(!s||!s.points||!s.points.length){
    T.innerHTML='<tbody><tr><td style="color:var(--ink-3);padding:14px">no divergence data yet — run '
      +'<code>tools/tt_divergence_viz/accrue_div.sh</code></td></tr></tbody>';
    if(sub) sub.textContent=''; return;
  }
  const gmax=Math.max(...s.points.map(p=>p.games||0));
  if(sub) sub.textContent='· up to '+gmax.toLocaleString()+' seeds/size · ref TT'+(s.ref_bits||22);
  let h='<thead><tr>'
    +'<th>table</th><th>entries</th><th>bytes</th><th>games</th><th>diverged</th>'
    +'<th>divergence</th><th>95% interval</th><th>model P(W&gt;M)</th><th>avg decision</th>'
    +'</tr></thead><tbody class="mono">';
  s.points.slice().sort((a,b)=>a.bits-b.bits).forEach(p=>{
    const bd=bounds(p.diverged,p.games);
    const ship=p.bits===13;
    const bytes=p.bits>=10?((2**p.bits*16)/1024/(p.bits>=20?1024:1)).toFixed(0)+(p.bits>=20?' MiB':' KiB'):(2**p.bits*16)+' B';
    const mc=model?model.ccdf.find(d=>d.bits===p.bits):null;
    const modelp=mc?(mc.p>0?pctP(mc.p):'0'):'—';
    const rate = p.diverged>0? (p.diverged/p.games*100).toFixed( p.diverged/p.games<0.001?3:2 )+'%' : '0';
    const ci = p.diverged>0
       ? (bd.lo*100).toFixed(bd.lo<0.001?3:2)+'–'+(bd.hi*100).toFixed(bd.hi<0.01?3:2)+'%'
       : '&lt; '+(bd.hi*100).toFixed(bd.hi<0.01?3:2)+'%';
    const L=lat[String(p.bits)];
    const latStr = L&&L.avg_ms!=null ? L.avg_ms.toFixed(L.avg_ms<10?2:1)+' ms' : '<span style="color:var(--ink-3)">—</span>';
    h+='<tr'+(ship?' style="background:color-mix(in srgb, var(--ship) 12%, transparent)"':'')+'>'
      +'<td style="text-align:left'+(ship?';font-weight:700;color:var(--ship)':'')+'">TT'+p.bits+(ship?' ◄ shipped':'')+'</td>'
      +'<td>'+fmtEntries(2**p.bits)+'</td><td>'+bytes+'</td>'
      +'<td>'+p.games.toLocaleString()+'</td><td>'+p.diverged.toLocaleString()+'</td>'
      +'<td'+(p.diverged>0?'':' style="color:var(--ink-3)"')+'>'+rate+'</td>'
      +'<td style="color:var(--ink-2)">'+ci+'</td>'
      +'<td style="color:var(--ink-3)">'+modelp+'</td>'
      +'<td>'+latStr+'</td></tr>';
  });
  T.innerHTML=h+'</tbody>';
}

// ---- working-set density: bits-needed histogram per selected cell ----
function drawHist(){
  while(hsvg.firstChild) hsvg.removeChild(hsvg.firstChild);
  const HW=920, HH=420, HM={l:56,r:20,t:16,b:52};
  const pW=HW-HM.l-HM.r, pH=HH-HM.t-HM.b;
  // selected cells
  const sel=[];
  for(const bot of state.bots) for(const pc of state.pcs){ const c=cells[bot+'_pc'+pc]; if(c) sel.push(c); }
  // x categories: "none" (W=0) then bits 1..maxBits
  let maxBits=13;
  sel.forEach(c=> c.wbits.forEach((n,b)=>{ if(n>0 && b>maxBits) maxBits=b; }));
  maxBits=Math.min(maxBits+1, 24);
  const cats=maxBits+1;                       // slot 0 = none, slots 1..maxBits = bits
  const slotW=pW/cats;
  const xSlot = i => HM.l + i*slotW + slotW/2; // center of slot i (0=none,1..=bits)
  // y: fraction of games; scale to the max W>0 bucket (none is annotated, not scaled to)
  let yMax=0;
  sel.forEach(c=>{ for(let b=1;b<=maxBits;b++){ const f=c.wbits[b]/c.games; if(f>yMax)yMax=f; } });
  if(yMax<=0) yMax=1;
  const yLogFloor=1e-4;
  function hY(f){
    if(state.y==='lin') return HM.t + (1-f/yMax)*pH;
    const lo=Math.log10(yLogFloor), hi=Math.log10(Math.max(yMax,1e-3));
    const lf=Math.log10(Math.max(f,yLogFloor));
    return HM.t + (hi-lf)/(hi-lo)*pH;
  }
  const hbase=HM.t+pH;
  // grid + y labels
  if(state.y==='lin'){
    for(let i=0;i<=4;i++){ const f=yMax*i/4, y=HM.t+(1-i/4)*pH;
      hsvg.appendChild(el('line',{x1:HM.l,y1:y,x2:HM.l+pW,y2:y,class:'gridline'}));
      hsvg.appendChild(hylab(y,(f*100).toFixed(f<0.05?1:0)+'%')); }
  } else {
    [1,0.1,0.01,0.001,0.0001].forEach(f=>{ if(f>yMax*1.5)return; const y=hY(f);
      hsvg.appendChild(el('line',{x1:HM.l,y1:y,x2:HM.l+pW,y2:y,class:'gridline'}));
      hsvg.appendChild(hylab(y,(f*100)>=1?(f*100)+'%':(f*100).toFixed(f>=0.001?1:2)+'%')); });
  }
  hsvg.appendChild(el('line',{x1:HM.l,y1:hbase,x2:HM.l+pW,y2:hbase,class:'axisline'}));
  // x labels
  for(let i=0;i<=maxBits;i++){ const x=xSlot(i);
    const lab = i===0 ? 'none' : String(i);
    hsvg.appendChild(hxlab(x,hbase+16,lab));
    if(i>0 && (i%2===0||i===maxBits)) hsvg.appendChild(hxlab(x,hbase+29, fmtEntries(2**i)));
  }
  hsvg.appendChild(el('text',{x:HM.l+pW/2,y:HH-6,'text-anchor':'middle',class:'axis-title'})).textContent
    = 'bits needed  =  ceil(log2 W)   (table entries below)';
  const yt=el('text',{x:15,y:HM.t+pH/2,'text-anchor':'middle',class:'axis-title',transform:'rotate(-90 15 '+(HM.t+pH/2)+')'});
  yt.textContent='fraction of games'; hsvg.appendChild(yt);
  // Table-of-T-bits fits a game iff bits-needed <= T, so its "fits | overflows"
  // boundary sits at the RIGHT edge of slot T (between slot T and T+1).
  const refB=(b,stroke,label,dy,anchor)=>{ if(b>maxBits)return; const x=HM.l+(b+1)*slotW;
    hsvg.appendChild(el('line',{x1:x,y1:HM.t,x2:x,y2:hbase,class:'refline',stroke}));
    const t=el('text',{x:x+(anchor==='end'?-3:3),y:HM.t+dy,class:'reflabel',fill:stroke,'text-anchor':anchor||'start'});
    t.textContent=label; hsvg.appendChild(t); };
  refB(12,'var(--accent)','64 KiB',24,'end'); refB(13,'var(--ship)','TT13 shipped',11,'end');
  // per-cell stepped bars (grouped within each bit slot)
  const hot=[];
  const groupW=slotW*0.8, bw=sel.length? groupW/sel.length : groupW;
  sel.forEach((c,si)=>{
    const col=color(c.bot);
    for(let b=1;b<=maxBits;b++){
      const f=c.wbits[b]/c.games; if(f<=0) continue;
      const x0=HM.l + b*slotW + slotW*0.1 + si*bw;
      const y=hY(f);
      hsvg.appendChild(el('rect',{x:x0.toFixed(1),y:y.toFixed(1),width:Math.max(1,bw-1).toFixed(1),
        height:Math.max(0,hbase-y).toFixed(1),fill:col,opacity:.85,rx:1}));
      hot.push({x:x0+bw/2,y,html:htipBar(c,b,f)});
    }
    // "none" marker for this cell at slot 0
    const nf=c.zeroGames/c.games;
    const nx=HM.l+slotW*0.1+si*bw, ny=hY(Math.min(nf,yMax));
    hsvg.appendChild(el('rect',{x:nx.toFixed(1),y:ny.toFixed(1),width:Math.max(1,bw-1).toFixed(1),
      height:Math.max(0,hbase-ny).toFixed(1),fill:col,opacity:.35,rx:1}));
    hot.push({x:nx+bw/2,y:ny,html:htipNone(c,nf)});
  });
  // note when the no-solve bars are clipped (their true % is in the tooltip)
  if(sel.some(c=> c.zeroGames/c.games > yMax)){
    const t=el('text',{x:xSlot(0),y:HM.t-4,'text-anchor':'middle'});
    t.setAttribute('fill','var(--ink-3)'); t.style.fontSize='9.5px';
    t.textContent='no-solve ↑ clipped'; hsvg.appendChild(t); }
  // hover
  const hhit=el('rect',{x:HM.l,y:HM.t,width:pW,height:pH,fill:'transparent'}); hsvg.appendChild(hhit);
  hhit.addEventListener('mousemove',ev=>{
    const p=hsvg.createSVGPoint(); p.x=ev.clientX; p.y=ev.clientY;
    const loc=p.matrixTransform(hsvg.getScreenCTM().inverse());
    let best=null,bd=1e9; for(const h of hot){const dd=(h.x-loc.x)**2+(h.y-loc.y)**2; if(dd<bd){bd=dd;best=h;}}
    if(best&&bd<2500){ const cr=hsvg.getBoundingClientRect(), sx=cr.width/HW, sy=cr.height/HH;
      htip.innerHTML=best.html; htip.style.opacity=1; htip.style.left=(best.x*sx)+'px'; htip.style.top=(best.y*sy)+'px';
    } else htip.style.opacity=0;
  });
  hhit.addEventListener('mouseleave',()=>htip.style.opacity=0);
  buildHLegend(sel);
}
function hylab(y,t){const e=el('text',{x:52,y:y+3,'text-anchor':'end'});e.setAttribute('fill','var(--ink-3)');e.style.fontSize='11px';e.textContent=t;return e;}
function hxlab(x,y,t){const e=el('text',{x,y,'text-anchor':'middle'});e.setAttribute('fill','var(--ink-3)');e.style.fontSize='10.5px';e.textContent=t;return e;}
function htipBar(c,b,f){
  return '<b style="color:'+color(c.bot)+'">'+PRETTY[c.bot]+' · pc'+c.pc+'</b>'
   +'<div class="row"><span class="k">needs</span><span>'+b+' bits · '+fmtEntries(2**b)+'</span></div>'
   +'<div class="row"><span class="k">games</span><span>'+Math.round(f*c.games)+' / '+c.games.toLocaleString()+'</span></div>'
   +'<div class="row"><span class="k">fraction</span><span>'+(f*100<0.1?(f*100).toFixed(2):(f*100).toFixed(1))+'%</span></div>';
}
function htipNone(c,nf){
  return '<b style="color:'+color(c.bot)+'">'+PRETTY[c.bot]+' · pc'+c.pc+'</b>'
   +'<div class="row"><span class="k">no solve (W=0)</span><span>'+(nf*100).toFixed(1)+'%</span></div>'
   +'<div class="row"><span class="k">games</span><span>'+c.zeroGames.toLocaleString()+' / '+c.games.toLocaleString()+'</span></div>'
   +'<div class="small" style="margin-top:3px">never reached the solver → can’t diverge</div>';
}
function buildHLegend(sel){
  const L=document.getElementById('hlegend'); L.innerHTML='';
  sel.forEach(c=>{ const d=document.createElement('span'); d.className='li';
    d.innerHTML='<span class="sw" style="background:'+color(c.bot)+'"></span>'+PRETTY[c.bot]+' · pc'+c.pc; L.appendChild(d); });
  if(!sel.length){ L.innerHTML='<span class="li" style="color:var(--ink-3)">select a bot + player count</span>'; }
  else { const n=document.createElement('span'); n.className='li'; n.style.color='var(--ink-3)';
    n.innerHTML='faded bar at left = no-solve share (W=0)'; L.appendChild(n); }
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
  const g = pt.games||s.games;
  const pcl = s.pc==='mixed'?'mixed pc':'pc '+s.pc;
  let b = bd.p>0 ? pctP(bd.p) : '0 of '+g.toLocaleString();
  let bound = 'p &lt; '+pctP(bd.hi)+' (95%)';
  return '<b style="color:'+color(s.bot)+'">'+PRETTY[s.bot]+' · '+pcl+'</b> — measured'
   +'<div class="row"><span class="k">table</span><span>TT'+pt.bits+'</span></div>'
   +'<div class="row"><span class="k">diverged</span><span>'+pt.diverged+' / '+g.toLocaleString()+'</span></div>'
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
