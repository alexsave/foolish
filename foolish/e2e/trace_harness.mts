// Ad-hoc: trace every decision of one seeded bot game (for cross-build
// divergence hunting), played by the production path - a table of bots on the
// kernel's bot cycle (e2e/helpers/bot_table.ts), one action per cycle. Prints
// the deal and each move compactly; two builds that play the same seed the same
// way print the same lines.
//   TRACE_KEYS=cordite,octogen TRACE_GI=0 TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/trace_harness.mts
// TRACE_AT=<n> also prints the board the n-th decision was made on.
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { botCycle, dealBotTable, lastDriveMoves, seedBytes } from './helpers/bot_table.ts';
import { fixtureExports, fixtureTable } from './helpers/table_fixture.ts';

const keys = (process.env.TRACE_KEYS ?? 'cordite,octogen').split(',');
const gi = Number(process.env.TRACE_GI ?? '0');
if (!process.env.E2E_VERBOSE) { console.log = () => {}; }
const out = console.error.bind(console);

const cardStr = (c: { suit: number; value: number }) => `${c.suit}.${c.value}`;
const MOVE = Object.fromEntries(Object.entries(L).filter(([k]) => k.startsWith('MOVE_')).map(([k, v]) => [v as number, k.slice(5).toLowerCase()]));

/** The loaded board, read through the generated accessors. */
function board() {
    const ex = fixtureExports();
    const m = L.memOf(ex.memory.buffer);
    const g = ex.wasm_game_ptr_internal();
    const cards = (at: (i: number) => number, n: number) => Array.from({ length: n }, (_, i) => ({
        suit: L.Card_get_suit(m, at(i)), value: L.Card_get_value(m, at(i)),
    }));
    return {
        hands: keys.map((_, s) => { const p = L.Game_players_at(g, s); return cards((i) => L.Player_hand_at(p, i), L.Player_get_hand_count(m, p)); }),
        deck: L.Game_get_deck_count(m, g),
        flip: L.Game_get_has_flipped(m, g) ? cardStr(cards(() => L.Game_flipped_at(g), 1)[0]) : '-',
        power: L.Game_get_power_suit(m, g),
        first: L.Game_get_first_attacker(m, g),
        defender: L.Game_get_defender(m, g),
        discard: L.Game_get_discard_pile_length(m, g),
        battles: Array.from({ length: L.Game_get_num_battles(m, g) }, (_, i) => {
            const b = L.Game_table_battles_at(g, i);
            return `${cardStr({ suit: L.Card_get_suit(m, L.Battle_attack_at(b)), value: L.Card_get_value(m, L.Battle_attack_at(b)) })}`
                + `/${cardStr({ suit: L.Card_get_suit(m, L.Battle_defense_at(b)), value: L.Card_get_value(m, L.Battle_defense_at(b)) })}`;
        }),
    };
}

let row = dealBotTable(keys, seedBytes(keys.length, gi), { gameId: `trace${gi}` });
fixtureTable().load(row.state, row.roster);
const dealt = board();
out(`deal ${dealt.hands.map((h, s) => `p${s}=[${h.map(cardStr)}]`).join(' ')} deck=${dealt.deck} flip=${dealt.flip} power=${dealt.power} first=${dealt.first}`);

let moves = 0;
for (let guard = 0; row.status === L.GAME_STATUS_PLAYING && guard < 4000; guard++) {
    if (moves === Number(process.env.TRACE_AT ?? -1)) {
        fixtureTable().load(row.state, row.roster);
        out(`AT#${moves} state=${JSON.stringify(board())} nlog=${row.log.length}B`);
    }
    const c = botCycle(row, { maxActions: 1 });
    if (c.drive.n === 0) break;
    for (const mv of lastDriveMoves()) {
        out(`#${moves} seat=${mv.seat} ${MOVE[mv.type]} cards=[${mv.cards.map(cardStr)}] atk=[${mv.attacks.map(cardStr)}]`);
        moves++;
    }
    row = c.row;
}
out(`done fool=${row.fool} moves=${moves}`);
