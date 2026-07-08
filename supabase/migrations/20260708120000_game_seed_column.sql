-- Store the 32-byte deal seed (hex) that a game was dealt from, so a deal can be
-- regenerated/audited and, with the move log, the whole game replayed exactly.
--
-- SENSITIVE — server-only, exactly like the `state` blob: knowing the seed lets
-- anyone recompute the shuffled deck (every hidden hand + every future draw), so
-- it is NEVER granted to anon/authenticated. The table-level REVOKE +
-- column-level GRANT from 20260707140000_hide_state_blob.sql already hides any
-- column not explicitly listed there; game_seed is deliberately omitted, so no
-- grant change is needed here. It reaches clients through none of the payload
-- builders (personalize_game / gameToPublicGame / the packed view blob).

ALTER TABLE games ADD COLUMN IF NOT EXISTS game_seed TEXT;  -- 64 hex chars (two 128-bit lanes), or NULL for legacy/never-dealt

-- commit_game gains p_game_seed. Drop the old 10-arg signature first: a plain
-- CREATE OR REPLACE with an extra DEFAULT arg would leave the old overload in
-- place, and a 10-argument call would then match both and error as ambiguous.
DROP FUNCTION IF EXISTS commit_game(TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB, JSONB, TEXT, TEXT, BOOLEAN);

CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_logs             JSONB   DEFAULT NULL,  -- legacy skew window only; new code passes NULL
  p_state            TEXT    DEFAULT NULL,  -- packed kernel state blob (hex); NULL leaves the column unchanged (never-dealt games)
  p_logs_packed      TEXT    DEFAULT NULL,  -- this move's logwire records (bare hex), appended under the version fence
  p_logs_reset       BOOLEAN DEFAULT FALSE, -- session reset (GAME_START in the records): replace instead of append
  p_game_seed        TEXT    DEFAULT NULL   -- deal seed (hex); set once at the deal, NULL on every other commit leaves it unchanged
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
    -- Same discipline as `state`: a WAITING reset (continue/lobby) clears the
    -- finished session's seed so the next deal writes a fresh one; every dealt
    -- commit either sets it (the deal) or leaves it (COALESCE with NULL).
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
