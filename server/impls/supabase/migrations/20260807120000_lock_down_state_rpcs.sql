-- SECURITY: restrict the state-mutating SECURITY DEFINER RPCs to the service role.
--
-- commit_game / create_game / the bot-lease helpers are SECURITY DEFINER (they run
-- as their owner and bypass RLS by design — the edge functions call them with the
-- service-role key). But Postgres grants EXECUTE to PUBLIC on every function by
-- default, and nothing here ever revoked it, so `anon`/`authenticated` could call
-- them straight through PostgREST (`POST /rest/v1/rpc/commit_game`, …). Because the
-- functions bypass RLS, that let any signed-in user rewrite ANY game's public state
-- and ANY player's hand (games.version is a client-readable column, so the CAS fence
-- is not a barrier) — a full authorization bypass over the app-layer checks in the
-- edge functions (verify_player_in_game, etc.), plus a griefing/DoS vector against
-- every live game.
--
-- This mirrors the lockdown migration 20260714120000 already applied to
-- delete_account. No client calls these RPCs directly (every caller is an edge
-- function using SUPABASE_SERVICE_ROLE_KEY), so revoking client EXECUTE changes no
-- app behavior. Idempotent; safe to re-run. The pg_proc loop covers every overload
-- that CREATE OR REPLACE with added DEFAULT params may have left in a live DB.

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
    -- Revoke the default PUBLIC grant (which anon/authenticated inherit) …
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC;', fn);
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM anon;', fn);
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM authenticated;', fn);
    -- … and re-grant only to the service role (the edge functions). REVOKE FROM
    -- PUBLIC also strips service_role's implicit grant, so this GRANT is required.
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO service_role;', fn);
  END LOOP;
END $$;
