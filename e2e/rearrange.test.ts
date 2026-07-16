// Vulnerability: the rearrange-hand endpoint reordered the caller's hand by an
// index list it only checked for length + range — not uniqueness. So
// card_indices:[0,0,0,0,0,0] passed, and `indices.map(i => hand[i])` produced six
// copies of one card (dropping the rest) — minting duplicate cards into the hand,
// persisted through commit_game. A cheater could clone a trump ace six times.
//
// Owns the rearrange validation scenario (pure — handleRearrangeHand needs no DB);
// the fast runner (e2e/validation/handlers_validation.test.ts) imports
// `registerRearrangeValidation`. The full e2e tests also drive it through the REAL
// CAS commit.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/adapter/utils.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { handleRearrangeHand } from '../supabase/functions/_shared/common/actions/rearrange.ts';
import { AnimationEvent, Card, Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';
import { checkCardConservation } from './dispatch.ts';

// ---- handpicked, pure validation (no DB) -----------------------------------
export function registerRearrangeValidation(): void {
    const card = (suit: number, value: number): Card => ({ suit, value });
    const mkGame = (hand: Card[]): Game => ({
        id: 'g', name: 'g', deck_length: 0, discard_pile_length: 0, flipped: null,
        status: GAME_STATUS.PLAYING, power_suit: 0, first_attacker: 0, defender: 0,
        table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], deck: [], logs: [],
        players: [
            { player_id: 'hero', name: 'hero', status: PLAYER_STATUS.IN, is_ai: false, hand, hand_length: hand.length, awaiting_attack: false, strategy_key: STRATEGY_KEY.HUMAN } as PrivatePlayer,
            { player_id: 'b', name: 'b', status: PLAYER_STATUS.IN, is_ai: false, hand: [], hand_length: 0, awaiting_attack: false, strategy_key: STRATEGY_KEY.HUMAN } as PrivatePlayer,
        ],
    });

    test('rearrange: duplicate indices ([0,0,...]) are rejected — no card cloning', () => {
        const hand = [card(0, 6), card(1, 7), card(2, 8), card(3, 9)];
        const g = mkGame(hand.slice());
        assert.throws(() => handleRearrangeHand(g, 'hero', new Array(hand.length).fill(0)), /invalid card indices/i);
        assert.deepEqual(g.players[0].hand.map((c) => `${c.suit}-${c.value}`), hand.map((c) => `${c.suit}-${c.value}`), 'hand untouched by rejected call');
    });

    test('rearrange: a real permutation reorders; bad length / non-array / non-member rejected', () => {
        const hand = [card(0, 6), card(1, 7), card(2, 8), card(3, 9)];
        const g = mkGame(hand.slice());
        handleRearrangeHand(g, 'hero', [3, 2, 1, 0]);
        assert.deepEqual(g.players[0].hand.map((c) => `${c.suit}-${c.value}`), [...hand].reverse().map((c) => `${c.suit}-${c.value}`));
        assert.throws(() => handleRearrangeHand(g, 'hero', [0, 1, 2]), /invalid card indices/i, 'wrong length');
        assert.throws(() => handleRearrangeHand(g, 'hero', 'nope' as any), /must be an array|missing/i, 'non-array');
        assert.throws(() => handleRearrangeHand(g, 'unknown', [0, 1, 2, 3]), /not in this game/i, 'non-member');
    });
}

// ---- full e2e: the REAL handler through the REAL CAS commit -----------------
if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });

    async function freshGame(): Promise<{ gameId: string; human: string }> {
        const gameId = `rh${uuid().slice(0, 5)}`;
        const human = uuid();
        await seedGame(gameId, [
            { id: human, name: 'H', is_ai: false, strategy_key: 'human' },
            { id: uuid(), name: 'B', is_ai: true, strategy_key: 'random' },
        ]);
        await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
        return { gameId, human };
    }

    test('rearrange-hand: duplicate indices ([0,0,...]) are rejected — no card duplication', async () => {
        const { gameId, human } = await freshGame();
        const before = await loadCompleteGame(gameId);
        const n = before.players.find((p) => p.player_id === human)!.hand.length;
        assert.ok(n > 0, 'human has a hand');

        const dupIndices = new Array(n).fill(0);
        await assert.rejects(
            executeWithGameLock(gameId, async (g) => { handleRearrangeHand(g, human, dupIndices); return { game: g, events: [] }; }, 'rh', false),
            /invalid card indices/i,
            'duplicate indices must be rejected',
        );
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `state intact after rejected exploit: ${chk.detail}`);

        const hand = before.players.find((p) => p.player_id === human)!.hand;
        const naive = dupIndices.map((i) => hand[i]);
        assert.equal(new Set(naive.map((c) => `${c.suit}-${c.value}`)).size, 1, 'unguarded map yields 6 copies of one card');
    });

    test('rearrange-hand: a real permutation reorders the hand and conserves cards', async () => {
        const { gameId, human } = await freshGame();
        const before = await loadCompleteGame(gameId);
        const hand = before.players.find((p) => p.player_id === human)!.hand;
        const reversed = hand.map((_, i) => hand.length - 1 - i);

        await executeWithGameLock(gameId, async (g) => { handleRearrangeHand(g, human, reversed); return { game: g, events: [] }; }, 'rh', false);

        const after = await loadCompleteGame(gameId);
        const newHand = after.players.find((p) => p.player_id === human)!.hand;
        assert.deepEqual(newHand.map((c) => `${c.suit}-${c.value}`), [...hand].reverse().map((c) => `${c.suit}-${c.value}`), 'hand reversed');
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conserved after legit rearrange: ${chk.detail}`);
    });

    test('rearrange-hand: out-of-range / wrong-length / non-array indices are rejected', async () => {
        const { gameId, human } = await freshGame();
        const g = await loadCompleteGame(gameId);
        const n = g.players.find((p) => p.player_id === human)!.hand.length;
        assert.throws(() => handleRearrangeHand(g, human, [n, 0, 1]), /invalid card indices/i, 'out of range + wrong length');
        assert.throws(() => handleRearrangeHand(g, human, 'nope' as any), /must be an array|missing/i, 'non-array');
        assert.throws(() => handleRearrangeHand(g, uuid(), new Array(n).fill(0).map((_, i) => i)), /not in this game/i, 'non-member');
    });

    registerRearrangeValidation();

    after(async () => { await pgPool.end(); });
}
