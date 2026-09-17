// Reproduces the "just-played card snaps back to my hand" flicker end to end:
// drive a REAL game (the real move path: executePackedAction -> the table_io CAS
// loop -> the C Table -> commit_table -> the kernel's per-viewer pushes), capture
// the exact broadcasts the attacking player's client receives, then feed them
// through the REAL client decision the deployed AnimationContext uses
// (resolveUnconfirmedAttackCovers) - and assert the client does NOT revert a
// card the server actually accepted.
//
// Two player-reported symptoms, one root cause (the broadcast-path speculative
// revert in resolveOptimisticConflicts):
//   Scenario B - "I put a card down and someone nearly immediately picked it up,
//                 resulting in a revert animation." The pickup broadcast sweeps my
//                 card off the table; the client wrongly flies it back to my hand.
//   Scenario A - "I play a card at almost the same time as someone else; it jumps
//                 to the table, back to my hand, then to the table again." A
//                 concurrent broadcast that predates my move's commit doesn't show
//                 my card yet.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import type { Card } from '../server/api/core/types.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, PLAYING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { legalMoves, mustReadTable } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { resolveUnconfirmedAttackCovers } from '../src/state/optimisticConflicts';
import { getTableCards, getCardKey } from '../src/utils/animationUtils';
import { decodeEventWire } from '../sdk/ts/wire/evwire.ts';
import type { ViewRoster } from '../sdk/ts/wire/view.ts';
import { base64ToBytes } from '../sdk/ts/wire/bytes.ts';

const rng = suiteRng('optimistic_revert');

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { __setTableDealSeedOverride(null); await pgPool.end(); });

const sameCard = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;
const rosterOf = (gameId: string, seats: { id: string; name: string; brain: string }[]): ViewRoster =>
    ({ id: gameId, name: gameId, players: seats.map((s) => ({ player_id: s.id, name: s.name, is_ai: s.brain !== '' })) });

// The broadcasts the given player's client would have received, newest last -
// the kernel's push bytes decoded with the REAL client decoder (it reads the as2
// sequence and ignores the as3 tail) into the {events, game} shape the animation
// pipeline consumes. preGood/prevGoodTs are dummies: these tests only look at
// events/cards/tables, never at good_players order or good_timestamp.
function streamFor(gameId: string, playerId: string, roster: ViewRoster) {
    const chan = `gu-${gameId}-${playerId}`;
    return broadcastLog
        .filter((b) => b.channel === chan && b.event === 'animation_events')
        .map((b) => decodeEventWire(base64ToBytes(b.payload.b), roster, { preGood: [], prevGoodTs: null })!);
}

// AnimationContext's inputs to the decision, pulled out of a raw broadcast exactly
// as handleAnimationMessage does: the final server table is the last event that
// carries a game_state.
function decisionInputs(payload: any) {
    const lastEventWithState = [...payload.events].reverse().find((e: any) => e.game_state);
    const serverState = lastEventWithState?.game_state ?? payload.game;
    return { serverTableCards: getTableCards(serverState), events: payload.events, finalGameState: payload.game || serverState };
}

test('SCENARIO B: a card the defender picks up is NOT reverted to my hand', async () => {
    // Two humans (so both get personalized broadcasts): the attacker (seat 0)
    // opens, the defender (seat 1) takes it at once.
    const gameId = `o${uuid().slice(0, 6)}`;
    const attackerId = uuid(), defenderId = uuid();
    await seedTable(gameId, fixture()
        .seats([{ id: attackerId, name: 'Hero' }, { id: defenderId, name: 'Rival' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '6h 9c Jd Qs Kh 7s').hand(1, '6s 8d Tc Jh Ad 7c')
        .deck('8h 9h Th 8s 9s Ts').trump('Ac').discard(17)
        .build());
    const t0 = await mustReadTable(gameId);
    const roster = rosterOf(gameId, t0.seats);

    // 1. The attacker plays ONE attack - this is the card they "put down".
    const attackMove = legalMoves(t0, (_, seat) => seat === 0).find((m) => m.kind === 'attack' && m.cards.length === 1);
    assert.ok(attackMove, 'attacker should have a single-card attack available');
    const myCard: Card = attackMove!.cards[0];
    assert.equal((await runAction(gameId, attackerId, attackMove!)).rejectCode, 0, 'the attack applies');

    // 2. The defender immediately picks up - sweeping my card off the table.
    const t1 = await mustReadTable(gameId);
    const pickupMove = legalMoves(t1, (_, seat) => seat === 1).find((m) => m.kind === 'pickup');
    assert.ok(pickupMove, 'defender should be able to pick up');
    assert.equal((await runAction(gameId, defenderId, pickupMove!)).rejectCode, 0, 'the pickup applies');
    await new Promise((r) => setImmediate(r));   // the broadcast is fire-and-forget

    // 3. The pickup broadcast the attacker's client receives.
    const stream = streamFor(gameId, attackerId, roster);
    const pickupBcast = stream.find((p) => p.events.some((e: any) => e.type === 'pickup'));
    assert.ok(pickupBcast, 'attacker should receive a pickup broadcast');
    const pickupEvent: any = pickupBcast.events.find((e: any) => e.type === 'pickup');
    assert.ok(pickupEvent.cards.some((c: Card) => sameCard(c, myCard)),
        'sanity: my card is among the cards the defender picked up (it WAS accepted, then swept)');

    // 4. The client processes that pickup while my optimistic attack is still pending
    //    (its own confirming broadcast not yet applied - the race the player hit).
    const { serverTableCards, events, finalGameState } = decisionInputs(pickupBcast);
    const { revert } = resolveUnconfirmedAttackCovers([myCard], serverTableCards, events, finalGameState);

    assert.ok(!revert.some((c) => sameCard(c, myCard)),
        `BUG: my card ${getCardKey(myCard)} was reverted to my hand even though the defender legitimately picked it up`);
});

test('SCENARIO A: a card still in flight is NOT reverted by a concurrent attack broadcast', async () => {
    // Rival and Hero both pile onto the same defender near-simultaneously. The
    // server serializes them; Hero's client, still holding its optimistic attack,
    // receives Rival's (lower-version) broadcast first - which does not yet contain
    // Hero's card. Hero's follow-up attack is a genuine LEGAL rank-match (computed
    // after Rival commits, exactly as the kernel would allow it), so a revert would
    // be wrong. Repeat across fresh deals to sweep table/hand sizes; the deals come
    // from the suite seed, so the sweep is the same every run.
    let checked = 0;
    for (let t = 0; t < 12; t++) {
        __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, () => rng.int(256)));
        const gameId = `a${uuid().slice(0, 6)}`;
        const ids = [uuid(), uuid(), uuid()];
        await seedLobby(gameId, ids.map((id, i) => ({ id, name: `A${i}`, ready: i > 0 })));
        await runMeta(gameId, ids[0], { type: 'start' });

        const g = await mustReadTable(gameId);
        assert.equal(g.status, L.GAME_STATUS_PLAYING, `fixture: the game dealt (trial ${t}, seed=${rng.seed})`);
        const roster = rosterOf(gameId, g.seats);
        // The first attacker opens (the only seat that may); Hero is the other
        // non-defender, who may only add a matching rank once a card is down.
        const rivalId = g.seats[g.firstAttacker].id;
        const heroId = g.seats.find((_, i) => i !== g.defender && i !== g.firstAttacker)?.id;
        if (!heroId) continue;

        const rivalAttack = legalMoves(g, (s) => s.id === rivalId).find((m) => m.kind === 'attack' && m.cards.length === 1);
        if (!rivalAttack) continue;

        // Rival commits first - Hero's optimistic card is NOT in this broadcast.
        assert.equal((await runAction(gameId, rivalId, rivalAttack)).rejectCode, 0, 'rival attack applies');

        // Hero's LEGAL follow-up (the kernel only offers rank-matching adds here).
        const g2 = await mustReadTable(gameId);
        const heroAttack = legalMoves(g2, (s) => s.id === heroId).find((m) => m.kind === 'attack' && m.cards.length === 1);
        if (!heroAttack) continue; // Hero holds no matching rank this deal - skip.
        const heroCard: Card = heroAttack.cards[0];

        // The concurrent broadcast Hero's client sees while its own attack is pending.
        await new Promise((r) => setImmediate(r));
        const rivalBcast = streamFor(gameId, heroId, roster).find((p) => p.events.some((e: any) => e.type === 'attack_pass'));
        if (!rivalBcast) continue;

        const { serverTableCards, events, finalGameState } = decisionInputs(rivalBcast);
        const { revert } = resolveUnconfirmedAttackCovers([heroCard], serverTableCards, events, finalGameState);
        assert.ok(!revert.some((c) => sameCard(c, heroCard)),
            `BUG: Hero's still-valid card ${getCardKey(heroCard)} was reverted by a concurrent attack broadcast (trial ${t}, seed=${rng.seed})`);
        checked++;
    }
    assert.ok(checked > 0, `expected at least one valid concurrent-attack trial (seed=${rng.seed})`);
});
