// Ad-hoc: trace every decision of one pinned-RNG bot game (for cross-build
// divergence hunting). Prints deal + each move compactly.
import { start_game, game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { processBotAction, shouldBotActCore } from '../supabase/functions/_shared/common/pure_bot_actions.ts';
import { __setBotSeedSource } from '../sdk/ts/wasm/bots.ts';
import { __setKernelSeedSource } from '../sdk/ts/wasm/engine.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS } from '../supabase/functions/_shared/core/types.ts';

const mkLcgU32 = (seed: number) => {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s; };
};
const cardStr = (c: { suit: number; value: number }) => `${c.suit}.${c.value}`;

const keys = (process.env.TRACE_KEYS ?? 'semtex,octogen').split(',');
const gi = Number(process.env.TRACE_GI ?? '0');
if (!process.env.E2E_VERBOSE) { console.log = () => {}; }
const out = console.error.bind(console);

__setKernelSeedSource(mkLcgU32(0xDEA1 ^ gi));
const meta = mkLcgU32(0xB07 ^ gi);
__setBotSeedSource(() => meta());

const g: Game = {
    players: keys.map((k, i) => ({
        player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
        hand: [], awaiting_attack: false, hand_length: 0, strategy_key: k,
    } as PrivatePlayer)),
    deck: [], logs: [], id: `trace${gi}`, name: 'trace', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [],
};
start_game(g);
out(`deal p0=[${g.players[0].hand.map(cardStr)}] p1=[${g.players[1].hand.map(cardStr)}] deck=${g.deck.length} flip=${g.flipped ? cardStr(g.flipped) : '-'} power=${g.power_suit} first=${g.first_attacker}`);

let guard = 0, moves = 0;
while (game_done(g) === null && ++guard < 2000) {
    let acted = false;
    for (let i = 0; i < g.players.length; i++) {
        const p = g.players[i];
        if (!shouldBotActCore(g, p, i)) continue;
        if (moves === Number(process.env.TRACE_AT ?? -1)) {
            const { calculateLegalMoves } = await import('../supabase/functions/_shared/common/bot_strategy.ts');
            const lm = calculateLegalMoves(g, p.player_id);
            out(`AT#${moves} seat=${i} strat=${p.strategy_key}`);
            out(`  state=${JSON.stringify({ b: g.table_battles, h: g.players.map(x => x.hand), d: g.deck.length, disc: g.discard_pile_length, def: g.defender, fa: g.first_attacker })}`);
            out(`  nlogs=${g.logs.length} logs=${JSON.stringify(g.logs.slice(-8))}`);
            out(`  legal=${JSON.stringify(lm)}`);
            const { wasmChooseMoveDirect, STRAT, __setBotSeedSource: setSeed } = await import('../sdk/ts/wasm/bots.ts');
            const strat = STRAT[p.strategy_key as keyof typeof STRAT];
            for (const logs of [true, false]) {
                setSeed(() => 12345);
                out(`  probe logs=${logs} -> ${JSON.stringify(wasmChooseMoveDirect(g, p.player_id, strat, { logs }))}`);
            }
            __setBotSeedSource(() => meta());
        }
        const r = await processBotAction(g, p);
        if (r) {
            out(`#${moves} seat=${i} ${r.moveType} cards=[${(r.move.cards ?? []).map(cardStr)}] atk=[${(r.move.attack_cards ?? []).map(cardStr)}]`);
            moves++; acted = true; break;
        }
    }
    if (!acted) break;
}
out(`done=${game_done(g)} moves=${moves}`);
