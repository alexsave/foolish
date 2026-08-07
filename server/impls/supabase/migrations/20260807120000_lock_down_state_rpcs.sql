-- SECURITY: restrict the state-mutating SECURITY DEFINER RPCs to the service role.
--
-- These functions bypass RLS by design; the edge functions call them with the
-- service-role key. But Postgres grants EXECUTE to PUBLIC on every function by
-- default and nothing revoked it, so anon (not even signed in) and authenticated
-- could call them through PostgREST (POST /rest/v1/rpc/commit_game) and rewrite
-- any game's state or any player's hand. games.version is client-readable, so
-- the CAS fence is no barrier.
--
-- Mirrors the delete_account lockdown in 20260714120000. No client calls these
-- RPCs, so this changes no app behavior. Idempotent. The pg_proc loop is
-- deliberate: CREATE OR REPLACE with added DEFAULT params leaves stale overloads
-- behind, and naming signatures would miss them.

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
