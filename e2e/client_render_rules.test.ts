/* =============================================================================
 * What a board shows that is a rule of the game is the kernel's to say
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 6a. The web's components used to decide,
 * each in its own file, whether the viewer is offered Good (ActionButtons,
 * KeyboardPlayMode), which seat the sword marks (PlayerRing), where the shield
 * stands (DefenderShield) and what the stock shows while cards fly out of it
 * (DeckAndFlipped). Those are rules, so they are C now (c/src/client_table.h
 * client_view_rules), asked through the client slot about any board a screen
 * holds (sdk/ts/table/client_table.ts rules): the slot's own, or one the host
 * changed - an optimistic move, a board between two animation steps, a replay's
 * board before its deal - written back through the generated TableView writer.
 *
 * The boards are the server's: a fixture composed in the kernel, loaded into the
 * C Table, written by table_envelope for a viewer, and read by the client slot.
 * Where the old TypeScript and the kernel disagree the kernel is right: a seat
 * that is out is not offered Good (handle_good refuses NOT_IN_STATUS), and a
 * view that is not one is refused rather than answered.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fixture, fixtureTable, PLAYING, OUT, type FixtureSeat } from './helpers/table_fixture.ts';
import { clientTable, type TableView } from '../sdk/ts/table/client_table.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';

const seats: FixtureSeat[] = [{ id: 'u-ann', name: 'Ann' }, { id: 'u-bob', name: 'Bob' }, { id: 'u-cat', name: 'Cat' }];

/** The board `b` as `viewer` reads it off its envelope. */
function viewOf(b: { state: Uint8Array; roster: Uint8Array }, viewer: number): TableView {
    const t = fixtureTable();
    assert.equal(t.load(b.state, b.roster), L.TABLE_OK);
    const env = t.envelope('g-rules', viewer, 3);
    assert.ok(env instanceof Uint8Array, `envelope: ${env}`);
    const v = clientTable().adoptEnvelope(env);
    assert.ok(v, 'the envelope reads');
    return v;
}

// Ann led 7h (Bob covered with 9h); Cat has said nothing; a stock of 12 under the Kc.
const bout = (opts: { good?: number[]; table?: string[]; out?: boolean } = {}) => {
    const f = fixture().seats(seats).status(PLAYING).trump('Kc').deck('6s 7s 8s 9s Ts Js Qs Ks As 6d 7d 8d')
        .hand(0, '6h Ah').hand(1, 'Qh Td').hand(2, opts.out ? '' : 'Jd Qd Kd')
        .table(...(opts.table ?? ['7h/9h'])).attacker(0).defender(1).good(...(opts.good ?? []));
    if (opts.out) f.seatStatus(2, OUT).eliminated(2);
    return f.build();
};

test('Good: offered to a seat that may say it over a covered table, and to no one else', () => {
    const t = clientTable();
    assert.equal(t.rules(viewOf(bout(), 2)).canSayGood, true, 'an attacker who has not said it');
    assert.equal(t.rules(viewOf(bout(), 0)).canSayGood, true, 'the lead, once the table is covered');
    assert.equal(t.rules(viewOf(bout({ good: [2] }), 2)).canSayGood, false, 'not once said');
    assert.equal(t.rules(viewOf(bout({ good: [0] }), 2)).canSayGood, true, 'another seat\'s Good is not this one\'s');
    assert.equal(t.rules(viewOf(bout(), 1)).canSayGood, false, 'never the defender');
    assert.equal(t.rules(viewOf(bout({ table: ['7h/9h', '8h'] }), 2)).canSayGood, false, 'not while an attack is uncovered');
    assert.equal(t.rules(viewOf(bout(), -1)).canSayGood, false, 'a spectator is offered nothing');
    const empty = viewOf(bout(), 2);
    assert.equal(t.rules({ ...empty, battles: [] }).canSayGood, false, 'not over an empty table');
});

test('Good: a seat that is out, or a finished game, is offered nothing (handle_good refuses both)', () => {
    const t = clientTable();
    const out = viewOf(bout({ out: true }), 2);
    assert.equal(out.seats[2].status, V.PLAYER_STATUS_OUT, 'the viewer is out');
    assert.equal(t.rules(out).canSayGood, false, 'an out seat over a covered table');
    const over = viewOf(bout(), 2);
    assert.equal(t.rules({ ...over, status: V.GAME_STATUS_GAME_OVER }).canSayGood, false, 'a finished game');
});

test('the sword marks the next lead on an empty dealt table; the shield the defender once dealt', () => {
    const t = clientTable();
    const v = viewOf(bout(), 0);
    assert.deepEqual([t.rules(v).firstAttackerBadge, t.rules(v).defenderBadge], [-1, 1], 'mid-bout: the shield only');
    const cleared = { ...v, battles: [] };
    assert.deepEqual([t.rules(cleared).firstAttackerBadge, t.rules(cleared).defenderBadge], [0, 1], 'an empty table: both');
    const midDeal = { ...cleared, hasFlipped: false, flipped: { suit: V.CARD_NONE_SUIT, value: V.CARD_NONE_VALUE } };
    assert.deepEqual([t.rules(midDeal).firstAttackerBadge, t.rules(midDeal).defenderBadge], [-1, -1], 'a stock before its trump: neither');
    const late = { ...midDeal, deckCount: 0 };
    assert.deepEqual([t.rules(late).firstAttackerBadge, t.rules(late).defenderBadge], [0, 1], 'stock and trump drawn out: both');
});

test('the stock: cards in flight leave the pile, and those bound for the trump slot stay on its count', () => {
    const t = clientTable();
    const v = viewOf(bout(), 0);
    const pick = (fromDeck: number, toFlipped: number, view: TableView = v) => {
        const r = t.rules(view, fromDeck, toFlipped);
        return [r.deckPile, r.deckBadge, r.showDeckPile, r.showFlippedSlot, r.showTrumpIcon];
    };
    assert.deepEqual(pick(0, 0), [12, 13, true, true, false], 'the stock under its trump');
    assert.deepEqual(pick(4, 0), [8, 9, true, true, false], 'four cards on their way out');
    const dealing = { ...v, deckCount: 7, hasFlipped: false };
    assert.deepEqual(pick(7, 1, dealing), [0, 1, false, true, false], 'the last card flying to the trump slot');
    assert.deepEqual(pick(9, 0, dealing), [0, 0, false, false, true], 'more in flight than the stock holds shows no stock');
    assert.deepEqual(pick(0, 0, { ...v, deckCount: 0, hasFlipped: false }), [0, 0, false, false, true], 'stock and trump gone: the power suit');
});

test('a view that is not one is refused, never answered', () => {
    const t = clientTable();
    const v = viewOf(bout(), 0);
    assert.throws(() => t.rules({ ...v, mySeat: 3 }), /refused/, 'a viewer the board does not seat');
    assert.throws(() => t.rules(v, -1, 0), /refused/, 'a negative flight');
    assert.throws(() => t.rules({ ...v, seats: Array.from({ length: 9 }, () => v.seats[0]) }), RangeError, 'more seats than a table has');
});

test('the answer is kept per view object and flight, so a render asks the kernel once', () => {
    const t = clientTable();
    const v = viewOf(bout(), 2);
    assert.equal(t.rules(v), t.rules(v), 'the same object for the same view');
    assert.notEqual(t.rules(v, 1, 0), t.rules(v), 'a different flight is a different question');
    const said = { ...v, goodMask: 1 << 2 };
    assert.equal(t.rules(said).canSayGood, false, 'a changed board is asked afresh');
    assert.equal(t.rules(v).canSayGood, true, 'and the first answer is untouched');
});
