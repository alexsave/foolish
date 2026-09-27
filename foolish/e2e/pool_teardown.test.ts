// Ending a test file's pool must close every connection before it resolves.
//
// The harness teardown ends the pool and then drops the file's database WITH
// (FORCE). Stock pg-pool end() resolves as soon as it has asked its idle clients
// to end, while their backends are still attached to the database; FORCE then
// terminates them, the FATAL they answer with surfaces as an uncaughtException,
// and node:test marks the file red although every test in it passed. It showed up
// in the full run as concurrent_games.test.ts (a 24-client pool) failing with
// "terminating connection due to administrator command". The drained end() lives
// in e2e/adapters/supabase.ts; this pins it.

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applyPlatformShim, pgPool } from './harness.ts';

// Watch every client from the first one the pool opens (applyPlatformShim's).
const closed = new Map<unknown, boolean>();
pgPool.on('connect', (client) => {
    closed.set(client, false);
    client.once('end', () => closed.set(client, true));
});

before(async () => { await applyPlatformShim(); });

test('pool.end() resolves only after every pooled connection has closed', async () => {
    // Fill the pool, use every client, and hand them all back: the idle state the
    // teardown finds a file's pool in.
    const width = pgPool.options.max;
    const clients = await Promise.all(Array.from({ length: width }, () => pgPool.connect()));
    await Promise.all(clients.map((c) => c.query('SELECT 1')));
    clients.forEach((c) => c.release());
    assert.equal(pgPool.idleCount, width, 'every client is idle before the pool ends');

    await pgPool.end();

    const stillOpen = [...closed.values()].filter((isClosed) => !isClosed).length;
    assert.equal(closed.size, width, 'observed every client the pool opened');
    assert.equal(stillOpen, 0, `${stillOpen} of ${width} connections were still open when pool.end() resolved`);
});
