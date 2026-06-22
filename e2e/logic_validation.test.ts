// VALIDATION (pure, no Postgres): handpicked regressions for the REAL server
// action handlers. Small deterministic versions of cover.test.ts, rearrange.test.ts
// and the targeted assertions in fuzz.test.ts — each pins a specific exploit/bug
// the handler logic must keep rejecting.

// Deno globals some transitive server imports read at load time.
(globalThis as any).Deno = (globalThis as any).Deno || { env: { get: (k: string) => process.env[k] || 'x' } };
(globalThis as any).EdgeRuntime = (globalThis as any).EdgeRuntime || { waitUntil: () => {} };

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Game, Card, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, PrivatePlayer } from '../supabase/functions/_shared/types.ts';
import { handleCover } from '../supabase/functions/_shared/actions/cover.ts';
import { handleAttack } from '../supabase/functions/_shared/actions/attack.ts';
import { handleRearrangeHand } from '../supabase/functions/_shared/actions/rearrange.ts';

const card = (suit: number, value: number): Card => ({ suit, value });
const player = (id: string, hand: Card[]): PrivatePlayer => ({
    player_id: id, name: id, status: PLAYER_STATUS.IN, is_ai: false,
    hand, awaiting_attack: false, hand_length: hand.length, strategy_key: STRATEGY_KEY.HUMAN,
});
const baseGame = (players: PrivatePlayer[], table: Game['table_battles'], defender = 1): Game => ({
    id: 'g', name: 'g', deck_length: 0, discard_pile_length: 0, flipped: null,
    status: GAME_STATUS.PLAYING, power_suit: 0, first_attacker: 0, defender,
    table_battles: table, elimination_order: [], good_timestamp: null, good_players: [],
    deck: [], logs: [], players,
});

// --- cover.test.ts: validate/execute matching-mismatch (the SEVERE 500) -------
test('cover: double-tapping an already-covered same-rank attack is rejected gracefully (no SEVERE 500)', () => {
    const g = baseGame(
        [player('attacker', []), player('defender', [card(0, 7), card(0, 8)])],
        [{ attack: card(0, 6), defense: null }, { attack: card(1, 6), defense: null }]);
    handleCover(g, 'defender', [card(0, 7)], [card(0, 6)]); // legit cover of 6♠
    assert.throws(
        () => handleCover(g, 'defender', [card(0, 8)], [card(0, 6)]), // 6♠ already covered
        (e: any) => e.message.includes('is not on the table') && !e.message.includes('SEVERE'),
        'graceful rejection, not the uncaught SEVERE');
});

test('cover: the still-uncovered same-rank attack can be covered', () => {
    const g = baseGame(
        [player('attacker', []), player('defender', [card(0, 7), card(0, 8)])],
        [{ attack: card(0, 6), defense: null }, { attack: card(1, 6), defense: null }]);
    handleCover(g, 'defender', [card(0, 7)], [card(0, 6)]);
    assert.doesNotThrow(() => handleCover(g, 'defender', [card(0, 8)], [card(1, 6)])); // cover 6♥
});

// --- rearrange.test.ts: the index-list card-cloning exploit --------------------
test('rearrange: duplicate indices ([0,0,...]) are rejected — no card cloning', () => {
    const hand = [card(0, 6), card(1, 7), card(2, 8), card(3, 9)];
    const g = baseGame([player('hero', hand.slice()), player('b', [])], [], 0);
    assert.throws(() => handleRearrangeHand(g, 'hero', new Array(hand.length).fill(0)), /invalid card indices/i);
    // the hand is untouched by the rejected call
    assert.deepEqual(g.players[0].hand.map((c) => `${c.suit}-${c.value}`), hand.map((c) => `${c.suit}-${c.value}`));
});

test('rearrange: a real permutation reorders the hand; bad length / non-array / non-member rejected', () => {
    const hand = [card(0, 6), card(1, 7), card(2, 8), card(3, 9)];
    const g = baseGame([player('hero', hand.slice()), player('b', [])], [], 0);
    handleRearrangeHand(g, 'hero', [3, 2, 1, 0]);
    assert.deepEqual(g.players[0].hand.map((c) => `${c.suit}-${c.value}`), [...hand].reverse().map((c) => `${c.suit}-${c.value}`));
    assert.throws(() => handleRearrangeHand(g, 'hero', [0, 1, 2]), /invalid card indices/i, 'wrong length');
    assert.throws(() => handleRearrangeHand(g, 'hero', 'nope' as any), /must be an array|missing/i, 'non-array');
    assert.throws(() => handleRearrangeHand(g, 'unknown', [0, 1, 2, 3]), /not in this game/i, 'non-member');
});

// --- fuzz.test.ts: the targeted always-reject invariants (card duplication hole)
test('attack: forged card, identical-duplicate, and non-member attacks are all rejected', () => {
    const hand = [card(0, 10), card(1, 10), card(2, 12)];
    const g = baseGame([player('atk', hand.slice()), player('def', [card(3, 14)])], [], 1);
    // forged card not in hand
    assert.throws(() => handleAttack(g, 'atk', [card(3, 9)]), /not in/i, 'forged card');
    // the object-identity duplicate hole: [X, X] must be rejected, never duplicated
    const x = card(0, 10);
    assert.throws(() => handleAttack(g, 'atk', [{ ...x }, { ...x }]), /duplicate/i, 'identical duplicate');
    // a player who isn't in the game
    assert.throws(() => handleAttack(g, 'ghost', [card(0, 10)]), /not found in game/i, 'non-member');
});
