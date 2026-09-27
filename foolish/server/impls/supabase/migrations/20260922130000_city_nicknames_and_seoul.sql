-- The bots ARE their cities now, and Seoul joins them.
--
-- Two changes, both data. The schema is untouched.
--
-- 1. Every nickname becomes its city. The roster is named after the road to
--    Moscow (docs/IOS_BOT_NAMING.md) and until now the site stored the ordnance
--    name and mapped it to a city at render. There is no map any more (owner,
--    2026-09-22): the stored name IS the city, so what a page shows is what this
--    table says, and what a replay blob embeds is what that replay shows. An
--    old replay code that stored '%Octogen 1' still renders '%Octogen 1',
--    because that is the bot that played it - a replay code is a replay code.
--
--    strategy_key does NOT change. The kernel's brains keep the names they are
--    called by in c/src, and the key is what c/src/bot_roster.c dispatches on;
--    only the player-facing nickname moves.
--
--    Keyed on strategy_key rather than on the old nickname text, so it does not
--    depend on how a row happens to be spelled, and the trailing instance number
--    is carried over verbatim.
--
-- 2. Seoul joins the site: seven robusta rows. The ladder advertises seven
--    cities and the site seated six - robusta (tier 5) was offline-only, so the
--    site jumped New York (tier 3) straight to Madrid (tier 6). The shipped
--    bots.wasm has always LINKED the brain for free, because firecracker IS
--    robusta's Monte Carlo, so only `seeded` in c/src/bot_roster.c changed.
--
-- No Elo reset here. Migration 20260922120000 zeroed every rating on the site
-- minutes earlier, so the seven Seouls join a ladder where nothing has played.
--
-- ONE GUARDED BLOCK, per migrations/README.md: `supabase start` and `db reset`
-- apply migrations BEFORE seed.sql, and seed.sql is the schema, so on every
-- database except hosted this would run before the table exists. Hosted is the
-- only database with anything here to change - everywhere else seed.sql already
-- carries these names and these rows.

DO $$
BEGIN
    IF to_regclass('public.bots') IS NULL THEN
        RAISE NOTICE 'city_nicknames_and_seoul: no bots table here, so this database is built from seed.sql, which already holds these names - nothing to migrate';
        RETURN;
    END IF;

    -- '%Cordite 4' -> '%St. Petersburg 4'. The prefix is the reserved bot marker
    -- that makes bot-vs-human recoverable from a replay code alone; the suffix is
    -- whatever trailing " <n>" the row carried, or nothing.
    UPDATE bots b
    SET nickname = '%' || m.city || COALESCE(substring(b.nickname from '\s\d+$'), '')
    FROM (VALUES
        ('random',      'Miami'),
        ('handwritten', 'New York'),
        ('robusta',     'Seoul'),
        ('firecracker', 'Madrid'),
        ('blackpowder', 'Vienna'),
        ('cordite',     'St. Petersburg'),
        ('octogen',     'Moscow')
    ) AS m(key, city)
    WHERE b.strategy_key = m.key
      AND b.nickname <> '%' || m.city || COALESCE(substring(b.nickname from '\s\d+$'), '');

    -- Seoul, seven deep like every other family. Idempotent on the nickname.
    INSERT INTO bots (nickname, strategy_key)
    SELECT v.nickname, v.strategy_key
    FROM (VALUES
        ('%Seoul 1', 'robusta'),
        ('%Seoul 2', 'robusta'),
        ('%Seoul 3', 'robusta'),
        ('%Seoul 4', 'robusta'),
        ('%Seoul 5', 'robusta'),
        ('%Seoul 6', 'robusta'),
        ('%Seoul 7', 'robusta')
    ) AS v(nickname, strategy_key)
    WHERE NOT EXISTS (SELECT 1 FROM bots b WHERE b.nickname = v.nickname);
END $$;
