const ROOT = '/Users/alex/Dev/foolish';
const { urlToGame } = await import(`${ROOT}/supabase/functions/_shared/common/replay/codec.ts`);
const { decodeReplay } = await import(`${ROOT}/supabase/functions/_shared/common/replay/decode.ts`);
const extras = await import(`${ROOT}/supabase/functions/_shared/common/replay/extras.ts`);

const url = process.argv[2];
const d = decodeReplay(urlToGame(url));

const SUITS = ['♠','♥','♣','♦'];
const VALS = {1:'2',2:'3',3:'4',4:'5',5:'6',6:'7',7:'8',8:'9',9:'10',10:'J',11:'Q',12:'K',13:'A'};
const c = (card) => !card || card.suit < 0 ? '??' : (VALS[card.value]||card.value) + SUITS[card.suit];
const pairs = (cp) => (cp||[]).map(p => p.target ? `${c(p.primary)}→on→${c(p.target)}` : c(p.primary)).join(' ');

// names + per-move timing gaps from the -extras suffix
let names = null, gaps = null;
try {
  const code = url.replace(/^.*FOOLISH\.CARDS\//i,'');
  const parts = extras.splitReplayCode(code);
  if (parts.extras) {
    const moveCount = d.logs.filter(l=>['attack','cover','pass','pickup'].includes(l.log_type)).length;
    const ex = extras.decodeExtras(parts.extras, d.playerCount, moveCount);
    names = ex.names; gaps = ex.gaps || ex.moveGaps || null;
    console.log('EXTRAS KEYS:', Object.keys(ex));
  }
} catch(e){ console.log('extras:', e.message); }

const nm = (s) => s==null ? '   --   ' : `${s}:${names && names[s] ? names[s] : 'seat'+s}`;

console.log(`\n${d.playerCount} players · trump ${c(d.trumpCard)} (suit ${d.powerSuit}) · first attacker seat ${d.firstAttacker}`);
console.log(`names: ${names ? names.map((n,i)=>i+':'+n).join('  ') : '(none)'}`);
console.log(`elimination order (seats, first out first): ${d.eliminationOrder.join(' ')}`);
console.log(`FOOL (loser) = seat ${d.fool} = ${nm(d.fool)}`);
console.log(`total events: ${d.logs.length}\n`);

const start = Number(process.argv[3] || 0);
let i = 0, mv = 0;
for (const l of d.logs) {
  const isMove = ['attack','cover','pass','pickup'].includes(l.log_type);
  if (i++ >= start) {
    const t = gaps && isMove && gaps[mv]!=null ? ` (+${(gaps[mv]/1000).toFixed(1)}s)` : '';
    console.log(`#${String(i).padStart(3)} ${l.log_type.padEnd(16)} ${nm(l.seat).padEnd(14)} ${pairs(l.card_pairs)}${t}`);
  }
  if (isMove) mv++;
}
