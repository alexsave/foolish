// Ad-hoc M1 stack canary (not part of the suite). Paints the bots.wasm shadow
// stack with 0xA5 after a warmup, drives the heaviest known stack corpus
// (deep 2-player endgame solves across every MC family + an 8-player game)
// through the production bot path, then reports the high-water mark.
//
// --stack-first puts the stack at [0, stackSize) growing DOWN from stackSize;
// between exported calls no wasm frames are live, so any byte below the
// high-water is left non-0xA5.  high-water = stackSize - lowest_untouched.
import { game_done, shouldBotActCore } from '../server/api/common/common_utils.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { processBotAction } from '../server/api/common/pure_bot_actions.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS } from '../server/api/core/types.ts';

const STACK_SIZE = Number(process.env.STACK_SIZE ?? 22528);  // current bots -z stack-size (Makefile WASM_BOT_LDFLAGS)

// Capture every wasm memory + its initial size; the bots module is the one with
// the largest initial linear memory (rules=5 pages, guards=1 page, bots>=17).
const seen: { mem: WebAssembly.Memory; initial: number }[] = [];
const RealInstance = WebAssembly.Instance;
(WebAssembly as any).Instance = function (mod: WebAssembly.Module, imports?: WebAssembly.Imports) {
    const inst = new RealInstance(mod, imports);
    const m = (inst.exports as any).memory;
    if (m instanceof WebAssembly.Memory) seen.push({ mem: m, initial: m.buffer.byteLength });
    return inst;
} as any;
(WebAssembly as any).Instance.prototype = RealInstance.prototype;
const botsMemOf = () => seen.slice().sort((a, b) => b.initial - a.initial)[0]?.mem ?? null;

const mkPlayer = (i: number, key: string): PrivatePlayer => ({
    player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: key as PrivatePlayer['strategy_key'],
});
const mkGame = (id: string, keys: string[]): Game => ({
    players: keys.map((k, i) => mkPlayer(i, k)),
    deck: [], logs: [], id, name: id, status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [],
});

async function playGame(keys: string[]) {
    const g = mkGame(`canary-${keys.join('-')}`, keys);
    start_game(g);
    let guard = 0;
    while (game_done(g) === null && ++guard < 4000) {
        let acted = false;
        for (let i = 0; i < g.players.length; i++) {
            const p = g.players[i];
            if (!shouldBotActCore(g, p, i)) continue;
            const r = await processBotAction(g, p);
            if (r) { acted = true; break; }
        }
        if (!acted) break;
    }
}

// Warm up so the module is instantiated and the TT bump-alloc has happened.
await playGame(['octogen', 'semtex']);
const botsMem = botsMemOf();
if (!botsMem) { console.error('FAILED to capture bots.wasm memory'); process.exit(1); }

// Paint the dead stack.
new Uint8Array(botsMem.buffer).fill(0xA5, 64, STACK_SIZE - 64);

// Heavy corpus: deepest endgames are 2-player (longest solves), across every
// MC family; plus wide 8-player states.  Multiple seeds via start_game's RNG.
// IMPORTANT: every strategy wasm_choose_move dispatches is a potential
// high-water frame, so the corpus must exercise the fat-framed HEURISTICS
// (espresso/champion/handwritten/simple_heuristic) as the DECIDING bot too, not
// just the MC families — their choose frames stack under their own callers.
// Only the SHIPPED wasm bots (Durak Bot Ordnance Chart drop): the MC families
// octogen/cordite/firecracker/blackpowder plus the dispatchable heuristics
// handwritten_prod and simple_heuristic. The MC bots internally exercise their
// espresso/handwritten (arena) rollout policies, which carry the fattest
// remaining frames — firecracker rolls out with espresso on EVERY sample and
// blackpowder in its multi-player endgames, so both drive espresso's 1v1 body.
const families = ['octogen', 'cordite', 'firecracker', 'blackpowder'];
const heuristics = ['handwritten_prod', 'simple_heuristic'];
const matchups: string[][] = [];
for (const f of families) { matchups.push([f, f]); matchups.push([f, 'cordite']); }
for (const h of heuristics) { matchups.push([h, h]); matchups.push([h, 'octogen']); }
matchups.push(['octogen', 'cordite', 'handwritten_prod', 'simple_heuristic', 'octogen', 'cordite', 'handwritten_prod', 'simple_heuristic']); // 8p mixed
matchups.push(['octogen', 'octogen', 'octogen', 'octogen', 'octogen', 'octogen', 'octogen', 'octogen']);

const ROUNDS = 6;
for (let r = 0; r < ROUNDS; r++) {
    for (const keys of matchups) {
        await playGame(keys);
    }
}

// Scan for high-water: lowest still-painted byte from the bottom.
const mem = new Uint8Array(botsMem.buffer);
let low = 64;
while (low < STACK_SIZE && mem[low] === 0xA5) low++;
const highWater = STACK_SIZE - low;
console.log('---');
console.log('stack high-water =', highWater, 'bytes =', (highWater / 1024).toFixed(1), 'KiB');
console.log('stack size       =', STACK_SIZE, 'bytes =', (STACK_SIZE / 1024).toFixed(1), 'KiB');
console.log('margin at 128KiB =', (131072 / highWater).toFixed(2) + 'x');
console.log('margin at 64KiB  =', (65536 / highWater).toFixed(2) + 'x');
