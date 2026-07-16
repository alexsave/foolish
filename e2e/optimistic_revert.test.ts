// Reproduces the "just-played card snaps back to my hand" flicker end to end:
// drive a REAL game (real executeWithGameLock + handlers + commit_game +
// broadcastAnimationEvents), capture the exact broadcasts the attacking player's
// client receives, then feed them through the REAL client decision the deployed
// AnimationContext uses (resolveUnconfirmedAttackCovers) — and assert the client
// does NOT revert a card the server actually accepted.
//
// Two player-reported symptoms, one root cause (the broadcast-path speculative
// revert in resolveOptimisticConflicts):
//   Scenario B — "I put a card down and someone nearly immediately picked it up,
//                 resulting in a revert animation." The pickup broadcast sweeps my
//                 card off the table; the client wrongly flies it back to my hand.
//   Scenario A — "I play a card at almost the same time as someone else; it jumps
//                 to the table, back to my hand, then to the table again." A
//                 concurrent broadcast that predates my move's commit doesn't show
//                 my card yet.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool, broadcastLog } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/adapter/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { AnimationEvent, Card } from '../supabase/functions/_shared/types.ts';
import { legalMovesFor, applyPlayerMove } from './dispatch.ts';
import { resolveUnconfirmedAttackCovers } from '../src/state/optimisticConflicts';
import { getTableCards, getCardKey } from '../src/utils/animationUtils';
import { decodeEventWire } from '../supabase/functions/_shared/sdk/ts/wire/evwire.ts';
import { ViewRoster } from '../supabase/functions/_shared/sdk/ts/wire/view.ts';
import { base64ToBytes } from '../supabase/functions/_shared/sdk/ts/wire/bytes.ts';
import { __setKernelSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

const sameCard = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;

// The broadcasts the given player's client would have received, newest last —
// packed {t,s,v,b} payloads decoded with the REAL client decoder into the
// {events, game} shape the animation pipeline consumes. preGood/prevGoodTs
// are dummies: these tests only look at events/cards/tables, never at
// good_players order or good_timestamp.
function streamFor(gameId: string, playerId: string, roster: ViewRoster) {
    const chan = `gu-${gameId}-${playerId}`;
    return broadcastLog
        .filter((b: any) => b.channel === chan && b.event === 'animation_events')
        .map((b: any) => decodeEventWire(base64ToBytes(b.payload.b), roster, { preGood: [], prevGoodTs: null })!);
}

// AnimationContext's inputs to the decision, pulled out of a raw broadcast exactly
// as handleAnimationMessage does: the final server table is the last event that
// carries a game_state.
function decisionInputs(payload: any) {
    const lastEventWithState = [...payload.events].reverse().find((e: any) => e.game_state);
    const serverState = lastEventWithState?.game_state ?? payload.game;
    return { serverTableCards: getTableCards(serverState), events: payload.events, finalGameState: payload.game || serverState };
}

// Seed two humans (so both get personalized broadcasts), start, and return roles.
async function startTwoHumanGame() {
    const gameId = `o${uuid().slice(0, 6)}`;
    const hero = uuid();
    const rival = uuid();
    const seeded = [
        { id: hero, name: 'Hero', is_ai: false, strategy_key: 'human' },
        { id: rival, name: 'Rival', is_ai: false, strategy_key: 'human' },
    ];
    await seedGame(gameId, seeded);
    const roster: ViewRoster = { id: gameId, name: gameId, players: seeded.map((p) => ({ player_id: p.id, name: p.name, is_ai: p.is_ai })) };
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
    const g = await loadCompleteGame(gameId);
    const attackerId = g.players[g.first_attacker].player_id;
    const defenderId = g.players[g.defender].player_id;
    return { gameId, hero, rival, attackerId, defenderId, roster };
}

test('SCENARIO B: a card the defender picks up is NOT reverted to my hand', async () => {
    const { gameId, attackerId, defenderId, roster } = await startTwoHumanGame();

    // 1. The attacker plays ONE attack — this is the card they "put down".
    let g = await loadCompleteGame(gameId);
    const attackMove = legalMovesFor(g, (p) => p === attackerId).find((m) => m.move.type === 'attack' && m.move.cards?.length === 1);
    assert.ok(attackMove, 'attacker should have a single-card attack available');
    const myCard: Card = attackMove!.move.cards![0];
    await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, attackMove!) }), 'attack', false);

    // 2. The defender immediately picks up — sweeping my card off the table.
    g = await loadCompleteGame(gameId);
    const pickupMove = legalMovesFor(g, (p) => p === defenderId).find((m) => m.move.type === 'pickup');
    assert.ok(pickupMove, 'defender should be able to pick up');
    await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pickupMove!) }), 'pickup', false);

    // 3. The pickup broadcast the attacker's client receives.
    const stream = streamFor(gameId, attackerId, roster);
    const pickupBcast = stream.find((p) => p.events.some((e: any) => e.type === 'pickup'));
    assert.ok(pickupBcast, 'attacker should receive a pickup broadcast');
    const pickupEvent = pickupBcast.events.find((e: any) => e.type === 'pickup');
    assert.ok(pickupEvent.cards.some((c: Card) => sameCard(c, myCard)),
        'sanity: my card is among the cards the defender picked up (it WAS accepted, then swept)');

    // 4. The client processes that pickup while my optimistic attack is still pending
    //    (its own confirming broadcast not yet applied — the race the player hit).
    const { serverTableCards, events, finalGameState } = decisionInputs(pickupBcast);
    const { revert } = resolveUnconfirmedAttackCovers([myCard], serverTableCards, events, finalGameState);

    assert.ok(!revert.some((c) => sameCard(c, myCard)),
        `BUG: my card ${getCardKey(myCard)} was reverted to my hand even though the defender legitimately picked it up`);
});

test('SCENARIO A: a card still in flight is NOT reverted by a concurrent attack broadcast', async () => {
    // Rival and Hero both pile onto the same defender near-simultaneously. The
    // server serializes them; Hero's client, still holding its optimistic attack,
    // receives Rival's (lower-version) broadcast first — which does not yet contain
    // Hero's card. Hero's follow-up attack is a genuine LEGAL rank-match (computed
    // after Rival commits, exactly as the kernel would allow it), so a revert would
    // be wrong. Repeat across fresh games to sweep table/hand sizes.
    // Pin the kernel deal seed: unpinned, roughly 1 run in 40 dealt twelve
    // straight hands where the hero held no rank-matching follow-up, failing
    // the `checked > 0` floor as a flake. This sequence is verified to
    // produce matching deals and keeps the sweep deterministic.
    let kseed = 0xa11ce;
    __setKernelSeedSource(() => { kseed = (kseed * 48271) % 0x7fffffff; return kseed; });
    let checked = 0;
    for (let t = 0; t < 12; t++) {
        const gameId = `a${uuid().slice(0, 6)}`;
        const p0 = uuid();
        const p1 = uuid();
        const p2 = uuid();
        const seeded = [
            { id: p0, name: 'A0', is_ai: false, strategy_key: 'human' },
            { id: p1, name: 'A1', is_ai: false, strategy_key: 'human' },
            { id: p2, name: 'A2', is_ai: false, strategy_key: 'human' },
        ];
        await seedGame(gameId, seeded);
        const roster: ViewRoster = { id: gameId, name: gameId, players: seeded.map((p) => ({ player_id: p.id, name: p.name, is_ai: p.is_ai })) };
        await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);

        let g = await loadCompleteGame(gameId);
        const defenderId = g.players[g.defender].player_id;
        const nonDefenders = g.players.filter((p) => p.player_id !== defenderId).map((p) => p.player_id);
        const rivalId = nonDefenders[0];
        const heroId = nonDefenders[1];
        if (!rivalId || !heroId) continue;

        const rivalAttack = legalMovesFor(g, (p) => p === rivalId).find((m) => m.move.type === 'attack' && m.move.cards?.length === 1);
        if (!rivalAttack) continue;

        // Rival commits first — Hero's optimistic card is NOT in this broadcast.
        await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, rivalAttack) }), 'rival', false);

        // Hero's LEGAL follow-up (the kernel only offers rank-matching adds here).
        g = await loadCompleteGame(gameId);
        const heroAttack = legalMovesFor(g, (p) => p === heroId).find((m) => m.move.type === 'attack' && m.move.cards?.length === 1);
        if (!heroAttack) continue; // Hero holds no matching rank this deal — skip.
        const heroCard: Card = heroAttack.move.cards![0];

        // The concurrent broadcast Hero's client sees while its own attack is pending.
        const rivalBcast = streamFor(gameId, heroId, roster).find((p) => p.events.some((e: any) => e.type === 'attack_pass'));
        if (!rivalBcast) continue;

        const { serverTableCards, events, finalGameState } = decisionInputs(rivalBcast);
        const { revert } = resolveUnconfirmedAttackCovers([heroCard], serverTableCards, events, finalGameState);
        assert.ok(!revert.some((c) => sameCard(c, heroCard)),
            `BUG: Hero's still-valid card ${getCardKey(heroCard)} was reverted by a concurrent attack broadcast`);
        checked++;
    }
    assert.ok(checked > 0, 'expected at least one valid concurrent-attack trial');
});

after(async () => { await pgPool.end(); });
