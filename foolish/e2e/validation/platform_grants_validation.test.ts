// Fast validation runner for the platform-grant scenarios. They build their own
// database (seed.sql under Supabase's default privileges), so unlike db_validation
// they cannot share its seed.sql database and get a file of their own. The
// scenarios live in e2e/db_platform_grants.test.ts.
import './../harness.ts';
import { after } from 'node:test';
import { pgPool } from '../harness.ts';
import { registerPlatformGrantsValidation } from '../db_platform_grants.test.ts';

registerPlatformGrantsValidation();

after(async () => { await pgPool.end(); });
