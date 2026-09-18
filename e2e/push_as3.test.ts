// Phase 5b (docs/C_GAME_SHAPE_MIGRATION.md): the server labels its realtime
// pushes `as3`, and a lobby push that changes who sits where names the new seats
// itself.
//
// The web client reads a push with the table identity it kept for the game from
// the last envelope or roster-carrying push (sdk/ts/table/client_table.ts), and
// takes the as3 flag from the payload's `t` (src/contexts/AnimationContext.tsx).
// Here the client holds the lobby as it was BEFORE a join - one seat - and the
// join's push must seat the joiner under their name from its own bytes, then
// leave that roster kept, so the next push (which carries no roster) still names
// the joiner. Nothing here reads the roster from the database or a JSON extra
// (the `r` extra is gone, Q7).
//
// Driven through the real meta handler against a real Postgres, with the pushes
// recorded where the server's batched realtime POST goes (harness broadcastLog).

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import './helpers/edge.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { mustReadTable } from './helpers/table_play.ts';
import { runMeta, seedLobby } from './helpers/table_server.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { ClientTable, type ClientExports } from '../sdk/ts/table/client_table.ts';
import { __clientKernelExports } from '../sdk/ts/wasm/bots.ts';
import { base64ToBytes } from '../sdk/ts/wire/bytes.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// The broadcast is fire-and-forget; drain the queue so broadcastLog is settled.
const settle = () => new Promise((r) => setImmediate(() => setImmediate(r)));

const pushesOn = (from: number, topic: string) =>
    broadcastLog.slice(from).filter((b) => b.event === 'animation_events' && b.channel === topic);

/** The client's reading of a realtime payload, as AnimationContext.tsx reads it: the as3 flag from `t`, the kept identity. */
function clientRead(client: ClientTable, gameId: string, payload: Record<string, unknown>) {
    assert.deepEqual(Object.keys(payload).sort(), ['b', 's', 't', 'v'], 'the payload is exactly {t,s,v,b}');
    return client.readPush(base64ToBytes(payload.b as string), {
        as3: payload.t === 'as3', gameId, version: payload.v as number, identity: 'kept',
    });
}

if (!process.env.VALIDATION_ONLY) {
before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { await pgPool.end(); });

test('a lobby join push is as3 and seats the joiner from its own bytes over a stale kept roster', async () => {
    const gameId = `a3${uuid().slice(0, 4)}`;
    const host = uuid(), joiner = uuid();
    await seedLobby(gameId, [{ id: host, name: 'Host' }]);
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1) ON CONFLICT DO NOTHING', [joiner]);

    // The host's client holds the one-seat lobby: its envelope, as a page load reads it.
    const before = await mustReadTable(gameId);
    const server = fixtureTable();
    assert.equal(server.load(before.state, before.roster), L.TABLE_OK, 'the lobby loads');
    const envelope = server.envelope(gameId, 0, before.version);
    assert.ok(envelope instanceof Uint8Array, 'the host has an envelope');
    const client = new ClientTable(__clientKernelExports() as unknown as ClientExports);
    const held = client.adoptEnvelope(envelope as Uint8Array);
    assert.ok(held, 'the host client adopts the lobby');
    assert.deepEqual(held!.seats.map((s) => s.id), [host], 'the kept roster is the stale one-seat lobby');

    // The join.
    const from = broadcastLog.length;
    await runMeta(gameId, joiner, { type: 'join' }, 'Joiner');
    await settle();
    const hostTopic = `gu-${gameId}-${host}`;
    const joinPushes = pushesOn(from, hostTopic);
    assert.equal(joinPushes.length, 1, 'the join pushes once to the host');
    const payload = joinPushes[0].payload as Record<string, unknown>;
    assert.equal(payload.t, 'as3', 'the push is labelled as3');
    const bytes = base64ToBytes(payload.b as string);

    // Held to the product's reading: no refetch, the joiner named by the push.
    const read = clientRead(client, gameId, payload);
    assert.ok(read, `the host client reads the join push (${JSON.stringify(client.lastRefusal())})`);
    assert.deepEqual(read!.final.seats.map((s) => [s.id, s.name]), [[host, 'Host'], [joiner, 'Joiner']],
        'the push seats the joiner under their name, over the stale kept roster');
    assert.equal(read!.final.mySeat, 0, 'written for the host');
    assert.equal(read!.final.version, payload.v, 'at the pushed version');

    // The roster is the push's own, not the kept one: without any identity the push still names both seats.
    const alone = client.readPush(bytes, { as3: true, identity: 'none' });
    assert.ok(alone, 'the join push reads with no identity at all');
    assert.deepEqual(alone!.final.seats.map((s) => s.id), [host, joiner], 'and names the seats from its roster block');
    // (readPush keeps the identity of what it read: restore the join's, as the product would hold it.)
    assert.ok(clientRead(client, gameId, payload), 'the join push re-reads');

    // The next push carries no roster, and the client names the joiner from what it kept from the join push.
    const from2 = broadcastLog.length;
    await runMeta(gameId, joiner, { type: 'start' }, 'Joiner');
    await settle();
    const next = pushesOn(from2, hostTopic);
    assert.equal(next.length, 1, 'the joiner getting ready pushes once to the host');
    const nextPayload = next[0].payload as Record<string, unknown>;
    assert.equal(nextPayload.t, 'as3', 'labelled as3');
    const nextBytes = base64ToBytes(nextPayload.b as string);
    assert.equal(nextBytes[nextBytes.length - 1], 0, 'a push that did not change the roster ends in a zero flags byte');
    const nextRead = clientRead(client, gameId, nextPayload);
    assert.ok(nextRead, `the host client reads the next push (${JSON.stringify(client.lastRefusal())})`);
    assert.deepEqual(nextRead!.final.seats.map((s) => [s.id, s.name]), [[host, 'Host'], [joiner, 'Joiner']],
        'the kept roster is the one the join push carried');
});
}
