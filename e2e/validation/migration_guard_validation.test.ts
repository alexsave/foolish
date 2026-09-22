// A migration is a delta for the ONE database that is not built from seed.sql.
//
// seed.sql is the schema here, and `supabase start` / `supabase db reset` apply
// migrations BEFORE seed.sql. So on every database except hosted, a file in
// server/impls/supabase/migrations/ runs before the tables it edits exist. The
// first migration after the history collapsed into seed.sql hit exactly that and
// failed CI's edge-serve lane with `relation "bots" does not exist`; memory.yml's
// own comment had recorded the hazard ("did not survive a fresh database
// together") and it had simply stopped mattering while the directory was empty.
//
// The fix cannot live in config.toml: `[db.migrations] enabled = false` skips
// migrations on `db push` too, which is how deploy.yml reaches hosted. So it
// lives in each file, and this is what keeps that from being a convention
// somebody forgets - every .sql here must be one guarded block that declines to
// run when the schema is absent.
//
// AN EMPTY DIRECTORY IS THE NORMAL STATE, so most of the time this test checks
// nothing, and that is honest rather than hidden: it exists for the day there is
// a file, which is the day the rule is easy to forget. migrations/README.md
// states the rule in prose for whoever writes that file.
//
// Pure test - no Postgres, no network, no compiler.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { resolve, join } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const DIR = join(REPO, 'server/impls/supabase/migrations');

/** The .sql files in the migrations directory, if any. */
function migrations(): string[] {
    return readdirSync(DIR).filter((f) => f.endsWith('.sql')).sort();
}

/** The file with `--` comments and blank lines removed. */
function statements(body: string): string {
    return body
        .split('\n')
        .map((ln) => (ln.trimStart().startsWith('--') ? '' : ln))
        .join('\n')
        .trim();
}

test('every migration is one block that declines to run without the schema', () => {
    const offenders: string[] = [];
    for (const f of migrations()) {
        const sql = statements(readFileSync(join(DIR, f), 'utf8'));
        if (!/^DO \$\$/.test(sql) || !/END \$\$;$/.test(sql)) {
            offenders.push(`${f}: its statements are not a single DO $$ ... END $$; block`);
            continue;
        }
        if (!sql.includes('to_regclass(')) {
            offenders.push(`${f}: the block has no to_regclass() check, so it cannot tell whether the schema is there`);
        }
        if (!/\bRETURN;/.test(sql)) {
            offenders.push(`${f}: the block never returns early, so a database without the schema still runs the body`);
        }
    }
    assert.deepEqual(offenders, [],
        'a migration runs BEFORE seed.sql on every database that is built from seed.sql, so it has to\n'
        + 'be a single guarded block - `IF to_regclass(\'public.<table>\') IS NULL THEN RAISE NOTICE ...;\n'
        + 'RETURN; END IF;` - or it fails on a fresh stack instead of no-opping:\n  '
        + `${offenders.join('\n  ')}\n`
        + 'See server/impls/supabase/migrations/README.md.');
});
