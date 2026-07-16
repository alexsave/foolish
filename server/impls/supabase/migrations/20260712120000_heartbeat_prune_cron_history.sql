-- pg_cron's `cron.job_run_details` grows unbounded: it appends a row on EVERY
-- job run and never prunes. With the bot-heartbeat firing every 10s (~8,600
-- runs/day) it reached ~224k rows / 151 MB in 26 days and, together with pg_net's
-- response-log bloat, blew past the 500 MB free-tier storage cap.
--
-- Fix: fold a lightweight prune into the heartbeat job itself (per the operator's
-- request — no separate cron entry). The DELETE prepended below scans only the
-- last ~2 days of rows (a few thousand) and removes anything older, so the table
-- stays small forever. end_time IS NULL for an in-flight run, and `NULL < ...` is
-- NULL, so currently-running jobs are never deleted.
--
-- The one-time reclaim (TRUNCATE cron.job_run_details + VACUUM FULL
-- net._http_response) was run out-of-band; VACUUM cannot run inside a migration
-- transaction and TRUNCATE's disk was already reclaimed. This migration only
-- makes the ongoing prevention reproducible.
--
-- NOTE ON pg_net: net._http_response is self-pruned by the extension (~6h TTL), so
-- its LIVE rows stay small; its 296 MB was dead-tuple bloat that only VACUUM FULL
-- reclaims. Extra DELETEs wouldn't help, so we don't prune it from cron. If it
-- bloats again over months, re-run: VACUUM (FULL) net._http_response;

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
  );
  $$
);
