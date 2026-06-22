// E2E: the REAL bot-lease plpgsql (try_acquire / renew / release — lifted verbatim
// from the migrations) running in real Postgres.
//
// Owns the lease validation scenarios; the fast runner
// (e2e/validation/db_validation.test.ts) imports `registerLeaseValidation` and
// provides the shared DB before/after.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';

const acquire = async (id: string, ttl: number) => (await pgPool.query('SELECT try_acquire_bot_lease($1,$2) AS t', [id, ttl])).rows[0].t;
const renew = async (id: string, tok: string, ttl: number) => (await pgPool.query('SELECT renew_bot_lease($1,$2,$3) AS r', [id, tok, ttl])).rows[0].r;
const release = async (id: string, tok: string) => pgPool.query('SELECT release_bot_lease($1,$2)', [id, tok]);
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function game(): Promise<string> {
    const id = `l${uuid().slice(0, 6)}`;
    await seedGame(id, [{ id: uuid(), name: 'B', is_ai: true, strategy_key: 'random' }]);
    return id;
}

export function registerLeaseValidation(): void {
    test('lease: exactly one of many concurrent acquires wins (mutual exclusion)', async () => {
        const id = await game();
        const tokens = await Promise.all(Array.from({ length: 30 }, () => acquire(id, 30_000)));
        assert.equal(tokens.filter((t) => t != null).length, 1, 'exactly one winner');
    });

    test('lease: a dead driver is recovered after its TTL expires', async () => {
        const id = await game();
        const tok = await acquire(id, 200);
        assert.ok(tok, 'acquired');
        assert.equal(await acquire(id, 200), null, 'blocked while live');
        await sleep(350);
        assert.ok(await acquire(id, 200), 're-acquired after TTL');
    });

    test('lease: renew fences a stale token (a superseded loop cannot extend)', async () => {
        const id = await game();
        const stale = await acquire(id, 150);
        await sleep(250); // let it lapse
        const owner = await acquire(id, 30_000);
        assert.equal(await renew(id, owner, 30_000), true, 'owner renews');
        assert.equal(await renew(id, stale, 30_000), false, 'stale token fenced');
        void release;
    });
}

if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });
    registerLeaseValidation();
    after(async () => { await pgPool.end(); });
}
