// E2E: the REAL cover handler (supabase/functions/_shared/actions/cover.ts) — the
// validate/execute matching-mismatch fix. Pure deployed code, no DB needed.

import './harness.ts'; // Deno globals for any transitive server import
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Game, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/types.ts';
import { handleCover } from '../supabase/functions/_shared/actions/cover.ts';

const card = (suit: number, value: number) => ({ suit, value });
const makeGame = (): Game => ({
    id: 'cov', name: 'cov', deck_length: 0, discard_pile_length: 0, flipped: null,
    status: GAME_STATUS.PLAYING, power_suit: 0, first_attacker: 0, defender: 1,
    table_battles: [{ attack: card(0, 6), defense: null }, { attack: card(1, 6), defense: null }],
    elimination_order: [], good_timestamp: null, good_players: [], deck: [], logs: [],
    players: [
        { player_id: 'attacker', name: 'A', status: PLAYER_STATUS.IN, is_ai: false, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.HUMAN },
        { player_id: 'defender', name: 'D', status: PLAYER_STATUS.IN, is_ai: false, hand: [card(0, 7), card(0, 8)], awaiting_attack: false, hand_length: 2, strategy_key: STRATEGY_KEY.HUMAN },
    ],
});

test('cover: double-tapping an already-covered same-rank attack is rejected gracefully (no SEVERE 500)', () => {
    const g = makeGame();
    handleCover(g, 'defender', [card(0, 7)], [card(0, 6)]); // cover 7♠ legitimately
    assert.throws(
        () => handleCover(g, 'defender', [card(0, 8)], [card(0, 6)]), // 7♠ already covered; 7♥ still uncovered
        (e: any) => e.message.includes('is not on the table') && !e.message.includes('SEVERE'),
        'must be a graceful rejection, not the uncaught SEVERE',
    );
});

test('cover: the still-uncovered same-rank attack can be covered', () => {
    const g = makeGame();
    handleCover(g, 'defender', [card(0, 7)], [card(0, 6)]); // cover 7♠
    assert.doesNotThrow(() => handleCover(g, 'defender', [card(0, 8)], [card(1, 6)])); // cover 7♥
});
