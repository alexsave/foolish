// Ad-hoc harness (not part of the suite): plays full bot-vs-bot games through
// the production path (processBotAction -> registry -> bots.wasm) and reports
// process RSS growth, to verify the endgame solvers' wasm heap footprint.
import { start_game, game_done } from '../server/api/common/common_utils.ts';
import { processBotAction, shouldBotActCore } from '../server/api/common/pure_bot_actions.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS } from '../server/api/core/types.ts';

const rssMB = () => Math.round(process.memoryUsage().rss / 1048576);

// Capture every wasm memory as modules instantiate so we can report the real
// (virtual) footprint the edge runtime bills against, not just touched RSS.
const memories: WebAssembly.Memory[] = [];
const RealInstance = WebAssembly.Instance;
(WebAssembly as any).Instance = function (mod: WebAssembly.Module, imports?: WebAssembly.Imports) {
    const inst = new RealInstance(mod, imports);
    const m = (inst.exports as any).memory;
    if (m instanceof WebAssembly.Memory) memories.push(m);
    return inst;
} as any;
(WebAssembly as any).Instance.prototype = RealInstance.prototype;
const wasmMB = () => memories.map(m => Math.round(m.buffer.byteLength / 1048576)).join('+');

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

console.log('start rss:', rssMB(), 'MB');
const matchups: string[][] = [['semtex', 'octogen'], ['cordite', 'semtex'], ['fulminate', 'octogen']];
for (const keys of matchups) {
    const g = mkGame(`mem-${keys.join('-')}`, keys);
    start_game(g);
    let guard = 0, moves = 0;
    while (game_done(g) === null && ++guard < 2000) {
        let acted = false;
        for (let i = 0; i < g.players.length; i++) {
            const p = g.players[i];
            if (!shouldBotActCore(g, p, i)) continue;
            const r = await processBotAction(g, p);
            if (r) { moves++; acted = true; break; }
        }
        if (!acted) break;
    }
    console.log(`${keys.join(' vs ')}: done=${game_done(g) !== null} moves=${moves} rss=${rssMB()}MB wasm=${wasmMB()}MB`);
}
console.log('final rss:', rssMB(), 'MB');
