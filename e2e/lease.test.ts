// E2E: the REAL bot-lease plpgsql (try_acquire / renew / release - lifted verbatim
// from the migrations) running in real Postgres.
//
// Owns the lease validation scenarios; the fast runner
// (e2e/validation/db_validation.test.ts) imports `registerLeaseValidation` and
// provides the shared DB before/after.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import { seedLobby } from './helpers/table_server.ts';

type Query = { query: (sql: string, params?: unknown[]) => Promise<{ rows: any[] }> };
const acquire = async (id: string, ttl: number, db: Query = pgPool) =>
    (await db.query('SELECT try_acquire_bot_lease($1,$2) AS t', [id, ttl])).rows[0].t;
const renew = async (id: string, tok: string, ttl: number) =>
    (await pgPool.query('SELECT renew_bot_lease($1,$2,$3) AS r', [id, tok, ttl])).rows[0].r;

// TIME. The lease compares bot_lease_until with the database's now(). These tests
// used to move that clock by sleeping: acquire with a 200 ms TTL, expect a second
// acquire to be blocked, sleep 350 ms, expect a third to win. Under CPU load the
// second acquire could itself land more than 200 ms after the first, find the
// lease lapsed and win, and "blocked while live" failed (2 runs in 15 with 48 busy
// loops on 8 cores). Nothing here reads the wall clock now:
//
// - "At once" is one transaction. now() is the transaction's start time, so every
//   statement in it sees the same instant however long the machine takes between
//   them, and a lease granted in it is live at that instant by construction.
// - "Later" is the stored expiry moved back (lapse): to a function that compares
//   the expiry with now(), that is exactly what the passing of that time is.

/** Runs `body` in one transaction, where every now() is the same instant. */
async function atOneInstant<T>(body: (db: Query) => Promise<T>): Promise<T> {
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        const out = await body(c);
        await c.query('COMMIT');
        return out;
    } catch (e) {
        await c.query('ROLLBACK');
        throw e;
    } finally { c.release(); }
}

/** `ms` pass for the game's lease: its stored expiry goes back by that much. */
const lapse = (id: string, ms: number) => pgPool.query(
    'UPDATE games SET bot_lease_until = bot_lease_until - make_interval(secs => $2 / 1000.0) WHERE id = $1', [id, ms]);

// The lease lives on the games row; any kernel-owned row will do.
async function game(): Promise<string> {
    const id = `l${uuid().slice(0, 6)}`;
    await seedLobby(id, [{ id: uuid(), name: 'B', brain: 'random' }]);
    return id;
}

export function registerLeaseValidation(): void {
    test('lease: exactly one of many concurrent acquires wins (mutual exclusion)', async () => {
        const id = await game();
        const tokens = await Promise.all(Array.from({ length: 30 }, () => acquire(id, 30_000)));
        assert.equal(tokens.filter((t) => t != null).length, 1, 'exactly one winner');
    });

    test('lease: a live lease blocks, and a dead driver is recovered after its TTL expires', async () => {
        const id = await game();
        const tok = await atOneInstant(async (db) => {
            const t = await acquire(id, 200, db);
            assert.ok(t, 'acquired');
            const { rows: [r] } = await db.query(
                "SELECT bot_lease_until = now() + interval '200 milliseconds' AS exact FROM games WHERE id = $1", [id]);
            assert.equal(r.exact, true, 'the lease runs exactly its TTL from the instant it was taken');
            assert.equal(await acquire(id, 200, db), null, 'blocked while live');
            return t;
        });
        await lapse(id, 1_000);
        const again = await acquire(id, 200);
        assert.ok(again, 're-acquired after TTL');
        assert.notEqual(again, tok, 'a new holder');
    });

    test('lease: renew fences a stale token (a superseded loop cannot extend)', async () => {
        const id = await game();
        const stale = await acquire(id, 150);
        await lapse(id, 1_000);
        const owner = await acquire(id, 30_000);
        assert.ok(owner, 'the next loop takes the lapsed lease');
        assert.equal(await renew(id, owner, 30_000), true, 'owner renews');
        await lapse(id, 1_000);   // well inside the renewed TTL
        assert.equal(await acquire(id, 200), null, 'the renewed lease still blocks');
        assert.equal(await renew(id, stale, 30_000), false, 'stale token fenced');
    });
}

if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });
    registerLeaseValidation();
    after(async () => { await pgPool.end(); });
}
