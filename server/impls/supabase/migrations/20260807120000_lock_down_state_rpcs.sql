-- SECURITY: restrict the state-mutating SECURITY DEFINER RPCs to the service role.
--
-- These functions bypass RLS by design; the edge functions call them with the
-- service-role key. But PostgREST exposes them as POST /rest/v1/rpc/<name>, and
-- both anon and authenticated hold EXECUTE: Postgres grants it to PUBLIC by
-- default, and the live schema ALSO carries explicit GRANT ALL ... TO anon /
-- authenticated. Nothing ever revoked either. anon is the unauthenticated role
-- behind the anon key shipped in the web bundle, so this was reachable by anyone
-- with no account at all, to rewrite any game's state or any player's hand.
-- games.version is client-readable, so the CAS fence is no barrier.
--
-- Revoking from PUBLIC alone would leave the explicit anon grant standing and
-- look like it worked, hence all three REVOKEs.
--
-- Mirrors the delete_account lockdown in 20260714120000. No client calls these
-- RPCs, so this changes no app behavior. Idempotent. The pg_proc loop is
-- deliberate: CREATE OR REPLACE with added DEFAULT params leaves stale overloads
-- behind, and naming signatures would miss them.
--
-- e2e/db_grants.test.ts asserts the general invariant so the next such function
-- fails CI instead of shipping open.

DO $$
DECLARE
  fn regprocedure;
BEGIN
  FOR fn IN
    SELECT p.oid::regprocedure
    FROM pg_proc p
    JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'public'
      AND p.proname IN (
        'commit_game', 'create_game',
        'try_acquire_bot_lease', 'release_bot_lease', 'renew_bot_lease'
      )
  LOOP
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC;', fn);
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM anon;', fn);
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM authenticated;', fn);
    -- REVOKE FROM PUBLIC also strips service_role's implicit grant, so re-grant.
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO service_role;', fn);
  END LOOP;
END $$;
