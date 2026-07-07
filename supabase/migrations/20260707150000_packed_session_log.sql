-- Packed session log on games (docs/PACKED_WIRE_CUTOVER.md, "logwire").
--
-- The game's session log stream — until now JSONB rows in game_logs — is
-- persisted as ONE append-only packed byte column, in the kernel's log
-- record format plus a u48 timestamp per record (the replay extras need
-- per-move timing). Appends ride inside the same version-fenced commit as
-- the state blob, so a move's log records land exactly-once by construction
-- (the old path needed a 10-minute freshness heuristic + ON CONFLICT id).
-- DRAW identities are masked before the bytes reach the database
-- (wasm_export_logs_masked / the appendLogs convention), so the stored
-- stream is safe for every reader, including bot belief imports.
--
-- The column holds BARE hex (no \x prefix) so appends are plain string
-- concatenation; hexToBytes accepts both forms. A session reset (GAME_START
-- in the appended records — start/continue) replaces the column instead of
-- appending, which is what "current session" used to mean via the
-- last-GAME_START scan over game_logs.
--
-- game_logs stays for legacy in-flight games (rows written before this
-- deploys); new commits stop writing it. Drop it once no active game
-- predates the column.

ALTER TABLE games ADD COLUMN IF NOT EXISTS logs_packed TEXT;

-- Signature change: drop the old 8-arg overload first (PostgREST must not
-- see two candidates).
DROP FUNCTION IF EXISTS commit_game(TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB, JSONB, TEXT);

CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_logs             JSONB   DEFAULT NULL,
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
    good_players = g.good_players, state = COALESCE(p_state, state),
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(p_logs_packed, '')
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

  -- Legacy skew window only: functions deployed before the packed session
  -- log still pass p_logs; new code always passes NULL.
  IF p_logs IS NOT NULL AND jsonb_array_length(p_logs) > 0 THEN
    INSERT INTO game_logs (id, game_id, log_type, player_id, card_pairs, defender_index, created_at)
    SELECT (l->>'id')::uuid,
           p_game_id,
           (l->>'log_type')::log_type,
           l->>'player_id',
           COALESCE(l->'card_pairs', '[]'::jsonb),
           (l->>'defender_index')::int,
           (l->>'created_at')::timestamp
    FROM jsonb_array_elements(p_logs) AS l
    ON CONFLICT (id) DO NOTHING;
  END IF;

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version);
END;
$$;
