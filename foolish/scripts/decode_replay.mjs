// Print a replay share link's decoded log stream, move by move, with the seat
// names and move gaps its extras carry. The kernel decodes it (the test build's
// record reader, e2e/helpers/replay_decode.ts).
//
//   bash tools/structgen/gen.sh   # once, if nothing has generated in this tree
//   node --import tsx scripts/decode_replay.mjs "<replay link or code>" [first event]
//
// The gen.sh line is there because this is run by hand rather than through an
// npm script, and sdk/ts/gen is a build output: every npm lane generates it
// through a pre-hook (package.json "//gen"), and nothing does that for a bare
// `node` invocation.
import { ensureBotsAsync } from '../sdk/ts/wasm/bots.ts';
import { decodeReplayLink } from '../e2e/helpers/replay_decode.ts';
import * as G from '../sdk/ts/gen/game_layout.bots.ts';

const link = process.argv[2];
if (!link) { console.error('usage: node --import tsx scripts/decode_replay.mjs <replay link or code> [first event]'); process.exit(2); }
await ensureBotsAsync();
const d = decodeReplayLink(link);
const s = d.summary;

const LOG = {};
for (const [k, v] of Object.entries(G)) if (k.startsWith('LOG_')) LOG[v] = k.slice(4).toLowerCase();
const MOVES = new Set([G.LOG_ATTACK, G.LOG_COVER, G.LOG_PASS, G.LOG_PICKUP]);
const SUITS = ['♠', '♥', '♣', '♦'];
const VALS = { 1: '2', 2: '3', 3: '4', 4: '5', 5: '6', 6: '7', 7: '8', 8: '9', 9: '10', 10: 'J', 11: 'Q', 12: 'K', 13: 'A' };
const c = (card) => (!card || card.suit < 0 ? '??' : (VALS[card.value] ?? card.value) + SUITS[card.suit]);
const pairs = (ps) => ps.map((p) => (p.target ? `${c(p.primary)}→on→${c(p.target)}` : c(p.primary))).join(' ');
const nm = (seat) => (seat < 0 ? '   --   ' : `${seat}:${d.names?.[seat] ?? `seat${seat}`}`);

console.log(`\n${s.numPlayers} players · trump ${c(s.trump)} (suit ${s.powerSuit}) · first attacker seat ${s.firstAttacker} · format ${s.version}`);
console.log(`names: ${d.names ? d.names.map((n, i) => `${i}:${n}`).join('  ') : '(none)'}`);
console.log(`elimination order (seats, first out first): ${s.elimination.join(' ')}`);
console.log(`FOOL (loser) = seat ${s.fool} = ${nm(s.fool)}`);
console.log(`total events: ${d.logs.length}\n`);

const start = Number(process.argv[3] || 0);
let i = 0, mv = 0;
for (const l of d.logs) {
  const isMove = MOVES.has(l.type);
  if (i++ >= start) {
    const gap = d.moveGaps && isMove ? d.moveGaps[mv] : undefined;
    const t = gap != null ? ` (+${gap.toFixed(1)}s)` : '';
    console.log(`#${String(i).padStart(3)} ${String(LOG[l.type]).padEnd(16)} ${nm(l.seat).padEnd(14)} ${pairs(l.pairs)}${t}`);
  }
  if (isMove) mv++;
}
