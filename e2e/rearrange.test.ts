// Vulnerability: the rearrange-hand endpoint reordered the caller's hand by an
// index list it only checked for length + range — not uniqueness. So
// card_indices:[0,0,0,0,0,0] passed, and `indices.map(i => hand[i])` produced six
// copies of one card (dropping the rest) — minting duplicate cards into the hand,
// persisted through commit_game. A cheater could clone a trump ace six times.
//
// This drives the REAL handleRearrangeHand (the function the deployed
// rearrange-hand edge function now calls) through the REAL CAS commit and asserts
// the exploit input is rejected and a legit permutation is conserved.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/utils.ts';
import { start_game } from '../supabase/functions/_shared/common_utils.ts';
import { handleRearrangeHand } from '../supabase/functions/_shared/actions/rearrange.ts';
import { AnimationEvent } from '../supabase/functions/_shared/types.ts';
import { checkCardConservation } from './dispatch.ts';

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

    // the exploit payload
    const dupIndices = new Array(n).fill(0);
    await assert.rejects(
        executeWithGameLock(gameId, async (g) => { handleRearrangeHand(g, human, dupIndices); return { game: g, events: [] }; }, 'rh', false),
        /invalid card indices/i,
        'duplicate indices must be rejected',
    );
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `state intact after rejected exploit: ${chk.detail}`);

    // sanity: the unguarded reorder WOULD have duplicated (documents the bug).
    const hand = before.players.find((p) => p.player_id === human)!.hand;
    const naive = dupIndices.map((i) => hand[i]);
    assert.equal(new Set(naive.map((c) => `${c.suit}-${c.value}`)).size, 1, 'unguarded map yields 6 copies of one card');
});

test('rearrange-hand: a real permutation reorders the hand and conserves cards', async () => {
    const { gameId, human } = await freshGame();
    const before = await loadCompleteGame(gameId);
    const hand = before.players.find((p) => p.player_id === human)!.hand;
    const reversed = hand.map((_, i) => hand.length - 1 - i); // valid permutation

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

after(async () => { await pgPool.end(); });
