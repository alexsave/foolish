-- Spectator views: the SHARED, fully-masked (seat -1) view of a game, readable
-- by ANY authenticated user (docs/PLAYER_VIEWS.md). This replaces the get_game
-- edge function's spectate path — a non-participant has no player_views row
-- (that table is keyed by auth.uid()), so spectators read their initial snapshot
-- from here and get live updates over the RLS-guarded game-<id> broadcast.
--
-- `view` is FULLY masked (every hand a card-back, deck order hidden), so exposing
-- it to all authenticated users is safe — it must NEVER carry the raw games.state.
-- One row per game, written by the service role in commit_game / create_game's
-- version fence, alongside the per-player rows.

CREATE TABLE IF NOT EXISTS spectator_views (
  game_id    TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  view       TEXT NOT NULL,            -- fully-masked packed spectator envelope (bare hex), decodable by decodePackedGame
  version    BIGINT NOT NULL,          -- mirrors games.version
  status     TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

ALTER TABLE spectator_views ENABLE ROW LEVEL SECURITY;

-- The row is FULLY masked (seat -1), so ANY authenticated user may read it —
-- exactly the spectate case (a non-participant viewing a game). Mirrors the
-- game-<id> broadcast policy (authenticated-only). No client writes (service
-- role, which bypasses RLS, is the sole writer via commit_game / create_game).
DROP POLICY IF EXISTS "Authenticated can read spectator views" ON spectator_views;
CREATE POLICY "Authenticated can read spectator views" ON spectator_views
  FOR SELECT USING ((select auth.role()) = 'authenticated');
GRANT SELECT ON public.spectator_views TO authenticated;

-- =============================================================================
-- commit_game: add p_spectator (fully-masked seat -1 view) written in the same
-- version fence as the per-player p_views rows. Adding a parameter changes the
-- signature, so DROP the old 11-arg overload first (p_spectator DEFAULT NULL
-- keeps the still-live edge functions — which don't pass it — working during the
-- deploy window).
-- =============================================================================
DROP FUNCTION IF EXISTS commit_game(TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB, TEXT, TEXT, BOOLEAN, TEXT, JSONB);

CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_state            TEXT    DEFAULT NULL,  -- packed kernel state blob (hex); NULL leaves the column unchanged (never-dealt games)
  p_logs_packed      TEXT    DEFAULT NULL,  -- this move's logwire records (bare hex), appended under the version fence
  p_logs_reset       BOOLEAN DEFAULT FALSE, -- session reset (GAME_START in the records): replace instead of append
  p_game_seed        TEXT    DEFAULT NULL,  -- deal seed (hex); set once at the deal, NULL on every other commit leaves it unchanged
  p_views            JSONB   DEFAULT NULL,  -- per-participant masked view cache rows [{player_id,view,status}]; NULL leaves player_views untouched
  p_spectator        TEXT    DEFAULT NULL   -- fully-masked seat -1 spectator view (bare hex); NULL leaves spectator_views untouched
) RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_new_version BIGINT;
  g games%ROWTYPE;
BEGIN
  g := jsonb_populate_record(NULL::games, p_game);

  UPDATE games SET
    name = g.name, deck_length = g.deck_length, discard_pile_length = g.discard_pile_length,
    flipped = g.flipped, players = g.players, status = g.status, power_suit = g.power_suit,
    first_attacker = g.first_attacker, defender = g.defender, table_battles = g.table_battles,
    elimination_order = g.elimination_order, good_timestamp = g.good_timestamp,
    good_players = g.good_players,
    -- A WAITING commit is the `continue` reset (or a lobby op): the finished
    -- session's volatile state must NOT survive into the new lobby. A stale
    -- blob desyncs from the mutable lobby roster (seat-count mismatches brick
    -- every subsequent load) and leaks the previous session's hands through
    -- the blob-authoritative loaders. COALESCE alone never cleared it.
    state = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_state, state) END,
    -- Same discipline as `state`: a WAITING reset clears the finished session's
    -- seed so the next deal writes a fresh one; every dealt commit sets it (deal)
    -- or leaves it (COALESCE with NULL).
    game_seed = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_game_seed, game_seed) END,
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(p_logs_packed, '')
      WHEN g.status = 'waiting' THEN ''
      ELSE COALESCE(logs_packed, '') || COALESCE(p_logs_packed, '')
    END,
    updated_at = now(), version = version + 1
  WHERE id = p_game_id AND version = p_expected_version
  RETURNING version INTO v_new_version;

  IF NOT FOUND THEN
    RETURN jsonb_build_object('status', 'conflict');
  END IF;

  IF p_deck IS NOT NULL THEN
    INSERT INTO game_decks (game_id, deck) VALUES (p_game_id, p_deck)
    ON CONFLICT (game_id) DO UPDATE SET deck = EXCLUDED.deck, updated_at = now();
  END IF;

  IF p_hands IS NOT NULL AND jsonb_array_length(p_hands) > 0 THEN
    INSERT INTO player_hands (game_id, player_id, hand, awaiting_attack)
    SELECT p_game_id, (h->>'player_id')::uuid, h->'hand',
           COALESCE((h->>'awaiting_attack')::bool, false)
    FROM jsonb_array_elements(p_hands) AS h
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET hand = EXCLUDED.hand, awaiting_attack = EXCLUDED.awaiting_attack, updated_at = now();
  END IF;

  IF p_bot_hands IS NOT NULL AND jsonb_array_length(p_bot_hands) > 0 THEN
    INSERT INTO bot_hands (game_id, bot_id, hand, awaiting_attack)
    SELECT p_game_id, (b->>'bot_id')::uuid, b->'hand',
           COALESCE((b->>'awaiting_attack')::bool, false)
    FROM jsonb_array_elements(p_bot_hands) AS b
    ON CONFLICT (game_id, bot_id) DO UPDATE
      SET hand = EXCLUDED.hand, awaiting_attack = EXCLUDED.awaiting_attack, updated_at = now();
  END IF;

  -- The session log is the packed logs_packed column (handled in the UPDATE
  -- above); per-record game_logs rows were retired in migration 20260708120000.

  -- player_views personalized-view cache (docs/PLAYER_VIEWS.md), written in THIS
  -- version-fenced transaction so it can never be torn from games.state. p_views
  -- is the CURRENT participants' masked rows; NULL means "leave the cache
  -- untouched" (a view-build failure upstream — the game still commits). An
  -- empty array means "no human participants" and correctly prunes every row.
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', v_new_version, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();

    -- Prune rows for players no longer in the game (exited / removed), so a
    -- participant who left stops seeing the game in their dashboard list.
    DELETE FROM player_views
    WHERE game_id = p_game_id
      AND player_id NOT IN (
        SELECT (v->>'player_id')::uuid FROM jsonb_array_elements(p_views) AS v
      );
  END IF;

  -- The shared spectator view (seat -1), same version fence. One row per game.
  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, p_spectator, v_new_version, g.status, now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version);
END;
$$;

-- =============================================================================
-- create_game: add p_spectator (fully-masked seat -1 view, version 0) seeded in
-- the same transaction as the creator's player_views row. DROP the old 5-arg
-- overload first (signature change).
-- =============================================================================
DROP FUNCTION IF EXISTS create_game(TEXT, TEXT, UUID, JSONB, JSONB);

CREATE OR REPLACE FUNCTION create_game(
  p_game_id   TEXT,
  p_name      TEXT,
  p_player_id UUID,
  p_players   JSONB,
  p_views     JSONB DEFAULT NULL,  -- creator's masked view cache row(s); version 0
  p_spectator TEXT  DEFAULT NULL   -- fully-masked seat -1 spectator view (bare hex); version 0
) RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  INSERT INTO games (id, name, players, status)
    VALUES (p_game_id, p_name, p_players, 'waiting');

  INSERT INTO game_decks (game_id, deck)
    VALUES (p_game_id, '[]'::jsonb);

  INSERT INTO player_hands (game_id, player_id, hand, awaiting_attack)
    VALUES (p_game_id, p_player_id, '[]'::jsonb, false);

  -- Seed the player_views dashboard cache for the creator in the same
  -- transaction, so the new lobby is immediately readable from the client's
  -- direct player_views SELECT (docs/PLAYER_VIEWS.md). version 0 = the initial
  -- games.version.
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', 0, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  -- The shared spectator view (seat -1) for the new lobby.
  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, p_spectator, 0, 'waiting', now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;
