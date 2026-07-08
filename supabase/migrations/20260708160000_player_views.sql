-- player_views — a server-written, client-read PERSONALIZED VIEW CACHE
-- (docs/PLAYER_VIEWS.md).
--
-- Each player may see only their OWN hand + card-backs for everyone else: that
-- is FIELD-level masking inside one game row, which RLS cannot do (RLS hides
-- whole rows, not columns). Today the server unpacks games.state and emits a
-- per-viewer masked view through the get_game / get_my_games edge functions, so
-- every client read pays an edge cold start (~760ms floor).
--
-- This table flips that: mask at WRITE time (once per commit, per viewer) and
-- store each player's ALREADY-MASKED packed view in its own row. Now RLS
-- `player_id = auth.uid()` is SUFFICIENT — the row is pre-masked, nothing to
-- hide on read — so the client loads its dashboard list as a plain indexed
-- SELECT (no edge hop) and gets live pushes over Realtime. `view` is the same
-- packed single-game envelope the edge functions emit, decodable by the shared
-- decodePackedGame; it is masked for its owner and must NEVER carry raw
-- games.state.
--
-- Written ONLY by the service role, inside commit_game / create_game's
-- version-fenced transaction (the p_views param added below), so the cache can
-- never be torn from the authoritative state.

CREATE TABLE IF NOT EXISTS player_views (
  game_id    TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id  UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  view       TEXT NOT NULL,            -- MASKED packed view envelope (bare hex), decodable by decodePackedGame
  version    BIGINT NOT NULL,          -- mirrors games.version (optimistic token); client drops stale/reordered
  status     TEXT NOT NULL,            -- denormalized game_status for cheap list filtering/rendering
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (game_id, player_id)
);

-- dashboard list read: the caller's rows, newest game first.
CREATE INDEX IF NOT EXISTS idx_player_views_player ON player_views(player_id, updated_at DESC);

ALTER TABLE player_views ENABLE ROW LEVEL SECURITY;

-- Read ONLY your own rows; the row is already masked for its owner, so this
-- simple, auditable policy is the whole personalization boundary. NO client
-- writes: there is deliberately no INSERT/UPDATE/DELETE policy, so authenticated
-- can only SELECT; the service role (bypasses RLS) is the sole writer.
DROP POLICY IF EXISTS "Players can read their own views" ON player_views;
CREATE POLICY "Players can read their own views" ON player_views
  FOR SELECT USING (player_id = (select auth.uid()));

-- RLS gates ROWS; the table privilege must also be granted. SELECT only.
GRANT SELECT ON public.player_views TO authenticated;

-- Realtime: publish player_views so the client can subscribe to its own rows
-- (RLS-enforced) and receive live view pushes on every commit.
DO $$ BEGIN
  ALTER PUBLICATION supabase_realtime ADD TABLE public.player_views;
EXCEPTION
  WHEN undefined_object THEN NULL; -- no such publication: skip
  WHEN duplicate_object THEN NULL; -- already published: idempotent
END $$;

-- ---------------------------------------------------------------------------
-- Extend the writers to persist the cache in the SAME transaction.
--
-- Both gain a p_views param. Adding a parameter changes the signature (Postgres
-- would otherwise leave the old overload in place and make defaulted calls
-- ambiguous), so DROP the prior signature first, then recreate. p_views DEFAULTs
-- to NULL, so the currently-live edge functions (which call without it during
-- the expand-then-deploy window) keep working unchanged.
-- ---------------------------------------------------------------------------

DROP FUNCTION IF EXISTS commit_game(TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB, TEXT, TEXT, BOOLEAN, TEXT);

CREATE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_state            TEXT    DEFAULT NULL,
  p_logs_packed      TEXT    DEFAULT NULL,
  p_logs_reset       BOOLEAN DEFAULT FALSE,
  p_game_seed        TEXT    DEFAULT NULL,
  p_views            JSONB   DEFAULT NULL   -- per-participant masked view cache rows [{player_id,view,status}]
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
    state = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_state, state) END,
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

  -- player_views personalized-view cache, written in THIS version-fenced
  -- transaction so it can never be torn from games.state. NULL = leave the
  -- cache untouched (an upstream view-build failure — the game still commits);
  -- an empty array = no human participants, which prunes every row.
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', v_new_version, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();

    DELETE FROM player_views
    WHERE game_id = p_game_id
      AND player_id NOT IN (
        SELECT (v->>'player_id')::uuid FROM jsonb_array_elements(p_views) AS v
      );
  END IF;

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version);
END;
$$;

DROP FUNCTION IF EXISTS create_game(TEXT, TEXT, UUID, JSONB);

CREATE FUNCTION create_game(
  p_game_id   TEXT,
  p_name      TEXT,
  p_player_id UUID,
  p_players   JSONB,
  p_views     JSONB DEFAULT NULL   -- creator's masked view cache row(s); version 0
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

  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', 0, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;
