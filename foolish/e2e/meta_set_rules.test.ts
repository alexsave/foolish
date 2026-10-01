// The lobby's rules over the real server (docs/PODKIDNOY.md): the `set-rules`
// meta action, through the real handler (meta_actions.ts) and the real
// commit_table, read back the way every client reads a table - the envelopes in
// player_views and spectator_views, and the realtime pushes - through the
// kernel's client slot (sdk/ts/table/client_table.ts).
//
// What is pinned here is the server half only: who may set them (any seated
// player, not a stranger, not after the deal), that every viewer - the other
// seat and a spectator deciding whether to join - reads them, that the deal
// plays the rules standing at the last Ready, and that the stored row carries
// them (the v4 state blob's PASSING bit). The rule itself is the kernel's
// (c/tests/tests.c test_table_set_rules).

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { clientTable, type TableView } from '../sdk/ts/table/client_table.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { legalMoves, mustReadTable } from './helpers/table_play.ts';
import { runMeta, seedLobby } from './helpers/table_server.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const gid = () => `sr${uuid().slice(0, 6)}`;
const settle = () => new Promise((r) => setImmediate(() => setImmediate(r)));

/** A BYTEA column as the pool reads it ('\x...'), as bytes. */
const bytesOf = (col: unknown): Uint8Array =>
    col instanceof Uint8Array ? col : Uint8Array.from(Buffer.from(String(col).replace(/^\\x/, ''), 'hex'));

/** `userId`'s stored envelope, read the way the web reads it. */
async function playerView(gameId: string, userId: string): Promise<TableView> {
    const { rows } = await pgPool.query('SELECT view FROM player_views WHERE game_id = $1 AND player_id = $2', [gameId, userId]);
    assert.equal(rows.length, 1, `a player_views row for ${userId}`);
    const v = clientTable().adoptEnvelope(bytesOf(rows[0].view));
    assert.ok(v, `the stored envelope reads (${JSON.stringify(clientTable().lastRefusal())})`);
    return v;
}

/** The spectator's stored envelope: what somebody deciding whether to join reads. */
async function spectatorView(gameId: string): Promise<TableView> {
    const { rows } = await pgPool.query('SELECT view FROM spectator_views WHERE game_id = $1', [gameId]);
    assert.equal(rows.length, 1, 'a spectator_views row');
    const v = clientTable().adoptEnvelope(bytesOf(rows[0].view));
    assert.ok(v, `the stored spectator envelope reads (${JSON.stringify(clientTable().lastRefusal())})`);
    return v;
}

/** The last push on `topic` since `from`, read the way the web reads one, decoded with no kept identity. */
function lastPush(topic: string, from: number): TableView {
    const sends = broadcastLog.slice(from).filter((b) => b.channel === topic && b.event === 'animation_events');
    assert.ok(sends.length > 0, `a push on ${topic}`);
    const p = sends[sends.length - 1].payload;
    assert.equal(p.t, 'as3');
    const read = clientTable().readPush(Uint8Array.from(Buffer.from(p.b, 'base64')), { as3: true, identity: 'none' });
    assert.ok(read, `the push reads (${JSON.stringify(clientTable().lastRefusal())})`);
    return read.final;
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(async () => { await pgPool.end(); });

test('set-rules: a seated player sets podkidnoy, every viewer reads it, and the deal plays it', async () => {
    const a = uuid(), b = uuid(), g = gid();
    await seedLobby(g, [{ id: a, name: 'Ana', ready: false }, { id: b, name: 'Bo', ready: false }]);
    const v0 = (await mustReadTable(g)).version;

    // A new table is the classic game, and asking for it again changes nothing.
    const same = clientTable().adoptEnvelope((await runMeta(g, a, { type: 'set-rules', passing: true })).body as Uint8Array);
    assert.equal(same?.passing, true, 'a new table reads passing');
    assert.equal((await mustReadTable(g)).version, v0, 'setting the rules the table already has commits nothing');

    const from = broadcastLog.length;
    const res = await runMeta(g, a, { type: 'set-rules', passing: false });
    await settle();
    const mine = clientTable().adoptEnvelope(res.body as Uint8Array);
    assert.ok(mine, 'the setter is answered with its envelope');
    assert.equal(mine.passing, false, "the setter's envelope reads podkidnoy");
    const t1 = await mustReadTable(g);
    assert.equal(t1.version, v0 + 1, 'the change commits');
    assert.equal(t1.state[0], L.TABLE_STATE_FORMAT_V4, 'the stored row is v4');
    assert.equal(t1.state[1] & L.TABLE_STATE_FLAG_PASSING, 0, "and its flag byte's PASSING is clear");

    const bv = await playerView(g, b);
    assert.equal(bv.passing, false, "the other seat's stored envelope reads podkidnoy");
    assert.equal(clientTable().rules(bv).canSetRules, true, 'and that seat may change it');
    const sv = await spectatorView(g);
    assert.equal(sv.passing, false, 'a spectator deciding whether to join reads podkidnoy');
    assert.equal(clientTable().rules(sv).canSetRules, false, 'and may not change it');
    assert.equal(lastPush(`gu-${g}-${b}`, from).passing, false, "the other seat's push carries podkidnoy");
    assert.equal(lastPush(`game-${g}`, from).passing, false, "and so does the spectators' push");

    // Both ready - the changer included - and the deal is podkidnoy.
    await runMeta(g, a, { type: 'start' });
    await runMeta(g, b, { type: 'start' });
    const dealt = await mustReadTable(g);
    assert.equal(dealt.status, L.GAME_STATUS_PLAYING, 'the last Ready deals');
    assert.equal(dealt.state[0], L.TABLE_STATE_FORMAT_V4);
    assert.equal(dealt.state[1] & L.TABLE_STATE_FLAG_PASSING, 0, "the dealt row's PASSING is clear: the game is podkidnoy");
    assert.ok(legalMoves(dealt).every((m) => m.kind !== 'pass'), 'no seat of the dealt game is offered a transfer');
    const table = fixtureTable();
    assert.equal(table.load(dealt.state, dealt.roster), L.TABLE_OK, 'the dealt row loads');
    assert.equal((await playerView(g, b)).passing, false, "the other seat's dealt envelope reads podkidnoy");
    assert.equal(clientTable().rules(await playerView(g, b)).canSetRules, false, 'and nobody may change it now');

    await assert.rejects(runMeta(g, b, { type: 'set-rules', passing: true }), /not in its lobby/i, 'set-rules after the deal is refused');
    assert.equal((await mustReadTable(g)).version, dealt.version, 'and commits nothing');
});

test('set-rules: a stranger and a malformed body are refused, and commit nothing', async () => {
    const a = uuid(), b = uuid(), stranger = uuid(), g = gid();
    await seedLobby(g, [{ id: a, name: 'Ana', ready: false }, { id: b, name: 'Bo', ready: false }]);
    const v0 = (await mustReadTable(g)).version;
    await assert.rejects(runMeta(g, stranger, { type: 'set-rules', passing: false }), /not in game/i,
        'somebody not at the table may not set the rules');
    for (const passing of [0, 1, 'false', null, undefined]) {
        await assert.rejects(runMeta(g, a, { type: 'set-rules', passing }), Error, `passing: ${JSON.stringify(passing)} is malformed`);
    }
    const t = await mustReadTable(g);
    assert.equal(t.version, v0, 'nothing committed');
    assert.ok(t.state[1] & L.TABLE_STATE_FLAG_PASSING, 'the table is still the passing game');
});

test('set-rules: passing again before the last Ready deals the passing game', async () => {
    const a = uuid(), b = uuid(), g = gid();
    await seedLobby(g, [{ id: a, name: 'Ana', ready: false }, { id: b, name: 'Bo', ready: false }]);
    await runMeta(g, b, { type: 'set-rules', passing: false });
    await runMeta(g, a, { type: 'start' });
    await runMeta(g, a, { type: 'set-rules', passing: true });
    assert.equal((await playerView(g, b)).passing, true, 'the other seat reads passing again');
    await runMeta(g, b, { type: 'start' });
    const dealt = await mustReadTable(g);
    assert.equal(dealt.status, L.GAME_STATUS_PLAYING, 'the last Ready deals');
    assert.ok(dealt.state[1] & L.TABLE_STATE_FLAG_PASSING, 'the dealt game is the passing game');
});
