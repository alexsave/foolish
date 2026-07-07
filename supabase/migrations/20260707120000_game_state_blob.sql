-- Packed kernel state blob on games.
--
-- The server now persists the whole VOLATILE game state (positions, deck,
-- battles, per-seat hands/status, good-mask, elimination) as one hex-encoded
-- blob produced by the C rules kernel (wasm_state_serialize, versioned) and
-- reconstructs from it on load — instead of re-parsing the scattered JSONB
-- columns + game_decks/player_hands/bot_hands joins. See engine.ts
-- serializeGameState/deserializeGameState and e2e/state_codec.test.ts (36k
-- round-trips, byte-lossless, ~96-byte blobs).
--
-- Transitional: the JSONB/hand columns are still dual-written as a
-- client-facing read-model (the web client reads player_hands/games directly),
-- so this is additive and reversible — dropping those tables + a get_game
-- edge function is a later step. Identity (id/name/strategy_key) and the two
-- presentation fields the kernel doesn't model (good_players order,
-- good_timestamp value) stay in their columns and reattach on load.

ALTER TABLE games ADD COLUMN IF NOT EXISTS state TEXT;

-- commit_game gains p_state (hex blob). Adding a parameter changes the
-- function's signature, so the old 7-arg overload must be dropped first —
-- otherwise PostgREST could resolve to either. p_state defaults NULL and is
-- COALESCEd, so a never-dealt (waiting) commit leaves the column untouched.
DROP FUNCTION IF EXISTS commit_game(TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB, JSONB);

CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_logs             JSONB DEFAULT NULL,
  p_state            TEXT  DEFAULT NULL
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
