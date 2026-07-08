-- Retire the game_logs table.
--
-- The session log has lived in the packed games.logs_packed column since
-- migration 20260707150000 (kernel log records + u48 timestamps, DRAW
-- identities pre-masked). game_logs — the original per-record JSONB store —
-- was kept only as a read fallback for sessions that were in flight when that
-- column shipped. Those are long gone, so the table (and the p_logs write path
-- that fed it) is now dead weight: nothing reads it (loadCurrentSessionLogs and
-- the finalize fallback were removed) and new commits never wrote it (p_logs
-- has been NULL since the packed cutover).
--
-- This drops the table, its log_type enum, and the commit_game p_logs argument.
-- The commit_game signature changes (one fewer parameter), so the old overload
-- is dropped first — PostgREST must not see two candidates.

-- 1. Redefine commit_game WITHOUT p_logs / the game_logs INSERT. Drop the old
--    10-arg overload first (CREATE OR REPLACE can't change the arg list).
DROP FUNCTION IF EXISTS commit_game(TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB, JSONB, TEXT, TEXT, BOOLEAN);

CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_state            TEXT    DEFAULT NULL,
  p_logs_packed      TEXT    DEFAULT NULL,
  p_logs_reset       BOOLEAN DEFAULT FALSE
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
    -- session's volatile state must NOT survive into the new lobby.
    state = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_state, state) END,
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
  -- above); per-record game_logs rows are retired.

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version);
END;
$$;

-- 2. Drop the table (CASCADE clears its indexes + RLS policies) and its enum.
DROP TABLE IF EXISTS game_logs CASCADE;
DROP TYPE  IF EXISTS log_type  CASCADE;
