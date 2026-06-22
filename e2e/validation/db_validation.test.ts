// Fast validation runner for the Postgres-backed scenarios. The scenarios live in
// their domain files (server.test.ts, lease.test.ts, meta.test.ts,
// concurrent_games.test.ts); this runner provides the single shared DB lifecycle
// and executes the handpicked deterministic cases. Run with VALIDATION_ONLY=1 so
// importing those files does NOT register their heavy loops or duplicate before/after.
import './../harness.ts';
import { before, beforeEach, after } from 'node:test';
import { applySchema, resetDb, pgPool } from '../harness.ts';
import { registerServerValidation } from '../server.test.ts';
import { registerLeaseValidation } from '../lease.test.ts';
import { registerMetaValidation } from '../meta.test.ts';
import { registerConcurrentValidation } from '../concurrent_games.test.ts';

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

registerServerValidation();
registerLeaseValidation();
registerMetaValidation();
registerConcurrentValidation();

after(async () => { await pgPool.end(); });
