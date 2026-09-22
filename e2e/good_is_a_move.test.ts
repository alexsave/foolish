/* =============================================================================
 * A bot says good, and a push goes out carrying it
 * =============================================================================
 * The owner, on watching a bot check in: "if a bot just says 'good', we don't
 * send anything back to the client, right? should we? Because right now all the
 * goods are just getting lumped in with whatever 'animation causing move'
 * follows. Well guess what - goods are now animation-causing moves."
 *
 * He was right about the cause too: "it's just a Typescript-ism that used to
 * make it never sent." A `good` is the one action the kernel has no CARD for, so
 * it emits no animation event - there is no ANIM_EVT_GOOD in c/src/anim_plan.h,
 * and all twelve event types there are card motion. The adapter then gated every
 * broadcast on `products.nEvents > 0`, so a good that did not also close the
 * bout was never broadcast at all: the check reached a screen only folded into
 * whatever moved next. c/src/bot_drive.c's classify() had been written to mirror
 * that same TypeScript test, so the kernel was mirroring a mirror.
 *
 * WHAT THIS FILE HOLDS, on a board built so that a bot's ONLY legal move is a
 * good that leaves the bout open:
 *
 *   - the kernel reports the move it has no event for: TableCommit.goods_changed
 *     is true while n_events is 0, which is the whole of the fix's wire;
 *   - the cycle is PACED like a move (bot_drive.c classify), not bundled away as
 *     a silent passive worth no beat at all;
 *   - the push exists, and a CLIENT reads it: a push of no events is a legal
 *     push, and the board it carries has the new mask on it. This is the part a
 *     product flag alone would not prove - if `readPush` refused an empty stream
 *     the feature would be a server that shouts into a closed door;
 *   - the web's opening role beat turns that mask into a role change, which is
 *     what makes the empty stream a MOVE on screen rather than a state update;
 *   - and the negative controls: a cycle that moves no goods says so, and a good
 *     that DOES close the bout nets the mask back to where it started (set by
 *     handle_good, cleared by the round transition, one operation) and is
 *     broadcast by its sweep's events, as it always was.
 *
 * The last test is the TypeScript-ism itself. Both server gates and the browser
 * harness that mirrors them are read as source and held to asking goods_changed,
 * because "every broadcast site asks" is the property, and a fourth site added
 * later with the old test would be the same bug wearing a new file name.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import { animRolesGoodsOpening } from '../sdk/ts/wasm/bots.ts';
import { pushToSequence } from '../src/state/pushSequence.ts';
import { fixture, fixtureTable, PLAYING, type TableFixture } from './helpers/table_fixture.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const GID = 'goodmove';
const NOW = 1_726_600_000_000;
const SEED = '00ff';

/**
 * Seat 0 is the human and the first attacker, seat 1 the bot defending, seat 2 a
 * bot attacker holding a card that matches nothing on the table. The one battle
 * is already covered, so seat 2 can neither throw in nor cover: `good` is its
 * whole menu. Seat 0 has not said good, so that good closes nothing - which is
 * exactly the case that used to be broadcast to nobody.
 */
const openBout = (): TableFixture => fixture()
    .seats([{ id: 'a', name: 'Ann' }, { id: 'b', name: 'Bob', brain: 'cordite' }, { id: 'c', name: 'Cid', brain: 'cordite' }])
    .status(PLAYING)
    .attacker(0).defender(1)
    .hand(0, '6h 7h').hand(1, 'Qs').hand(2, 'Kd')
    .table('7c/8c')
    .deck('Ts Jd').trump('As')
    .goodTimestamp(true)
    .build();

/** One bot cycle on a fixture, committed, exactly as bot_actions.ts runCycle makes one. */
function cycle(f: TableFixture) {
    const t = fixtureTable();
    assert.equal(t.load(f.state, f.roster), L.TABLE_OK, 'the row loads');
    assert.equal(t.setDealSeed(SEED), L.TABLE_OK, 'the deal seed is taken');
    const drive = t.botDrive(null);
    assert.ok(typeof drive !== 'number', `the drive runs (${drive})`);
    const delay = t.cycleDelayMs();
    const products = t.commit(GID, 2, NOW);
    assert.ok(typeof products !== 'number', `commit products (${products})`);
    return { table: t, drive, delay, products };
}

test('a bot\'s good is a commit the kernel reports even though it carries no event', () => {
    const { drive, products } = cycle(openBout());

    assert.equal(drive.n, 1, 'one action was driven');
    assert.deepEqual(drive.seats, [2], 'by the seat whose only move is a good');
    assert.equal(drive.stop, L.BOT_STOP_EVENTS, 'and the cycle ENDED on it - a good is not bundled away any more');

    assert.equal(products.nEvents, 0, 'a good flies no card, so the stream is empty');
    assert.equal(products.goodsChanged, true, 'and goods_changed is the only thing that says a push must go out');
    assert.equal(products.status, L.GAME_STATUS_PLAYING, 'the bout is still open: nothing else happened');
});

test('the cycle is paced like a move, not like a silent passive', () => {
    const { delay } = cycle(openBout());
    // bot_pacing_ms: a visible move with a human watching is 3000ms, a bundled
    // passive is 0. The number is the kernel's; what this pins is WHICH class a
    // good is priced in, which is the classify() half of the owner's rule.
    assert.ok(delay > 0, `a good earns a beat of its own (delay was ${delay}ms)`);
    assert.equal(delay, 3000, 'the same beat any other visible move gets with a human at the table');
});

test('the push goes out, a client reads it, and the board it carries is the whole move', () => {
    const { table, products } = cycle(openBout());

    const bytes = table.push(GID, 0);
    assert.ok(bytes instanceof Uint8Array, `the human seat's push is built (${bytes})`);

    const read = clientTable().readPush(bytes as Uint8Array, { as3: true, gameId: GID, version: 2 });
    assert.ok(read, `a push of no events is still a push a client reads (${JSON.stringify(clientTable().lastRefusal())})`);
    assert.equal(read.steps.length, 0, 'and it has no steps, because there is no card to fly');

    const seq = pushToSequence(read);
    assert.equal(seq.events.length, 0, 'the sequence is empty');
    assert.equal(seq.viewerSeat, 0, 'and it is the human\'s');
    assert.equal(seq.game.goodMask, 1 << 2, 'the trailer board carries the good: the mask IS the move');

    // The mask before this push had no goods on it at all, so every bit in it is
    // an ADDED good - which is the one kind that leads a stream (anim_goods_opening).
    const before = { defender: 1, firstAttacker: 0, goodMask: 0 };
    const opening = animRolesGoodsOpening(before, seq.game.goodMask);
    assert.ok(opening, 'the kernel turns that mask into a role change: on screen this is a move, not a state update');
    assert.equal(opening.goodMask, 1 << 2, 'the badge that turns is the seat that said good');

    assert.equal(products.nEvents, 0, 'and none of this needed a single event');
});

test('a cycle that moves no goods says so, and a good that closes the bout is an ordinary push', () => {
    // NOTHING GOOD HAPPENED: the defender must cover, which is a card and an
    // event, and no mask moves. goods_changed has to be false or the flag would
    // be a second name for "something happened" and gate nothing.
    const covering = fixture()
        .seats([{ id: 'a', name: 'Ann' }, { id: 'b', name: 'Bob', brain: 'cordite' }])
        .status(PLAYING)
        .attacker(0).defender(1)
        .hand(0, '6h').hand(1, 'Kc')
        .table('7c')
        .deck('Ts Jd').trump('As')
        .build();
    const cover = cycle(covering);
    assert.ok(cover.products.nEvents > 0, 'the cover flies a card');
    assert.equal(cover.products.goodsChanged, false, 'and moves no goods');

    // THE OTHER HALF OF THE OLD RULE, and the one place goods_changed says NO to
    // a good. A good that closes the bout is set by handle_good and then cleared
    // by the round transition handle_good runs itself, both inside ONE operation,
    // so the mask this flag compares is the same at both ends of it and the flag
    // is false. That is correct and it is why the flag is a mask COMPARISON
    // rather than a "somebody said good" bit: what a push has to carry is the
    // difference a viewer would see, and there is none - the badge never wore
    // that check. The sweep speaks for this operation, as it always did.
    const closing = fixture()
        .seats([{ id: 'a', name: 'Ann' }, { id: 'b', name: 'Bob', brain: 'cordite' }])
        .status(PLAYING)
        .attacker(1).defender(0)
        .hand(0, '6h').hand(1, 'Qs')
        .table('7c/8c')
        .deck('Ts Jd').trump('As')
        .goodTimestamp(true)
        .build();
    const closed = cycle(closing);
    assert.equal(closed.drive.n, 1, 'the bot acted');
    assert.ok(closed.products.nEvents > 0, 'a good that empties the table sweeps it, and a sweep is events');
    assert.equal(closed.products.closedRound, true, 'the bout closed');
    assert.equal(closed.products.goodsChanged, false, 'and the mask ends where it started, so the flag says no');
    // Which is the point of the gate being an OR: this push goes out on its
    // events, that one on its goods, and no broadcast site has to know which.
    assert.ok(closed.products.nEvents > 0 || closed.products.goodsChanged, 'it is broadcast either way');
});

test('every broadcast site asks goods_changed, not nEvents alone', () => {
    const sites = [
        'server/impls/supabase/functions/_shared/adapter/table_io.ts',
        'server/impls/supabase/functions/_shared/adapter/bot_actions.ts',
        // The browser harness mirrors the adapter on purpose, so a stale mirror
        // here would hide the fix from every browser run.
        'e2e/fake_supabase.mts',
    ];
    for (const site of sites) {
        const src = readFileSync(new URL(`../${site}`, import.meta.url), 'utf8');
        const gates = [...src.matchAll(/^\s*if \((?:p|products)\.nEvents[^\n]*$/gm)].map((m) => m[0].trim());
        assert.ok(gates.length > 0, `${site}: no push gate found - has it moved? this test must move with it`);
        for (const gate of gates) {
            assert.match(gate, /goodsChanged/, `${site}: a broadcast gated on nEvents alone drops every good: ${gate}`);
        }
    }
});
