-- seat_ratings: the read the kernel's rating of a finished game starts from.
--
-- New, and nothing deployed calls it until the functions in this same deploy
-- do, so it is safe on either side of them. seed.sql has the reasoning, on the
-- function, which is seed.sql's character for character
-- (e2e/validation/migration_guard_validation.test.ts holds the two equal).
--
-- ONE GUARDED BLOCK, per migrations/README.md: `supabase start` and `db reset`
-- apply migrations BEFORE seed.sql, and seed.sql is the schema, so on every
-- database except hosted this would run before `bots` exists. Everywhere else
-- seed.sql creates this function itself.

DO $$
BEGIN
    IF to_regclass('public.bots') IS NULL THEN
        RAISE NOTICE 'seat_ratings: no bots table here, so this database is built from seed.sql, which already holds this function - nothing to migrate';
        RETURN;
    END IF;

    EXECUTE $ddl$
CREATE OR REPLACE FUNCTION seat_ratings(p_ids UUID[])
RETURNS INTEGER[]
LANGUAGE sql
STABLE
SET search_path = public
AS $seat_ratings$
  SELECT COALESCE(array_agg(COALESCE(b.elo_rating, u.elo_rating, -1) ORDER BY s.ord), '{}')
  FROM unnest(p_ids) WITH ORDINALITY AS s(id, ord)
  LEFT JOIN bots b ON b.id = s.id
  LEFT JOIN user_elo_ratings u ON u.user_id = s.id;
$seat_ratings$;
$ddl$;
END $$;
