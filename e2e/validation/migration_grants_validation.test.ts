// Fast validation runner for the hosted-schema grant scenarios. They build their
// own databases (the frozen hosted schema plus the migrations, then seed.sql), so
// unlike db_validation they cannot share its seed.sql database and get a file
// of their own. The scenarios live in e2e/db_migration_grants.test.ts.
import './../harness.ts';
import { after } from 'node:test';
import { pgPool } from '../harness.ts';
import { registerMigrationGrantsValidation } from '../db_migration_grants.test.ts';

registerMigrationGrantsValidation();

after(async () => { await pgPool.end(); });
