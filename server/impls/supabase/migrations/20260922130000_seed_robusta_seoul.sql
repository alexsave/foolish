-- Seoul joins the site: seven robusta rows.
--
-- The ladder is seven cities and the website seated six of them. robusta - tier
-- 5, the ladder's Seoul - was offline-only, so the site jumped New York (tier 3)
-- straight to Madrid (tier 6) and one advertised city was unreachable. The
-- shipped bots.wasm has always LINKED the brain for free, because firecracker IS
-- robusta's Monte Carlo, so the code ships either way; only `seeded` in
-- c/src/bot_roster.c had to change, and e2e/bot_roster_parity holds the shipped
-- module to that table.
--
-- Seven rows, like every other family: eight seats is a human plus seven
-- opponents, so seven is what it takes to fill a table with one rung.
--
-- No Elo reset here. Migration 20260922120000 zeroed every rating on the site minutes
-- before this, so these seven join a ladder where nothing has played yet.
--
-- ONE GUARDED BLOCK, per migrations/README.md: `supabase start` and `db reset`
-- apply migrations BEFORE seed.sql, and seed.sql is the schema, so on every
-- database except hosted this file would otherwise run before the table exists.
-- Hosted is the only database with anything here to change - everywhere else
-- seed.sql already carries these rows.

DO $$
BEGIN
    IF to_regclass('public.bots') IS NULL THEN
        RAISE NOTICE 'seed_robusta_seoul: no bots table here, so this database is built from seed.sql, which already holds these rows - nothing to migrate';
        RETURN;
    END IF;

    -- Idempotent on the nickname, like the ladder migration beside it.
    INSERT INTO bots (nickname, strategy_key)
    SELECT v.nickname, v.strategy_key
    FROM (VALUES
        ('%Robusta 1', 'robusta'),
        ('%Robusta 2', 'robusta'),
        ('%Robusta 3', 'robusta'),
        ('%Robusta 4', 'robusta'),
        ('%Robusta 5', 'robusta'),
        ('%Robusta 6', 'robusta'),
        ('%Robusta 7', 'robusta')
    ) AS v(nickname, strategy_key)
    WHERE NOT EXISTS (SELECT 1 FROM bots b WHERE b.nickname = v.nickname);
END $$;
