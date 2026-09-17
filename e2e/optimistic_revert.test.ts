// Reproduces the "just-played card snaps back to my hand" flicker end to end:
// drive a REAL game (the real move path: executePackedAction -> the table_io CAS
// loop -> the C Table -> commit_table -> the kernel's per-viewer pushes), capture
// the exact broadcasts the attacking player's client receives, then feed them
// through the REAL client decision the deployed AnimationContext uses
// (resolveUnconfirmedAttackCovers, the kernel's client_conflict_verdicts over
// the push's own boards) - and assert the client does NOT revert a card the
// server actually accepted. The boards on the client's side are the kernel's
// too: the optimistic board a tap leaves (client_optimistic_apply) and the push
// boards that keep a pending card (client_board_edit KEEP), held to the boards
// the server then commits, so a kept card is where the server puts it.
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
import type { ViewCard as Card } from '../src/state/view';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, PLAYING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { legalMoves, mustReadTable } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { resolveUnconfirmedAttackCovers } from '../src/state/optimisticConflicts';
import { keepPending, optimisticBoard } from '../src/state/clientBoards';
import { getTableCards, getCardKey } from '../src/utils/animationUtils';
import { clientTable, type TableView } from '../sdk/ts/table/client_table.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import type { TableState } from './helpers/table_play.ts';
import { readPushSequence } from './helpers/client_read.ts';
import type { ReadRoster as ViewRoster } from './helpers/client_read.ts';
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
// sequence and ignores the as3 tail) into the {events, game} boards the animation
// pipeline consumes (src/state/pushSequence.ts).
function streamFor(gameId: string, playerId: string, roster: ViewRoster) {
    const chan = `gu-${gameId}-${playerId}`;
    return broadcastLog
        .filter((b) => b.channel === chan && b.event === 'animation_events')
        .map((b) => readPushSequence(base64ToBytes(b.payload.b), roster)!);
}

// AnimationContext's inputs to the decision, pulled out of a raw broadcast exactly
// as handleAnimationMessage does: the push's last board is the last event that
// carries a game_state, and its final board is the push's own.
function decisionInputs(payload: any): { open: TableView; events: any[]; final: TableView } {
    const lastEventWithState = [...payload.events].reverse().find((e: any) => e.game_state);
    const serverState = lastEventWithState?.game_state ?? payload.game;
    return { open: serverState, events: payload.events, final: payload.game || serverState };
}

// The board a seat holds for a stored row: the envelope the server serves it, read as the web reads it.
function heldBoard(t: TableState, seat: number): TableView {
    const table = fixtureTable();
    assert.equal(table.load(t.state, t.roster), L.TABLE_OK, 'the row loads');
    const envelope = table.envelope(t.gameId, seat, t.version);
    assert.ok(envelope instanceof Uint8Array, 'the seat is served its board');
    const view = clientTable().adoptEnvelope(envelope);
    assert.ok(view, 'and reads it');
    return view!;
}

const tableHas = (v: TableView, c: Card) => getTableCards(v).filter((x) => sameCard(x, c)).length;
const handHas = (v: TableView, c: Card) => v.myHand.some((x) => sameCard(x, c));

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
    // The tap: the board the attacker holds, with the attack standing on it.
    const optimistic = optimisticBoard(heldBoard(t0, 0), attackMove!.wire);
    assert.ok(optimistic, 'the kernel makes the optimistic board');
    assert.equal(tableHas(optimistic!, myCard), 1, 'my card stands on the optimistic table once');
    assert.ok(!handHas(optimistic!, myCard), 'and has left my hand');
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

    // The attack's own confirmation commits the board the tap predicted.
    const attackBcast = stream.find((p) => p.events.some((e: any) => e.type === 'attack_pass'));
    assert.ok(attackBcast, 'attacker should receive its own attack broadcast');
    assert.deepEqual(attackBcast.game.battles, optimistic!.battles, 'the server commits the optimistic table');
    assert.deepEqual(attackBcast.game.myHand, optimistic!.myHand, 'and the optimistic hand');

    // 4. The client processes that pickup while my optimistic attack is still pending
    //    (its own confirming broadcast not yet applied - the race the player hit).
    const { open, events, final } = decisionInputs(pickupBcast);
    const { revert, merge, clear } = resolveUnconfirmedAttackCovers([myCard], open, events, final);

    assert.ok(!revert.some((c) => sameCard(c, myCard)),
        `BUG: my card ${getCardKey(myCard)} was reverted to my hand even though the defender legitimately picked it up`);
    assert.ok(clear.some((c) => sameCard(c, myCard)) && merge.length === 0,
        'the swept card is cleared: its tracking drops and the pickup animates it, nothing is merged back');
    assert.equal(tableHas(final, myCard), 0, 'the board the pickup leaves has my card off the table');
    assert.ok(!handHas(final, myCard), 'and not in my hand: the defender holds it');
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

        const { open, events, final } = decisionInputs(rivalBcast);
        const { revert, merge } = resolveUnconfirmedAttackCovers([heroCard], open, events, final);
        assert.ok(!revert.some((c) => sameCard(c, heroCard)),
            `BUG: Hero's still-valid card ${getCardKey(heroCard)} was reverted by a concurrent attack broadcast (trial ${t}, seed=${rng.seed})`);
        assert.ok(merge.some((c) => sameCard(c, heroCard)), `the pending card is kept (trial ${t}, seed=${rng.seed})`);

        // The kept card stands on every board of that push, once, and out of Hero's hand -
        // as the kernel's optimistic board for the tap has it.
        const heroSeat = g2.seats.findIndex((s) => s.id === heroId);
        const optimistic = optimisticBoard(heldBoard(g2, heroSeat), heroAttack.wire);
        assert.ok(optimistic && tableHas(optimistic, heroCard) === 1 && !handHas(optimistic, heroCard), 'the tap leaves the card on the table');
        const boards = [...rivalBcast.events.map((e: any) => e.game_state).filter(Boolean), rivalBcast.game] as TableView[];
        const kept = boards.map((b) => keepPending(b, [{ card: heroCard }]));
        for (const b of kept) {
            assert.ok(b, 'the kernel keeps the card on the push board');
            assert.equal(tableHas(b!, heroCard), 1, `the kept card stands once (trial ${t}, seed=${rng.seed})`);
            assert.ok(!handHas(b!, heroCard), 'and is not in the hand');
        }

        // Then Hero's own move commits, and its board is the kept one: nothing jumps.
        assert.equal((await runAction(gameId, heroId, heroAttack)).rejectCode, 0, 'hero attack applies');
        await new Promise((r) => setImmediate(r));
        const heroBcast = streamFor(gameId, heroId, roster).filter((p) => p.events.some((e: any) => e.type === 'attack_pass')).pop();
        assert.ok(heroBcast, 'hero receives its own attack broadcast');
        assert.deepEqual(heroBcast.game.battles, kept[kept.length - 1]!.battles, `the committed table is the kept one (trial ${t}, seed=${rng.seed})`);
        assert.deepEqual(heroBcast.game.myHand, kept[kept.length - 1]!.myHand, 'and so is the hand');
        checked++;
    }
    assert.ok(checked > 0, `expected at least one valid concurrent-attack trial (seed=${rng.seed})`);
});
