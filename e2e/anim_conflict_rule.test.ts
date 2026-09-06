/* =============================================================================
 * ONE CONFLICT RULE, ASKED FOUR TIMES
 * =============================================================================
 * "Does the arriving broadcast account for this card being where my optimistic
 * move put it?" AnimationContext.resolveOptimisticConflicts asks that four
 * times - about a pending attack/cover, about a pass, about my attacks after a
 * pass moved the shield, and about a pickup - and routed only the FIRST through
 * the kernel. The other three re-derived it in TypeScript.
 *
 * c/src/anim_plan.h says why that is not a stylistic point: the rule has a
 * PRECEDENCE ("CLEAR is tested BEFORE the standing sets, because a card the
 * incoming stream moves may also stand on its opening table"), a pool rule and
 * a masked-back rule. A re-derivation that checks only capacity has none of
 * them, and the symptom is the flicker the header describes - a card flying
 * home red only for the next board to snap it back.
 *
 * This pins the shim (animConflictVerdicts) against the kernel, including the
 * cases the TypeScript copies got wrong.
 *
 * Pure kernel/wasm test - needs no Postgres.
 *
 * MUTATION-CHECKED (2026-09-06), each applied, run, and reverted:
 *   the wasm wrapper writes verdicts into g_io as it reads the motions, instead
 *   of reading them all out first
 *       -> NOTHING fails, and the copy-out stays anyway. Verdict i is written at
 *          byte i while motion i is read from byte 4 + 3i, so the write index
 *          can never catch the read pointer for any input this entry accepts.
 *          The copy-out is defensive, not load-bearing; recorded rather than
 *          claimed as covered.
 *   the wrapper passes dest straight through without the masked-back mapping
 *   (ANIM_TABLE_NONE -> ANIM_CARD_NONE)
 *       -> "a masked back is KEPT" fails
 *   server_may_still_accept drops the is_cover exclusion
 *       -> "a cover is never reverted for capacity" fails
 *   server_may_still_accept uses < instead of <= on the capacity comparison
 *       -> "capacity is inclusive: exactly-fits is accepted" fails
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    animConflictVerdicts, animEventTypeCode, ANIM_DEST, ANIM_TRANSPORT_SERVER,
    __setAnimTransport, AnimConflictMotion, AnimConflictInputs,
} from '../sdk/ts/wasm/bots.ts';
import { Card, Battle } from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });
const B = (attack: Card, defense: Card | null = null): Battle => ({ attack, defense });

__setAnimTransport(ANIM_TRANSPORT_SERVER);

/** Inputs with nothing vouched for: every card reverts unless a rule saves it. */
const bare = (over: Partial<AnimConflictInputs> = {}): AnimConflictInputs => ({
    events: [], openTable: [], myHand: [],
    pendingAttacks: 0, defenderHand: 6, finalUncovered: 0,
    ...over,
});

// A sweep, as the stream states it. tableCleared and the moved set are no longer
// inputs: the kernel derives both from these events (anim_conflict_sweep), which
// is the point of this shape - a test cannot hand the rule a table it says was
// cleared by a stream that never swept it.
const sweep = (...cards: Card[]) => [{ type: animEventTypeCode('pickup'), cards }];
const trash = (...cards: Card[]) => [{ type: animEventTypeCode('cards_to_trash'), cards }];

test('CLEAR beats the standing sets - the precedence the TS copies had no notion of', () => {
    // The card is BOTH moved by the arriving stream (a pickup sweeps it) and
    // standing on the opening table - which is what a pickup's cards do by
    // definition. The rule must say CLEAR, not KEEP and not REVERT: the forward
    // replay animates it off, so a red flight home first is the flicker.
    const card = C(0, 9);
    const v = animConflictVerdicts(
        [{ card, dest: ANIM_DEST.table }],
        bare({ events: sweep(card), openTable: [B(card)] }));
    assert.deepEqual(v, ['clear'], 'a card the incoming stream moves is CLEAR');
});

test('a card standing on the board the broadcast opens on is KEPT', () => {
    const card = C(1, 11);
    assert.deepEqual(
        animConflictVerdicts([{ card, dest: ANIM_DEST.table }], bare({ openTable: [B(card)] })),
        ['keep'], 'standing on the open table');
    // A cover stands too - both sides of a battle are in the set.
    const cover = C(3, 5);
    assert.deepEqual(
        animConflictVerdicts([{ card: cover, dest: ANIM_DEST.table }],
            bare({ openTable: [B(card, cover)] })),
        ['keep'], 'a cover stands on the table it covers');
});

test('a card that landed in MY HAND is judged against my hand, not the table', () => {
    // The pickup branch. The same card standing on the table means nothing for a
    // motion whose destination was my hand.
    const card = C(2, 7);
    assert.deepEqual(
        animConflictVerdicts([{ card, dest: ANIM_DEST.hand }], bare({ myHand: [card] })),
        ['keep'], 'in my hand on the opening board');
    assert.deepEqual(
        animConflictVerdicts([{ card, dest: ANIM_DEST.hand }],
            bare({ openTable: [B(card)], pendingAttacks: 1, defenderHand: 0 })),
        ['revert'], 'standing on the TABLE does not vouch for a hand motion');
});

test('a pool destination is KEPT - no per-card view to fly home from', () => {
    assert.deepEqual(
        animConflictVerdicts([{ card: C(0, 3), dest: ANIM_DEST.pool }],
            bare({ pendingAttacks: 1, defenderHand: 0 })),
        ['keep'], 'conjuring a ghost back out of a pile is its own bug class');
});

test('a masked back is KEPT - it has no identity to conflict on', () => {
    assert.deepEqual(
        animConflictVerdicts([{ card: null, dest: ANIM_DEST.table }],
            bare({ pendingAttacks: 1, defenderHand: 0 })),
        ['keep'], 'a back landed into a badge, not onto a view');
});

test('capacity is inclusive: exactly-fits is accepted', () => {
    // server_may_still_accept: final_uncovered + pending_attacks <= defender_hand.
    const card = C(0, 4);
    const at = (defenderHand: number) => animConflictVerdicts(
        [{ card, dest: ANIM_DEST.table }],
        bare({ pendingAttacks: 2, finalUncovered: 3, defenderHand }))[0];
    assert.equal(at(5), 'keep', '3 + 2 <= 5 still fits, so the card may yet be accepted');
    assert.equal(at(4), 'revert', '3 + 2 > 4 cannot fit');
});

test('a cover is never reverted for capacity - capacity is an attack rule', () => {
    const card = C(3, 12);
    const inputs = bare({ pendingAttacks: 4, finalUncovered: 4, defenderHand: 0 });
    assert.deepEqual(animConflictVerdicts([{ card, dest: ANIM_DEST.table }], inputs),
        ['revert'], 'an attack in that position reverts');
    assert.deepEqual(animConflictVerdicts([{ card, dest: ANIM_DEST.table, isCover: true }], inputs),
        ['keep'], 'the same card as a COVER does not');
});

test('a cleared table refuses everything it did not name', () => {
    // table_cleared short-circuits hope: a sweep took the table away, so a card
    // the sweep did not name never reached it.
    assert.deepEqual(
        animConflictVerdicts([{ card: C(1, 6), dest: ANIM_DEST.table }],
            bare({ events: trash(), defenderHand: 52 })),
        ['revert'], 'capacity cannot rescue a card off a table that is gone');
});

test('CLEAR spares a flight only when the card already stands where the replay starts', () => {
    // The distinction the pickup branch turns on, and the one I got wrong first.
    // CLEAR means "the incoming stream animates this card itself". For a card my
    // move put on the TABLE that is a free pass: the sweep lifts it off the
    // table, which is where it already is. For a card my move put in MY HAND it
    // is not: the sweep carries it from the TABLE, so the web still has to fly it
    // home first. The kernel says CLEAR for both - correctly, since it is a
    // verdict about doom, not about flights - and anim_plan.h leaves the flight
    // to the caller.
    const card = C(0, 8);
    const swept = bare({ events: sweep(card), openTable: [B(card)] });
    assert.deepEqual(animConflictVerdicts([{ card, dest: ANIM_DEST.table }], swept), ['clear'],
        'a card my attack put on the table: the sweep takes it from where it is');
    assert.deepEqual(animConflictVerdicts([{ card, dest: ANIM_DEST.hand }], swept), ['clear'],
        'a card my pickup put in my hand: same verdict, but the web must fly it back first');
});

test('the SWEEP is the kernel\'s to name: only a pickup or a trash clears the table', () => {
    // The rule that used to live in TypeScript as `new Set(['pickup',
    // 'cards_to_trash'])`. A stream of ordinary placements moves nothing and
    // clears nothing, so a card it does not account for is judged on capacity
    // alone - it is NOT doomed by a table that was never swept.
    const card = C(2, 6);
    const fits = { pendingAttacks: 1, finalUncovered: 0, defenderHand: 6 };

    const busy = [
        { type: animEventTypeCode('attack_pass'), cards: [C(0, 3)] },
        { type: animEventTypeCode('cover'), cards: [C(1, 4)] },
        { type: animEventTypeCode('refill'), cards: [C(3, 2)] },
    ];
    assert.deepEqual(
        animConflictVerdicts([{ card, dest: ANIM_DEST.table }], bare({ events: busy, ...fits })),
        ['keep'], 'no sweep in the stream, so capacity still has the last word');

    // The same stream with a trash appended: now the table IS gone, and the
    // card the trash did not name never reached it.
    assert.deepEqual(
        animConflictVerdicts([{ card, dest: ANIM_DEST.table }],
            bare({ events: [...busy, ...trash(C(0, 3))], ...fits })),
        ['revert'], 'a sweep short-circuits hope, even with room to spare');

    // A sweep that names the card itself is CLEAR, not REVERT - and the kernel
    // reads that off the same events.
    assert.deepEqual(
        animConflictVerdicts([{ card, dest: ANIM_DEST.table }],
            bare({ events: [...busy, ...trash(card)], ...fits })),
        ['clear'], 'the sweep carries this very card off');
});

test('verdicts come back per motion, in input order', () => {
    const standing = C(0, 10), swept = C(1, 10), doomed = C(2, 10);
    const v = animConflictVerdicts([
        { card: doomed, dest: ANIM_DEST.table },
        { card: standing, dest: ANIM_DEST.table },
        { card: swept, dest: ANIM_DEST.table },
    ], bare({
        events: sweep(swept), openTable: [B(standing)],
        pendingAttacks: 1, defenderHand: 0,
    }));
    assert.deepEqual(v, ['revert', 'keep', 'clear'], 'one verdict per motion, in order');
});

test('a long motion list is not corrupted by its own output', () => {
    // The wrapper writes verdicts back into the same buffer the motions were
    // read from, so it must read them all out first. 40 motions is well past
    // the point where the output would overwrite unread input.
    const motions: AnimConflictMotion[] = [];
    for (let i = 0; i < 40; i++) motions.push({ card: C(i % 4, (i % 13) + 1), dest: ANIM_DEST.pool });
    const v = animConflictVerdicts(motions, bare());
    assert.equal(v.length, 40, 'every motion answered');
    assert.ok(v.every((x) => x === 'keep'), 'a pool is kept, all 40 of them');
});
