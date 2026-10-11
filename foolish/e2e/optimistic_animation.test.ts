/* =============================================================================
 * Optimistic-animation dedup regression test (the "card animates twice" bug)
 * =============================================================================
 * Exercises the REAL client routine: settlePush (src/state/optimisticResolve.ts),
 * which asks the kernel what an arriving push does to my unanswered moves
 * (c/src/client_table.h client_push_settle).
 *
 * The bug: when you play a card, the optimistic animation plays, then the
 * server's confirming broadcast plays it AGAIN. Cause - the version gate
 * released the optimistic entry for any card now on the authoritative table,
 * INCLUDING the card the very same broadcast was confirming, so the per-event
 * dedup downstream no longer recognised it and re-animated it.
 *
 * Every case plays it for real: the tap's optimistic board is the kernel's
 * (client_optimistic_apply), the confirming push is the C Table's, and both name
 * the card by the same seat.
 *
 * Pure logic - no Postgres, no harness. Owns the optimistic-animation validation
 * scenarios; the fast runner (e2e/validation/client_validation.test.ts) imports
 * `registerOptimisticValidation`.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { tableCards, sameCard } from '../src/state/view';
import { rememberPending, settlePush, type OptimisticState } from '../src/state/optimisticResolve';
import { optimisticBoard } from '../src/state/clientBoards';
import { clientTable, encodeAction } from '../sdk/ts/table/client_table.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import { fixture, fixtureTable, PLAYING } from './helpers/table_fixture.ts';
import { readPushSequence } from './helpers/client_read.ts';

// Rival (seat 1, this client) leads 6s: the board before the tap, the kernel's
// board after it, and the push the server confirms it with.
const mine = { suit: 0, value: 5 };
function tapAndConfirm() {
    const seats = [{ id: 'u-hero', name: 'Hero' }, { id: 'u-rival', name: 'Rival' }];
    const fx = fixture().seats(seats).status(PLAYING).attacker(1).defender(0).deterministic()
        .hand(0, '6h 9c Jd Qs Kh 7s').hand(1, '6s 8d Tc Jh Ad 7c').deck('8h 9h Th 8s 9s Ts').trump('Ac').build();
    const table = fixtureTable();
    assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK);
    const envelope = table.envelope('g-anim', 1, 5);
    assert.ok(envelope instanceof Uint8Array);
    const held = clientTable().adoptEnvelope(envelope)!;
    const wire = encodeAction({ kind: 'attack', cards: [mine] });
    const optimistic = optimisticBoard(held, wire)!;
    assert.equal(table.act('u-rival', wire, null, 0), L.TABLE_APPLIED, 'the attack applies');
    assert.ok(typeof table.commit('g-anim', 6, 0) !== 'number');
    const push = table.push('g-anim', 1);
    assert.ok(push instanceof Uint8Array);
    const seq = readPushSequence(push, { id: 'g-anim', name: '', players: seats.map((s) => ({ player_id: s.id, name: s.name, is_ai: false })) })!;
    // What the tap remembers (AnimationContext `play`): one table-bound card.
    const pending: OptimisticState = { motions: [], pass: null };
    rememberPending(pending, [mine], V.ANIM_DEST_TABLE, 0);
    return { held, optimistic, seq, pending };
}

export function registerOptimisticValidation(): void {
    test('a card the same broadcast confirms is settled by ITS EVENT, which is not played again (no double-play)', () => {
        const { optimistic, seq, pending } = tapAndConfirm();
        assert.ok(tableCards(optimistic).some((c) => sameCard(c, mine)), 'the card stands on the optimistic board');
        const confirming = seq.events.find((e) => e.type === 'attack_pass')!;
        assert.equal(confirming.seat, optimistic.mySeat, 'the push names the seat the tap predicted for');
        const push = { events: seq.events.map((e) => ({ ...e })), game: seq.game };
        assert.deepEqual(settlePush(pending, push, true), [], 'nothing flies home');
        assert.equal(push.events.some((e) => e.type === 'attack_pass'), false, 'the confirming event is not animated a second time');
        assert.equal(pending.motions.length, 0, 'and the prediction is answered');
        assert.deepEqual(seq.game.battles, optimistic.battles, 'the confirmed table is the optimistic one');
    });

    test('an on-table card whose confirming broadcast was dropped is settled by the board of a later versioned one', () => {
        // A later-versioned broadcast that does NOT name our card (its own confirming
        // broadcast was reordered/dropped by the gate) still shows it on the table.
        const { seq, pending } = tapAndConfirm();
        const unrelated = { ...seq.events[0], type: 'cover', seat: 0, cards: [{ suit: 2, value: 10 }] };
        const push = { events: [unrelated], game: seq.game };
        assert.deepEqual(settlePush(pending, push, true), []);
        assert.equal(pending.motions.length, 0, 'the lingering prediction is released (dropped-broadcast safety net)');
        assert.equal(push.events.length, 1, 'and the push still plays its own event');
    });

    test('a sequence with no version releases nothing by its board', () => {
        const { seq, pending } = tapAndConfirm();
        assert.deepEqual(settlePush(pending, { events: [], game: seq.game }, false), []);
        assert.equal(pending.motions.length, 1, 'a replay is not authoritative about my moves');
    });

    test('a card that is not yet on the authoritative table stays pending', () => {
        const { held, pending } = tapAndConfirm();
        assert.deepEqual(settlePush(pending, { events: [], game: held }, true), []);
        assert.equal(pending.motions.length, 1, 'nothing on table yet - keep the prediction');
    });
}

if (!process.env.VALIDATION_ONLY) registerOptimisticValidation();
