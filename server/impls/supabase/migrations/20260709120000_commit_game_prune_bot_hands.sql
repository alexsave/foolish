-- commit_game: prune bot_hands for bots no longer in the roster, inside the same
-- version-fenced transaction. This lets handleExit (remove-bot) drop its separate
-- pre-commit `bot_hands` DELETE round-trip — one fewer cold-isolate round-trip on
-- the remove-bot path. Same 12-arg signature as 20260709050000, so CREATE OR
-- REPLACE (no DROP). Body identical to that migration except the bot_hands block,
-- which now runs even for an empty array (last bot removed) and prunes stragglers.

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

  -- bot_hands: upsert the current lobby bots, then prune any bot no longer in the
  -- roster (a removed bot) so handleExit needs no separate DELETE round-trip.
  -- Guarded to lobby commits (p_bot_hands NOT NULL); a dealt commit passes NULL,
  -- so mid-game bot hands are never touched. Empty array => prune every bot.
  IF p_bot_hands IS NOT NULL THEN
    IF jsonb_array_length(p_bot_hands) > 0 THEN
      INSERT INTO bot_hands (game_id, bot_id, hand, awaiting_attack)
      SELECT p_game_id, (b->>'bot_id')::uuid, b->'hand',
             COALESCE((b->>'awaiting_attack')::bool, false)
      FROM jsonb_array_elements(p_bot_hands) AS b
      ON CONFLICT (game_id, bot_id) DO UPDATE
        SET hand = EXCLUDED.hand, awaiting_attack = EXCLUDED.awaiting_attack, updated_at = now();
    END IF;

    DELETE FROM bot_hands
    WHERE game_id = p_game_id
      AND bot_id NOT IN (
        SELECT (b->>'bot_id')::uuid FROM jsonb_array_elements(p_bot_hands) AS b
      );
  END IF;

  -- player_views personalized-view cache (docs/PLAYER_VIEWS.md), written in THIS
  -- version-fenced transaction so it can never be torn from games.state.
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
