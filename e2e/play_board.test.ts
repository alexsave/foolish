/* =============================================================================
 * WHAT A GESTURE MEANS: the kernel's answer, reached from TypeScript
 * =============================================================================
 * Five web sites used to answer "what does this gesture mean" in slightly
 * different words - DragContext.determineGameAction, ActionButtons'
 * handleCoverClick, canCoverCards, KeyboardInputHandler and KeyboardPlayMode.
 * c/src/legal.h's play_* rules are one answer for all of them, and
 * client_table.h's client_play is the door the web reaches them through.
 *
 * The rules take a PUBLISHED PAIR - the menu the kernel enumerated for a seat,
 * and the table it was enumerated on - and read nothing else. The browser holds
 * no menu, so client_play makes the pair from the board the screen already has
 * (the same masked game client_validate judges on) and never hands a menu byte
 * to the host. Every question below therefore starts from a TableView, which is
 * all a screen ever has.
 *
 * Pure kernel/wasm test - needs no Postgres and no DOM.
 *
 * MUTATION-CHECKED (2026-09-18), each applied, run, and reverted:
 *   client_play spells "no cover" 0xff instead of legal.h's LEGAL_WIRE_NONE
 *       -> "every single-card cover in the menu resolves onto its own battle"
 *          fails. This was a real bug on the parked branch, not a hypothetical:
 *          0xff makes battle_is_uncovered false everywhere, so the board answers
 *          "nothing is coverable" and answers it silently. The constant is
 *          generated now (sdk/ts/gen/view_layout.bots.ts) and the packing is C,
 *          so the TS side of that drift is gone; this holds the C side.
 *   play_best_cover_target picks the leftmost coverable battle (the web's old
 *   rule) instead of the highest
 *       -> "the cover button aims at the highest attack, not the leftmost" fails
 *   client_play lets CLIENT_PLAY_COVER_BUTTON fall through to the open table
 *   when nothing is coverable (drop the best_cover < 0 guard)
 *       -> "the Cover button resolves to nothing when it has nothing to aim at"
 *          fails
 *   client_play reads the gesture's target as given for the Cover button (never
 *   substitutes best_cover)
 *       -> "the Cover button plays the move it aims at" fails
 *   client_view_rules' can_say_good drops its `covered` term
 *       -> "Good agrees between the render rule and the gesture rule" fails
 *   client_play decodes a non-cover's attack bytes as cards (they are padding:
 *   card_to_id of a zeroed Card, which is no card)
 *       -> "an open-table drop passes when a pass is legal..." fails. Found by
 *          reading the menu writer rather than by a symptom, and red before the
 *          guard went in.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import { clientTable, type TableView, type ViewCard as Card } from '../sdk/ts/table/client_table.ts';
import { MemTable, boardFixture, fixtureView, residentMoves, type MemMove } from './helpers/table_mem.ts';
import { dropTarget, gestureCards } from '../src/state/view.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });
const same = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;
const isCard = (c: Card) => !(c.suit === V.CARD_NONE_SUIT && c.value === V.CARD_NONE_VALUE);

const seedOf = (np: number, s: number) =>
    Uint8Array.from({ length: 32 }, (_, i) => (i * 31 + s * 13 + np) & 0xff);

/** One mid-game position: the board a seat is shown, and the menu the kernel enumerated for it. */
interface Position { view: TableView; moves: MemMove[]; seat: number }

/**
 * Walk a seeded game of `np` bot seats, handing every position of every seat
 * still in to `visit`. The kernel deals it and the kernel's own bot cycle moves
 * it on, so every board visited is one a real game reaches.
 */
function walkPositions(np: number, s: number, visit: (p: Position) => void): void {
    const seats = Array.from({ length: np }, (_, i) => ({ id: `p${i}`, name: `P${i}`, brain: 'random' }));
    const t = MemTable.deal(seats, seedOf(np, s), 'pb');
    for (let guard = 0; guard < 300; guard++) {
        const b = t.board();
        if (b.status !== L.GAME_STATUS_PLAYING) return;
        for (let seat = 0; seat < np; seat++) {
            if (b.seats[seat].status !== L.PLAYER_STATUS_IN) continue;
            const view = t.view(seat);
            t.load();
            visit({ view, moves: residentMoves(seat), seat });
        }
        if (t.drive(1).drive.n === 0) return;
    }
}

/** The battle index of an uncovered attack, or -1. */
const battleOf = (view: TableView, attack: Card): number =>
    view.battles.findIndex((b) => !isCard(b.defense) && same(b.attack, attack));

test('every single-card cover in the menu resolves onto its own battle', () => {
    let checked = 0;
    for (let s = 0; s < 4; s++) {
        walkPositions(3, s, ({ view, moves, seat }) => {
            if (seat !== view.defender) return;
            for (const m of moves) {
                if (m.kind !== 'cover' || m.cards.length !== 1) continue;
                const attack = m.attack_cards![0];
                const battle = battleOf(view, attack);
                if (battle < 0) continue;

                const hit = clientTable().play(view, m.cards, battle);
                assert.equal(hit.moveType, V.MOVE_COVER, 'a legal cover drop resolves to a cover');
                assert.ok(same(hit.cards[0], m.cards[0]), 'with the dragged card');
                assert.ok(hit.attackCards.some((a) => same(a, attack)),
                    'onto the battle it was dropped on');
                checked++;
            }
        });
    }
    assert.ok(checked > 40, `enough covers exercised (${checked})`);
});

test('the coverable set is the menu\'s own cover set', () => {
    let checked = 0;
    for (let s = 0; s < 4; s++) {
        walkPositions(3, s, ({ view, moves, seat }) => {
            if (seat !== view.defender || view.battles.length === 0) return;
            for (const card of view.myHand) {
                const kernelSet = clientTable().play(view, [card], V.PLAY_TARGET_HAND).coverable;
                // The same question asked of the menu directly: which uncovered
                // battles does a single-card cover with THIS card name?
                const fromMenu = new Set<number>();
                for (const m of moves) {
                    if (m.kind !== 'cover' || m.cards.length !== 1 || !same(m.cards[0], card)) continue;
                    const battle = battleOf(view, m.attack_cards![0]);
                    if (battle >= 0) fromMenu.add(battle);
                }
                assert.deepEqual([...kernelSet].sort((a, b) => a - b),
                    [...fromMenu].sort((a, b) => a - b), 'the kernel set is the menu set');
                checked++;
            }
        });
    }
    assert.ok(checked > 100, `enough selections exercised (${checked})`);
});

// Two seats, spades (0) trump, seat 0 attacks and seat 1 defends over `table`.
const twoSeat = (defenderHand: Card[], table: { attack: Card; defense: Card | null }[]): TableView =>
    fixtureView(boardFixture({
        hands: [6, defenderHand], table, powerSuit: 0, attacker: 0, defender: 1,
    }), 1);

test('the cover button aims at the highest attack, not the leftmost', () => {
    // The behaviour change this lands. The table holds a low attack first and a
    // higher one second, and the selection beats both. The web used to take the
    // leftmost - "the order the attackers happened to throw in" - and the kernel
    // takes the highest.
    const view = twoSeat([C(1, 12)], [
        { attack: C(1, 5), defense: null },
        { attack: C(1, 10), defense: null },
    ]);
    const p = clientTable().play(view, [C(1, 12)], V.PLAY_TARGET_HAND);
    assert.deepEqual([...p.coverable], [0, 1], 'the queen beats both');
    assert.equal(p.bestCover, 1, 'the button aims at the ten (battle 1), not the five (battle 0)');
});

test('a trump outranks every non-trump when the button chooses', () => {
    // Strength is not rank alone: a low trump on the table outranks a high
    // non-trump, so the button must aim at the trump even though it is second
    // and lower by pip.
    const view = twoSeat([C(0, 9)], [
        { attack: C(1, 12), defense: null },   // queen of a plain suit
        { attack: C(0, 6), defense: null },    // six of trumps
    ]);
    const p = clientTable().play(view, [C(0, 9)], V.PLAY_TARGET_HAND);
    assert.deepEqual([...p.coverable], [0, 1], 'the trump nine beats both');
    assert.equal(p.bestCover, 1, 'the trump six outranks the plain queen');
});

test('the Cover button plays the move it aims at', () => {
    const view = twoSeat([C(1, 12)], [
        { attack: C(1, 5), defense: null },
        { attack: C(1, 10), defense: null },
    ]);
    const p = clientTable().play(view, [C(1, 12)], V.CLIENT_PLAY_COVER_BUTTON);
    assert.equal(p.moveType, V.MOVE_COVER, 'the button resolves to a cover');
    assert.ok(same(p.attackCards[0], C(1, 10)), 'onto the ten it aims at, not the five');
});

test('the Cover button resolves to nothing when it has nothing to aim at', () => {
    // A ten of another plain suit does not beat the ten on the table, so the
    // button has nothing to aim at - but the same selection bounces the bout,
    // and falling through to the open table is exactly what it must not do.
    const view = twoSeat([C(2, 10)], [
        { attack: C(1, 10), defense: null },
    ]);
    const p = clientTable().play(view, [C(2, 10)], V.CLIENT_PLAY_COVER_BUTTON);
    assert.equal(p.bestCover, -1, 'nothing to aim at');
    assert.equal(p.moveType, -1, 'and so no move');
    assert.equal(clientTable().play(view, [C(2, 10)], V.PLAY_TARGET_TABLE).moveType, V.MOVE_PASS,
        'while the open table reads it as a pass');
});

test('a drop back in the hand is a rearrange for both roles', () => {
    // The attacker branch of play_resolve reads only the cards, so without the
    // hand being answered first a resolver told "the hand" hands back a
    // perfectly good attack.
    let checked = 0;
    walkPositions(3, 0, ({ view }) => {
        if (view.myHand.length === 0) return;
        assert.equal(clientTable().play(view, [view.myHand[0]], V.PLAY_TARGET_HAND).moveType, -1,
            'the hand is never a play');
        checked++;
    });
    assert.ok(checked > 20, `enough hand drops exercised (${checked})`);
});

test('Good agrees between the render rule and the gesture rule', () => {
    // client_view_rules decides whether the board SHOWS Good; play_can_say_good
    // decides whether a gesture may make it. They are two computations of one
    // rule and nothing but this holds them together.
    let sawTrue = 0, sawFalse = 0;
    for (let s = 0; s < 3; s++) {
        walkPositions(3, s, ({ view }) => {
            const can = clientTable().play(view, [], V.PLAY_TARGET_HAND).canSayGood;
            const fullyCovered = view.battles.length > 0 && view.battles.every((b) => isCard(b.defense));
            if (!fullyCovered) {
                assert.equal(can, false, 'no good over an uncovered attack, and none on an empty table');
                sawFalse++;
            } else if (can) {
                sawTrue++;
            }
            assert.equal(clientTable().rules(view).canSayGood, can,
                'the board offers Good exactly when the gesture rule allows it');
        });
    }
    assert.ok(sawFalse > 50, `enough not-yet positions (${sawFalse})`);
    assert.ok(sawTrue > 0, `at least one position where good is live (${sawTrue})`);
});

test('an open-table drop passes when a pass is legal, else auto-covers only when unambiguous', () => {
    let passes = 0, covers = 0;
    for (let s = 0; s < 4; s++) {
        walkPositions(3, s, ({ view, moves, seat }) => {
            if (seat !== view.defender || view.battles.length === 0) return;
            for (const card of view.myHand) {
                const hit = clientTable().play(view, [card], V.PLAY_TARGET_TABLE);
                if (hit.moveType < 0) continue;
                if (hit.moveType === V.MOVE_PASS) {
                    assert.ok(moves.some((m) => m.kind === 'pass' && m.cards.length === 1 && same(m.cards[0], card)),
                        'a resolved pass is in the menu');
                    // A move that covers nothing names no attack. The menu wire
                    // pads the attack bytes of a non-cover with card_to_id of a
                    // zeroed Card, which is not a card at all, so the answer has
                    // to spell those the no-card rather than decode them.
                    assert.ok(hit.attackCards.every((a) => !isCard(a)),
                        'and it names no attack it covers');
                    passes++;
                } else {
                    assert.equal(hit.moveType, V.MOVE_COVER, 'the only other open-table play is a cover');
                    // Unambiguous: exactly one menu cover uses this selection.
                    const n = moves.filter((m) => m.kind === 'cover' && m.cards.length === 1
                        && same(m.cards[0], card)).length;
                    assert.equal(n, 1, 'an auto-cover is only offered when it is unambiguous');
                    covers++;
                }
            }
        });
    }
    assert.ok(passes + covers > 20, `enough open-table drops (${passes} pass, ${covers} cover)`);
});

test('a spectator has no gesture', () => {
    const view = fixtureView(boardFixture({
        hands: [6, 6], table: [{ attack: C(1, 10), defense: null }], powerSuit: 0, attacker: 0, defender: 1,
    }), -1);
    assert.throws(() => clientTable().play(view, [C(1, 12)], V.PLAY_TARGET_TABLE),
        /refused/, 'no seat, no menu, no answer');
});

/* ---------------------------------------------------------------------------
 * The browser's half of a gesture: turning a pointer into a target and a
 * selection (src/state/view.ts). It is all that is left in TypeScript of what
 * DragContext.determineGameAction used to decide, so it is held here beside the
 * rules it feeds.
 * ------------------------------------------------------------------------- */

test('a drop names the battle it landed on, the hand, or the open table', () => {
    const view = twoSeat([C(1, 12)], [
        { attack: C(1, 5), defense: null },
        { attack: C(1, 10), defense: null },
    ]);
    assert.equal(dropTarget(view, true, 1), V.PLAY_TARGET_HAND, 'the hand wins over any battle under it');
    assert.equal(dropTarget(view, false, 1), 1, 'a battle names itself');
    assert.equal(dropTarget(view, false, null), V.PLAY_TARGET_TABLE, 'no battle under the pointer is the open table');
    assert.equal(dropTarget(view, false, 2), V.PLAY_TARGET_TABLE,
        'a battle index this board does not hold is the open table, not a drop on a battle that is not there');
    assert.equal(dropTarget(view, false, -1), V.PLAY_TARGET_TABLE, 'and neither is a negative one');
});

test('a gesture carries the selection only when the dragged card is in it', () => {
    const sel = [C(1, 5), C(1, 10)];
    assert.deepEqual(gestureCards(sel, C(1, 5)), sel, 'dragging a selected card plays the whole selection');
    assert.deepEqual(gestureCards(sel, C(2, 7)), [C(2, 7)],
        'dragging an unselected card plays it alone - how one card is played while several are picked');
    assert.deepEqual(gestureCards([], C(2, 7)), [C(2, 7)], 'nothing selected is the dragged card alone');
});
