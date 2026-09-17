// wrap400's RESPONSE WIRE - the generic tail every `meta` request and the
// `action` bump nudge come back through.
//
// That tail used to be `new Response(JSON.stringify(personalViewOf(...)))`: a
// whole personalized game re-serialized as JSON. It returns the PACKED envelope
// - the same bytes `create` returns, `player_views.view` stores and a client
// fetch decodes - and since Phase 4b those bytes are the C Table's
// (table_envelope), handed through wrap400 untouched.
//
// Suites that reach the server through handleMetaAction run BELOW wrap400, so a
// JSON body could come back with every one of them green. This test goes through
// the real handlers wrap400 returns (meta/index.ts, action/index.ts) over a real
// Request with a real signed token (e2e/helpers/edge.ts), on kernel-owned rows,
// and holds each response to the kernel's own envelope for the caller's seat at
// the row's version, byte for byte. That comparison replaces the retired
// personalViewOf oracle.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import { postJson, settle, tokenFor, type EdgeResponse } from './helpers/edge.ts';
import { fixture, fixtureTable, PLAYING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { mustReadTable, type TableState } from './helpers/table_play.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { GAME_RESP_FORMAT } from '../sdk/ts/wire/view.ts';
import { decodeEnvelope } from './helpers/client_read.ts';
import type { PersonalGame } from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// ---- fixtures ---------------------------------------------------------------

const HUMAN_A = uuid(), HUMAN_B = uuid(), BOT = uuid(), OUTSIDER = uuid();
const SEATS = [{ id: HUMAN_A, name: 'A' }, { id: HUMAN_B, name: 'B' }, { id: BOT, name: 'Bot', brain: 'random' }];

/** A lobby nobody has readied in: A's `start` readies A and deals nothing. */
async function lobby(): Promise<string> {
    const g = `w${uuid().slice(0, 5)}`;
    await seedTable(g, fixture().title('wrap400 wire').seats(SEATS).build(), { version: 42 });
    return g;
}

/** A dealt game at version 42: a `start` on it is moot and commits nothing. */
async function dealt(): Promise<string> {
    const g = `w${uuid().slice(0, 5)}`;
    await seedTable(g, fixture().title('wrap400 wire').seats(SEATS)
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '6h 7h 8h 9h Th Jh').hand(1, '6s 7s 8s 9s Ts Js').hand(2, '6d 7d 8d 9d Td Jd')
        .deck('6c 7c 8c 9c Tc Jc Qc Kc').trump('Ac')
        .build(), { version: 42 });
    return g;
}

/** The kernel's envelope for `userId` on the stored row `t` (the fixtures' table holds it after mustReadTable). */
function kernelEnvelope(t: TableState, userId: string): Uint8Array {
    const table = fixtureTable();
    const env = table.envelope(t.gameId, table.seatOf(userId), t.version);
    if (typeof env === 'number') throw new Error(`fixture: no envelope (${env})`);
    return env;
}

async function tok(userId: string, name: string): Promise<string> {
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1) ON CONFLICT DO NOTHING', [userId]);
    return tokenFor(userId, name);
}

/** The shape every wrap400 success must have: octet-stream bytes that lead with the envelope format and are not JSON. */
function assertPackedBody(res: EdgeResponse, tag: string): void {
    assert.equal(res.status, 200, `${tag}: 200 (${JSON.stringify(res.json)})`);
    assert.equal(res.type, 'application/octet-stream', `${tag}: octet-stream`);
    assert.equal(res.bytes[0], GAME_RESP_FORMAT, `${tag}: leads with the packed envelope format byte`);
    // The body is BYTES, not a JSON document. This is the assertion that
    // fails the moment the tail goes back to JSON.stringify.
    assert.throws(() => JSON.parse(new TextDecoder().decode(res.bytes)), `${tag}: body must not parse as JSON`);
}

before(async () => { await applySchema(); });
// A bump may have woken the bot loop: let it finish before the next test truncates under it.
beforeEach(async () => { await settle(); await resetDb(); __clearGameCache(); });
after(async () => { await settle(); });

// ---- the wire ---------------------------------------------------------------

test('wrap400 answers with the PACKED game envelope, never a JSON game', async () => {
    for (const [tag, make, version] of [['lobby', lobby, 43], ['dealt', dealt, 42]] as const) {
        const gameId = await make();
        const res = await postJson('meta', await tok(HUMAN_A, 'A'), { type: 'start', game_id: gameId });
        assertPackedBody(res, tag);

        const decoded = decodeEnvelope(res.bytes);
        assert.ok(decoded, `${tag}: the web client reads it`);
        assert.equal(decoded!.version, version, `${tag}: the row's version rides the envelope (a lobby ready commits, a moot start does not)`);
        assert.equal(decoded!.seat, 0, `${tag}: the caller's seat`);

        // And it is exactly the kernel's envelope for this caller at that version.
        const t = await mustReadTable(gameId);
        assert.equal(t.version, version, `${tag}: the stored row agrees`);
        assert.deepEqual(res.bytes, kernelEnvelope(t, HUMAN_A), `${tag}: byte for byte the kernel's envelope for seat 0`);
    }
});

test('wrap400: a caller with no seat gets the spectator envelope', async () => {
    const gameId = await dealt();
    const t = await mustReadTable(gameId);
    const expected = kernelEnvelope(t, OUTSIDER);
    // The one JSON request an outsider may send: the bump nudge.
    const res = await postJson('action', await tok(OUTSIDER, 'Nobody'), { type: 'bump', game_id: gameId });
    assertPackedBody(res, 'spectator');

    const decoded = decodeEnvelope(res.bytes);
    assert.ok(decoded, 'spectator envelope decodes');
    assert.equal(decoded!.seat, -1, 'seat -1');
    assert.equal(decoded!.version, 42);
    assert.equal((decoded!.game as PersonalGame).self, undefined, 'a spectator gets no self');
    assert.deepEqual(res.bytes, expected, 'byte for byte the kernel\'s spectator envelope');
    // Masking is still the kernel's: real counts, and not the bytes a seated player gets.
    assert.deepEqual(decoded!.game.players.map((p) => p.hand_length), [6, 6, 6], 'hand counts are real');
    assert.notDeepEqual(res.bytes, kernelEnvelope(t, HUMAN_A), 'not seat 0\'s envelope');
});

test('wrap400: seat 1 sees its OWN hand', async () => {
    const gameId = await dealt();
    const res = await postJson('meta', await tok(HUMAN_B, 'B'), { type: 'start', game_id: gameId });
    assertPackedBody(res, 'seat 1');
    const decoded = decodeEnvelope(res.bytes);
    assert.ok(decoded);
    assert.equal(decoded!.seat, 1);
    const self = (decoded!.game as PersonalGame).self;
    assert.ok(self, 'seat 1 gets a self');
    const t = await mustReadTable(gameId);
    assert.deepEqual(self!.hand, t.seats[1].hand, 'the viewer\'s own hand is real');
    assert.equal(self!.player_id, HUMAN_B);
    assert.deepEqual(res.bytes, kernelEnvelope(t, HUMAN_B), 'byte for byte the kernel\'s envelope for seat 1');
});
