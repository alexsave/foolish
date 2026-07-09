-- Client-direct create: make create_game safe to call DIRECTLY from the
-- authenticated client, so the `create` edge function can be deleted (it only
-- ever wrapped this RPC with a cold edge boot + JWKS auth + a cold-isolate
-- PostgREST hop — the ~594ms create latency).
--
-- The security boundary is auth.uid(): for an authenticated caller we derive the
-- creator's identity from the VERIFIED JWT and ignore any client-supplied
-- identity params (so a client can't create a game as another user). The
-- explicit-param path is kept only for a service-role caller (no user JWT) — and
-- for the deploy window, so the still-live create edge function keeps working
-- until the function deploy removes it. EXECUTE is locked to authenticated +
-- service_role (never anon).
--
-- Arg types are unchanged (TEXT, TEXT, UUID, JSONB, JSONB) — this only adds
-- DEFAULTs + a new body — so CREATE OR REPLACE needs no DROP.

CREATE OR REPLACE FUNCTION create_game(
  p_game_id   TEXT,
  p_name      TEXT  DEFAULT NULL,
  p_player_id UUID  DEFAULT NULL,
  p_players   JSONB DEFAULT NULL,
  p_views     JSONB DEFAULT NULL
) RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_uid      UUID := auth.uid();
  v_player   UUID;
  v_name     TEXT;
  v_players  JSONB;
  v_username TEXT;
BEGIN
  IF v_uid IS NOT NULL THEN
    -- Authenticated client-direct call: identity from the verified token only.
    v_player   := v_uid;
    v_username := current_setting('request.jwt.claims', true)::jsonb -> 'user_metadata' ->> 'username';
    v_name     := coalesce(v_username, 'Player') || '''s Game';
    v_players  := jsonb_build_array(jsonb_build_object(
                    'player_id', v_uid, 'name', v_username, 'status', 'idle', 'is_ai', false));
  ELSE
    -- Service-role caller (no user JWT): trust the explicit params.
    v_player  := p_player_id;
    v_name    := p_name;
    v_players := p_players;
  END IF;

  INSERT INTO games (id, name, players, status)
    VALUES (p_game_id, v_name, v_players, 'waiting');

  INSERT INTO game_decks (game_id, deck)
    VALUES (p_game_id, '[]'::jsonb);

  INSERT INTO player_hands (game_id, player_id, hand, awaiting_attack)
    VALUES (p_game_id, v_player, '[]'::jsonb, false);

  -- Only the caller's own player_views row is honored.
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, v_player, v->>'view', 0, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    WHERE (v->>'player_id')::uuid = v_player
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;

REVOKE ALL ON FUNCTION create_game(TEXT, TEXT, UUID, JSONB, JSONB) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION create_game(TEXT, TEXT, UUID, JSONB, JSONB) TO authenticated, service_role;
