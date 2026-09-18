// The lobby edits are guarded to the WAITING lobby (and the lobby is capped at
// 8 seats); continue is guarded to a finished game. The e2e meta suite drives
// real games that are always WAITING when these run, so the "wrong status" /
// "lobby full" refusals go unexercised there. The judgement is the C Table's
// (table_join / table_reseat / table_retitle / table_continue); this drives the
// REAL handler (meta_actions.ts) on kernel-owned rows and pins the refusal each
// one surfaces (table_io.ts refusalMessage) and that a refusal commits nothing.
//
// Deleted with the TS Game: the winner / fool announcement `continue` used to
// return as a message event. A push carries no messages any more (plan Q7); who
// finished where rides the ending move's events, and the reset itself is pinned
// below and in e2e/meta.test.ts 'meta:continue - resets a finished game back to
// the lobby'.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import { fixture, GAME_OVER, IDLE, IN, OUT, PLAYING, WAITING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { mustReadTable } from './helpers/table_play.ts';
import { runMeta } from './helpers/table_server.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const gid = () => `g${uuid().slice(0, 5)}`;

/** Two humans mid-game: h1 attacks, h2 defends. */
async function playing(h1: string, h2: string): Promise<string> {
    const g = gid();
    await seedTable(g, fixture().seats([{ id: h1, name: 'h1' }, { id: h2, name: 'h2' }])
        .status(PLAYING).hand(0, '6h 7h 8h').hand(1, '6s 7s 8s').deck('9d Td').trump('Ac').build());
    return g;
}

/** The refusal, and that the row did not move. */
async function refused(gameId: string, userId: string, body: Record<string, unknown>, why: RegExp, what: string): Promise<void> {
    const v0 = (await mustReadTable(gameId)).version;
    await assert.rejects(runMeta(gameId, userId, body), why, what);
    assert.equal((await mustReadTable(gameId)).version, v0, `${what}: nothing committed`);
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(async () => { await pgPool.end(); });

test('join is rejected outside the WAITING lobby and when the lobby is full', async () => {
    const h1 = uuid(), h2 = uuid(), newbie = uuid();
    await refused(await playing(h1, h2), newbie, { type: 'join' }, /not in its lobby/i, 'join blocked on a live game');

    const full = gid();
    await seedTable(full, fixture().seats(Array.from({ length: 8 }, (_, i) => ({ id: uuid(), name: `h${i}` }))).build());
    await refused(full, newbie, { type: 'join' }, /full \(max 8/i, 'join blocked at 8 players');
});

test('rearrange-players and update-name are lobby-only', async () => {
    const h1 = uuid(), h2 = uuid();
    const over = gid();
    await seedTable(over, fixture().seats([{ id: h1, name: 'h1' }, { id: h2, name: 'h2' }])
        .status(GAME_OVER).eliminated(0).discard(36).seatStatus(0, IDLE).seatStatus(1, IDLE).build());
    await refused(over, h1, { type: 'rearrange-players', new_order: [h2, h1] }, /not in its lobby/i, 'rearrange blocked once the game started');

    await refused(await playing(h1, h2), h1, { type: 'update-name', new_name: 'x' }, /not in its lobby/i, 'rename blocked once the game started');
});

test('continue is rejected while the game is still in progress', async () => {
    const h1 = uuid(), h2 = uuid();
    await refused(await playing(h1, h2), h1, { type: 'continue' }, /is not over/i, 'continue blocked mid-game');
});

test('continue resets a finished game to the lobby, clearing the fool\'s hand', async () => {
    const w = uuid(), f = uuid();
    const g = gid();
    // The winner is OUT; the fool is still IN, holding cards.
    await seedTable(g, fixture().seats([{ id: w, name: 'w' }, { id: f, name: 'f' }])
        .status(GAME_OVER).seatStatus(0, OUT).seatStatus(1, IN).hand(1, '6c 7c').eliminated(0).discard(34).powerSuit(1).build());
    const v0 = (await mustReadTable(g)).version;
    await runMeta(g, w, { type: 'continue' });
    const t = await mustReadTable(g);
    assert.equal(t.status, WAITING, 'game returns to the lobby');
    assert.equal(t.statusColumn, 'waiting');
    assert.equal(t.version, v0 + 1, 'and the reset committed');
    assert.ok(t.seats.every((s) => s.hand.length === 0), 'hands cleared on reset');
    assert.deepEqual(t.seats.map((s) => s.status), [IDLE, IDLE], 'both humans back to idle');
    assert.deepEqual(t.eliminated, [], 'the finish order is gone with the session');
});

test('an unknown meta action type is rejected', async () => {
    const h1 = uuid(), h2 = uuid();
    await refused(await playing(h1, h2), h1, { type: 'no-such-action' }, /unknown meta action type/i, 'unknown type');
});
