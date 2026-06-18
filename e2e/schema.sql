-- Supabase platform shim for the bare-Postgres e2e harness.
--
-- This does NOT define the app schema — supabase/seed.sql is the single source
-- of truth for that and is applied (verbatim, unmodified) by applySchema()
-- right after this file. The shim only stands up the bits of the Supabase
-- PLATFORM that seed.sql's RLS policies / triggers reference at creation time,
-- so the real schema loads on a vanilla Postgres. The harness connects as a
-- superuser (RLS bypassed) and replaces Realtime with an in-process recorder,
-- so these objects are inert — they only need to EXIST, not behave.
--
-- Re-runnable: drops and recreates the schemas each call (per test-file setup).

DROP SCHEMA IF EXISTS public CASCADE;
CREATE SCHEMA public;
DROP SCHEMA IF EXISTS auth CASCADE;
CREATE SCHEMA auth;
DROP SCHEMA IF EXISTS realtime CASCADE;
CREATE SCHEMA realtime;

-- Roles the seed's RLS policies target (`TO authenticated` / `TO service_role`).
DO $$ BEGIN CREATE ROLE anon; EXCEPTION WHEN duplicate_object THEN NULL; END $$;
DO $$ BEGIN CREATE ROLE authenticated; EXCEPTION WHEN duplicate_object THEN NULL; END $$;
DO $$ BEGIN CREATE ROLE service_role; EXCEPTION WHEN duplicate_object THEN NULL; END $$;

-- auth: GoTrue's users table (the FK target + raw_user_meta_data the
-- enforce_username_not_bot trigger reads) and the uid()/role() the policies call.
CREATE TABLE auth.users (
  id UUID PRIMARY KEY,
  raw_user_meta_data JSONB
);
CREATE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS $$ SELECT NULL::uuid $$;
CREATE FUNCTION auth.role() RETURNS text LANGUAGE sql STABLE AS $$ SELECT 'service_role'::text $$;

-- realtime: the messages table + topic()/broadcast_changes() the seed's realtime
-- RLS policies and chat trigger reference. Inert here (no Realtime in the harness).
CREATE TABLE realtime.messages (
  topic TEXT,
  extension TEXT
);
CREATE FUNCTION realtime.topic() RETURNS text LANGUAGE sql STABLE AS $$ SELECT ''::text $$;
CREATE FUNCTION realtime.broadcast_changes(
  topic_name TEXT, event_name TEXT, operation TEXT,
  table_name TEXT, table_schema TEXT, new_record RECORD, old_record RECORD,
  level TEXT DEFAULT 'ROW'
) RETURNS void LANGUAGE plpgsql AS $$ BEGIN END $$;
