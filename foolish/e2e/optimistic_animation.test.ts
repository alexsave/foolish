/* =============================================================================
 * Optimistic-animation dedup regression test (the "card animates twice" bug)
 * =============================================================================
 * Exercises the REAL client helpers: the same kernel dedup key the
 * AnimationContext keys optimistic animations by (anim_plan.h anim_event_key,
 * through animEventKey), and the real staleOptimisticKeysOnTable the version
 * gate uses to release them.
 *
 * The bug: when you play a card, the optimistic animation plays, then the
 * server's confirming broadcast plays it AGAIN. Cause — the version gate
 * released the optimistic entry for any card now on the authoritative table,
 * INCLUDING the card the very same broadcast was confirming, so the per-event
 * dedup downstream no longer recognised it and re-animated it.
 *
 * The last case plays it for real: the tap's optimistic board is the kernel's
 * (client_optimistic_apply), the confirming push is the C Table's, and both key
 * the card by the same seat.
 *
 * Pure logic — no Postgres, no harness. Owns the optimistic-animation validation
 * scenarios; the fast runner (e2e/validation/client_validation.test.ts) imports
 * `registerOptimisticValidation`.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { tableCards, type ViewCard as Card } from '../src/state/view';
import { getCardKey } from '../src/utils/animationUtils';
import { animEventKey } from '../sdk/ts/wasm/bots.ts';
import { staleOptimisticKeysOnTable } from '../src/state/optimisticAnimation';
import { optimisticBoard } from '../src/state/clientBoards';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, fixtureTable, PLAYING } from './helpers/table_fixture.ts';
import { readPushSequence } from './helpers/client_read.ts';

const SELF = 0;   // my seat
const card: Card = { suit: 1, value: 9 };

// How AnimationContext keys an optimistic attack: ('attack_pass', card, hand->table, self).
// The key is the KERNEL's, so it is a number and has no fields to read back; a
// caller that wants the card keeps the card, which is what the pending map does.
const optimisticAttackKey = animEventKey('attack_pass', card, 'hand', 'table', SELF);
/** The pending map's shape: the kernel's key, and what was predicted under it. */
const pending = (key: number, c: Card): [number, { card: Card }][] => [[key, { card: c }]];

// The server's confirming attack broadcast (the as3 push table_act's commit writes).
const serverAttackEvent = {
    type: 'attack_pass',
    seat: SELF,
    cards: [card],
    from_location: 'hand',
    to_location: 'table',
};

// The authoritative game state the broadcast carries: the card is now on table.
const tableCardsAfter: Card[] = [card];

export function registerOptimisticValidation(): void {
    test('version gate does NOT release an optimistic card the same broadcast confirms (no double-play)', () => {
        const release = staleOptimisticKeysOnTable(pending(optimisticAttackKey, card), tableCardsAfter, [serverAttackEvent]);
        assert.deepEqual(release, [], 'must not pre-release a card named by this broadcast — the dedup handles it');

        // …and because it was NOT released, the per-event dedup still recognises it
        // (this is the exact match AnimationContext does), so the server event is
        // skipped instead of animating a second time.
        const optimisticKeys = new Set([optimisticAttackKey]);
        const serverKey = animEventKey(
            serverAttackEvent.type,
            serverAttackEvent.cards[0],
            serverAttackEvent.from_location,
            serverAttackEvent.to_location,
            serverAttackEvent.seat,
        );
        assert.ok(optimisticKeys.has(serverKey), 'server confirming event must still match the optimistic key');
    });

    test('version gate DOES release an on-table optimistic card whose confirming broadcast was dropped', () => {
        // A later-versioned broadcast that does NOT name our card (its own confirming
        // broadcast was reordered/dropped by the gate) still shows it on the table.
        const unrelatedEvent = { type: 'cover', cards: [{ suit: 2, value: 10 }], from_location: 'hand', to_location: 'table' };
        const release = staleOptimisticKeysOnTable(pending(optimisticAttackKey, card), tableCardsAfter, [unrelatedEvent]);
        assert.deepEqual(release, [optimisticAttackKey], 'must release the lingering optimistic entry (dropped-broadcast safety net)');
    });

    test('version gate leaves optimistic cards that are not yet on the authoritative table', () => {
        const release = staleOptimisticKeysOnTable(pending(optimisticAttackKey, card), [], []);
        assert.deepEqual(release, [], 'nothing on table yet — keep the optimistic entry');
        // sanity: a key is five fields packed, so a different card keys differently
        assert.notEqual(animEventKey('attack_pass', { suit: 2, value: 9 }, 'hand', 'table', SELF), optimisticAttackKey);
        // …and the key crosses as a plain number, exact: no BigInt, no string.
        assert.ok(Number.isSafeInteger(optimisticAttackKey), 'the kernel key is an exact JS integer');
    });

    test('the kernel\'s optimistic board and the server\'s confirming push key my card alike (no double-play)', () => {
        const seats = [{ id: 'u-hero', name: 'Hero' }, { id: 'u-rival', name: 'Rival' }];
        const fx = fixture().seats(seats).status(PLAYING).attacker(1).defender(0).deterministic()
            .hand(0, '6h 9c Jd Qs Kh 7s').hand(1, '6s 8d Tc Jh Ad 7c').deck('8h 9h Th 8s 9s Ts').trump('Ac').build();
        const table = fixtureTable();
        assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK);
        const envelope = table.envelope('g-anim', 1, 5);
        assert.ok(envelope instanceof Uint8Array);
        const held = clientTable().adoptEnvelope(envelope)!;

        // The tap: Rival (seat 1, this client) leads 6s; the kernel makes the board it leaves.
        const mine = { suit: 0, value: 5 };
        const wire = encodeAction({ kind: 'attack', cards: [mine] });
        const optimistic = optimisticBoard(held, wire)!;
        assert.ok(optimistic && tableCards(optimistic).some((c) => getCardKey(c) === getCardKey(mine)), 'the card stands on the optimistic board');
        const key = animEventKey('attack_pass', mine, 'hand', 'table', optimistic.mySeat);

        // The server applies it and pushes Rival its confirmation.
        assert.equal(table.act('u-rival', wire, null, 0), L.TABLE_APPLIED, 'the attack applies');
        const products = table.commit('g-anim', 6, 0);
        assert.ok(typeof products !== 'number');
        const push = table.push('g-anim', 1);
        assert.ok(push instanceof Uint8Array);
        const seq = readPushSequence(push, { id: 'g-anim', name: '', players: seats.map((s) => ({ player_id: s.id, name: s.name, is_ai: false })) })!;
        const confirming = seq.events.find((e) => e.type === 'attack_pass')!;
        assert.equal(confirming.seat, optimistic.mySeat, 'the push names the seat the tap keyed');
        assert.equal(animEventKey(confirming.type, confirming.cards![0], confirming.from_location!, confirming.to_location!, confirming.seat), key,
            'so the confirmation matches the optimistic key and is not animated again');
        assert.deepEqual(staleOptimisticKeysOnTable(pending(key, mine), tableCards(seq.game), seq.events.map((e) => ({ ...e }))), [],
            'and the version gate leaves it for that dedup');
        assert.deepEqual(seq.game.battles, optimistic.battles, 'the confirmed table is the optimistic one');
    });
}

if (!process.env.VALIDATION_ONLY) registerOptimisticValidation();
