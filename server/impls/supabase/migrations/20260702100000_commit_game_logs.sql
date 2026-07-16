-- Fold the move's game_logs into commit_game.
--
-- Every move used to cost two awaited round-trips after compute: the
-- commit_game CAS, then a separate game_logs upsert (saveGameLogs). The logs
-- are now a parameter of commit_game, inserted in the SAME transaction as the
-- version-gated state write. One round-trip per move instead of two, and
-- strictly better correctness: logs can no longer be lost between a
-- successful commit and a failed follow-up insert (the replay encoder needs
-- the complete session), and a conflicted commit inserts nothing.
--
-- p_logs defaults to NULL so the previous 6-argument call shape keeps working
-- during deploy; the old 6-arg function must be dropped first or the new
-- default-argument version would be an ambiguous overload.
DROP FUNCTION IF EXISTS commit_game(TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB);

CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_logs             JSONB DEFAULT NULL
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
    good_players = g.good_players, updated_at = now(), version = version + 1
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

  -- This move's logs, atomic with the state they describe. Array order is
  -- preserved by jsonb_array_elements, so game_logs.seq keeps the emit order
  -- the replay encoder depends on. ON CONFLICT keeps the write idempotent
  -- (same UUIDs re-sent by a retried request insert nothing).
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
