-- The site's bot rows become the city ladder, seven deep.
--
-- Three changes, all of them data. The schema is untouched.
--
-- 1. simple_heuristic is off the site. The website renders a bot seat as its
--    rung's city on the road to Moscow (docs/IOS_BOT_NAMING.md; the rendering is
--    src/common/botName.ts), the ladder is seven cities, and this rung is not
--    one of them - it was dropped when the ladder went international, and the
--    table in docs/ARCHITECTURE.html §7 has not listed it since. A seeded row
--    with no city renders as "Simple Heuristic 2" on a board between Miami and
--    Madrid. The rung keeps its brain, its tier and its offline flag; only
--    `seeded` changed (c/src/bot_roster.c).
--
-- 2. `%0x00C0FFEE` is off the site. It was the second Handwritten row, predates
--    the ladder, and is the one seeded nickname that is not a rung's name, so on
--    a board of cities it reads as a bug rather than as a joke. The nickname
--    parser still passes a leading `0x...` through verbatim: old replay blobs
--    carry it embedded at encode time and those games still render.
--
-- 3. Every remaining family goes to seven rows. Eight seats is a human plus
--    seven opponents, so seven is what it takes to fill a table with one rung -
--    a thing you could do with Miami and with nothing else before this.
--
-- 4. Every Elo rating on the site resets to the base, humans included. The
--    leaderboard is ONE ladder rated by the same per-game pairwise Elo
--    (updateEloRatings in functions/_shared/utils.ts), so a rating is a claim
--    about the population it was earned against - and this migration replaces
--    that population: two families leave, four go from three rows to seven, and
--    24 of the 42 bots have never played a hand. Keeping the old numbers would
--    rank humans against opponents that no longer exist and seat fresh 1000s
--    beside veterans on the same board. A wipe is the honest reading, and it is
--    the owner's call (2026-09-22).
--
-- READ server/impls/supabase/migrations/README.md BEFORE DEPLOYING. This is the
-- first migration since the history collapsed into seed.sql, so hosted's
-- schema_migrations still records the 39 deleted versions and `supabase db push`
-- refuses to run until they are repaired once, by hand, from a terminal. The
-- README has the command.
--
-- WHAT THIS DOES TO A GAME IN FLIGHT. Nothing to its play: a seat's brain lives
-- in the kernel's roster blob (games.roster), written when the bot was added,
-- and the bot heartbeat drives the kernel from that row alone - it never reads
-- this table. bot_hands rows cascade away, which nothing reads either. The one
-- visible effect is that such a game's final Elo update is skipped: finalize.ts
-- raises when a seated bot has no row, inside the catch that exists so a game
-- never breaks over ratings. That is bounded by the games in flight at deploy,
-- which is why the rows go and the brains stay: the shipped bots.wasm still
-- links simple_heuristic, so those games finish playing.
--
-- The deleted bots' own Elo history goes with their rows. Human ratings are in
-- user_elo_ratings and are untouched.

DELETE FROM bots WHERE strategy_key = 'simple_heuristic';
DELETE FROM bots WHERE nickname = '%0x00C0FFEE';

-- Idempotent, and stated as the whole target set rather than as the per-family
-- delta: if hosted's rows have drifted from seed.sql in either direction, this
-- lands on the set seed.sql builds from scratch. The '%' is written in rather
-- than applied by a later UPDATE (seed.sql does it that way only to keep its
-- literal list readable); it is the reserved bot prefix that makes bot-vs-human
-- recoverable from a replay code alone.
INSERT INTO bots (nickname, strategy_key)
SELECT v.nickname, v.strategy_key
FROM (VALUES
    ('%Random 1', 'random'),
    ('%Random 2', 'random'),
    ('%Random 3', 'random'),
    ('%Random 4', 'random'),
    ('%Random 5', 'random'),
    ('%Random 6', 'random'),
    ('%Random 7', 'random'),
    ('%Handwritten 1', 'handwritten'),
    ('%Handwritten 2', 'handwritten'),
    ('%Handwritten 3', 'handwritten'),
    ('%Handwritten 4', 'handwritten'),
    ('%Handwritten 5', 'handwritten'),
    ('%Handwritten 6', 'handwritten'),
    ('%Handwritten 7', 'handwritten'),
    ('%Firecracker 1', 'firecracker'),
    ('%Firecracker 2', 'firecracker'),
    ('%Firecracker 3', 'firecracker'),
    ('%Firecracker 4', 'firecracker'),
    ('%Firecracker 5', 'firecracker'),
    ('%Firecracker 6', 'firecracker'),
    ('%Firecracker 7', 'firecracker'),
    ('%Blackpowder 1', 'blackpowder'),
    ('%Blackpowder 2', 'blackpowder'),
    ('%Blackpowder 3', 'blackpowder'),
    ('%Blackpowder 4', 'blackpowder'),
    ('%Blackpowder 5', 'blackpowder'),
    ('%Blackpowder 6', 'blackpowder'),
    ('%Blackpowder 7', 'blackpowder'),
    ('%Cordite 1', 'cordite'),
    ('%Cordite 2', 'cordite'),
    ('%Cordite 3', 'cordite'),
    ('%Cordite 4', 'cordite'),
    ('%Cordite 5', 'cordite'),
    ('%Cordite 6', 'cordite'),
    ('%Cordite 7', 'cordite'),
    ('%Octogen 1', 'octogen'),
    ('%Octogen 2', 'octogen'),
    ('%Octogen 3', 'octogen'),
    ('%Octogen 4', 'octogen'),
    ('%Octogen 5', 'octogen'),
    ('%Octogen 6', 'octogen'),
    ('%Octogen 7', 'octogen')
) AS v(nickname, strategy_key)
WHERE NOT EXISTS (SELECT 1 FROM bots b WHERE b.nickname = v.nickname);

-- The base is the column default and finalize.ts's BASE_RATING, both 1000.
-- games_played goes to 0 with it, which is also what hides a row from the
-- leaderboard until it plays again (both queries filter games_played > 0), so
-- the board reads empty rather than showing a field of identical 1000s.
UPDATE bots SET elo_rating = 1000, previous_elo = 1000, games_played = 0;
UPDATE user_elo_ratings SET elo_rating = 1000, previous_elo = 1000, games_played = 0;
