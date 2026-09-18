// Ad-hoc M1 stack canary (not part of the suite). Paints the bots.wasm shadow
// stack with 0xA5 after a warmup, drives the heaviest known stack corpus
// (deep 2-player endgame solves across every MC family + 8-player games)
// through the production bot path - tables of bots on the kernel's bot cycle,
// as the server's loop drives them (e2e/helpers/bot_table.ts) - then reports
// the high-water mark.
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/stack_canary.mts
//
// --stack-first puts the stack at [0, stackSize) growing DOWN from stackSize;
// between exported calls no wasm frames are live, so any byte below the
// high-water is left non-0xA5.  high-water = stackSize - lowest_untouched.
import { playBotTable, seedBytes } from './helpers/bot_table.ts';
import { fixtureExports } from './helpers/table_fixture.ts';

const STACK_SIZE = Number(process.env.STACK_SIZE ?? 22528);  // current bots -z stack-size (Makefile WASM_BOT_LDFLAGS)
if (!process.env.E2E_VERBOSE) { console.warn = () => {}; }

let game = 0;
function playGame(brains: string[]) {
    playBotTable(brains, seedBytes(brains.length, game), { gameId: `canary-${game++}` });
}

// Warm up so the module is instantiated and the TT bump-alloc has happened.
playGame(['octogen', 'cordite']);
const botsMem = fixtureExports().memory;

// Paint the dead stack.
new Uint8Array(botsMem.buffer).fill(0xA5, 64, STACK_SIZE - 64);

// Heavy corpus: deepest endgames are 2-player (longest solves), across every
// MC family; plus wide 8-player states. Multiple deals via the seed.
// IMPORTANT: every strategy the roster dispatches is a potential high-water
// frame, so the corpus must exercise the fat-framed HEURISTICS (handwritten,
// simple_heuristic) as the DECIDING bot too, not just the MC families - their
// choose frames stack under their own callers. Only the SHIPPED wasm bots (the
// bot_roster keys): the MC families octogen/cordite/firecracker/blackpowder plus
// the heuristics handwritten and simple_heuristic. The MC bots internally
// exercise their espresso/handwritten (arena) rollout policies, which carry the
// fattest remaining frames - firecracker rolls out with espresso on EVERY sample
// and blackpowder in its multi-player endgames, so both drive espresso's 1v1 body.
const families = ['octogen', 'cordite', 'firecracker', 'blackpowder'];
const heuristics = ['handwritten', 'simple_heuristic'];
const matchups: string[][] = [];
for (const f of families) { matchups.push([f, f]); matchups.push([f, 'cordite']); }
for (const h of heuristics) { matchups.push([h, h]); matchups.push([h, 'octogen']); }
matchups.push(['octogen', 'cordite', 'handwritten', 'simple_heuristic', 'octogen', 'cordite', 'handwritten', 'simple_heuristic']); // 8p mixed
matchups.push(['octogen', 'octogen', 'octogen', 'octogen', 'octogen', 'octogen', 'octogen', 'octogen']);

const ROUNDS = Number(process.env.CANARY_ROUNDS ?? 6);
for (let r = 0; r < ROUNDS; r++) {
    for (const brains of matchups) playGame(brains);
}

// Scan for high-water: lowest still-painted byte from the bottom.
const mem = new Uint8Array(botsMem.buffer);
let low = 64;
while (low < STACK_SIZE && mem[low] === 0xA5) low++;
const highWater = STACK_SIZE - low;
console.log('---');
console.log('games played     =', game);
console.log('stack high-water =', highWater, 'bytes =', (highWater / 1024).toFixed(1), 'KiB');
console.log('stack size       =', STACK_SIZE, 'bytes =', (STACK_SIZE / 1024).toFixed(1), 'KiB');
console.log('margin at 128KiB =', (131072 / highWater).toFixed(2) + 'x');
console.log('margin at 64KiB  =', (65536 / highWater).toFixed(2) + 'x');
