-- ---------------------------------------------------------------------------
-- The bot heartbeat stops paying for games nobody is playing
-- ---------------------------------------------------------------------------
--
-- Measured on hosted (wngpfwmwkltonwosqflx, read-only) on 2026-09-18:
--
--   cron.job          one bot-heartbeat job, '10 seconds', 8,622 runs in 24 h
--                     => 258,660 net.http_post calls / 30 days, every one of
--                     them an edge invocation, against a 500,000 free-tier
--                     month of which the dashboard had already spent 277,366.
--   games             90 rows. 53 waiting, 36 game_over, and ONE playing:
--                     24a407, created 2026-07-13, needs_bots = true.
--   24a407            version 323,658 and round_epoch 321,086, both frozen -
--                     two dumps 410 s apart read the same numbers - while
--                     updated_at advanced the whole time, and sat at exactly
--                     bot_lease_until + 1 second in both.
--
-- That +1 second is release_bot_lease: it sets bot_lease_until = now() - 1s and
-- the update_games_updated_at trigger stamps updated_at = now() on the same
-- UPDATE. So the heartbeat, driving a game abandoned 67 days ago, refreshes the
-- very column the scan's one-hour ABANDON bound reads. The guard that was meant
-- to stop this is the thing the loop keeps resetting, and it can never fire.
--
-- Two halves, and the second does not work without the first.
--
-- 1. A LIVENESS CLOCK THE LOOP CANNOT WIND
--
-- games.last_commit_at: now() at the moment `version` changed, and OLD's value
-- at every other moment. Not "when the row was written" - `updated_at` already
-- means that, and means it for the lease, for a backfill, for a column added by
-- some migration next year. `version` moves in exactly ONE place in the whole
-- schema - commit_table's `version = version + 1`, the contract migration
-- (20260918210000) having retired commit_game - and that is a kernel commit: a
-- move happened. Nothing else can move it, and nothing can move last_commit_at
-- without it, because the trigger below writes the column on EVERY insert and
-- update, so a writer that supplies its own value has it overwritten rather
-- than honoured.
--
-- The rejected alternative was to stop the lease RPCs bumping updated_at (an
-- `updated_at = updated_at` in their UPDATE, or exempting them from the
-- trigger). It fixes the two writers we know about and leaves the guard resting
-- on the assumption that nobody ever adds a third - the next innocuous UPDATE
-- games, from a backfill or an admin tool or a column nobody has thought of
-- yet, silently revives exactly this bug, with no test that could see it coming.
-- A derived column cannot be defeated that way: an unrelated writer would have
-- to bump `version`, and bumping `version` breaks the CAS that every commit and
-- every client's optimistic concurrency runs on, so it fails loudly instead.
--
-- Backfilled from updated_at rather than from now(): updated_at is an upper
-- bound on the row's last commit, so no game that IS live can be excluded by
-- this migration. 24a407 keeps being driven for one more hour and then stops.
--
-- 2. AN IDLE TICK THAT COSTS NOTHING
--
-- The job's POST gets a WHERE. The cadence does not change: 10 seconds while a
-- game is live, because gating costs no latency and widening the interval buys
-- the same saving by making bots slower for the people who ARE playing.
--
-- The gate is deliberately WEAKER than the scan's own filter - it asks only for
-- a row the kernel says needs bots whose last commit is inside the same one-hour
-- window, and leaves the 10-second staleness test where it already lives, in
-- the function. So every tick that would have dispatched still posts, and the
-- ticks that now cost nothing are exactly the ticks that used to post and
-- dispatch nothing. Behaviour while somebody is playing is unchanged.
--
-- What "something to drive" means is not a guess: games.needs_bots IS the
-- kernel's verdict (c/src/table.h table_needs_bots - PLAYING, and a bot seat
-- still IN), written by every commit, and it is already the scan's predicate.
-- A WAITING lobby is never the heartbeat's business: needs_bots is false for
-- every status but playing, and the lobby paths wake the bots inline through
-- scheduleBotLoop on the commit that starts the game, so the tick that opens
-- the gate for a new table is the same tick it would have been driven on. All
-- 53 waiting rows on hosted were last touched on 2026-07-13 or earlier, and not
-- one of them has been driven by a heartbeat in its life.
--
-- IDEMPOTENT: every statement is IF NOT EXISTS / OR REPLACE / a by-name
-- re-CREATE, the backfill matches no row on a second run, and cron.schedule()
-- upserts on (jobname, username), so `supabase db push` may replay this.
-- ---------------------------------------------------------------------------

-- ---------------------------------------------------------------------------
-- 1. The column
-- ---------------------------------------------------------------------------
ALTER TABLE games ADD COLUMN IF NOT EXISTS last_commit_at TIMESTAMPTZ;

COMMENT ON COLUMN games.last_commit_at IS
  'now() at the moment `version` last changed, and OLD''s value at every other moment (trigger games_stamp_last_commit). The bot heartbeat''s abandon guard: a lease acquire/release, or any other write that is not a kernel commit, leaves it alone. updated_at cannot serve - the lease RPCs bump it through update_games_updated_at, which is what made the guard unfirable.';

-- Seeded from updated_at, which is an upper bound on the row's last commit, so
-- no live game can be excluded by this migration. The updated_at trigger is held
-- off for the statement: a backfill is not an update anybody made to a game, and
-- rewriting 90 rows' updated_at would be this migration doing the very thing it
-- exists to stop. Matches no row on a replay.
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM games WHERE last_commit_at IS NULL) THEN
    ALTER TABLE games DISABLE TRIGGER update_games_updated_at;
    UPDATE games SET last_commit_at = updated_at AT TIME ZONE 'UTC' WHERE last_commit_at IS NULL;
    ALTER TABLE games ENABLE TRIGGER update_games_updated_at;
  END IF;
END $$;

ALTER TABLE games ALTER COLUMN last_commit_at SET DEFAULT now();
ALTER TABLE games ALTER COLUMN last_commit_at SET NOT NULL;

-- ---------------------------------------------------------------------------
-- 2. The trigger that owns it
-- ---------------------------------------------------------------------------
-- Writes the column on every INSERT and every UPDATE, so the value is a
-- function of "did version change" and nothing else. A writer cannot set it,
-- cannot preserve a stale one, and cannot refresh it without committing.
CREATE OR REPLACE FUNCTION games_stamp_last_commit()
RETURNS TRIGGER
LANGUAGE plpgsql
SET search_path = ''
AS $$
BEGIN
  IF TG_OP = 'INSERT' OR NEW.version IS DISTINCT FROM OLD.version THEN
    NEW.last_commit_at := now();
  ELSE
    NEW.last_commit_at := OLD.last_commit_at;
  END IF;
  RETURN NEW;
END;
$$;

-- Deliberately not revoked: it RETURNS trigger, which PostgREST does not
-- expose, and the lockdown in 20260807120000 / 20260917000000 skips trigger
-- functions for that reason. Revoking here would put the migration-built schema
-- and seed.sql into disagreement, which is exactly what e2e/db_migration_grants
-- exists to catch.
DROP TRIGGER IF EXISTS games_stamp_last_commit ON games;
CREATE TRIGGER games_stamp_last_commit
  BEFORE INSERT OR UPDATE ON games
  FOR EACH ROW
  EXECUTE FUNCTION games_stamp_last_commit();

-- ---------------------------------------------------------------------------
-- 3. The gate's index
-- ---------------------------------------------------------------------------
-- idx_games_bot_scan (updated_at) WHERE needs_bots stays: it is the SCAN's
-- range, and the scan still bounds staleness on updated_at. This one is the
-- GATE's, and it leads on last_commit_at so the EXISTS below is a range scan
-- that stops at the first live row instead of walking every bot game that ever
-- stalled. Both are partial on needs_bots, which is one row on hosted today.
CREATE INDEX IF NOT EXISTS idx_games_bot_gate ON games (last_commit_at) WHERE needs_bots;

-- ---------------------------------------------------------------------------
-- 4. The gated job
-- ---------------------------------------------------------------------------
-- Shape notes, because pg_cron sends this as a SIMPLE QUERY on this instance
-- (cron.use_background_workers is off), which makes the two statements one
-- implicit transaction block:
--   * Both statements are happy there. The DELETE is the cron-history prune
--     migration 20260712120000 folded in; it stays, it is not an edge
--     invocation, and it is what keeps cron.job_run_details from growing
--     without bound.
--   * `SELECT f() WHERE <false>` returns zero rows and never evaluates f().
--     That is what makes the tick free: no request is queued, no response row
--     is written, no function is invoked.
--   * now() is transaction time, so the gate and the DELETE see one clock.
-- The interval below is the scan's ABANDON_MS (functions/bot-heartbeat/index.ts)
-- and the two are asserted equal in e2e/heartbeat_gate.test.ts - a gate NARROWER
-- than the scan would strand a game the scan was willing to drive.
SELECT cron.schedule(
  'bot-heartbeat',
  '10 seconds',
  $$
  DELETE FROM cron.job_run_details WHERE end_time < now() - interval '2 days';
  SELECT net.http_post(
    url := 'https://wngpfwmwkltonwosqflx.supabase.co/functions/v1/bot-heartbeat',
    headers := jsonb_build_object(
      'Content-Type',  'application/json',
      'Authorization', 'Bearer ' || (SELECT decrypted_secret FROM vault.decrypted_secrets WHERE name = 'service_role_key'),
      'apikey',        (SELECT decrypted_secret FROM vault.decrypted_secrets WHERE name = 'service_role_key')
    ),
    body := '{}'::jsonb,
    timeout_milliseconds := 5000
  )
  WHERE EXISTS (
    SELECT 1 FROM games
    WHERE needs_bots
      AND last_commit_at > now() - interval '1 hour'
  );
  $$
);

-- To check on it later:
--   select jobname, schedule, active from cron.job where jobname = 'bot-heartbeat';
--   select count(*) from net._http_response where created > now() - interval '1 hour';  -- posts, not ticks
--   select id, version, updated_at, last_commit_at from games where needs_bots;
