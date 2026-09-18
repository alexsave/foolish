-- pg_net and pg_cron, as the hosted platform hands them to a migration.
--
-- e2e/schema.sql shims the Supabase platform's SCHEMAS (auth, realtime); this
-- shims its two EXTENSIONS. The hosted project has pg_net 0.14.0 and pg_cron 1.6
-- installed, the bot-heartbeat cron job lives in `cron.job`, and every heartbeat
-- POST leaves a row in `net._http_response`. A migration that maintains either
-- table cannot be replayed on a bare Postgres without them, so the two tables and
-- the one function a migration calls are declared here, column for column as
-- `information_schema.columns` reports them on wngpfwmwkltonwosqflx.
--
-- Only the SHAPE is real. There is no background worker here: nothing sends an
-- HTTP request, nothing reaps a response on the TTL, nothing fires a job. A test
-- that wants any of that drives it itself, which is the point - the production
-- behaviour under test is what a scheduled SQL command does to a table, and that
-- part is genuine Postgres.
--
-- Re-runnable: IF NOT EXISTS / OR REPLACE throughout, so a suite may apply it
-- after e2e/schema.sql or after applySchema() without caring which ran.

CREATE SCHEMA IF NOT EXISTS net;
CREATE SCHEMA IF NOT EXISTS cron;

-- pg_net's response log. UNLOGGED and with no primary key, exactly as pg_net
-- creates it; `created` carries the default that makes the extension's TTL sweep
-- possible, and the lone index on it is the one the dashboard reports as
-- `net._http_response_created_idx`.
CREATE UNLOGGED TABLE IF NOT EXISTS net._http_response (
    id            bigint,
    status_code   integer,
    content_type  text,
    headers       jsonb,
    content       text,
    timed_out     boolean,
    error_msg     text,
    created       timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS _http_response_created_idx ON net._http_response (created);

-- Autovacuum off, because that is what the hosted table's numbers say it is in
-- practice: over its whole lifetime `pg_stat_all_tables` recorded ONE autovacuum
-- (2026-08-05) against 433,295 inserts and 431,140 deletes. The reason is in the
-- migration this fixture supports - the TTL sweep's own scan prunes dead tuples
-- on the page, which holds n_dead_tup near zero and keeps the table permanently
-- under the autovacuum threshold, so the free space map is never written and
-- every insert extends the file. Declaring it off here makes the harness
-- reproduce that deterministically instead of racing a local autovacuum daemon.
ALTER TABLE net._http_response SET (autovacuum_enabled = false);

CREATE TABLE IF NOT EXISTS cron.job (
    jobid     bigserial PRIMARY KEY,
    schedule  text    NOT NULL,
    command   text    NOT NULL,
    nodename  text    NOT NULL DEFAULT 'localhost',
    nodeport  integer NOT NULL DEFAULT coalesce(inet_server_port(), 5432),
    database  text    NOT NULL DEFAULT current_database(),
    username  text    NOT NULL DEFAULT CURRENT_USER,
    active    boolean NOT NULL DEFAULT true,
    jobname   text
);
-- pg_cron's own uniqueness, and the reason cron.schedule() is an upsert rather
-- than an insert: re-scheduling a job by name REPLACES it. Every migration in
-- this repo that touches the heartbeat leans on that for idempotence.
CREATE UNIQUE INDEX IF NOT EXISTS job_jobname_username_uniq ON cron.job (jobname, username);

CREATE TABLE IF NOT EXISTS cron.job_run_details (
    jobid          bigint,
    runid          bigserial PRIMARY KEY,
    job_pid        integer,
    database       text,
    username       text,
    command        text,
    status         text,
    return_message text,
    start_time     timestamptz,
    end_time       timestamptz
);

CREATE OR REPLACE FUNCTION cron.schedule(job_name text, schedule text, command text)
RETURNS bigint
LANGUAGE sql
AS $$
    INSERT INTO cron.job (jobname, schedule, command)
    VALUES (job_name, schedule, command)
    ON CONFLICT (jobname, username) DO UPDATE
        SET schedule = EXCLUDED.schedule,
            command  = EXCLUDED.command,
            active   = true
    RETURNING jobid;
$$;

CREATE OR REPLACE FUNCTION cron.unschedule(job_name text)
RETURNS boolean
LANGUAGE sql
AS $$
    DELETE FROM cron.job WHERE jobname = job_name RETURNING true;
$$;
