-- SECURITY: relock the SECURITY DEFINER RPCs, and take table writes away from
-- the client roles.
--
-- 1. commit_game has been callable by anon since 20260906120000.
--
-- That migration changed commit_game's signature, so it DROPped the function and
-- CREATEd it again. CREATE OR REPLACE keeps a function's grants; a new function
-- does not inherit anything. It gets EXECUTE for PUBLIC from Postgres and for
-- anon and authenticated from Supabase's default privileges, which is exactly
-- the exposure 20260807120000 had closed. Nothing repeated that lockdown, so the
-- anon key shipped in the web bundle could POST /rest/v1/rpc/commit_game and
-- rewrite any game's state, roster and views. seed.sql did repeat it, and
-- e2e/db_grants.test.ts only ever loaded seed.sql, so CI stayed green.
--
-- This time the lockdown is not a list of names. It covers every SECURITY
-- DEFINER function in public, including every overload, because a definer
-- function bypasses RLS and no client calls any of them: every .rpc() caller is
-- an edge function holding the service-role key. Trigger functions are skipped;
-- PostgREST does not expose them and revoking EXECUTE does not stop a trigger.
-- A function a client is meant to call must be granted explicitly, after this.
--
-- 2. games accepted a direct INSERT from any signed-in user.
--
-- The "Authenticated users can create games" policy dates from before the
-- create edge function and its service-role create_game RPC, and nothing has
-- inserted a game with a user token since. Supabase grants ALL on every public
-- table to anon and authenticated, and 20260707140000 only took SELECT back, so
-- POST /rest/v1/games wrote a row whose players, seats, status and state blob
-- the caller chose. The action endpoint then plays that row like any other game
-- and scores its end into the ELO of whoever it lists.
--
-- The policy goes, and so does every client write privilege on every public
-- table, not just games. RLS denies the other writes today, but only because
-- each table's policies happen to say so; the privileges are what make a
-- missing or wrong policy harmless. The one write a client does make on purpose
-- is sending chat, a direct INSERT into chat_messages gated by the "Players can
-- send chat messages to their games" policy, and it is granted back. SELECT is
-- untouched, including games' column-level SELECT grant.
--
-- Idempotent. Changes no app behavior: the edge functions use the service role,
-- which keeps EXECUTE and bypasses RLS. e2e/db_migration_grants.test.ts replays
-- the migrations over the hosted schema under Supabase's default privileges and
-- asserts both invariants, so the next migration that drops and recreates a
-- definer function, or adds a table, without this fails CI.

-- ---------------------------------------------------------------------------
-- 1. SECURITY DEFINER functions: service_role only
-- ---------------------------------------------------------------------------
DO $$
DECLARE
  fn regprocedure;
BEGIN
  FOR fn IN
    SELECT p.oid::regprocedure
    FROM pg_proc p
    JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'public'
      AND p.prosecdef
      AND p.prorettype <> 'trigger'::regtype
  LOOP
    -- All three: revoking from PUBLIC alone leaves the explicit anon and
    -- authenticated grants from the default privileges standing.
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC, anon, authenticated;', fn);
    -- REVOKE FROM PUBLIC also strips service_role's implicit grant, so re-grant.
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO service_role;', fn);
  END LOOP;
END $$;

-- ---------------------------------------------------------------------------
-- 2. Table writes: service_role only, except sending chat
-- ---------------------------------------------------------------------------
DROP POLICY IF EXISTS "Authenticated users can create games" ON games;

DO $$
DECLARE
  rel regclass;
BEGIN
  FOR rel IN
    SELECT c.oid::regclass
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'public'
      AND c.relkind IN ('r', 'p', 'v', 'm', 'f')
  LOOP
    EXECUTE format(
      'REVOKE INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER ON %s FROM PUBLIC, anon, authenticated;',
      rel);
  END LOOP;
END $$;

GRANT INSERT ON public.chat_messages TO authenticated;
