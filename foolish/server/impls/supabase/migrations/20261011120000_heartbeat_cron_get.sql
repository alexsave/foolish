-- The bot heartbeat's cron becomes a GET with no body.
--
-- The job posted `'{}'::jsonb` at bot-heartbeat every ten seconds. The function
-- never read it: a scan is the bare URL and a drive is `?game=<id>`. It was
-- there because pg_net's http_post takes a jsonb body and nothing else, so the
-- one way to send no JSON is to not POST. net.http_get sends the same headers
-- to the same URL and nothing after them.
--
-- cron.schedule() upserts on the job name, so this replaces the command of the
-- one 'bot-heartbeat' job and leaves its ten-second schedule and its gate as
-- they are. The statement is seed.sql's, character for character
-- (e2e/validation/migration_guard_validation.test.ts holds the two equal).
--
-- The deployed function already answers a GET the way it answers a POST (it
-- looks at the URL and at nothing else), so this is safe on either side of the
-- function deploy.
--
-- ONE GUARDED BLOCK, per migrations/README.md: `supabase start` and `db reset`
-- apply migrations BEFORE seed.sql, and seed.sql is the schema, so on every
-- database except hosted this would run before `games` exists. Everywhere else
-- seed.sql schedules this command itself.

DO $$
BEGIN
    IF to_regclass('public.games') IS NULL THEN
        RAISE NOTICE 'heartbeat_cron_get: no games table here, so this database is built from seed.sql, which already schedules the GET - nothing to migrate';
        RETURN;
    END IF;
    IF to_regprocedure('cron.schedule(text,text,text)') IS NULL THEN
        RAISE NOTICE 'heartbeat_cron_get: pg_cron is not installed, so there is no job to change';
        RETURN;
    END IF;

  PERFORM cron.schedule(
    'bot-heartbeat',
    '10 seconds',
    $job$
  DELETE FROM cron.job_run_details WHERE end_time < now() - interval '2 days';
  SELECT net.http_get(
    url := 'https://wngpfwmwkltonwosqflx.supabase.co/functions/v1/bot-heartbeat',
    headers := jsonb_build_object(
      'Authorization', 'Bearer ' || (SELECT decrypted_secret FROM vault.decrypted_secrets WHERE name = 'service_role_key'),
      'apikey',        (SELECT decrypted_secret FROM vault.decrypted_secrets WHERE name = 'service_role_key')
    ),
    timeout_milliseconds := 5000
  )
  WHERE EXISTS (
    SELECT 1 FROM games
    WHERE needs_bots
      AND last_commit_at > now() - interval '1 hour'
  );
  $job$
  );
END $$;
