// Cross-check: tokenize a hand-built game state in TS and print the token
// sequence + logits hash. The C `cross_check` test builds the SAME state
// and prints its own sequence; the two outputs should match line-for-line.
//
// Run with:
//   tsx cnitro/tests/cross_check_ts.ts
//   ./cnitro/build/cnitro_cross_check
// then `diff` the two outputs.

import {
    tokenize,
    InProgress,
    NUM_ACTIONS,
} from '../../supabase/functions/_shared/strategies/nitro_nn.ts';
import {
    Game,
    GAME_STATUS,
    PLAYER_STATUS,
    PrivatePlayer,
} from '../../supabase/functions/_shared/types.ts';

const print = (s: string) => process.stdout.write(s + '\n');

function makePlayer(id: string, hand: number[][]): PrivatePlayer {
    return {
        player_id: id, name: id, status: PLAYER_STATUS.IN, hand_length: hand.length,
        is_ai: true, awaiting_attack: false, strategy_key: 'random',
        hand: hand.map(([s, v]) => ({ suit: s, value: v })),
    };
}

const game: Game = {
    id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0,
    flipped: null, deck: [],
    players: [
        makePlayer('p0', [[0,5],[1,7],[2,9],[3,12]]),       // 5S 7H 9C QD
        makePlayer('p1', [[0,8],[1,6],[2,10],[3,11]]),      // 8S 6H 10C JD
    ],
    power_suit: 3,                  // diamonds = trump
    first_attacker: 0, defender: 1,
    table_battles: [
        { attack: { suit: 0, value: 5 }, defense: null },
    ],
    elimination_order: [],
    good_timestamp: null,
    good_players: [],
    logs: [],
};

const ip: InProgress = { role: 'idle', cardsChosen: [] };
const t = tokenize(game, 'p0', ip);
print(`tokens.length=${t.tokens.length}`);
print(`tokens=${t.tokens.join(',')}`);
print(`vocab_size=${72} num_actions=${NUM_ACTIONS}`);
