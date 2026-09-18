// ADVERSARIAL: the TypeScript production layer - the endpoints a malicious
// client actually reaches. Drives the REAL server modules (handleMetaAction and
// executePackedAction over table_io's CAS loop and the C Table) against real
// Postgres, with the caller's auth id, and hammers them with concurrency. Two
// axes:
//   1. authorization / logic exploits (can I affect state I shouldn't?)
//   2. rapid-fire concurrency (can I race the CAS commit into duplicating or
//      losing a card, or wedge/deadlock the loop?)
// The hard invariant after everything: card conservation holds (counted from
// the stored state blob) and no request crashes the server (a clean refusal is
// fine; a crash-class TypeError isn't).
//
// Moves are packed now (the JSON move path is refused at the edge,
// e2e/table_server_seat.test.ts), so the hostile card payloads here are hostile
// awire BYTES and hostile numbers pushed through the client's encoder.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { ACTION_STATUS, AWIRE_KIND, encodeAction, type AwireMove } from '../sdk/ts/wire/awire.ts';
import { handleMetaAction } from '../server/impls/supabase/functions/_shared/adapter/meta_actions.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { fixture, FixtureRefused, WAITING } from './helpers/table_fixture.ts';
import { checkCardConservation, legalMoves, mustReadTable } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// A pinned deal: the concurrency tests pick moves by rule, so a fixed deal makes
// a red run replay exactly.
__setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 29 + 7) & 0xff));

const CRASH = /cannot read|is not a function|is not iterable|maximum call stack|reading '|undefined is not|out of bounds|unreachable/i;
const crashed = (e: unknown) => CRASH.test(String((e as Error)?.message ?? e));

const H0 = uuid(), H1 = uuid(), H2 = uuid();
/** Alice, Bob and Carol in a lobby (all ready), dealt by Alice's `start` unless `started` is false. */
async function freshLobby(id: string, started = true): Promise<void> {
    await seedLobby(id, [
        { id: H0, name: 'Alice', ready: !started },
        { id: H1, name: 'Bob' },
        { id: H2, name: 'Carol' },
    ]);
    if (started) {
        await runMeta(id, H0, { type: 'start' });
        assert.equal((await mustReadTable(id)).status, L.GAME_STATUS_PLAYING, 'fixture: the game dealt');
    }
}

/** Seats `n` bots rows for add-bot to choose among. */
async function botRows(n: number): Promise<void> {
    for (let i = 0; i < n; i++)
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3) ON CONFLICT DO NOTHING', [uuid(), `Bot${i}`, 'random']);
}

before(applySchema);
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { await pgPool.end(); });

// ---------------------------------------------------------------------------
// 1. AUTHORIZATION - the meta endpoint takes target ids from the body
// ---------------------------------------------------------------------------

test('exit: any lobby player may kick any other (intended) without corrupting state', async () => {
    const id = 'advexit';
    await freshLobby(id, false); // lobby (exit requires WAITING)
    // Bob kicks Alice by supplying her player_id - this is intentional lobby
    // management. What matters adversarially: it only removes the named player,
    // leaves the rest intact, and can't be turned into corruption.
    await runMeta(id, H1, { type: 'exit', player_id: H0 }, 'Bob');
    const t = await mustReadTable(id);
    assert.deepEqual(t.seats.map((s) => s.id), [H1, H2], 'Alice was kicked (allowed); Bob and Carol untouched');
    // Kicking a non-member / already-gone id must reject, not corrupt.
    await assert.rejects(runMeta(id, H1, { type: 'exit', player_id: H0 }, 'Bob'), /not in game/i);
    assert.equal((await mustReadTable(id)).version, t.version, 'the refused kick committed nothing');
});

test('exit: removing yourself still works', async () => {
    const id = 'advexit2';
    await freshLobby(id, false);
    await runMeta(id, H1, { type: 'exit' }, 'Bob');
    assert.deepEqual((await mustReadTable(id)).seats.map((s) => s.id), [H0, H2], 'Bob removed himself; Alice untouched');
});

test('add-bot flood is capped at 8 seats (no oversized-lobby crash)', async () => {
    const id = 'advflood';
    // A human who is not ready blocks add-bot's auto-deal, so a client could
    // otherwise flood the roster with bots and then crash the deal on start.
    await seedLobby(id, [{ id: H0, name: 'Alice', ready: false }]);
    await botRows(20);
    let full = false;
    for (let i = 0; i < 20; i++) {
        try { await runMeta(id, H0, { type: 'add-bot' }, 'Alice'); }
        catch (e) { assert.match(String((e as Error).message), /full \(max 8/i); full = true; break; }
    }
    assert.ok(full, 'add-bot must reject once the lobby is full');
    const t = await mustReadTable(id);
    assert.equal(t.seats.length, 8, 'lobby capped at 8');
    assert.equal(t.status, WAITING, 'and it never dealt');

    // An oversized lobby cannot even be represented: the durable roster refuses a ninth seat.
    assert.throws(
        () => fixture().seats(Array.from({ length: 9 }, (_, i) => ({ id: uuid(), name: `X${i}`, brain: 'random' }))).build(),
        (e: unknown) => e instanceof FixtureRefused && e.reason === 'ROSTER_E_FULL',
        'a nine-seat roster is refused by the kernel');

    // Even if a stored row were corrupt (pre-fix data, a torn write), a deal on
    // it must refuse cleanly rather than crash, and write nothing.
    const bad = 'advbad';
    await seedLobby(bad, [{ id: H0, name: 'Alice', ready: false }, { id: H1, name: 'Bob' }]);
    const other = fixture().seats(Array.from({ length: 8 }, (_, i) => ({ id: uuid(), name: `X${i}`, brain: 'random' }))).build();
    const hex = (b: Uint8Array) => `\\x${Buffer.from(b).toString('hex')}`;
    // Each case names the refusal it must draw. The columns are hex TEXT until the
    // BYTEA migration (the next deploy's PR), so cutting the roster to 7 characters
    // leaves an odd number of hex digits and the column reader refuses it one step
    // before the kernel sees it; on BYTEA the same cut is whole bytes and the
    // refusal is the kernel's. Either way it is a named refusal that writes nothing.
    for (const [what, refusal, sql, args] of [
        ['a roster seating 8 under a 2-seat state', /does not load/i, 'UPDATE games SET roster = $2 WHERE id = $1', [hex(other.roster)]],
        ['a truncated roster', /odd number of digits/i, 'UPDATE games SET roster = substring(roster from 1 for 7) WHERE id = $1', []],
    ] as const) {
        await pgPool.query(sql, [bad, ...args]);
        __clearGameCache();
        const stored = (await pgPool.query('SELECT version, state, roster FROM games WHERE id = $1', [bad])).rows[0];
        const err = await runMeta(bad, H0, { type: 'start' }, 'Alice').then(() => null, (e) => e);
        assert.ok(err, `${what}: the deal is refused`);
        assert.ok(!crashed(err), `${what}: a clean refusal, not a crash (${String(err?.message)})`);
        assert.match(String(err.message), refusal, `${what}: refused, and named`);
        assert.deepEqual((await pgPool.query('SELECT version, state, roster FROM games WHERE id = $1', [bad])).rows[0], stored, `${what}: nothing written`);
    }
});

// ---------------------------------------------------------------------------
// 2. RAPID-FIRE CONCURRENCY - race the CAS commit
// ---------------------------------------------------------------------------

test('concurrency: add-bot flood fired all at once still caps at 8 seats', async () => {
    const id = 'advfloodrace';
    await seedLobby(id, [{ id: H0, name: 'Alice', ready: false }]);
    await botRows(30);
    // 30 add-bot requests fired concurrently against one version - the CAS loop
    // serializes them, and the cap must hold across the whole burst.
    await Promise.all(Array.from({ length: 30 }, () => runMeta(id, H0, { type: 'add-bot' }, 'Alice').catch(() => null)));
    const t = await mustReadTable(id);
    assert.ok(t.seats.length <= 8, `concurrent flood still capped, got ${t.seats.length}`);
    assert.ok(t.seats.length > 1, 'and some of the burst landed');
    // no duplicate bot ids slipped through the race
    const ids = t.seats.map((s) => s.id);
    assert.equal(new Set(ids).size, ids.length, 'no duplicate players from the race');
});

test('concurrency: 200 overlapping submits against one state cannot dup/lose a card', async () => {
    const id = 'advrace';
    await freshLobby(id);
    const moves = legalMoves(await mustReadTable(id));
    assert.ok(moves.length > 0, 'fixture: somebody can move');
    const kinds = Object.values(AWIRE_KIND);
    // Fire the same legal moves from many clients simultaneously, plus a burst of
    // malformed submits interleaved (a card count the bytes do not carry, cards
    // on a pickup) - all racing one version.
    const attempts: Promise<unknown>[] = [];
    for (let i = 0; i < 200; i++) {
        const pick = moves[i % moves.length];
        const wire = i % 3 === 0 ? pick.wire : new Uint8Array([kinds[i % 5], 1, (i * 7) % 52]);
        attempts.push(runAction(id, pick.playerId, wire).catch(() => null)); // rejections are fine; only the invariant matters
    }
    await Promise.all(attempts);
    const chk = await checkCardConservation(id);
    assert.ok(chk.ok, `card conservation after 200-way race: ${chk.detail}`);
    assert.ok((await mustReadTable(id)).version > 1, 'some move of the race applied');
});

test('concurrency: rapid full-game self-play through the CAS loop stays conserved', async () => {
    const id = 'advrapid';
    await freshLobby(id);
    const total0 = (await checkCardConservation(id)).detail;
    const pickup = encodeAction({ kind: 'pickup' });
    let guard = 0;
    while (guard++ < 4000) {
        const t = await mustReadTable(id);
        if (t.status !== L.GAME_STATUS_PLAYING) break;
        const moves = legalMoves(t);
        if (!moves.length) break;
        // Fire the next move AND two stale/garbage duplicates concurrently.
        const m = moves[0];
        await Promise.all([
            runAction(id, m.playerId, m).catch(() => null),
            runAction(id, m.playerId, m).catch(() => null),
            runAction(id, H2, pickup).catch(() => null),
        ]);
        const chk = await checkCardConservation(id);
        assert.ok(chk.ok, `conservation mid-rapid-game (was ${total0}): ${chk.detail}`);
    }
    assert.ok(guard > 5, 'the game progressed');
});

// ---------------------------------------------------------------------------
// 3. game_id / payload type confusion at the load boundary
// ---------------------------------------------------------------------------

test('meta: hostile game_id / bot_id / player_id values never crash the handler', async () => {
    const id = 'advmeta';
    await freshLobby(id, false);
    const hostile: Record<string, unknown>[] = [
        { type: 'exit', bot_id: { $ne: null } },
        { type: 'exit', bot_id: ['a', 'b'] },
        { type: 'exit', player_id: "1' OR '1'='1" },
        { type: 'exit', bot_id: 'nonexistent' },
        { type: 'add-bot', strategy_key: "'; DROP TABLE games;--" },
        { type: 'rearrange-hand', card_indices: 'not-an-array' },
        { type: 'rearrange-hand', card_indices: [0, 0, 0] },
        { type: 'rearrange-hand', card_indices: [999, -1] },
        { type: 'rearrange-players', new_order: [{ __proto__: { admin: true } }] },
        { type: 'rearrange-players', new_order: [H0, H0, H0] },
        { type: '__proto__' },
        { type: 'constructor' },
        { type: 'update-name', new_name: 'x'.repeat(100000) },
        { type: 'update-name', new_name: { toString: () => { throw new Error('boom'); } } },
    ];
    const user = { id: H0, user_metadata: { username: 'Alice' } } as never;
    const send = (body: Record<string, unknown>) => handleMetaAction({ user, userName: 'Alice', body, reqId: 'h' });
    const barrage: [string, () => Promise<unknown>][] = [
        ...hostile.map((b): [string, () => Promise<unknown>] => [JSON.stringify(b), () => runMeta(id, H0, b, 'Alice')]),
        ...[{ $ne: null }, ['advmeta'], "advmeta' OR '1'='1", 'x'.repeat(10000), 42, null].map((gid): [string, () => Promise<unknown>] =>
            [`game_id ${JSON.stringify(gid)}`.slice(0, 60), () => send({ type: 'exit', game_id: gid })]),
    ];
    for (const [what, run] of barrage) {
        const err = await run().then(() => null, (e) => e); // a refusal is acceptable; a crash-class error is not
        assert.ok(!crashed(err), `hostile meta request crashed the handler: ${what.slice(0, 80)}: ${String(err?.message)}`);
        // A malformed id is a malformed request, never "no target given": an
        // `exit` naming a non-string bot_id must not make Alice leave.
        assert.ok((await mustReadTable(id)).seats.some((s) => s.id === H0), `Alice is still seated after: ${what.slice(0, 80)}`);
    }
    // and the table survived it all: it still loads, and seats who it should
    const t = await mustReadTable(id);
    assert.ok(t.seats.length > 0, 'game intact after hostile meta barrage');
    assert.ok(t.seats.every((s) => [H0, H1, H2].includes(s.id)), 'nobody the barrage named was seated');
});

// ---------------------------------------------------------------------------
// 4. numeric / structural card edge cases reaching the kernel's wire decode
// ---------------------------------------------------------------------------

test('numeric edge-case cards and hostile wires are rejected or applied cleanly, never crash', async () => {
    const id = 'advnum';
    await freshLobby(id);
    const t0 = await mustReadTable(id);
    const fa = t0.seats[t0.firstAttacker].id;
    // Hostile numbers through the client's encoder (it clamps them into a card).
    const evil = [
        [{ suit: 0, value: Infinity }], [{ suit: 0, value: -Infinity }], [{ suit: 0, value: NaN }],
        [{ suit: 0, value: 1e300 }], [{ suit: 1e300, value: 5 }], [{ suit: -0, value: -0 }],
        [{ suit: 0, value: 13.0000001 }], [{ suit: 0.5, value: 5.5 }],
        [{ suit: 0, value: 5, extra: 'x'.repeat(10000) }],
        [{ suit: { valueOf: () => 0 }, value: { valueOf: () => 5 } }],
    ];
    const wires: [string, Uint8Array][] = evil.map((cards) =>
        [JSON.stringify(cards).slice(0, 60), encodeAction({ kind: 'attack', cards } as unknown as AwireMove)]);
    // Hostile bytes the encoder would never write.
    for (const raw of [
        [], [0], [0, 255], [0, 1], [0, 1, 0xff], [0, 1, 0xfe], [0, 1, 200], [0, 2, 0, 0],
        [0, 29, ...Array(29).fill(0)], [1, 1, 60, 70], [1, 2, 0, 1], [3, 1, 0], [4, 5], [9, 0], [255, 255, 255],
    ]) wires.push([`raw [${raw.slice(0, 6).join(',')}]`, new Uint8Array(raw)]);

    for (const [what, wire] of wires) {
        const v0 = (await mustReadTable(id)).version;
        const out = await runAction(id, fa, wire).then((r) => r, (e) => e as Error);
        if (out instanceof Error) {
            assert.ok(!crashed(out), `${what}: a crash, not a refusal: ${out.message}`);
            assert.equal((await mustReadTable(id)).version, v0, `${what}: a refused wire commits nothing`);
        } else if (out.status !== ACTION_STATUS.APPLIED) {
            assert.equal((await mustReadTable(id)).version, v0, `${what}: a rejected move commits nothing`);
        }
        const chk = await checkCardConservation(id);
        assert.ok(chk.ok, `conservation after ${what}: ${chk.detail}`);
    }
});
