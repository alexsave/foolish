# This directory is empty, and that is its normal state

`seed.sql` is the schema.
It is the whole schema, it is the only definition of it, and every database this project runs on is built from it: a `supabase start`, each e2e test's own database, and a new hosted project.

This used to be a history - 39 files recording how the hosted database got from June 2026 to September 2026, every one of them long since applied there and every one of them also written into `seed.sql`, which a test kept equal to their end state object for object.
They are deleted.
`git log -- server/impls/supabase/migrations` still has all of them and they are worth reading for the reasoning; nothing in the tree reads them.

A merge to `main` now runs **no SQL at all** against the hosted database.
`.github/workflows/deploy.yml` skips its schema step when there is no `.sql` file here, which there normally is not, and goes straight to deploying the edge functions.

## The day someone needs to change the schema

The hosted project is the one database that cannot be rebuilt from `seed.sql` - that file drops every table it finds - so a change has to arrive there as a delta.
Write it **twice**, in the same commit:

1. In `seed.sql`, in its final form, where a new database will get it.
2. Here, as `<UTC timestamp>_<what_it_does>.sql`, in the form that takes today's production database to it.

The deploy workflow's schema step then sees a `.sql` file, runs `supabase db push`, and applies it before the functions that depend on it deploy.

Every file here must be ONE GUARDED BLOCK, and this one was learned the hard way in CI:

```sql
DO $$
BEGIN
    IF to_regclass('public.bots') IS NULL THEN
        RAISE NOTICE 'this database is built from seed.sql, which already holds the end state';
        RETURN;
    END IF;
    -- the delta
END $$;
```

`supabase start` and `supabase db reset` apply migrations BEFORE seed.sql.
seed.sql is the schema, so on every database except hosted a file here runs before the tables it edits exist, and an unguarded delta fails with `relation "bots" does not exist`.
The first migration written after the collapse did exactly that to the `edge-serve` lane.
Hosted is the only database with anything here to change - everywhere else seed.sql already produces the end state - so a delta declines to run when the schema is absent rather than erroring.

`[db.migrations] enabled = false` in `config.toml` does not solve it: that switch skips migrations on `db push` too, which is how `deploy.yml` reaches hosted.
`e2e/validation/migration_guard_validation.test.ts` holds the rule, so it is checked rather than remembered.

Two things about the filename, both learned the hard way on hosted:

- The timestamp decides the order. `supabase db push` applies in filename order and keys its history on the version string, so a file that depends on another must sort after it.
- No two files may share a timestamp. Two branches in flight can pick the same prefix; when both merge, the order between them falls back to their titles, which is to say alphabetically, which is to say arbitrarily.

## Before the first one: a one-time cleanup, run by hand

Hosted's `supabase_migrations.schema_migrations` still records the 39 versions whose files this commit deleted, and `supabase db push` **refuses to run** while a remote version has no local file:

```
LegacyDbPushMissingLocalError: Remote migration versions not found in local migrations directory.
```

Measured against a real Postgres carrying those 39 rows, with both an empty local directory and no directory at all.
Nothing trips over this today, because nothing runs `db push` today.
Whoever adds the first new migration clears the rows once, from their own terminal, before merging it:

```sh
supabase link --project-ref wngpfwmwkltonwosqflx --workdir server/impls
supabase migration repair --status reverted --linked --workdir server/impls \
  20250628051540 20260615120000 20260616030000 20260616030001 20260616040000 \
  20260616050000 20260618120000 20260701120000 20260702090000 20260702090001 \
  20260702090002 20260702100000 20260702110000 20260707120000 20260707140000 \
  20260707150000 20260708120000 20260708130000 20260708140000 20260708160000 \
  20260709050000 20260709120000 20260711120000 20260711130000 20260712120000 \
  20260713120000 20260714120000 20260715120000 20260807120000 20260906120000 \
  20260917000000 20260917120000 20260917130000 20260917135000 20260917140000 \
  20260918200000 20260918210000 20260918220000 20260918230000
```

`repair --status reverted` DELETEs those rows from the history table and touches no schema: it is bookkeeping.
It is idempotent - repairing a version whose row is already gone succeeds and does nothing - and it can never be needed twice, because these 39 are the whole history as it stood at the collapse into `seed.sql` and no new one can join them.
After it, `supabase db push` reports "Remote database is up to date", and a new migration file applies normally.
Both halves were measured on a Postgres seeded with the 39 rows before this was written down.
