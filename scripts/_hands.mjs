const ROOT='/Users/alex/Dev/foolish';
const { urlToGame } = await import(`${ROOT}/supabase/functions/_shared/common/replay/codec.ts`);
const { decodeReplay } = await import(`${ROOT}/supabase/functions/_shared/common/replay/decode.ts`);
const d = decodeReplay(urlToGame(process.argv[2]));
const N=d.playerCount; const h=Array(N).fill(0);
const names=['ALEX','CorditeMax1','Cordite2','Random2','Cordite3','Random5','CorditeMax2'];
const from=Number(process.argv[3]||0);
let i=0;
for(const l of d.logs){ i++;
  const n=(l.card_pairs||[]).length;
  if(l.log_type==='draw') h[l.seat]+=n;
  else if(l.log_type==='attack'||l.log_type==='cover'||l.log_type==='pass') h[l.seat]-=n;
  else if(l.log_type==='pickup') h[l.seat]+=n;
  if(i>=from && (l.log_type==='attack'||l.log_type==='cover'||l.log_type==='pass'||l.log_type==='pickup'||l.log_type==='player_out'))
    console.log(`#${String(i).padStart(3)} ${l.log_type.padEnd(10)} ${names[l.seat]??''}  | hands: `+h.map((x,s)=>`${names[s].slice(0,4)}=${x}`).join(' '));
}
