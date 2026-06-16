-- pg_cron heartbeat that keeps bot games progressing without depending on a
-- browser tab's poll. It is a DUMB TRIGGER: every 10s it POSTs the bot-heartbeat
-- edge function's SCAN endpoint (empty body). ALL "which games need driving" logic
-- lives in the function (TypeScript), not here.
--
-- PREREQUISITES (do these first):
--   1. Deploy the `bot-heartbeat` edge function.
--   2. Store the service-role key in Vault so it isn't written in plaintext here:
--        select vault.create_secret('<YOUR_SERVICE_ROLE_KEY>', 'service_role_key');
--      (Settings → API has the service_role key. Re-run create_secret only once.)
--
-- Cost: pg_cron/pg_net are free. The only metered cost is edge invocations, and the
-- scan fires zero drives when no game qualifies.

CREATE EXTENSION IF NOT EXISTS pg_cron;
CREATE EXTENSION IF NOT EXISTS pg_net;

-- Idempotent: drop any prior version of the job before (re)creating it.
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM cron.job WHERE jobname = 'bot-heartbeat') THEN
    PERFORM cron.unschedule('bot-heartbeat');
  END IF;
END $$;

-- '10 seconds' uses pg_cron's sub-minute interval syntax (pg_cron >= 1.5; Supabase
-- ships a newer version). If your instance rejects it, fall back to '* * * * *'
-- (1 minute) — but that makes bots-only games lurch in 60s bursts.
SELECT cron.schedule(
  'bot-heartbeat',
  '10 seconds',
  $$
  SELECT net.http_post(
    url := 'https://wngpfwmwkltonwosqflx.supabase.co/functions/v1/bot-heartbeat',
    headers := jsonb_build_object(
      'Content-Type',  'application/json',
      'Authorization', 'Bearer ' || (SELECT decrypted_secret FROM vault.decrypted_secrets WHERE name = 'service_role_key'),
      'apikey',        (SELECT decrypted_secret FROM vault.decrypted_secrets WHERE name = 'service_role_key')
    ),
    body := '{}'::jsonb,
    timeout_milliseconds := 5000
  );
  $$
);

-- To inspect / stop later:
--   select * from cron.job where jobname = 'bot-heartbeat';
--   select * from cron.job_run_details where jobid = (select jobid from cron.job where jobname='bot-heartbeat') order by start_time desc limit 20;
--   select cron.unschedule('bot-heartbeat');
